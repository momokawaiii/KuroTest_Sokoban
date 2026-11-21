#include "RuneBoxPawn.h"

#include "AbilitySystemComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PuzzleLevel.h"

namespace RuneBoxTags
{
    UE_DEFINE_GAMEPLAY_TAG(Levitating, "Puzzle.Box.Levitating");
    UE_DEFINE_GAMEPLAY_TAG(PhaseLocked, "Puzzle.Box.PhaseLocked");
    UE_DEFINE_GAMEPLAY_TAG(Hiss, "Puzzle.Box.Hiss");
}

URuneBoxStatusEffect::URuneBoxStatusEffect()
{
    // The solver ends statuses on puzzle turns. A GAS wall-clock timer would run while aiming.
    DurationPolicy = EGameplayEffectDurationType::Infinite;
}

ARuneBoxPawn::ARuneBoxPawn()
{
    AutoPossessAI = EAutoPossessAI::Disabled;
    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoxMesh"));
    Mesh->SetMobility(EComponentMobility::Movable);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RootComponent = Mesh;
    Abilities = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("BoxAbilities"));
}

void ARuneBoxPawn::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    Abilities->InitAbilityActorInfo(this, this);
}

UAbilitySystemComponent* ARuneBoxPawn::GetAbilitySystemComponent() const
{
    return Abilities;
}

void ARuneBoxPawn::SyncState(const FRuneBoxState& State)
{
    BoxId = State.BoxId;
    const auto SyncEffect = [this](bool bWanted, FGameplayTag Tag, FActiveGameplayEffectHandle& Handle)
    {
        const bool bApplied = Abilities->GetActiveGameplayEffect(Handle) != nullptr;
        if (bWanted && !bApplied)
        {
            auto Spec = Abilities->MakeOutgoingSpec(URuneBoxStatusEffect::StaticClass(), 1.f, Abilities->MakeEffectContext());
            if (!ensure(Spec.IsValid())) return;
            Spec.Data->DynamicGrantedTags.AddTag(Tag);
            Handle = Abilities->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
        }
        else if (!bWanted && bApplied)
        {
            Abilities->RemoveActiveGameplayEffect(Handle);
            Handle.Invalidate();
        }
    };
    SyncEffect(State.LevitationTurns > 0, RuneBoxTags::Levitating, Levitation);
    SyncEffect(State.bPhaseLocked, RuneBoxTags::PhaseLocked, PhaseLock);
    SyncEffect(State.bHiss, RuneBoxTags::Hiss, Hiss);
}
