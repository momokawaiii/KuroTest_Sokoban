#include "PuzzleViewportMode.h"

#include "CanvasItem.h"
#include "Components/ArrowComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "HitProxies.h"
#include "Materials/Material.h"
#include "PreviewScene.h"
#include "PrimitiveDrawInterface.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "UnrealWidget.h"

const FEditorModeID FPuzzleViewportMode::ModeId = TEXT("EM_ResonantRunesPuzzle");

namespace
{
    FBox ElementBounds(const FPuzzleLevelDefinition& Level, EPuzzleTool Tool, FIntVector P, bool bHiss)
    {
        const FVector Base = Level.GridToWorld(P);
        const float Step = Level.GridCellSize;
        switch (Tool)
        {
        case EPuzzleTool::Floor: return Level.FloorBounds(P);
        case EPuzzleTool::Wall: return Level.BlockBounds(P);
        case EPuzzleTool::Box: return Level.BoxBounds(P, bHiss);
        case EPuzzleTool::Goal: return FBox::BuildAABB(Base + FVector(0, 0, .035f * Level.LayerHeight()), FVector(.46f * Step, .46f * Step, .035f * Level.LayerHeight()));
        case EPuzzleTool::Player: return FBox::BuildAABB(Base + FVector(0, 0, .34f * Level.LayerHeight()), FVector(.20f * Step, .20f * Step, .34f * Level.LayerHeight()));
        default: return Level.BlockBounds(P);
        }
    }

    FBox BoardBounds(const FPuzzleLevelDefinition& Level)
    {
        const FVector Pad(Level.GridCellSize * .5, Level.GridCellSize * .5, Level.LayerHeight() * .5);
        return FBox(Level.GridToWorld(FIntVector::ZeroValue) - Pad, Level.GridToWorld(Level.GridDimensions - FIntVector(1)) + Pad);
    }
}

TOptional<FIntVector> FPuzzleViewportMode::OnLayer(const FPuzzleLevelDefinition& Level, int32 Layer, FVector Origin, FVector Direction)
{
    if (FMath::IsNearlyZero(Direction.Z)) return {};
    const double PlaneZ = Level.GridToWorld({0, 0, Layer}).Z;
    const double Time = (PlaneZ - Origin.Z) / Direction.Z;
    if (Time < 0.) return {};
    FIntVector Grid = Level.WorldToGrid(Origin + Direction * Time);
    Grid.Z = Layer;
    return Grid;
}

bool FPuzzleViewportMode::IsEditing() const
{
    return Model && !Model->bPlaying && Model->Level() && Model->Level()->HasValidGrid();
}

void FPuzzleViewportMode::Attach(TSharedRef<FPuzzleAuthoring> Authoring, FPreviewScene& Scene)
{
    Model = Authoring;
    Preview = &Scene;
}

