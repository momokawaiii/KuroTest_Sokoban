#include "ShengLuGame.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CurveTable.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "Sound/SoundBase.h"

namespace
{
    // The parent Water02_Hit emits Anchor at 0 and Anchor001 at 1.1 seconds.
    constexpr float AnchorFallSeconds = 1.1f;

    // Holding.NE_BasicEmitter_Light001.FloatFromCurve002, sampled by normalized particle age.
    constexpr float PalmDiffuse[] = {
        0.00000000f, 0.24912958f, 0.40640458f, 0.52311355f, 0.61531842f, 0.69055831f, 0.75308257f, 0.80555725f,
        0.84977496f, 0.88699836f, 0.91814297f, 0.94387907f, 0.96468884f, 0.98088986f, 0.99261838f, 0.99971282f,
        0.99928755f, 0.99689621f, 0.99274558f, 0.98676729f, 0.97890645f, 0.96912551f, 0.95740938f, 0.94376999f,
        0.92825007f, 0.91092569f, 0.89190698f, 0.87133634f, 0.84938455f, 0.82624465f, 0.80212390f, 0.77723581f,
        0.75179207f, 0.72599465f, 0.70003092f, 0.67406923f, 0.64825732f, 0.62272078f, 0.59756446f, 0.57287347f,
        0.54871517f, 0.52514076f, 0.50218832f, 0.47988456f, 0.45824629f, 0.43728277f, 0.41699678f, 0.39738637f,
        0.37844527f, 0.36016446f, 0.34253255f, 0.32553610f, 0.30916086f, 0.29339147f, 0.27821216f, 0.26360700f,
        0.24955961f, 0.23605403f, 0.22307441f, 0.21060503f, 0.19863066f, 0.18713638f, 0.17610767f, 0.16553056f,
        0.15539154f, 0.14567748f, 0.13637558f, 0.12747386f, 0.11896053f, 0.11082408f, 0.10305390f, 0.09563947f,
        0.08857068f, 0.08183788f, 0.07543178f, 0.06934348f, 0.06356436f, 0.05808617f, 0.05290101f, 0.04800117f,
        0.04337932f, 0.03902841f, 0.03494160f, 0.03111231f, 0.02753426f, 0.02420138f, 0.02110776f, 0.01824778f,
        0.01561598f, 0.01320707f, 0.01101595f, 0.00903780f, 0.00726783f, 0.00570148f, 0.00433433f, 0.00316211f,
        0.00218067f, 0.00138603f, 0.00077434f, 0.00034183f, 0.00008489f, 0.00000000f,
    };
}

