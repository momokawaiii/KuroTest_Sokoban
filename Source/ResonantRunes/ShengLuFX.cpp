#include "ShengLuGame.h"
#include "AbilitySystemComponent.h"
#include "Components/AudioComponent.h"
#include "NativeGameplayTags.h"
#include "NiagaraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

namespace
{
    constexpr float FadeDelay=6.5f;
    constexpr float FadeDuration=5.f;
    UE_DEFINE_GAMEPLAY_TAG_STATIC(PlayerLevitationTag, "Puzzle.Player.Levitating");
}

void AShengLuBoard::ClearWind()
{
    WindAge=WindFadeAge=-1.f;
    Tornado->DeactivateImmediate();
    WindLoop->Stop();
    WindLoop->SetSound(nullptr);
    WindBoxId = INDEX_NONE;
}

void AShengLuBoard::BeginWind(FVector Foot, int32 BoxId, float Age)
{
    ClearWind();
    WindFoot=Foot;
    WindBoxId=BoxId;
    WindAge=Age;
    Tornado->SetWorldLocation(Foot);
    Tornado->SetVariableFloat(TEXT("User.Air02AgeOffset"),Age);
    Tornado->SetVariableBool(TEXT("User.IsActive"),true);
    UpdateWind(0.f);
    Tornado->Activate(true);
    if (Age<FadeDelay) PlaySound(WindLoop,TEXT("AirTornado"));
}

void AShengLuBoard::UpdateWindCamera(UNiagaraComponent* Effect) const
{
    auto* PC=Cast<APlayerController>(GetController());
    if (!PC || !PC->PlayerCameraManager) return;
    Effect->SetVariablePosition(TEXT("User.Air02CameraWorld"),PC->PlayerCameraManager->GetCameraLocation());
}

void AShengLuBoard::UpdateWind(float Dt)
{
    if (WindAge<0.f) return;
    WindAge+=Dt;
    UpdateWindCamera(Tornado);
    if (const auto* PC=Cast<APlayerController>(GetController()); PC && PC->PlayerCameraManager)
    {
        const FVector ToCamera=PC->PlayerCameraManager->GetCameraLocation()-Tornado->GetComponentLocation();
        Tornado->SetVariableFloat(TEXT("User.Air02ViewYaw"),FMath::Atan2(ToCamera.Y,ToCamera.X));
    }
    const auto* Box = State.FindBox(WindBoxId);
    if (WindFadeAge<0.f && (WindAge>=FadeDelay || !Box || Box->LevitationTurns==0))
    {
        WindFadeAge=WindAge>=FadeDelay?WindAge-FadeDelay:0.f;
        Tornado->SetVariableBool(TEXT("User.IsActive"),false);
        WindLoop->FadeOut(2.f,0.f);
    }
    else if (WindFadeAge>=0.f) WindFadeAge+=Dt;
    if (WindFadeAge>=FadeDuration) { ClearWind(); return; }

    const float Fade=FMath::Max(0.f,WindFadeAge);
    Tornado->SetVariableFloat(TEXT("User.Air02FadeAge"),Fade);
    const float Body=FMath::SmoothStep(.42f,1.05f,WindAge)*(1.f-FMath::SmoothStep(0.f,3.5f,Fade));
    const float Runes=FMath::SmoothStep(.29f,.57f,WindAge)*(1.f-FMath::SmoothStep(0.f,2.9f,Fade));
    const float Seal=FMath::SmoothStep(.11f,.26f,WindAge)*(1.f-FMath::SmoothStep(2.0f,FadeDuration,Fade));
    Tornado->SetVariableVec4(TEXT("User.Air02Phase"),FVector4(WindAge,Body,Runes,Seal));
}

bool AShengLuBoard::IsPlayerLevitating() const
{
    return Abilities->HasMatchingGameplayTag(PlayerLevitationTag);
}

void AShengLuBoard::SyncPlayerWindState()
{
    Abilities->SetLooseGameplayTagCount(PlayerLevitationTag, State.PlayerWindTurns > 0 ? 1 : 0);
    UpdatePlayerWind();
}

void AShengLuBoard::UpdatePlayerWind()
{
    if (!IsPlayerLevitating())
    {
        if (PlayerWind->IsActive()) PlayerWind->DeactivateImmediate();
        return;
    }
    const FVector Center = Cell(SurfaceUnder(State.PlayerPosition)) + FVector(0, 0, 100);
    PlayerWind->SetWorldLocation(Center);
    PlayerWind->SetVariableVec3(TEXT("User.CenterPosition"), Center);
    UpdateWindCamera(PlayerWind);
    if (!PlayerWind->IsActive())
    {
        PlayerWind->SetVariableBool(TEXT("User.IsActive"), true);
        PlayerWind->SetVariableBool(TEXT("User.StartCharge"), true);
        PlayerWind->SetVariableVec4(TEXT("User.Air02Phase"), FVector4(1, 0, 1, 1));
        PlayerWind->Activate(true);
    }
}

void AShengLuBoard::DrawEffects(float Dt)
{
    EffectTime+=Dt;
    UpdateWind(Dt);
    UpdatePlayerWind();
    DrawPulse();
}