void FPuzzleViewportMode::RefreshScene()
{
    if (!Preview || !Model) return;
    if (!Model->Crop) CropAnchor.Reset();
    const int32 SoloLayer = Model->bSoloLayer ? Model->Layer : INDEX_NONE;
    if (SceneRevision == Model->LayoutRevision && IsolatedLayer == SoloLayer) return;
    SceneRevision = Model->LayoutRevision;
    IsolatedLayer = SoloLayer;
    Hover.Reset();
    ClearScene();

    const auto* Level = Model->Level();
    if (!Level || !Level->HasValidGrid()) return;
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto* Stone = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Level/Element/M_Stone.M_Stone"));
    if (!Cube) return;

    struct FMeshBatch
    {
        FLinearColor Tint;
        TArray<FTransform> Transforms;
    };
    TMap<FColor, FMeshBatch> Batches;
    const auto Add = [&](FPuzzleElement Item, FLinearColor Tint, FBox Bounds)
    {
        if ((SoloLayer != INDEX_NONE && Item.Position.Z != SoloLayer) || !Level->IsValidGridCoordinate(Item.Position)) return;
        auto& Batch = Batches.FindOrAdd(Tint.ToFColor(false));
        Batch.Tint = Tint;
        Batch.Transforms.Emplace(FQuat::Identity, Bounds.GetCenter(), Bounds.GetSize() / 100.);
        Pieces.Add({Item, Bounds});
    };
    const int32 Area = Level->GridDimensions.X * Level->GridDimensions.Y;
    const int32 Begin = SoloLayer == INDEX_NONE ? 0 : SoloLayer * Area;
    const int32 End = SoloLayer == INDEX_NONE ? Level->StaticCells.Num() : Begin + Area;
    for (int32 Index = Begin; Index < End; ++Index)
    {
        const auto& Cell = Level->StaticCells[Index];
        if (!(Cell.bHasFloor || Cell.bBlocked)) continue;
        const auto P = Level->IndexToGrid(Index);
        if (Cell.bHasFloor) Add({EPuzzleTool::Floor, P}, {.27f, .33f, .43f}, Level->FloorBounds(P));
        if (Cell.bBlocked) Add({EPuzzleTool::Wall, P}, {.38f, .44f, .58f}, Level->BlockBounds(P));
    }
    for (const auto& Box : Level->InitialBoxes)
    {
        const FLinearColor Color = Box.bHiss ? FLinearColor(.59f, .23f, .83f) :
            Box.bPhaseLocked ? FLinearColor(.18f, .41f, .87f) : FLinearColor(.12f, .66f, .57f);
        Add({EPuzzleTool::Box, Box.GridPosition, Box.BoxId}, Color, Level->BoxBounds(Box.GridPosition, Box.bHiss));
    }
    for (const auto& Goal : Level->Goals)
    {
        const FBox Bounds = ElementBounds(*Level, EPuzzleTool::Goal, Goal.GridPosition, false);
        const FVector Extent = Bounds.GetExtent();
        const double Edge = Level->GridCellSize * .025;
        for (const double Sign : {-1., 1.})
        {
            Add({EPuzzleTool::Goal, Goal.GridPosition, Goal.GoalId}, {1.f, .61f, .14f},
                FBox::BuildAABB(Bounds.GetCenter() + FVector(Sign * (Extent.X - Edge), 0, 0), FVector(Edge, Extent.Y, Extent.Z)));
            Add({EPuzzleTool::Goal, Goal.GridPosition, Goal.GoalId}, {1.f, .61f, .14f},
                FBox::BuildAABB(Bounds.GetCenter() + FVector(0, Sign * (Extent.Y - Edge), 0), FVector(Extent.X - Edge * 2., Edge, Extent.Z)));
        }
    }
    const FIntVector Spawn = Level->PlayerStart;
    if (Level->IsValidGridCoordinate(Spawn) && (SoloLayer == INDEX_NONE || Spawn.Z == SoloLayer))
    {
        const FBox Bounds = ElementBounds(*Level, EPuzzleTool::Player, Spawn, false);
        Add({EPuzzleTool::Player, Spawn}, {.40f, .79f, .20f}, Bounds);

        const FIntPoint Facing = Level->GetPlayerStartDirection();
        const FVector Base = Level->GridToWorld(Spawn);
        const FVector Forward = Level->GridToWorld(Spawn + FIntVector(Facing.X, Facing.Y, 0)) - Base;
        const FVector ArrowOrigin(Base.X, Base.Y, Bounds.Max.Z + Level->GridCellSize * .06f);
        auto* Arrow = NewObject<UArrowComponent>(GetTransientPackage(), NAME_None, RF_Transient);
        Arrow->SetArrowFColor(FColor(167, 255, 98));
        Arrow->SetArrowSize(Level->GridCellSize * .64f / Arrow->ArrowLength);
        Preview->AddComponent(Arrow, FTransform(Forward.Rotation(), ArrowOrigin));
        PreviewComponents.Add(Arrow);
    }
    for (const auto& Pair : Batches)
    {
        auto* Mesh = NewObject<UInstancedStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
        Mesh->SetStaticMesh(Cube);
        if (Stone) Mesh->SetMaterial(0, Stone);
        const FLinearColor Tint = Pair.Value.Tint;
        Mesh->SetDefaultCustomPrimitiveDataVector4(0, FVector4(Tint.R, Tint.G, Tint.B, 1.));
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->AddInstances(Pair.Value.Transforms, false, false, false);
        Preview->AddComponent(Mesh, FTransform::Identity);
        PreviewComponents.Add(Mesh);
    }
}

