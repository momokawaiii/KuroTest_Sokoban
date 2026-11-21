#include "PuzzleLevel.h"
#include "PuzzleSolver.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/StructuredArchive.h"

namespace
{
    const FGuid PuzzleVersionId(0xA5F6A530, 0xBCFB4604, 0x818FBB27, 0xC32E2297);
    constexpr int32 ExplicitSkillList = 1;
    constexpr int32 IndependentBoxSize = 2;
    const FCustomVersionRegistration PuzzleVersion(PuzzleVersionId, IndependentBoxSize, TEXT("ResonantRunesPuzzle"));
}

int32 FPuzzleLevelDefinition::CellCount(FIntVector Dimensions)
{
    if (Dimensions.GetMin() <= 0 || Dimensions.GetMax() > MaxAxis) return 0;
    const int32 Count = Dimensions.X * Dimensions.Y * Dimensions.Z;
    return Count <= MaxCells ? Count : 0;
}

bool FPuzzleLevelDefinition::HasValidGrid() const
{
    const int32 Count = CellCount(GridDimensions);
    return Count > 0 && Count == StaticCells.Num() && !GridOrigin.ContainsNaN() &&
        FMath::IsFinite(GridCellSize) && GridCellSize > 0.f &&
        FMath::IsFinite(LayerHeightOverride) && LayerHeightOverride >= 0.f &&
        FMath::IsFinite(BoxSideOverride) && BoxSideOverride >= 0.f;
}

bool FPuzzleLevelDefinition::IsValidGridCoordinate(FIntVector Position) const
{
    return Position.GetMin() >= 0 && Position.X < GridDimensions.X && Position.Y < GridDimensions.Y &&
        Position.Z < GridDimensions.Z;
}

int32 FPuzzleLevelDefinition::GridToIndex(FIntVector Position) const
{
    if (!IsValidGridCoordinate(Position) || CellCount(GridDimensions) == 0) return INDEX_NONE;
    return Position.X + GridDimensions.X * (Position.Y + GridDimensions.Y * Position.Z);
}

FIntVector FPuzzleLevelDefinition::IndexToGrid(int32 Index) const
{
    if (Index < 0 || Index >= CellCount(GridDimensions)) return InvalidPosition;
    return {Index % GridDimensions.X, (Index / GridDimensions.X) % GridDimensions.Y,
        Index / (GridDimensions.X * GridDimensions.Y)};
}

FVector FPuzzleLevelDefinition::GridToWorld(FIntVector Position) const
{
    if (bSwapXY) Swap(Position.X, Position.Y);
    return GridOrigin + FVector(Position.X * double(GridCellSize), Position.Y * double(GridCellSize), Position.Z * double(LayerHeight()));
}

FIntVector FPuzzleLevelDefinition::WorldToGrid(FVector Position) const
{
    if (GridCellSize <= 0.f || LayerHeight() <= 0.f) return InvalidPosition;
    FVector Offset = Position - GridOrigin;
    if (bSwapXY) Swap(Offset.X, Offset.Y);
    const FVector Scaled(Offset.X / GridCellSize, Offset.Y / GridCellSize, Offset.Z / LayerHeight());
    if (Scaled.ContainsNaN() || Scaled.GetAbsMax() >= double(MAX_int32) - 1.) return InvalidPosition;
    return {FMath::RoundToInt32(Scaled.X), FMath::RoundToInt32(Scaled.Y), FMath::RoundToInt32(Scaled.Z)};
}

FIntPoint FPuzzleLevelDefinition::GetPlayerStartDirection() const
{
    switch (PlayerStartFacing)
    {
    case ERuneSpawnFacing::PositiveY: return {0, 1};
    case ERuneSpawnFacing::NegativeX: return {-1, 0};
    case ERuneSpawnFacing::NegativeY: return {0, -1};
    default: return {1, 0};
    }
}

const FGridCellData* FPuzzleLevelDefinition::CellAt(FIntVector Position) const
{
    const int32 Index = GridToIndex(Position);
    return StaticCells.IsValidIndex(Index) ? &StaticCells[Index] : nullptr;
}

FBox FPuzzleLevelDefinition::FloorBounds(FIntVector Position) const
{
    const float HalfHeight = .05f * GridCellSize;
    return FBox::BuildAABB(GridToWorld(Position) - FVector(0, 0, HalfHeight), FVector(.48f * GridCellSize, .48f * GridCellSize, HalfHeight));
}

