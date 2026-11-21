#pragma once
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
class UPostProcessComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
#include "ShengLuPostComponent.generated.h"

UCLASS(Blueprintable)
class AShengLuPostActor : public AActor
{
    GENERATED_BODY()
public:
    AShengLuPostActor();
    void SetMaterial(UMaterialInterface* Material);
    void SetActive(bool bActive);
    void SetMatParameters(const FVector4& A,const FVector4& B,const FVector4& C,const FVector4& D);
private:
    UPROPERTY() TObjectPtr<UPostProcessComponent> Post;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> Instance;
};

UCLASS(BlueprintType, Blueprintable)
class UShengLuPostComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") TObjectPtr<UMaterialInterface> PostMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") FVector4 Parameter1{};
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") FVector4 Parameter2{};
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") FVector4 Parameter3{};
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") FVector4 Parameter4{};
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NatureTheory") bool Alive=true;
    UPROPERTY(EditDefaultsOnly, Category="NatureTheory") TSubclassOf<AShengLuPostActor> PostActorClass;
    UFUNCTION(BlueprintCallable, Category="NatureTheory") void SetParameter2(FVector4 Value);
    UFUNCTION(BlueprintCallable, Category="NatureTheory") void SetParameter3(FVector4 Value);
    UFUNCTION(BlueprintCallable, Category="NatureTheory") void SetParameter4(FVector4 Value);
    UFUNCTION(BlueprintCallable, Category="NatureTheory") void SetParameter1(FVector4 Value);
    UFUNCTION(BlueprintCallable, Category="NatureTheory") void SetAlive(bool Value);
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
private:
    void UpdateParameters();
    UPROPERTY(Transient) TObjectPtr<AShengLuPostActor> EffectActor;
};