void FPuzzleViewportMode::ClearScene()
{
    if (Preview)
        for (auto* Component : PreviewComponents) Preview->RemoveComponent(Component);
    PreviewComponents.Reset();
    Pieces.Reset();
}

void FPuzzleViewportMode::Exit()
{
    ClearScene();
    Preview = nullptr;
    Model.Reset();
    Hover.Reset();
    CropAnchor.Reset();
    DragWorld.Reset();
    FEdMode::Exit();
}

void FPuzzleViewportMode::FrameBoard(FEditorViewportClient& Client, bool bTopView)
{
    Client.SetViewportType(bTopView ? LVT_OrthoXY : LVT_Perspective);
    if (!bTopView) Client.SetViewRotation(FVector(1, 1, -1).Rotation());
    if (const auto* Level = Model->Level(); Level && Level->HasValidGrid())
    {
        const auto& Dims = Level->GridDimensions;
        const FVector HalfCell(Level->GridCellSize * .5, Level->GridCellSize * .5, 0.);
        FBox Frame(Level->GridToWorld({0, 0, Model->Layer}) - HalfCell,
            Level->GridToWorld({Dims.X - 1, Dims.Y - 1, Model->Layer}) + HalfCell);
        for (const auto& Piece : Pieces) Frame += Piece.Bounds;
        Client.FocusViewportOnBox(Frame.ExpandBy(Level->GridCellSize * .38), true);
        if (!bTopView)
        {
            const FVector Center = Client.GetLookAtLocation();
            const FVector Offset = Client.GetViewLocation() - Center;
            const FVector Focus = Center - FVector(0, 0, FMath::Min(double(Level->GridCellSize) * .26, Frame.GetExtent().Z * .20));
            Client.SetLookAtLocation(Focus);
            Client.SetViewLocation(Focus + Offset * 1.16);
        }
    }
    Client.Invalidate();
}

FPuzzleElement FPuzzleViewportMode::Pick(FVector Origin, FVector Direction) const
{
    const FBox Bounds = BoardBounds(*Model->Level());
    const double RayLength = FVector::Distance(Origin, Bounds.GetCenter()) + Bounds.GetSize().Length() + 100.;
    const FVector End = Origin + Direction * RayLength;
    float Closest = 1.f;
    FPuzzleElement Found;
    for (const auto& Piece : Pieces)
    {
        FVector HitPoint, Normal;
        float Time;
        if (FMath::LineExtentBoxIntersection(Piece.Bounds, Origin, End, FVector::ZeroVector, HitPoint, Normal, Time) && Time < Closest)
        {
            Closest = Time;
            Found = Piece.Element;
        }
    }
    return Found;
}

bool FPuzzleViewportMode::HandleClick(FEditorViewportClient* Client, HHitProxy* Hit, const FViewportClick& Click)
{
    if (!IsEditing() || Click.GetKey() != EKeys::LeftMouseButton || Click.IsAltDown()) return false;
    if (Model->Crop) return true;
    if (Hit && Hit->IsA(HWidgetAxis::StaticGetType())) return false;
    if (Model->Tool == EPuzzleTool::Erase)
    {
        if (!Model->Erase(Pick(Click.GetOrigin(), Click.GetDirection()))) Model->SetStatus(TEXT("点击要擦除的网格元素"));
    }
    else if (Model->Tool == EPuzzleTool::Select)
    {
        const auto Element = Pick(Click.GetOrigin(), Click.GetDirection());
        Model->Select(Element);
        if (Element.IsSet()) Client->GetModeTools()->SetWidgetMode(UE::Widget::WM_Translate);
    }
    else
    {
        const auto Position = OnLayer(*Model->Level(), Model->Layer, Click.GetOrigin(), Click.GetDirection());
        if (Position) Model->Place(*Position);
        else Model->SetStatus(TEXT("射线无法到达当前 Z 层，请用顶视图或透视视图"));
    }
    Client->Invalidate();
    return true;
}

bool FPuzzleViewportMode::MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y)
{
    if (!IsEditing()) return false;
    if (Model->Crop)
    {
        UpdateCropRange(*Client);
        return true;
    }
    const auto Ray = Client->GetCursorWorldLocationFromMousePos();
    const auto NewHover = OnLayer(*Model->Level(), Model->Layer, Ray.GetOrigin(), Ray.GetDirection());
    if (HoverViewport != Viewport || NewHover != Hover)
    {
        HoverViewport = Viewport;
        Hover = NewHover;
        Client->Invalidate(false, false);
    }
    return false;
}