FBox FPuzzleLevelDefinition::BlockBounds(FIntVector Position) const
{
    const float HalfHeight = LayerHeight() * .5f;
    return FBox::BuildAABB(GridToWorld(Position) + FVector(0, 0, HalfHeight), FVector(.46f * GridCellSize, .46f * GridCellSize, HalfHeight));
}

FBox FPuzzleLevelDefinition::BoxBounds(FIntVector Position, bool bHiss) const
{
    const float Side = BoxSide();
    const float Half = .5f * Side;
    // Offset each axis to keep the faces of two intersecting boxes from being coplanar.
    const FVector PhaseOffset = bHiss ? FVector(.25, .06, .01) * Side : FVector::ZeroVector;
    return FBox::BuildAABB(GridToWorld(Position) + FVector(0, 0, Half) + PhaseOffset, FVector(Half));
}

FPuzzleLevelDefinition FPuzzleLevelDefinition::MakeShengLu(FVector Origin)
{
    FPuzzleLevelDefinition Level;
    Level.LevelId = FGuid(0x3B6B290C, 0x526F4A23, 0x84706530, 0x85654102);
    Level.DisplayName = NSLOCTEXT("Puzzle", "ShengLu", "升律 · 第 2 关");
    Level.GridDimensions = FIntVector(3, 3, 2);
    Level.GridOrigin = Origin;
    Level.GridCellSize = ShengLuLayout::CellSize;
    Level.LayerHeightOverride = ShengLuLayout::LayerHeight;
    Level.BoxSideOverride = ShengLuLayout::CrateSide;
    Level.bSwapXY = true;
    Level.AvailableSkills.Remove(ERuneSkill::Hiss);
    Level.StaticCells.SetNum(CellCount(Level.GridDimensions));
    for (int32 Index = 0; Index < 9; ++Index) Level.StaticCells[Index].bHasFloor = true;
    Level.StaticCells[Level.GridToIndex({2, 1, 0})].bBlocked = true;
    Level.InitialBoxes.Add({1, {1, 1, 0}});
    Level.Goals.Add({1, {2, 1, 1}, 1});
    Level.PlayerStart = FIntVector::ZeroValue;
    Level.NextBoxId = Level.NextGoalId = 2;
    return Level;
}

void UPuzzleLevelAsset::Serialize(FStructuredArchive::FRecord Record)
{
    Record.GetUnderlyingArchive().UsingCustomVersion(PuzzleVersionId);
    Super::Serialize(Record);
}

void UPuzzleLevelAsset::PostLoad()
{
    Super::PostLoad();
    const int32 Version = GetLinkerCustomVersion(PuzzleVersionId);
    if (Version < IndependentBoxSize && Definition.bShengLuRules_DEPRECATED)
        Definition.BoxSideOverride = ShengLuLayout::CrateSide;
    if (Version >= ExplicitSkillList) return;

    const int32 MaxSlots = Definition.bShengLuRules_DEPRECATED ? RuneSkills::Count - 1 : RuneSkills::Count;
    const int32 Count = FMath::Clamp(Definition.UnlockedSkillSlots, 1, MaxSlots);
    Definition.AvailableSkills.Reset(Count);
    for (int32 Index = 0; Index < Count; ++Index)
        Definition.AvailableSkills.Add(RuneSkills::Ordered[Index].Skill);
    Definition.UnlockedSkillSlots = RuneSkills::Count;
}

UPuzzleLevelAsset* UPuzzleLevelCatalog::FindLevel(FGuid LevelId) const
{
    if (!LevelId.IsValid()) return nullptr;
    for (UPuzzleLevelAsset* Level : Levels)
        if (Level && Level->Definition.LevelId == LevelId) return Level;
    return nullptr;
}

UPuzzleLevelAsset* UPuzzleLevelCatalog::NextLevel(const UPuzzleLevelAsset* Current) const
{
    bool bTakeNext = Current == nullptr;
    for (UPuzzleLevelAsset* Level : Levels)
    {
        if (!Level || !Level->Definition.LevelId.IsValid()) continue;
        if (bTakeNext) return Level;
        bTakeNext = Current == Level;
    }
    return nullptr;
}
