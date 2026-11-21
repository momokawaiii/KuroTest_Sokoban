#pragma once

#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "GameFramework/Pawn.h"
#include "NativeGameplayTags.h"
#include "RuneBoxPawn.generated.h"

struct FRuneBoxState;
class UStaticMeshComponent;

namespace RuneBoxTags
{
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Levitating);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(PhaseLocked);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Hiss);
}

UCLASS()
class URuneBoxStatusEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    URuneBoxStatusEffect();
};

UCLASS()
class RESONANTRUNES_API ARuneBoxPawn : public APawn, public IAbilitySystemInterface
{
    GENERATED_BODY()

public:
    ARuneBoxPawn();
    UAbilitySystemComponent* GetAbilitySystemComponent() const override;
    UStaticMeshComponent* GetBoxMesh() const { return Mesh; }
    UFUNCTION(BlueprintPure, Category="Puzzle") int32 GetBoxId() const { return BoxId; }
    void SyncState(const FRuneBoxState& State);

protected:
    void PostInitializeComponents() override;

private:
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
    UPROPERTY() TObjectPtr<UAbilitySystemComponent> Abilities;
    int32 BoxId = INDEX_NONE;
    FActiveGameplayEffectHandle Levitation, PhaseLock, Hiss;
};