bool FPuzzleViewportMode::CapturedMouseMove(FEditorViewportClient* Client, FViewport*, int32, int32)
{
    if (!IsEditing() || !Model->Crop || !CropAnchor) return false;
    UpdateCropRange(*Client);
    return true;
}

void FPuzzleViewportMode::UpdateCropRange(FEditorViewportClient& Client)
{
    if (!CropAnchor) return;
    const auto& Level = *Model->Level();
    const auto Ray = Client.GetCursorWorldLocationFromMousePos();
    const auto Point = OnLayer(Level, 0, Ray.GetOrigin(), Ray.GetDirection());
    if (!Point) return;

    auto& Crop = *Model->Crop;
    const FIntPoint Limit(Level.GridDimensions.X, Level.GridDimensions.Y);
    const FIntPoint End(FMath::Clamp(Point->X, 0, Limit.X - 1), FMath::Clamp(Point->Y, 0, Limit.Y - 1));
    FIntRect Range(FIntPoint(FMath::Min(CropAnchor->X, End.X), FMath::Min(CropAnchor->Y, End.Y)),
        FIntPoint(FMath::Max(CropAnchor->X, End.X) + 1, FMath::Max(CropAnchor->Y, End.Y) + 1));
    if (Crop.RetainedSize.X == Limit.X) { Range.Min.X = 0; Range.Max.X = Limit.X; }
    if (Crop.RetainedSize.Y == Limit.Y) { Range.Min.Y = 0; Range.Max.Y = Limit.Y; }
    Crop.Range = Range;
    Client.Invalidate(false, false);
}

bool FPuzzleViewportMode::MouseLeave(FEditorViewportClient* Client, FViewport* Viewport)
{
    if (HoverViewport == Viewport)
    {
        HoverViewport = nullptr;
        Hover.Reset();
        Client->Invalidate(false, false);
    }
    return false;
}

bool FPuzzleViewportMode::InputKey(FEditorViewportClient* Client, FViewport*, FKey Key, EInputEvent Event)
{
    if (!IsEditing()) return false;
    if (Model->Crop)
    {
        if (Key == EKeys::Escape && Event == IE_Pressed) { Model->CancelCrop(); return true; }
        if (Key == EKeys::Enter && Event == IE_Pressed) { Model->ApplyCrop(); return true; }
        if (Key != EKeys::LeftMouseButton) return false;
        if (Event == IE_Pressed)
        {
            CropAnchor.Reset();
            const auto Ray = Client->GetCursorWorldLocationFromMousePos();
            if (const auto Point = OnLayer(*Model->Level(), 0, Ray.GetOrigin(), Ray.GetDirection());
                Point && Model->Level()->IsValidGridCoordinate(*Point))
                CropAnchor = FIntPoint(Point->X, Point->Y);
        }
        UpdateCropRange(*Client);
        if (Event == IE_Released) CropAnchor.Reset();
        return true;
    }
    if (Event != IE_Pressed) return false;
    if (Key == EKeys::Escape && DragWorld) { bCancelDrag = true; Client->Invalidate(); return true; }
    if (Key == EKeys::Delete) { Model->Erase(Model->Selection); return true; }
    if (Key == EKeys::Tab && Model->Selection.Type == EPuzzleTool::Box) { Model->CycleBox(); return true; }
    if (Key == EKeys::PageUp || Key == EKeys::PageDown)
    {
        Model->SetLayer(Model->Layer + (Key == EKeys::PageUp ? 1 : -1));
        Hover.Reset();
        return true;
    }
    return false;
}

