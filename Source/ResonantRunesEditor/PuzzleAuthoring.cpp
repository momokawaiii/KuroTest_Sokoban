#include "PuzzleAuthoring.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PlayInEditorDataTypes.h"
#include "ScopedTransaction.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ShengLuGame.h"

#define LOCTEXT_NAMESPACE "PuzzleAuthoring"

namespace
{
    const FString CatalogPackage = FPackageName::ObjectPathToPackageName(FString(UPuzzleLevelCatalog::AssetPath));
    const FString PuzzleAssetPrefix(TEXT("DA_Puzzle_"));

    bool IsValidAssetName(const FString& Folder, const FString& Name)
    {
        return FPackageName::IsValidObjectPath((Folder / Name) + TEXT(".") + Name);
    }

    template<typename T>
    T* MakeAsset(const FString& PackageName, FName Name)
    {
        auto* Asset = NewObject<T>(CreatePackage(*PackageName), Name, RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
        return Asset;
    }

    int32 NextBoxId(const FPuzzleLevelDefinition& Level)
    {
        int64 Next = FMath::Max(Level.NextBoxId, 1);
        for (const auto& Box : Level.InitialBoxes) Next = FMath::Max(Next, int64(Box.BoxId) + 1);
        for (const auto& Goal : Level.Goals)
            Next = FMath::Max(Next, int64(FMath::Max(Goal.BoxId, Goal.HissBoxId)) + 1);
        if (Next <= MAX_int32) return int32(Next);

        TSet<int32> UsedIds;
        for (const auto& Box : Level.InitialBoxes) UsedIds.Add(Box.BoxId);
        for (const auto& Goal : Level.Goals)
        {
            UsedIds.Add(Goal.BoxId);
            UsedIds.Add(Goal.HissBoxId);
        }
        int32 Available = 1;
        while (UsedIds.Contains(Available)) ++Available;
        return Available;
    }

    int32 NextGoalId(const FPuzzleLevelDefinition& Level)
    {
        int64 Next = FMath::Max(Level.NextGoalId, 1);
        for (const auto& Goal : Level.Goals) Next = FMath::Max(Next, int64(Goal.GoalId) + 1);
        return Next < MAX_int32 ? int32(Next) : INDEX_NONE;
    }
}

FText PuzzleToolName(EPuzzleTool Tool)
{
    switch (Tool)
    {
    case EPuzzleTool::Floor: return LOCTEXT("Floor", "地板");
    case EPuzzleTool::Wall: return LOCTEXT("Wall", "硬障碍");
    case EPuzzleTool::Box: return LOCTEXT("Box", "符文箱");
    case EPuzzleTool::Goal: return LOCTEXT("Goal", "阵眼");
    case EPuzzleTool::Player: return LOCTEXT("Spawn", "出生点");
    case EPuzzleTool::Erase: return LOCTEXT("Erase", "擦除");
    default: return LOCTEXT("Select", "选择");
    }
}

FPuzzleAuthoring::FPuzzleAuthoring(UPuzzleLevelCatalog* ExistingCatalog)
{
    if (!ExistingCatalog) ExistingCatalog = FindObject<UPuzzleLevelCatalog>(nullptr, UPuzzleLevelCatalog::AssetPath);
    if (!ExistingCatalog && FPackageName::DoesPackageExist(CatalogPackage))
        ExistingCatalog = LoadObject<UPuzzleLevelCatalog>(nullptr, UPuzzleLevelCatalog::AssetPath);
    Levels.Reset(ExistingCatalog);
    if (Levels.Get() && !Levels->Levels.IsEmpty()) SetActive(Levels->Levels[0]);
    GEditor->RegisterForUndo(this);
}

FPuzzleAuthoring::~FPuzzleAuthoring()
{
    if (GEditor) GEditor->UnregisterForUndo(this);
}

void FPuzzleAuthoring::PostUndo(bool bSuccess)
{
    if (!bSuccess) return;
    Crop.Reset();
    Status.Reset();
    if (Levels.Get() && !Levels->Levels.Contains(Active.Get()))
        Active.Reset(Levels->Levels.IsEmpty() ? nullptr : Levels->Levels[0].Get());
    Selection = Resolve(Selection);
    Changed(true);
}

void FPuzzleAuthoring::Changed(bool bLayoutChanged)
{
    Layer = Level() ? FMath::Clamp(Layer, 0, FMath::Max(0, Level()->GridDimensions.Z - 1)) : 0;
    if (bLayoutChanged)
    {
        InitialOccupancy = Level() ? FPuzzleRuntimeState::FromDefinition(*Level()) : FPuzzleRuntimeState{};
        ++LayoutRevision;
    }
    OnChanged.ExecuteIfBound();
}

void FPuzzleAuthoring::SetStatus(FString Message) { Status = MoveTemp(Message); }

void FPuzzleAuthoring::SetActive(UPuzzleLevelAsset* Asset)
{
    Active.Reset(Asset);
    Crop.Reset();
    Layer = 0;
    Selection = {};
    Status.Reset();
    Changed(true);
}

void FPuzzleAuthoring::SetTool(EPuzzleTool NewTool)
{
    Tool = NewTool;
    Status.Reset();
    Changed();
}

void FPuzzleAuthoring::SetLayer(int32 NewLayer)
{
    Layer = NewLayer;
    Changed();
}

void FPuzzleAuthoring::Select(FPuzzleElement Element)
{
    Selection = Resolve(Element);
    if (Selection.IsSet()) Layer = Selection.Position.Z;
    Changed();
}

bool FPuzzleAuthoring::EnsureCatalog()
{
    if (Levels.Get()) return true;
    if (FPackageName::DoesPackageExist(CatalogPackage))
    {
        SetStatus(TEXT("无法读取现有目录资产"));
        return false;
    }
    Levels.Reset(MakeAsset<UPuzzleLevelCatalog>(CatalogPackage, TEXT("DA_PuzzleCatalog")));
    return true;
}

bool FPuzzleAuthoring::AddLevel(FPuzzleLevelDefinition Definition, FString BaseName, bool bExactName)
{
    if (bPlaying) return false;
    const FString Folder(UPuzzleLevelCatalog::Folder);
    if (!IsValidAssetName(Folder, BaseName))
    {
        SetStatus(TEXT("棋盘资产名无效。可用文字、数字或下划线，不能包含空格或路径符号"));
        return false;
    }
    FString PackageName, AssetName;
    FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(Folder / BaseName, TEXT(""), PackageName, AssetName);
    if (bExactName && AssetName != BaseName)
    {
        SetStatus(TEXT("这个棋盘资产名已经存在，请更换后缀"));
        return false;
    }
    if (!EnsureCatalog() || Levels->FindLevel(Definition.LevelId)) return false;
    auto* Created = MakeAsset<UPuzzleLevelAsset>(PackageName, FName(AssetName));
    Created->Definition = MoveTemp(Definition);
    AddExisting(Created);
    return Active.Get() == Created;
}

void FPuzzleAuthoring::AddExisting(UPuzzleLevelAsset* Asset)
{
    if (bPlaying || !Asset || !EnsureCatalog()) return;
    const FGuid Id = Asset->Definition.LevelId;
    if (!Id.IsValid() || (Levels->FindLevel(Id) && Levels->FindLevel(Id) != Asset))
    {
        SetStatus(TEXT("资产的关卡 ID 无效或重复"));
        return;
    }
    if (!Levels->Levels.Contains(Asset))
    {
        const FScopedTransaction Transaction(LOCTEXT("AddLevel", "加入网格关卡"));
        Levels->Modify();
        const int32 ActiveIndex = Levels->Levels.IndexOfByKey(Active.Get());
        Levels->Levels.Insert(Asset, ActiveIndex == INDEX_NONE ? Levels->Levels.Num() : ActiveIndex + 1);
    }
    SetActive(Asset);
}

bool FPuzzleAuthoring::NewLevel(FText Name, FIntVector Dimensions, float CellSize, bool bFloor, FString AssetSuffix)
{
    FPuzzleLevelDefinition Level;
    Level.LevelId = FGuid::NewGuid();
    Level.DisplayName = FText::TrimPrecedingAndTrailing(Name);
    Level.GridDimensions = Dimensions;
    Level.GridCellSize = CellSize;
    Level.StaticCells.SetNum(FPuzzleLevelDefinition::CellCount(Dimensions));
    if (!Level.HasValidGrid() || Level.DisplayName.IsEmptyOrWhitespace())
    {
        SetStatus(TEXT("请输入名称、有效的 X/Y/Z 尺寸与正数格宽"));
        return false;
    }
    if (bFloor)
        for (int32 Index = 0; Index < Dimensions.X * Dimensions.Y; ++Index) Level.StaticCells[Index].bHasFloor = true;
    AssetSuffix.TrimStartAndEndInline();
    const bool bHasCustomSuffix = !AssetSuffix.IsEmpty();
    if (!bHasCustomSuffix) AssetSuffix = ObjectTools::SanitizeObjectName(Level.DisplayName.ToString());
    return AddLevel(MoveTemp(Level), PuzzleAssetPrefix + AssetSuffix, bHasCustomSuffix);
}

bool FPuzzleAuthoring::ImportShengLu()
{
    auto Definition = FPuzzleLevelDefinition::MakeShengLu();
    if (Levels.Get())
        if (auto* Legacy = Levels->FindLevel(Definition.LevelId)) { SetActive(Legacy); return true; }
    return AddLevel(MoveTemp(Definition), TEXT("DA_ShengLu"));
}

bool FPuzzleAuthoring::Duplicate()
{
    if (!Level()) return false;
    auto Copy = *Level();
    Copy.LevelId = FGuid::NewGuid();
    Copy.DisplayName = FText::Format(LOCTEXT("Copy", "{0}（副本）"), Copy.DisplayName);
    const FString Name = Asset()->GetName() + TEXT("_Copy");
    return AddLevel(MoveTemp(Copy), Name);
}

void FPuzzleAuthoring::Remove()
{
    if (!Levels.Get() || !Asset() || bPlaying) return;
    const int32 Index = Levels->Levels.IndexOfByKey(Active.Get());
    if (Index == INDEX_NONE) return;
    {
        const FScopedTransaction Transaction(LOCTEXT("RemoveLevel", "从关卡目录移除"));
        Levels->Modify();
        Levels->Levels.RemoveAt(Index);
    }
    SetActive(Levels->Levels.IsEmpty() ? nullptr : Levels->Levels[FMath::Min(Index, Levels->Levels.Num() - 1)].Get());
}

bool FPuzzleAuthoring::Reorder(UPuzzleLevelAsset* Source, UPuzzleLevelAsset* Target, bool bAfter)
{
    if (!Levels.Get() || bPlaying || !Source || !Target || Source == Target) return false;
    const int32 Index = Levels->Levels.IndexOfByKey(Source);
    const int32 TargetIndex = Levels->Levels.IndexOfByKey(Target);
    if (Index == INDEX_NONE || TargetIndex == INDEX_NONE) return false;
    const int32 Insertion = TargetIndex + (bAfter ? 1 : 0);
    const int32 NewIndex = Insertion - (Insertion > Index ? 1 : 0);
    if (NewIndex == Index) return false;
    {
        const FScopedTransaction Transaction(LOCTEXT("MoveLevel", "调整关卡顺序"));
        Levels->Modify();
        Levels->Levels.RemoveAt(Index);
        Levels->Levels.Insert(Source, NewIndex);
    }
    Changed();
    return true;
}

bool FPuzzleAuthoring::Rename(UPuzzleLevelAsset* Asset, const FText& Name)
{
    if (bPlaying || !Asset || !Levels.Get() || !Levels->Levels.Contains(Asset)) return false;
    const FText Trimmed = FText::TrimPrecedingAndTrailing(Name);
    if (Trimmed.IsEmptyOrWhitespace() || Trimmed.EqualTo(Asset->Definition.DisplayName)) return false;
    {
        const FScopedTransaction Transaction(LOCTEXT("RenameLevel", "修改关卡显示名称"));
        Asset->SetFlags(RF_Transactional);
        Asset->Modify();
        Asset->Definition.DisplayName = Trimmed;
        Asset->PostEditChange();
    }
    Changed();
    return true;
}

bool FPuzzleAuthoring::RenameAsset(FString Name)
{
    if (bPlaying || !Asset() || !Catalog()) return false;
    Name.TrimStartAndEndInline();
    const FString Folder = FPackageName::GetLongPackagePath(Asset()->GetOutermost()->GetName());
    if (Name.IsEmpty() || !IsValidAssetName(Folder, Name))
    {
        SetStatus(TEXT("请输入有效的棋盘资产名，可用文字、数字和下划线"));
        return false;
    }
    if (Asset()->GetName() == Name) return true;

    auto& AssetTools = FAssetToolsModule::GetModule().Get();
    FString PackageName, UniqueName;
    AssetTools.CreateUniqueAssetName(Folder / Name, TEXT(""), PackageName, UniqueName);
    if (UniqueName != Name)
    {
        SetStatus(TEXT("这个棋盘资产名已经存在，请换一个名字"));
        return false;
    }
    if (!AssetTools.RenameAssets({FAssetRenameData(Asset(), Folder, Name)}))
    {
        SetStatus(TEXT("UE 未能重命名棋盘资产，请检查输出日志"));
        return false;
    }
    Catalog()->MarkPackageDirty();
    SetStatus(FString::Printf(TEXT("棋盘资产已改名为 %s"), *Name));
    Changed();
    return true;
}

bool FPuzzleAuthoring::Save()
{
    if (!Levels.Get() || bPlaying) return false;
    TArray<UPackage*> Packages;
    for (UPuzzleLevelAsset* Level : Levels->Levels)
        if (Level) Packages.AddUnique(Level->GetOutermost());
    const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, true) &&
        UEditorLoadingAndSavingUtils::SavePackages({Levels->GetOutermost()}, true);
    SetStatus(bSaved ? TEXT("关卡资产与目录已保存") : TEXT("保存未完成，请检查输出日志"));
    Changed();
    return bSaved;
}

