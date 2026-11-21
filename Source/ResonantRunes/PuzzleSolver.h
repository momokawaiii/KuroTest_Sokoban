#pragma once

#include "PuzzleLevel.h"
#include "PuzzleSolver.generated.h"

UENUM(BlueprintType)
enum class ERuneAction : uint8
{
    North, East, South, West, Levitate, Pulse, WaterAnchor UMETA(DisplayName="PhaseLock"), Hiss,
    LevitateSelf, ClearPlayerWind
};

namespace RuneSkills
{
    struct FSkill { ERuneSkill Skill; ERuneAction Action; const TCHAR* Name; };
    inline constexpr FSkill Ordered[] = {
        {ERuneSkill::Pulse, ERuneAction::Pulse, TEXT("极电千鸟")},
        {ERuneSkill::Levitate, ERuneAction::Levitate, TEXT("抟风九万")},
        {ERuneSkill::WaterAnchor, ERuneAction::WaterAnchor, TEXT("沦灭潭渊")},
        {ERuneSkill::Hiss, ERuneAction::Hiss, TEXT("镜花水月")}
    };
    inline constexpr int32 Count = UE_ARRAY_COUNT(Ordered);
    RESONANTRUNES_API int32 Slot(ERuneAction Action);
    RESONANTRUNES_API bool IsUnlocked(const FPuzzleLevelDefinition& Level, ERuneAction Action);
    RESONANTRUNES_API FString LockedMessage(ERuneAction Action);
}

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FPuzzleAction
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Action") ERuneAction Action = ERuneAction::North;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Action") int32 BoxId = INDEX_NONE;
    // Zero uses the player's facing. A nonzero Pulse direction must be axial and one cell long.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Action") FIntVector Direction = FIntVector::ZeroValue;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Action") FIntVector ObstacleCell = FPuzzleLevelDefinition::InvalidPosition;
};

USTRUCT(BlueprintType)
struct RESONANTRUNES_API FPuzzleRuntimeState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Puzzle") FIntVector PlayerPosition = FIntVector::ZeroValue;
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") int32 PlayerWindTurns = 0;
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") FIntPoint Facing{1, 0};
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") int32 SP = 0;
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") int32 Turn = 0;
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") TArray<FRuneBoxState> Boxes;
    UPROPERTY(BlueprintReadOnly, Category="Puzzle") TSet<FIntVector> HissObstacles;

    static FPuzzleRuntimeState FromDefinition(const FPuzzleLevelDefinition& Level);
    const FRuneBoxState* FindBox(int32 BoxId) const;
    TArrayView<const int32> GetBoxesAt(FIntVector Position) const;
    void RebuildOccupancy();

private:
    friend class FPuzzleSolver;
    TMap<FIntVector, TArray<int32>> Occupancy;
    void MoveBox(FRuneBoxState& Box, FIntVector Position);
};

struct RESONANTRUNES_API FPuzzleActionResult
{
    FPuzzleRuntimeState After;
    FIntVector BeforeGravity = FPuzzleLevelDefinition::InvalidPosition;
    FString Reason;
    bool IsValid() const { return Reason.IsEmpty(); }
};

class RESONANTRUNES_API FPuzzleSolver
{
public:
    static constexpr int32 LevitationDuration = 2;
    static constexpr int32 PlayerWindDuration = 4;
    static int32 Cost(ERuneAction Action);
    static FIntPoint Direction(ERuneAction Action);
    static FIntVector PushFromLook(FIntVector PlayerPosition, FIntVector BoxPosition, FVector GridLook);
    static bool IsCellBlocked(const FPuzzleLevelDefinition& Level, FIntVector Position);
    static bool HasStaticSupport(const FPuzzleLevelDefinition& Level, FIntVector Position);
    static bool CanOverlap(const FRuneBoxState& A, const FRuneBoxState& B);
    static bool CanEnterCell(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State,
        const FRuneBoxState& Box, FIntVector Target);
    static bool HasSupport(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State, const FRuneBoxState& Box);
    static TArray<FString> Validate(const FPuzzleLevelDefinition& Level);
    static FPuzzleActionResult Evaluate(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& Current, FPuzzleAction Action);
    static bool IsRestored(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State);
    static bool CanContinue(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State);

private:
    static FString SettleGravity(const FPuzzleLevelDefinition& Level, FPuzzleRuntimeState& State);
};
