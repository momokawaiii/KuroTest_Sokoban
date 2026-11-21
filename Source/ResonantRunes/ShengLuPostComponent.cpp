#include "ShengLuPostComponent.h"
#include "Components/PostProcessComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

AShengLuPostActor::AShengLuPostActor()
{
    Post=CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    RootComponent=Post;
    Post->bUnbound=true;
}

void AShengLuPostActor::SetMaterial(UMaterialInterface* Material)
{
    Instance=UMaterialInstanceDynamic::Create(Material,this);
    Post->Settings.AddBlendable(Instance,1.f);
}

void AShengLuPostActor::SetActive(bool bActive)
{
    Post->bEnabled=bActive;
}

void AShengLuPostActor::SetMatParameters(const FVector4& A,const FVector4& B,const FVector4& C,const FVector4& D)
{
    if (!Instance) return;
    Instance->SetVectorParameterValue(TEXT("Parameter1"),FLinearColor(A));
    Instance->SetVectorParameterValue(TEXT("Parameter2"),FLinearColor(B));
    Instance->SetVectorParameterValue(TEXT("Parameter3"),FLinearColor(C));
    Instance->SetVectorParameterValue(TEXT("Parameter4"),FLinearColor(D));
}

void UShengLuPostComponent::OnRegister()
{
    Super::OnRegister();
    auto* World=GetWorld();
    if (!PostMaterial || !World || !GetOwner() || IsTemplate() || (!World->IsGameWorld() && World->WorldType!=EWorldType::EditorPreview)) return;
    FActorSpawnParameters Params;
    Params.Owner=GetOwner();
    Params.ObjectFlags|=RF_Transient;
    Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    EffectActor=World->SpawnActor<AShengLuPostActor>(PostActorClass?PostActorClass.Get():AShengLuPostActor::StaticClass(),GetComponentTransform(),Params);
    if (EffectActor)
    {
        EffectActor->SetMaterial(PostMaterial);
        UpdateParameters();
        SetAlive(Alive);
    }
}

void UShengLuPostComponent::OnUnregister()
{
    if (IsValid(EffectActor))
    {
        EffectActor->SetActive(false);
        EffectActor->SetLifeSpan(5.f);
    }
    EffectActor=nullptr;
    Super::OnUnregister();
}
void UShengLuPostComponent::UpdateParameters()
{
    if (EffectActor) EffectActor->SetMatParameters(Parameter1,Parameter2,Parameter3,Parameter4);
}
void UShengLuPostComponent::SetAlive(bool Value)
{
    Alive=Value;
    if (EffectActor) EffectActor->SetActive(Alive);
}
void UShengLuPostComponent::SetParameter1(FVector4 Value) { Parameter1=Value; UpdateParameters(); }
void UShengLuPostComponent::SetParameter2(FVector4 Value) { Parameter2=Value; UpdateParameters(); }
void UShengLuPostComponent::SetParameter3(FVector4 Value) { Parameter3=Value; UpdateParameters(); }
void UShengLuPostComponent::SetParameter4(FVector4 Value) { Parameter4=Value; UpdateParameters(); }
