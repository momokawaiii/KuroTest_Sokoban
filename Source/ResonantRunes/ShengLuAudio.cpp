#include "ShengLuGame.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

void AShengLuBoard::SetupCastAudio()
{
    auto Voice=[&](FName Name,float Volume)
    {
        auto* Audio=CreateDefaultSubobject<UAudioComponent>(Name);
        Audio->SetupAttachment(RootComponent);
        Audio->SetAutoActivate(false);
        Audio->bAllowSpatialization=false;
        Audio->SetVolumeMultiplier(Volume);
        return Audio;
    };
    CastTransient=Voice("CastingOneShot",.65f);
    CastLoop=Voice("CastingLoop",.42f);
    ImpactLoop=Voice("ContactLoop",.35f);
    WindLoop=Voice("TornadoLoop",.32f);
    auto Load=[&](FName Key,const TCHAR* Folder,const TCHAR* Wave)
    {
        FString Path=FString::Printf(TEXT("/Game/SFX/Player/Skills/%s/S_Player_Skills_%s"),Folder,Wave);
        ConstructorHelpers::FObjectFinder<USoundBase> Source(*Path);
        CastSounds.Add(Key,Source.Object);
    };
    Load("AirCharge",TEXT("Air/Air02"),TEXT("Air02_Charge_Cue"));
    Load("AirRange",TEXT("Air/Air02"),TEXT("Air02_Range_Loop_Cue"));
    Load("AirRelease",TEXT("Air/Air02"),TEXT("Air02_Release_Cue"));
    Load("AirTornado",TEXT("Air/Air02"),TEXT("Air02_Tornado_Loop_Cue"));
    Load("ElecCharge",TEXT("Electric/01"),TEXT("Electric01_Charge"));
    Load("ElecReady",TEXT("Electric/01"),TEXT("Electric01_ChargeComplete"));
    Load("ElecTrail",TEXT("Electric/01"),TEXT("Electric01_TrailLoop"));
    Load("ElecHit",TEXT("Electric/01"),TEXT("Electric01_HitLoop_Cue"));
    Load("ElecEnd",TEXT("Electric/01"),TEXT("Electric01_TrailEnd"));
}

void AShengLuBoard::PlaySound(UAudioComponent* Voice,FName Key)
{
    if (USoundBase* Sound=CastSounds.FindRef(Key))
    {
        Voice->SetSound(Sound);
        Voice->Play();
    }
}