bool FPuzzleViewportMode::InputAxis(FEditorViewportClient* Client, FViewport* Viewport, int32, FKey Key, float Delta, float)
{
    if (!IsEditing() || !Client->IsPerspective() || DragWorld ||
        !Viewport->KeyState(EKeys::RightMouseButton) || Viewport->KeyState(EKeys::LeftMouseButton) ||
        Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt) ||
        (Key != EKeys::MouseX && Key != EKeys::MouseY)) return false;
    const FVector Focus = BoardBounds(*Model->Level()).GetCenter();
    const double UnitsPerPixel = FVector::Distance(Focus, Client->GetViewLocation()) * 1.5 / FMath::Max(1, Viewport->GetSizeXY().X);
    const FQuat ViewRotation = Client->GetViewRotation().Quaternion();
    const FVector Axis = Key == EKeys::MouseX ? ViewRotation.GetRightVector() : ViewRotation.GetUpVector();
    Client->SetViewLocation(Client->GetViewLocation() - Axis * Delta * UnitsPerPixel);
    Client->Invalidate(false, false);
    return true;
}

bool FPuzzleViewportMode::StartTracking(FEditorViewportClient* Client, FViewport* Viewport)
{
    if (!UsesTransformWidget() || !Viewport->KeyState(EKeys::LeftMouseButton) ||
        Client->GetCurrentWidgetAxis() == EAxisList::None) return false;
    DragWorld = Model->Level()->GridToWorld(Model->Resolve(Model->Selection).Position);
    bCancelDrag = false;
    return true;
}

bool FPuzzleViewportMode::InputDelta(FEditorViewportClient* Client, FViewport*, FVector& Drag, FRotator&, FVector&)
{
    if (!DragWorld) return false;
    if (!bCancelDrag) *DragWorld += Drag;
    Client->Invalidate();
    return true;
}

bool FPuzzleViewportMode::EndTracking(FEditorViewportClient*, FViewport*)
{
    if (!DragWorld) return false;
    const FVector Target = *DragWorld;
    DragWorld.Reset();
    if (!bCancelDrag && IsEditing()) Model->Move(Model->Level()->WorldToGrid(Target));
    return true;
}

bool FPuzzleViewportMode::UsesTransformWidget() const
{
    if (!IsEditing() || Model->Crop || Model->Tool != EPuzzleTool::Select) return false;
    const auto Selected = Model->Resolve(Model->Selection);
    return Selected.IsSet() && Selected.Type != EPuzzleTool::Player && (!Model->bSoloLayer || Selected.Position.Z == Model->Layer);
}

bool FPuzzleViewportMode::UsesTransformWidget(UE::Widget::EWidgetMode WidgetMode) const
{
    return WidgetMode == UE::Widget::WM_Translate && UsesTransformWidget();
}

EAxisList::Type FPuzzleViewportMode::GetWidgetAxisToDraw(UE::Widget::EWidgetMode WidgetMode) const
{
    return UsesTransformWidget(WidgetMode) ? EAxisList::XYZ : EAxisList::None;
}

FVector FPuzzleViewportMode::GetWidgetLocation() const
{
    if (!UsesTransformWidget()) return FVector::ZeroVector;
    const auto& Level = *Model->Level();
    const FIntVector Position = DragWorld && !bCancelDrag ? Level.WorldToGrid(*DragWorld) : Model->Resolve(Model->Selection).Position;
    return Level.GridToWorld(Position) + FVector(0, 0, .18f * Level.LayerHeight());
}

void FPuzzleViewportMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
    if (!IsEditing() || PDI->IsHitTesting()) return;
    const auto& Level = *Model->Level();
    const auto& Dims = Level.GridDimensions;
    const int32 Layer = Model->Layer;
    if (Model->bShowGrid || Model->Crop)
    {
        const auto Corner = [&](double X, double Y)
        {
            if (Level.bSwapXY) Swap(X, Y);
            return Level.GridToWorld({0, 0, Layer}) + FVector(X * Level.GridCellSize, Y * Level.GridCellSize, 1.);
        };
        const FLinearColor Guide(.31f, .40f, .53f, 1.f);
        for (int32 X = 0; X <= Dims.X; ++X) PDI->DrawLine(Corner(X - .5, -.5), Corner(X - .5, Dims.Y - .5), Guide, SDPG_World);
        for (int32 Y = 0; Y <= Dims.Y; ++Y) PDI->DrawLine(Corner(-.5, Y - .5), Corner(Dims.X - .5, Y - .5), Guide, SDPG_World);
    }

    if (Model->Crop)
    {
        const auto& Crop = *Model->Crop;
        if (Crop.Range.Width() > 0 && Crop.Range.Height() > 0)
        {
            const FVector Pad(Level.GridCellSize * .5, Level.GridCellSize * .5, .5);
            const FVector Min = Level.GridToWorld({Crop.Range.Min.X, Crop.Range.Min.Y, 0}) - Pad;
            const FVector Max = Level.GridToWorld({Crop.Range.Max.X - 1, Crop.Range.Max.Y - 1, 0}) + Pad;
            const FLinearColor Color = Crop.HasValidRange() ? FLinearColor(.22f, 1.f, .78f) : FLinearColor(1.f, .34f, .25f);
            DrawWireBox(PDI, FBox(Min, Max), Color, SDPG_Foreground, 3.f, 0.f, true);
        }
        return;
    }

    const FPuzzleElement Selected = Model->Resolve(Model->Selection);
    if (Selected.IsSet() && (!Model->bSoloLayer || Selected.Position.Z == Layer))
    {
        const auto* Box = Selected.Type == EPuzzleTool::Box ?
            Level.InitialBoxes.FindByPredicate([&](const auto& Item) { return Item.BoxId == Selected.Id; }) : nullptr;
        DrawWireBox(PDI, ElementBounds(Level, Selected.Type, Selected.Position, Box && Box->bHiss),
            FLinearColor(1.f, .75f, .2f), SDPG_Foreground, 2.f, 0.f, true);
    }

    const bool bDragging = DragWorld.IsSet() && !bCancelDrag;
    const bool bPlacing = Hover && HoverViewport == Viewport && Model->Tool != EPuzzleTool::Select && Model->Tool != EPuzzleTool::Erase;
    if (bDragging || bPlacing)
    {
        const auto Target = bDragging ? Level.WorldToGrid(*DragWorld) : *Hover;
        FPuzzleElement Item = bDragging ? Model->Selection : Model->Placement(Target);
        FString Reason;
        const bool bValid = Model->CheckPlacement(Item, Target, Reason);
        bool bHiss = false;
        if (bDragging && Item.Type == EPuzzleTool::Box)
            if (const auto* Box = Level.InitialBoxes.FindByPredicate([&](const auto& Box) { return Box.BoxId == Item.Id; })) bHiss = Box->bHiss;
        if (Target.GetMin() >= -1 && Target.X <= Dims.X && Target.Y <= Dims.Y && Target.Z <= Dims.Z)
            DrawWireBox(PDI, ElementBounds(Level, Item.Type, Target, bHiss),
                bValid ? FLinearColor(.22f, 1.f, .78f) : FLinearColor(1.f, .19f, .17f), SDPG_Foreground, 3.f, 0.f, true);
    }
}

void FPuzzleViewportMode::DrawHUD(FEditorViewportClient* Client, FViewport* Viewport, const FSceneView*, FCanvas* Canvas)
{
    if (!IsEditing()) return;
    if (Model->Crop) return;
    const bool bDragging = DragWorld.IsSet() && !bCancelDrag;
    const auto Position = bDragging ? TOptional<FIntVector>(Model->Level()->WorldToGrid(*DragWorld)) :
        HoverViewport == Viewport ? Hover : TOptional<FIntVector>{};
    if (!Position) return;
    const FString Caption = FString::Printf(TEXT("X %d    Y %d    Z %d"), Position->X, Position->Y, Position->Z);
    FString Reason;
    if (bDragging || (Model->Tool != EPuzzleTool::Select && Model->Tool != EPuzzleTool::Erase))
        Model->CheckPlacement(bDragging ? Model->Selection : Model->Placement(*Position), *Position, Reason);
    const float Baseline = Viewport->GetSizeXY().Y / Canvas->GetDPIScale() - 28.f;
    FCanvasTextItem Text(FVector2D(16, Baseline), FText::FromString(Caption), GEngine->GetSmallFont(), FLinearColor::White);
    Text.EnableShadow(FLinearColor::Black);
    Canvas->DrawItem(Text);
    if (!Reason.IsEmpty())
    {
        FCanvasTextItem Feedback(FVector2D(16, Baseline - 21.f), FText::FromString(Reason), GEngine->GetSmallFont(), FLinearColor(1.f, .57f, .53f));
        Feedback.EnableShadow(FLinearColor::Black);
        Canvas->DrawItem(Feedback);
    }
}