bool FPuzzleAuthoring::Validate()
{
    if (!Level()) return false;
    const auto Errors = FPuzzleSolver::Validate(*Level());
    SetStatus(Errors.IsEmpty() ? TEXT("校验通过") : FString::Join(Errors, TEXT("\n")));
    return Errors.IsEmpty();
}

bool FPuzzleAuthoring::TestLevel()
{
    if (bPlaying || Crop || GEditor->PlayWorld || !Validate()) return false;
    if (!FPackageName::DoesPackageExist(UPuzzleLevelCatalog::PlayMap))
    {
        SetStatus(TEXT("找不到共享地图 L_Puzzle，无法启动试玩"));
        return false;
    }
    AShengLuGameMode::SetPIELevel(Asset());
    FRequestPlaySessionParams Params;
    Params.WorldType = EPlaySessionWorldType::PlayInEditor;
    Params.GlobalMapOverride = UPuzzleLevelCatalog::PlayMap;
    Params.GameModeOverride = AShengLuGameMode::StaticClass();
    auto* PlaySettings = DuplicateObject(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
    PlaySettings->SetPlayNetMode(PIE_Standalone);
    PlaySettings->SetRunUnderOneProcess(true);
    PlaySettings->SetPlayNumberOfClients(1);
    PlaySettings->bLaunchSeparateServer = false;
    Params.EditorPlaySettings = PlaySettings;
    GEditor->RequestPlaySession(Params);
    return true;
}

void FPuzzleAuthoring::Commit(const FText& Label, TFunctionRef<void(FPuzzleLevelDefinition&)> Update)
{
    if (!Asset() || bPlaying) return;
    {
        const FScopedTransaction Transaction(Label);
        Asset()->SetFlags(RF_Transactional);
        Asset()->Modify();
        Update(Asset()->Definition);
        Asset()->PostEditChange();
    }
    Changed(true);
}

void FPuzzleAuthoring::UpdateLevel(TFunctionRef<void(FPuzzleLevelDefinition&)> Update)
{
    Commit(LOCTEXT("EditLevel", "编辑关卡属性"), Update);
}

bool FPuzzleAuthoring::Resize(FIntVector Dimensions, TOptional<FIntPoint> CropOrigin)
{
    if (!Level() || bPlaying || Dimensions == Level()->GridDimensions) return false;
    const int32 Count = FPuzzleLevelDefinition::CellCount(Dimensions);
    if (Count <= 0)
    {
        SetStatus(TEXT("尺寸无效。各轴 1–128，总格数最多 65536"));
        return false;
    }
    const auto OldSize = Level()->GridDimensions;
    const FIntPoint Retained(FMath::Min(Dimensions.X, OldSize.X), FMath::Min(Dimensions.Y, OldSize.Y));
    const bool bShrinking = Retained.X < OldSize.X || Retained.Y < OldSize.Y || Dimensions.Z < OldSize.Z;
    if (bShrinking && !CropOrigin)
    {
        SetStatus(TEXT("缩小网格需要在顶视图选择保留范围"));
        return false;
    }
    const auto Corner = CropOrigin.Get(FIntPoint::ZeroValue);
    if (Corner.X < 0 || Corner.Y < 0 || Corner.X > OldSize.X - Retained.X || Corner.Y > OldSize.Y - Retained.Y)
        return false;
    const FIntVector Offset(Corner.X, Corner.Y, 0);
    Crop.Reset();
    Commit(LOCTEXT("Resize", "调整网格尺寸"), [&](auto& Definition)
    {
        const auto Previous = Definition;
        Definition.GridDimensions = Dimensions;
        Definition.GridOrigin = Previous.GridToWorld(Offset);
        Definition.StaticCells.Reset();
        Definition.StaticCells.SetNum(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FIntVector Position = Definition.IndexToGrid(Index);
            if (const auto* Cell = Previous.CellAt(Position + Offset)) Definition.StaticCells[Index] = *Cell;
            else if (Position.Z == 0) Definition.StaticCells[Index].bHasFloor = true;
        }
        const auto Translate = [&](auto& Items)
        {
            for (auto& Item : Items) Item.GridPosition -= Offset;
            Items.RemoveAll([&](const auto& Item) { return !Definition.IsValidGridCoordinate(Item.GridPosition); });
        };
        Translate(Definition.InitialBoxes);
        Translate(Definition.Goals);
        if (Previous.IsValidGridCoordinate(Previous.PlayerStart))
        {
            Definition.PlayerStart -= Offset;
            if (!Definition.IsValidGridCoordinate(Definition.PlayerStart))
                Definition.PlayerStart = FPuzzleLevelDefinition::InvalidPosition;
        }
        if (Selection.IsSet())
        {
            Selection.Position -= Offset;
            Selection = Resolve(Selection);
        }
        Status = Definition.PlayerStart == FPuzzleLevelDefinition::InvalidPosition &&
            Previous.IsValidGridCoordinate(Previous.PlayerStart) ? TEXT("已裁剪。旧出生点超出范围，请重新放置") :
            bShrinking ? TEXT("已裁剪网格，可撤销") : TEXT("已扩大网格，新增底层格已铺地板");
    });
    return true;
}

bool FPuzzleAuthoring::BeginCrop(FIntVector Dimensions)
{
    if (bPlaying || !Level() || FPuzzleLevelDefinition::CellCount(Dimensions) <= 0)
    {
        SetStatus(TEXT("尺寸无效。各轴 1–128，总格数最多 65536"));
        return false;
    }
    const auto OldSize = Level()->GridDimensions;
    const FIntPoint Retained(FMath::Min(Dimensions.X, OldSize.X), FMath::Min(Dimensions.Y, OldSize.Y));
    Crop = FPuzzleGridCrop{Dimensions, Retained};
    if (Retained == FIntPoint(OldSize.X, OldSize.Y))
        Crop->Range = FIntRect(FIntPoint::ZeroValue, Retained);
    SetStatus(TEXT("在顶视图按住左键框选要保留的格子，然后点击「应用裁剪」。Esc 取消"));
    Changed();
    return true;
}

bool FPuzzleAuthoring::ApplyCrop()
{
    return Crop && Crop->HasValidRange() && Resize(Crop->Dimensions, Crop->Range.Min);
}

void FPuzzleAuthoring::CancelCrop()
{
    if (!Crop) return;
    Crop.Reset();
    SetStatus(TEXT("已取消裁剪"));
    Changed();
}

FPuzzleElement FPuzzleAuthoring::Resolve(FPuzzleElement Element) const
{
    if (!Level()) return {};
    const auto& Definition = *Level();
    if (Element.Type == EPuzzleTool::Box)
    {
        for (const auto& Box : Definition.InitialBoxes)
            if (Element.Id == Box.BoxId) return {Element.Type, Box.GridPosition, Box.BoxId};
    }
    else if (Element.Type == EPuzzleTool::Goal)
    {
        for (const auto& Goal : Definition.Goals)
            if (Element.Id == Goal.GoalId) return {Element.Type, Goal.GridPosition, Goal.GoalId};
    }
    else if (Element.Type == EPuzzleTool::Player && Definition.IsValidGridCoordinate(Definition.PlayerStart))
        return {Element.Type, Definition.PlayerStart};
    else if (const auto* Cell = Definition.CellAt(Element.Position))
    {
        if ((Element.Type == EPuzzleTool::Floor && Cell->bHasFloor) || (Element.Type == EPuzzleTool::Wall && Cell->bBlocked)) return Element;
    }
    return {};
}

FPuzzleElement FPuzzleAuthoring::Placement(FIntVector Position) const
{
    const auto* Definition = Level();
    const FIntVector Key = Tool == EPuzzleTool::Floor || Tool == EPuzzleTool::Wall ? FPuzzleLevelDefinition::InvalidPosition : Position;
    return {Tool, Key, !Definition ? INDEX_NONE : Tool == EPuzzleTool::Box ? NextBoxId(*Definition) :
        Tool == EPuzzleTool::Goal ? NextGoalId(*Definition) : INDEX_NONE};
}

bool FPuzzleAuthoring::CheckPlacement(FPuzzleElement Element, FIntVector Target, FString& Reason, const FRuneBoxState* Proposed) const
{
    const auto* Definition = Level();
    const auto* Cell = Definition ? Definition->CellAt(Target) : nullptr;
    if (!Cell) { Reason = TEXT("超出网格边界"); return false; }
    if ((Element.Type == EPuzzleTool::Box || Element.Type == EPuzzleTool::Goal) && Element.Id <= 0)
    { Reason = TEXT("元素需要有效 ID"); return false; }
    if (Element.Type == EPuzzleTool::Floor)
    {
        if (Cell->bHasFloor && Resolve(Element).Position != Target) { Reason = TEXT("该格已有地板"); return false; }
        return true;
    }
    if (Cell->bBlocked && Element.Type != EPuzzleTool::Box && !(Element.Type == EPuzzleTool::Wall && Resolve(Element).Position == Target))
    { Reason = TEXT("目标格已有硬障碍"); return false; }
    if (Element.Type == EPuzzleTool::Player && !FPuzzleSolver::HasStaticSupport(*Definition, Target))
    { Reason = TEXT("出生点需要地板或台顶"); return false; }
    if ((Element.Type == EPuzzleTool::Box || Element.Type == EPuzzleTool::Wall) && Definition->PlayerStart == Target)
    { Reason = TEXT("目标格是玩家出生点"); return false; }

    if (Element.Type == EPuzzleTool::Box)
    {
        FRuneBoxState Candidate{Element.Id, Target};
        if (Proposed) Candidate = *Proposed;
        else if (const auto* Existing = InitialOccupancy.FindBox(Element.Id)) Candidate = *Existing;
        if (!FPuzzleSolver::CanEnterCell(*Definition, InitialOccupancy, Candidate, Target))
        { Reason = TEXT("目标格有障碍或箱子占用冲突"); return false; }
    }
    if (Element.Type == EPuzzleTool::Player && !InitialOccupancy.GetBoxesAt(Target).IsEmpty())
    { Reason = TEXT("目标格已有符文箱"); return false; }
    if (Element.Type == EPuzzleTool::Wall)
        for (int32 BoxId : InitialOccupancy.GetBoxesAt(Target))
            if (!InitialOccupancy.FindBox(BoxId)->bHiss) { Reason = TEXT("目标格已有普通符文箱"); return false; }
    for (const auto& Goal : Definition->Goals)
        if (Goal.GridPosition == Target && (Element.Type == EPuzzleTool::Wall ||
            (Element.Type == EPuzzleTool::Goal && Element.Id != Goal.GoalId)))
        { Reason = TEXT("目标格已有阵眼"); return false; }
    return true;
}

bool FPuzzleAuthoring::Place(FIntVector Position)
{
    const auto Element = Placement(Position);
    if (bPlaying || !Element.IsSet() || !CheckPlacement(Element, Position, Status)) return false;
    Selection = Element;
    Selection.Position = Position;
    Commit(LOCTEXT("Place", "放置网格元素"), [&](auto& Definition)
    {
        auto& Cell = Definition.StaticCells[Definition.GridToIndex(Position)];
        switch (Tool)
        {
        case EPuzzleTool::Floor: Cell.bHasFloor = true; break;
        case EPuzzleTool::Wall: Cell.bBlocked = true; break;
        case EPuzzleTool::Box:
            Definition.InitialBoxes.Add({Element.Id, Position});
            Definition.NextBoxId = Element.Id == MAX_int32 ? 1 : Element.Id + 1;
            break;
        case EPuzzleTool::Goal:
        {
            Definition.Goals.Add({Element.Id, Position, FRuneGoalData::AnyBoxId});
            Definition.NextGoalId = Element.Id + 1;
            break;
        }
        case EPuzzleTool::Player: Definition.PlayerStart = Position; break;
        default: break;
        }
        Status.Reset();
    });
    return true;
}

bool FPuzzleAuthoring::Move(FIntVector Position)
{
    const auto Previous = Resolve(Selection);
    if (bPlaying || !Previous.IsSet() || Previous.Position == Position || !CheckPlacement(Previous, Position, Status)) return false;
    Selection.Position = Position;
    Layer = Position.Z;
    Commit(LOCTEXT("Move", "移动网格元素"), [&](auto& Definition)
    {
        auto& New = Definition.StaticCells[Definition.GridToIndex(Position)];
        switch (Previous.Type)
        {
        case EPuzzleTool::Floor: Definition.StaticCells[Definition.GridToIndex(Previous.Position)].bHasFloor = false; New.bHasFloor = true; break;
        case EPuzzleTool::Wall: Definition.StaticCells[Definition.GridToIndex(Previous.Position)].bBlocked = false; New.bBlocked = true; break;
        case EPuzzleTool::Box:
            Definition.InitialBoxes.FindByPredicate([&](const auto& Box) { return Box.BoxId == Previous.Id; })->GridPosition = Position;
            break;
        case EPuzzleTool::Goal:
            Definition.Goals.FindByPredicate([&](const auto& Goal) { return Goal.GoalId == Previous.Id; })->GridPosition = Position;
            break;
        case EPuzzleTool::Player: Definition.PlayerStart = Position; break;
        default: break;
        }
        Status.Reset();
    });
    return true;
}

void FPuzzleAuthoring::RotatePlayerStart(bool bClockwise)
{
    if (Crop || Resolve(Selection).Type != EPuzzleTool::Player) return;
    Commit(LOCTEXT("RotateSpawn", "旋转出生点"), [bClockwise](auto& Definition)
    {
        // Swapping the grid axes mirrors the direction of rotation in the viewport.
        const int32 Step = bClockwise != Definition.bSwapXY ? 1 : 3;
        const int32 Facing = (static_cast<int32>(Definition.PlayerStartFacing) + Step) % 4;
        Definition.PlayerStartFacing = static_cast<ERuneSpawnFacing>(Facing);
    });
}

bool FPuzzleAuthoring::Erase(FPuzzleElement Element)
{
    Element = Resolve(Element);
    if (bPlaying || !Element.IsSet()) return false;
    if (Element == Selection) Selection = {};
    Commit(LOCTEXT("Erase", "擦除网格元素"), [&](auto& Definition)
    {
        switch (Element.Type)
        {
        case EPuzzleTool::Floor: Definition.StaticCells[Definition.GridToIndex(Element.Position)].bHasFloor = false; break;
        case EPuzzleTool::Wall: Definition.StaticCells[Definition.GridToIndex(Element.Position)].bBlocked = false; break;
        case EPuzzleTool::Box: Definition.InitialBoxes.RemoveAll([&](const auto& Box) { return Box.BoxId == Element.Id; }); break;
        case EPuzzleTool::Goal: Definition.Goals.RemoveAll([&](const auto& Goal) { return Goal.GoalId == Element.Id; }); break;
        case EPuzzleTool::Player: Definition.PlayerStart = FPuzzleLevelDefinition::InvalidPosition; break;
        default: break;
        }
    });
    return true;
}

void FPuzzleAuthoring::ChangeBox(const FRuneBoxState& Box)
{
    if (!CheckPlacement(Selection, Box.GridPosition, Status, &Box)) return;
    Commit(LOCTEXT("EditBox", "编辑符文箱状态"), [&](auto& Definition)
    {
        if (auto* Found = Definition.InitialBoxes.FindByPredicate([&](const auto& Item) { return Item.BoxId == Box.BoxId; }))
            *Found = Box;
        Status.Reset();
    });
}

void FPuzzleAuthoring::ChangeGoal(const FRuneGoalData& Goal)
{
    Commit(LOCTEXT("EditGoal", "编辑阵眼条件"), [&](auto& Definition)
    {
        if (auto* Found = Definition.Goals.FindByPredicate([&](const auto& Item) { return Item.GoalId == Goal.GoalId; }))
            *Found = Goal;
        Status.Reset();
    });
}

bool FPuzzleAuthoring::RenumberBox(int32 BoxId)
{
    const auto Box = Resolve(Selection);
    if (bPlaying || Box.Type != EPuzzleTool::Box || Box.Id == BoxId) return false;
    if (BoxId <= 0 || InitialOccupancy.FindBox(BoxId))
    {
        SetStatus(TEXT("箱子编号必须是未被其他箱子使用的正整数"));
        return false;
    }
    Selection.Id = BoxId;
    Commit(LOCTEXT("RenumberBox", "修改符文箱编号"), [&](auto& Definition)
    {
        Definition.InitialBoxes.FindByPredicate([&](const auto& Item) { return Item.BoxId == Box.Id; })->BoxId = BoxId;
        for (auto& Goal : Definition.Goals)
        {
            if (Goal.BoxId == Box.Id) Goal.BoxId = BoxId;
            if (Goal.HissBoxId == Box.Id) Goal.HissBoxId = BoxId;
        }
        Status.Reset();
    });
    return true;
}

void FPuzzleAuthoring::CycleBox()
{
    if (!Level() || Selection.Type != EPuzzleTool::Box) return;
    for (const auto& Box : Level()->InitialBoxes)
    {
        if (Box.GridPosition != Selection.Position || Box.BoxId == Selection.Id) continue;
        Select({EPuzzleTool::Box, Box.GridPosition, Box.BoxId});
        return;
    }
}

float FPuzzleAuthoring::DefaultCellSize() const
{
    const auto* First = Levels.Get() ? Levels->NextLevel() : nullptr;
    return First ? First->Definition.GridCellSize : ShengLuLayout::CellSize;
}

bool FPuzzleAuthoring::CreateDemo()
{
    const FText Name = LOCTEXT("DemoName", "Z1 叠相练习");
    if (Levels.Get())
        for (UPuzzleLevelAsset* Existing : Levels->Levels)
            if (Existing && Existing->Definition.DisplayName.EqualTo(Name)) { SetActive(Existing); return true; }
    if (!NewLevel(Name, {4, 4, 3}, DefaultCellSize(), true)) return false;
    UpdateLevel([](auto& Definition) { Definition.InitialSP = 12; });
    SetTool(EPuzzleTool::Player);
    if (!Place({1, 0, 0})) return false;
    SetTool(EPuzzleTool::Wall);
    if (!Place({3, 2, 0})) return false;
    SetTool(EPuzzleTool::Box);
    if (!Place({2, 1, 1})) return false;
    FRuneBoxState Anchor = Level()->InitialBoxes.Last();
    Anchor.bPhaseLocked = true;
    ChangeBox(Anchor);
    if (!Place({1, 1, 0})) return false;
    SetTool(EPuzzleTool::Goal);
    if (!Place({2, 1, 1})) return false;
    FRuneGoalData Goal = Level()->Goals.Last();
    Goal.bHissPair = true;
    Goal.BoxId = Anchor.BoxId;
    Goal.HissBoxId = Level()->InitialBoxes.Last().BoxId;
    ChangeGoal(Goal);
    SetLayer(1);
    SetTool(EPuzzleTool::Select);
    return Validate();
}

#undef LOCTEXT_NAMESPACE
