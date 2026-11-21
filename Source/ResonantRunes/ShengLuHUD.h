#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "PuzzleSolver.h"
#include "ShengLuHUD.generated.h"

class AShengLuBoard;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
namespace EMouseCursor { enum Type : int; }

enum class ERuneHUDAction : uint8
{
    Skill1, Skill2, Skill3, Skill4, Undo, Retry, Accept, Inspect
};

UCLASS()
class RESONANTRUNES_API AShengLuHUD : public AHUD
{
    GENERATED_BODY()

public:
    AShengLuHUD();
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    void Tick(float DeltaSeconds) override;
    void PostRender() override;
    void DrawHUD() override;
    bool TryClick(FVector2D Mouse);
    bool RespondToResult(ERuneHUDAction Action);

private:
    struct FButton
    {
        ERuneHUDAction Action;
        FVector2D Position, Size;
        float Skew;

        bool Contains(FVector2D Point) const;
    };

    struct FMotion
    {
        void Update(AShengLuBoard& Board, double Now, FStringView Operation, bool bCanOperate);
        float ResultVisibility(double Now) const;

        TWeakObjectPtr<AShengLuBoard> Board;
        TOptional<ERuneAction> Selection;
        FString Operation;
        FString ContextTitle;
        int32 SP = 0;
        bool bCanOperate = false;
        double EnteredAt = 0., EquippedAt = -10., OperationAt = -10., SpentAt = -10., ResultAt = -1.;
        double ExitAt = -1., ContextAt = -10.;
        int32 HoveredSkill = INDEX_NONE;
        double HoveredAt = 0.;
        TOptional<ERuneHUDAction> PendingAction;
    };

    FMotion Motion;
    bool bResultButtons = false;
    TArray<FButton, TInlineAllocator<8>> Buttons;
    UPROPERTY() TObjectPtr<UMaterialInterface> HUDMaterial;
    UPROPERTY() TObjectPtr<UTexture2D> SkillAtlas;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> CursorTexture;
    FIntPoint CursorHotspot = FIntPoint::ZeroValue;
    TOptional<EMouseCursor::Type> PreviousMouseCursor;
};
