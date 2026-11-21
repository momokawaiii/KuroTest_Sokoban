#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PuzzleLevel.generated.h"

namespace ShengLuLayout
{
    constexpr float Scale = 3.f;
    constexpr float CellSize = 200.f * Scale;
    constexpr float LayerHeight = 140.f * Scale;
    constexpr float CrateSide = 105.f * Scale;
}

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FGridCellData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid") bool bHasFloor = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid") bool bBlocked = false;
};

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FRuneBoxState
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Box") int32 BoxId = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Box") FIntVector GridPosition = FIntVector::ZeroValue;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Box") bool bHiss = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Box") bool bPhaseLocked = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Box") int32 LevitationTurns = 0;
};

UENUM(BlueprintType)
enum class ERuneGoalState : uint8 { Any, Normal, Hiss };

UENUM(BlueprintType)
enum class ERuneSpawnFacing : uint8
{
    PositiveX UMETA(DisplayName="+X"),
    PositiveY UMETA(DisplayName="+Y"),
    NegativeX UMETA(DisplayName="−X"),
    NegativeY UMETA(DisplayName="−Y")
};

UENUM(BlueprintType)
enum class ERuneSkill : uint8
{
    Pulse UMETA(DisplayName="极电千鸟"),
    Levitate UMETA(DisplayName="抟风九万"),
    WaterAnchor UMETA(DisplayName="沦灭潭渊"),
    Hiss UMETA(DisplayName="镜花水月")
};

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FRuneGoalData
{
    GENERATED_BODY()

    static constexpr int32 AnyBoxId = 0;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") int32 GoalId = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") FIntVector GridPosition = FIntVector::ZeroValue;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") int32 BoxId = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") ERuneGoalState RequiredState = ERuneGoalState::Any;
    // A pair contains the specified normal box and the specified Hiss box, in that order.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") bool bHissPair = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") int32 HissBoxId = INDEX_NONE;
    // Applies to BoxId, including the normal box of a Hiss pair.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Goal") bool bRequireWaterAnchor = false;
};

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FPuzzleLevelDefinition
{
    GENERATED_BODY()

    static constexpr int32 MaxAxis = 128;
    static constexpr int32 MaxCells = 65536;
    static inline const FIntVector InvalidPosition{-1, -1, -1};

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Level") FGuid LevelId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Level") FText DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid") FIntVector GridDimensions{5, 5, 3};
    // World origin is the center of the bottom face of cell (0, 0, 0).
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid") FVector GridOrigin = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid") float GridCellSize = 200.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grid") float LayerHeightOverride = 0.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grid") float BoxSideOverride = 0.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grid") bool bSwapXY = false;
    UPROPERTY() TArray<FGridCellData> StaticCells;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Level") TArray<FRuneBoxState> InitialBoxes;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Level") TArray<FRuneGoalData> Goals;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Level") FIntVector PlayerStart = InvalidPosition;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Level") ERuneSpawnFacing PlayerStartFacing = ERuneSpawnFacing::PositiveX;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Level") int32 InitialSP = 8;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tutorial")
    TArray<ERuneSkill> AvailableSkills{ERuneSkill::Pulse, ERuneSkill::Levitate, ERuneSkill::WaterAnchor, ERuneSkill::Hiss};
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tutorial", meta=(MultiLine="true")) FText EntryTip;
    // Read the old rule flag only to migrate the asset's crate dimensions and skill list.
    UPROPERTY() bool bShengLuRules_DEPRECATED = false;
    // Serialized name and default for assets saved before the skill list.
    UPROPERTY() int32 UnlockedSkillSlots = 4;
    UPROPERTY() int32 NextBoxId = 1;
    UPROPERTY() int32 NextGoalId = 1;

    static FPuzzleLevelDefinition MakeShengLu(FVector Origin = FVector::ZeroVector);
    static int32 CellCount(FIntVector Dimensions);
    bool HasValidGrid() const;
    bool IsValidGridCoordinate(FIntVector Position) const;
    int32 GridToIndex(FIntVector Position) const;
    FIntVector IndexToGrid(int32 Index) const;
    FVector GridToWorld(FIntVector Position) const;
    FIntVector WorldToGrid(FVector Position) const;
    FIntPoint GetPlayerStartDirection() const;
    const FGridCellData* CellAt(FIntVector Position) const;
    float LayerHeight() const { return LayerHeightOverride > 0.f ? LayerHeightOverride : GridCellSize; }
    float BoxSide() const { return BoxSideOverride > 0.f ? BoxSideOverride : .65f * FMath::Min(GridCellSize, LayerHeight()); }
    FBox FloorBounds(FIntVector Position) const;
    FBox BlockBounds(FIntVector Position) const;
    FBox BoxBounds(FIntVector Position, bool bHiss = false) const;
};

UCLASS(BlueprintType)
class RESONANTRUNES_API UPuzzleLevelAsset : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Puzzle") FPuzzleLevelDefinition Definition;

    void Serialize(FStructuredArchive::FRecord Record) override;
    void PostLoad() override;
};

UCLASS(BlueprintType)
class RESONANTRUNES_API UPuzzleLevelCatalog : public UDataAsset
{
    GENERATED_BODY()
public:
    static constexpr const TCHAR* Folder = TEXT("/Game/Level/Map/Puzzles");
    static constexpr const TCHAR* AssetPath = TEXT("/Game/Level/Map/Puzzles/DA_PuzzleCatalog.DA_PuzzleCatalog");
    static constexpr const TCHAR* PlayMap = TEXT("/Game/Level/Map/L_Puzzle");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Puzzle") TArray<TObjectPtr<UPuzzleLevelAsset>> Levels;

    UFUNCTION(BlueprintPure, Category="Puzzle") UPuzzleLevelAsset* FindLevel(FGuid LevelId) const;
    UPuzzleLevelAsset* NextLevel(const UPuzzleLevelAsset* Current = nullptr) const;
};
