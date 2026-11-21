#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "PuzzleSolver.h"
#include "UObject/StrongObjectPtr.h"

enum class EPuzzleTool : uint8 { Select, Floor, Wall, Box, Goal, Player, Erase };

struct FPuzzleElement
{
    EPuzzleTool Type = EPuzzleTool::Select;
    FIntVector Position = FPuzzleLevelDefinition::InvalidPosition;
    int32 Id = INDEX_NONE;
    bool IsSet() const { return Type != EPuzzleTool::Select && Type != EPuzzleTool::Erase; }
    bool operator==(const FPuzzleElement& Other) const
    {
        if (Type != Other.Type) return false;
        if (Type == EPuzzleTool::Box || Type == EPuzzleTool::Goal) return Id == Other.Id;
        return Position == Other.Position;
    }
};

FText PuzzleToolName(EPuzzleTool Tool);

struct FPuzzleGridCrop
{
    FIntVector Dimensions;
    FIntPoint RetainedSize;
    FIntRect Range{0, 0, 0, 0};

    bool HasValidRange() const { return Range.Size() == RetainedSize; }
};

class FPuzzleAuthoring : public FEditorUndoClient
{
public:
    explicit FPuzzleAuthoring(UPuzzleLevelCatalog* ExistingCatalog = nullptr);
    ~FPuzzleAuthoring();
    void PostUndo(bool bSuccess) override;
    void PostRedo(bool bSuccess) override { PostUndo(bSuccess); }

    const FPuzzleLevelDefinition* Level() const { return Active.Get() ? &Active->Definition : nullptr; }
    UPuzzleLevelAsset* Asset() const { return Active.Get(); }
    UPuzzleLevelCatalog* Catalog() const { return Levels.Get(); }
    float DefaultCellSize() const;
    void SetActive(UPuzzleLevelAsset* Asset);
    void SetTool(EPuzzleTool Tool);
    void SetLayer(int32 Layer);
    void Select(FPuzzleElement Element);
    void Changed(bool bLayoutChanged = false);
    void SetStatus(FString Message);
    bool Resize(FIntVector Dimensions, TOptional<FIntPoint> CropOrigin = {});
    bool BeginCrop(FIntVector Dimensions);
    bool ApplyCrop();
    void CancelCrop();
    bool NewLevel(FText Name, FIntVector Dimensions, float CellSize, bool bFloor, FString AssetSuffix = {});
    bool ImportShengLu();
    bool Duplicate();
    void AddExisting(UPuzzleLevelAsset* Asset);
    void Remove();
    bool Reorder(UPuzzleLevelAsset* Source, UPuzzleLevelAsset* Target, bool bAfter);
    bool Rename(UPuzzleLevelAsset* Asset, const FText& Name);
    bool RenameAsset(FString Name);
    bool Save();
    bool Validate();
    bool TestLevel();
    bool CreateDemo();
    void UpdateLevel(TFunctionRef<void(FPuzzleLevelDefinition&)> Update);
    bool CheckPlacement(FPuzzleElement Element, FIntVector Target, FString& Reason, const FRuneBoxState* Proposed = nullptr) const;
    bool Place(FIntVector Position);
    bool Move(FIntVector Position);
    void RotatePlayerStart(bool bClockwise);
    bool Erase(FPuzzleElement Element);
    void ChangeBox(const FRuneBoxState& Values);
    bool RenumberBox(int32 BoxId);
    void ChangeGoal(const FRuneGoalData& Values);
    void CycleBox();
    FPuzzleElement Resolve(FPuzzleElement Element) const;
    FPuzzleElement Placement(FIntVector Position) const;

    FSimpleDelegate OnChanged;
    FString Status;
    EPuzzleTool Tool = EPuzzleTool::Select;
    FPuzzleElement Selection;
    TOptional<FPuzzleGridCrop> Crop;
    int32 Layer = 0;
    bool bSoloLayer = false;
    bool bShowGrid = true;
    bool bPlaying = false;
    uint64 LayoutRevision = 0;

private:
    bool EnsureCatalog();
    bool AddLevel(FPuzzleLevelDefinition Definition, FString BaseName, bool bExactName = false);
    void Commit(const FText& Label, TFunctionRef<void(FPuzzleLevelDefinition&)> Update);
    TStrongObjectPtr<UPuzzleLevelCatalog> Levels;
    TStrongObjectPtr<UPuzzleLevelAsset> Active;
    FPuzzleRuntimeState InitialOccupancy;
};