void AShengLuBoard::SetupWater()
{
    WaterCharge=CastSystem("WaterCharge",TEXT("Water/01"),TEXT("Water01_Charge"));
    WaterChargeArm=CastSystem("WaterChargeArm",TEXT("Water/01"),TEXT("Water01_Charge_Arm"));
    WaterHold=CastSystem("WaterHold",TEXT("Water/01"),TEXT("Water01_Holding"));
    WaterHoldArm=CastSystem("WaterHoldArm",TEXT("Water/01"),TEXT("Water01_Holding_Arm"));
    WaterRelease=CastSystem("WaterRelease",TEXT("Water/01"),TEXT("Water01_Release"));
    WaterBullet=CastSystem("WaterBullet",TEXT("Water/01"),TEXT("Water01_Bullet"));
    WaterBurst=CastSystem("WaterBurst",TEXT("Water/01"),TEXT("Water01_Hit"));
    FallingAnchor=CastSystem("WaterAnchor",TEXT("Water/02"),TEXT("Water02_Anchor"));
    AnchorHit=CastSystem("WaterAnchorHit",TEXT("Water/02"),TEXT("Water02_Anchor_Hit"));
    WaterAnchorCurves=LoadObject<UCurveTable>(nullptr,TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/02/Niagara/Water02_AnchorDriver"));
    WaterRange=CreateDefaultSubobject<UStaticMeshComponent>("WaterRange");
    WaterRange->SetupAttachment(RootComponent);
    WaterRange->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    WaterRange->SetCastShadow(false);
    WaterRange->SetVisibility(false);
    if (auto* Sphere=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/EngineMeshes/Sphere")))
    {
        WaterRange->SetStaticMesh(Sphere);
        WaterRange->SetRelativeScale3D(FVector(50.f/Sphere->GetBounds().BoxExtent.GetMax()));
    }
    WaterRange->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/VFX/Puzzle/Materials/M_RangeUI")));

    for (auto* FX : {WaterCharge.Get(),WaterHold.Get(),WaterRelease.Get()})
    {
        FX->SetupAttachment(Hands,TEXT("spine_03"));
        FX->SetRelativeLocationAndRotation(FVector(18.353626,58.237386,22.589980),FRotator(0,-80.00062,90));
    }
    for (auto* FX : {WaterChargeArm.Get(),WaterHoldArm.Get()})
    {
        FX->SetupAttachment(Hands,TEXT("lowerarm_r"));
        FX->SetRelativeRotation(FRotator(.039,-179.927,90.022));
    }
    WaterPalmFlash=CreateDefaultSubobject<UPointLightComponent>("WaterPalmFlash");
    WaterPalmFlash->SetupAttachment(WaterHold);
    WaterPalmFlash->SetCastShadows(false);
    WaterPalmFlash->SetAttenuationRadius(40.f);
    WaterPalmFlash->SetLightColor(FLinearColor(.622f,.774f,1.f));
    WaterPalmFlash->SetIntensityUnits(ELightUnits::Unitless);
    WaterPalmFlash->SetUseInverseSquaredFalloff(false);
    WaterPalmFlash->SetLightFalloffExponent(8.f);
    // ULightComponent clamps diffuse and specular scale to [0,1]. Put the
    // original 15:1 diffuse/specular ratio into intensity and specular scale.
    WaterPalmFlash->SetSpecularScale(1.f/15.f);
    WaterPalmFlash->SetVisibility(false);
    for (const TCHAR* Phase : {TEXT("Start"),TEXT("Loop"),TEXT("End")})
    {
        const FString Path=FString(TEXT("/Game/Character/FirstPerson/SM_FirstPerson/SkeletalMeshes/R_Lift_"))+Phase+TEXT("_Retarget");
        CastClips.Add(LoadObject<UAnimSequence>(nullptr,*Path));
    }
    WaterFlightAudio=CreateDefaultSubobject<UAudioComponent>("WaterFlightAudio");
    WaterFlightAudio->SetupAttachment(WaterBullet);
    WaterFlightAudio->SetAutoActivate(false);
    WaterFlightAudio->SetVolumeMultiplier(.55f);
    const TCHAR* Names[]={TEXT("Charging"),TEXT("ChargeComplete"),TEXT("Holding"),TEXT("Launch"),TEXT("Trail_Cue"),TEXT("Hit_Cue"),TEXT("Anchor_Hit_Cue")};
    const FName Keys[]={"WaterCharge","WaterReady","WaterHold","WaterRelease","WaterTrail","WaterHit","AnchorHit"};
    for (int I=0;I<UE_ARRAY_COUNT(Keys);++I)
    {
        const FString Family=I==6?TEXT("Water02"):TEXT("Water01");
        const FString Path=TEXT("/Game/SFX/Player/Skills/Water/")+Family+TEXT("/S_Player_Skills_")+Family+TEXT("_")+Names[I];
        CastSounds.Add(Keys[I],LoadObject<USoundBase>(nullptr,*Path));
    }
}

void AShengLuBoard::StartWater()
{
    StopWaterHands();
    UpdateWaterHandInputs();
    WaterCharge->SetVariableFloat(TEXT("User.ChargeDuration"),ChargeSeconds);
    WaterChargeArm->SetVariableFloat(TEXT("User.ChargeDuration"),ChargeSeconds);
    WaterCharge->Activate(true);
    WaterChargeArm->Activate(true);
    PlaySound(CastTransient,TEXT("WaterCharge"));
}

void AShengLuBoard::UpdateWaterHandInputs()
{
    const FVector Wrist=Hands->GetSocketLocation(TEXT("hand_r"));
    // The source skeleton places hand_center_r on Weapon_r_end.
    const FVector Palm=Hands->GetSocketTransform(TEXT("Weapon_r_end")).TransformPosition(
        FVector(-4.679615,6.882538,-4.638063));
    const FVector Center=WaterHold->GetComponentLocation();
    WaterCharge->SetVariablePosition(TEXT("User.HandPosition"),Palm);
    for (auto* FX : {WaterCharge.Get(),WaterHold.Get(),WaterHoldArm.Get()})
        FX->SetVariablePosition(TEXT("User.CenterPosition"),Center);
    // The 0.28 s Holding light washes the fingers at the mesh's first overshoot.
    const bool Flash=CastPhase==ECastPhase::Loop && PhaseTime<.28f;
    WaterPalmFlash->SetVisibility(Flash);
    if (Flash)
    {
        const float Sample=FMath::Clamp(PhaseTime/(.28f*.67040801f),0.f,1.f)*(UE_ARRAY_COUNT(PalmDiffuse)-1);
        const int32 Index=FMath::Min(FMath::FloorToInt(Sample),int32(UE_ARRAY_COUNT(PalmDiffuse))-2);
        const float Envelope=FMath::Lerp(PalmDiffuse[Index],PalmDiffuse[Index+1],Sample-Index);
        WaterPalmFlash->SetWorldLocation(Center);
        // The particle light drives diffuse separately from specular. Match its
        // short wash on the board's first-person mesh without boosting the reflections.
        constexpr float BoardPalmLightIntensity=8.f;
        WaterPalmFlash->SetIntensity(15.f*BoardPalmLightIntensity*Envelope);
    }
    for (auto* FX:{WaterCharge.Get(),WaterHold.Get(),WaterChargeArm.Get(),WaterHoldArm.Get()})
    {
        const auto& Frame=FX->GetComponentTransform();
        const bool Arm=FX==WaterChargeArm || FX==WaterHoldArm;
        if (Arm)
            FX->SetVariableVec3(TEXT("User.WristOffset"),Frame.InverseTransformPositionNoScale(Wrist));
        FX->SetVariableVec3(TEXT("User.PalmOffset"),Frame.InverseTransformPositionNoScale(Arm?Center:Palm));
        FX->SetVariableVec3(TEXT("User.PalmUp"),Frame.InverseTransformVectorNoScale(FVector::UpVector));
    }
}

void AShengLuBoard::StopWaterHands()
{
    WaterPalmFlash->SetVisibility(false);
    WaterRange->SetVisibility(false);
    for (auto* FX : {WaterCharge.Get(),WaterChargeArm.Get(),WaterHold.Get(),WaterHoldArm.Get(),WaterRelease.Get()})
        FX->DeactivateImmediate();
}

bool AShengLuBoard::WaterInFlight() const { return WaterAge>=0.f && !bAnchorLanded; }

bool AShengLuBoard::LaunchWater()
{
    if (!RuneSkills::IsUnlocked(Definition, ERuneAction::WaterAnchor) || Selected!=ERuneAction::WaterAnchor || (CastPhase!=ECastPhase::Start && CastPhase!=ECastPhase::Loop) ||
        IsAnimating() || WaterInFlight() || IsRestored()) return false;
    if (TargetCell.X<0) { Notice=TEXT("将准星对准棋盘落点"); return false; }
    if (bAimBox)
    {
        auto P=Preview();
        if (!P.IsValid()) { Notice=P.Reason; return false; }
    }
    ClearWater();
    bWaterHitBox=bAimBox;
    WaterBoxId=TargetBoxId;
    WaterAge=0.f;
    WaterFrom=WaterHold->GetComponentLocation();
    WaterTarget=AimPoint;
    WaterNormal=AimHit.ImpactNormal.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
    const auto* TargetMesh = BoxMesh(WaterBoxId);
    WaterAnchorTarget=TargetMesh?TargetMesh->Bounds.Origin+FVector::UpVector*TargetMesh->Bounds.BoxExtent.Z:WaterTarget;
    // The reference anchor spans about 120 px at 1920 px and 90 degrees HFOV.
    // Apply one scale to its source geometry, trajectory and contact for this cast.
    const float AnchorScale=FMath::Clamp(float(FVector::Dist(Eyes->GetComponentLocation(),WaterAnchorTarget))*.125f/(424.f*.7f),.08f,1.f);
    FallingAnchor->SetWorldScale3D(FVector(AnchorScale));
    AnchorHit->SetWorldScale3D(FVector(AnchorScale));
    // Keep the trip visible even when the selected surface is close to the hand.
    WaterTravel=FMath::Max(.28f,float(FVector::Dist(WaterFrom,WaterTarget)/7000.));
    StopWaterHands();
    CastLoop->Stop();
    WaterRelease->SetVariablePosition(TEXT("User.CenterPosition"),WaterFrom);
    WaterRelease->Activate(true);
    PlayHandPhase(ECastPhase::End);
    WaterBullet->SetWorldLocation(WaterFrom);
    WaterBullet->SetWorldRotation((WaterTarget-WaterFrom).Rotation());
    WaterBullet->SetVariableVec3(TEXT("User.FlightCameraForward"),Eyes->GetForwardVector());
    WaterBullet->SetVariableBool(TEXT("User.IsActive"),true);
    WaterBullet->Activate(true);
    // Keep the moving droplets; their collision event also owns a ground-splash card.
    WaterBullet->SetEmitterEnable(TEXT("Splash003"),false);
    PlaySound(CastTransient,TEXT("WaterRelease"));
    PlaySound(WaterFlightAudio,TEXT("WaterTrail"));
    Notice.Reset();
    RecordConfirmedSkill(ERuneAction::WaterAnchor);
    return true;
}

void AShengLuBoard::UpdateWater(float Dt)
{
    if (WaterAge<0.f) return;
    WaterAge+=Dt;
    const float Flight=FMath::Clamp(WaterAge/WaterTravel,0.f,1.f);
    if (!bWaterBurst)
    {
        WaterBullet->SetWorldLocation(FMath::Lerp(WaterFrom,WaterTarget,Flight));
        if (Flight<1.f) return;
        bWaterBurst=true;
        WaterBullet->SetVariableBool(TEXT("User.IsActive"),false);
        WaterBullet->Deactivate();
        // Minimal002_0 uses local +X as its shockwave normal.
        WaterBurst->SetWorldLocationAndRotation(WaterTarget+WaterNormal*6.f,FRotationMatrix::MakeFromX(WaterNormal).Rotator());
        WaterBurst->SetVariableVec3(TEXT("User.FlightCameraForward"),Eyes->GetForwardVector());
        WaterBurst->Activate(true);
        // The orb impact overlaps the rising anchor. Keep its radial burst but
        // omit the secondary ground cards spawned at scattered collision points.
        WaterBurst->SetEmitterEnable(TEXT("Splash003"),false);
        PlaySound(WaterFlightAudio,TEXT("WaterHit"));
        FallingAnchor->SetVariableBool(TEXT("User.IsActive"),true);
    }
    const float Fall=FMath::Clamp((WaterAge-WaterTravel)/AnchorFallSeconds,0.f,1.f);
    if (WaterAge-WaterTravel<4.f)
    {
        const float Age=WaterAge-WaterTravel;
        auto Curve=[&](FName Name)
        {
            const auto* Row=WaterAnchorCurves?WaterAnchorCurves->FindCurve(Name,TEXT("Water02 anchor"),false):nullptr;
            return Row?Row->Eval(Age):0.f;
        };
        // Preserve the source hover. Fit only the final tenth of a second so
        // the lowest point of the scaled mesh meets the board on the hit event.
        const float ContactHeight=240.329f*.7f;
        const float Height=FMath::Lerp(450.f*Curve("Height"),ContactHeight,FMath::SmoothStep(1.f,AnchorFallSeconds,Age))
            *FallingAnchor->GetComponentScale().Z;
        const float Facing=(WaterFrom-WaterAnchorTarget).Rotation().Yaw;
        FallingAnchor->SetWorldLocationAndRotation(WaterAnchorTarget+FVector::UpVector*Height,
            FRotator(90.f*Curve("Pitch"),Facing+30.f*Curve("Yaw"),0));
        FallingAnchor->SetVariableFloat(TEXT("User.Float1"),Curve("Dissolve"));
        FallingAnchor->SetVariableFloat(TEXT("User.Float2"),Curve("DissolveOut"));
        FallingAnchor->SetVariableVec3(TEXT("User.Vector1"),FVector(.7,.9,1.)*Curve("Glow"));
        if (!bAnchorLanded && !FallingAnchor->IsActive()) FallingAnchor->Activate(true);
    }
    if (!bAnchorLanded)
    {
        if (Fall<1.f) return;
        bAnchorLanded=true;
        AnchorHit->SetWorldLocation(WaterAnchorTarget);
        AnchorHit->SetVariableBool(TEXT("User.IsActive"),true);
        AnchorHit->SetVariableVec3(TEXT("User.Skill Direction"),-FVector::UpVector);
        AnchorHit->Activate(true);
        // The board and crate surfaces stay intact under the water impact.
        AnchorHit->SetEmitterEnable(TEXT("Minimal002"), false); // Stone debris meshes
        AnchorHit->SetEmitterEnable(TEXT("Minimal014"), false); // Ground crack decal
        PlaySound(WaterFlightAudio,TEXT("AnchorHit"));
        if (bWaterHitBox) TryPuzzleAction({ERuneAction::WaterAnchor, WaterBoxId});
    }
    if (WaterAge>=WaterTravel+AnchorFallSeconds+3.f) ClearWater();
}

void AShengLuBoard::ClearWater()
{
    WaterAge=-1.f; bWaterBurst=bAnchorLanded=bWaterHitBox=false;
    WaterBoxId=INDEX_NONE;
    WaterFlightAudio->Stop();
    for (auto* FX : {WaterBullet.Get(),WaterBurst.Get(),FallingAnchor.Get(),AnchorHit.Get()}) FX->DeactivateImmediate();
}

void UGA_WaterAnchor::ActivateAbility(const FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* Info,
    const FGameplayAbilityActivationInfo Activation,const FGameplayEventData*)
{
    auto* Board=Cast<AShengLuBoard>(Info->AvatarActor.Get());
    const bool Fired=Board && Board->LaunchWater();
    EndAbility(Handle,Info,Activation,false,!Fired);
}

