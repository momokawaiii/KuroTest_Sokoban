#if WITH_EDITOR
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/CurveTable.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraDecalRendererProperties.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraEmitter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

bool ApplyWaterEmitterRecipe(FVersionedNiagaraEmitterData&,const TSharedPtr<FJsonObject>&);
void PreserveWaterReaderSource(FVersionedNiagaraEmitterData&);

namespace
{
using FRow=TSharedPtr<FJsonObject>;
const FString WaterStudyDirectory=TEXT("NatureTheoryStudy/Water");
FRow ReadStudy(const FString& Directory, const FString& File)
{
    FRow Json;
    FString Text;
    if (FFileHelper::LoadFileToString(Text,*(FPaths::ProjectSavedDir()/Directory/File)))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json);
    return Json;
}
FRow ReadWater(const FString& File)
{
    return ReadStudy(WaterStudyDirectory, File);
}
bool SaveWater(UObject* Obj)
{
    auto* Pkg=Obj->GetOutermost();
    Obj->MarkPackageDirty();
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Pkg,Obj,*FPackageName::LongPackageNameToFilename(Pkg->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
void AuthorHandClips(const FString& Directory)
{
    const auto Data=ReadStudy(Directory, TEXT("Clips.json"));
    if (!Data) return;
    const FString Folder=TEXT("/Game/Character/FirstPerson/SM_FirstPerson/SkeletalMeshes/");
    auto* Skeleton=LoadObject<USkeleton>(nullptr,*(Folder+TEXT("SM_FirstPerson_Skeleton")));
    if (!Skeleton) return;
    for (const auto& Pair:Data->Values)
    {
        const auto Row=Pair.Value->AsObject();
        const FString Name(Pair.Key);
        TArray<FString> Bones;
        Row->TryGetStringArrayField(TEXT("bones"),Bones);
        const int Frames=Row->GetIntegerField(TEXT("frames"));
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes,*(FPaths::ProjectSavedDir()/Directory/(Name+TEXT(".bin")))) ||
            Bytes.Num()!=Frames*Bones.Num()*10*int(sizeof(float))) return;
        const FString Path=Folder+Name;
        auto* Clip=FindObject<UAnimSequence>(nullptr,*(Path+TEXT(".")+Name));
        if (!Clip && FPackageName::DoesPackageExist(Path)) Clip=LoadObject<UAnimSequence>(nullptr,*Path);
        if (!Clip)
        {
            Clip=NewObject<UAnimSequence>(CreatePackage(*Path),FName(Name),RF_Public|RF_Standalone|RF_Transactional);
            Clip->SetSkeleton(Skeleton);
            FAssetRegistryModule::AssetCreated(Clip);
        }
        auto& Ctrl=Clip->GetController();
        Ctrl.OpenBracket(FText::FromString(TEXT("Import NatureTheory hand animation")),false);
        Ctrl.InitializeModel();
        Ctrl.RemoveAllBoneTracks(false);
        Ctrl.SetFrameRate(FFrameRate(Row->GetIntegerField(TEXT("rate")),1),false);
        Ctrl.SetNumberOfFrames(FFrameNumber(Frames-1),false);
        for(int B=0;B<Bones.Num();++B)
        {
            TArray<FQuat4f> Rot;
            TArray<FVector3f> Pos,Scale;
            for(int F=0;F<Frames;++F)
            {
                const float* V=reinterpret_cast<const float*>(Bytes.GetData())+10*(F*Bones.Num()+B);
                Rot.Add(FQuat4f(V[0],V[1],V[2],V[3]).GetNormalized());
                Pos.Emplace(V[4],V[5],V[6]); Scale.Emplace(V[7],V[8],V[9]);
            }
            if (!Ctrl.AddBoneCurve(FName(Bones[B]),false) || !Ctrl.SetBoneTrackKeys(FName(Bones[B]),Pos,Rot,Scale,false))
            { Ctrl.CloseBracket(false); UE_LOG(LogTemp,Error,TEXT("Failed to import %s track %s"),*Name,*Bones[B]); return; }
        }
        Ctrl.NotifyPopulated();Ctrl.CloseBracket(false);
        Clip->PostEditChange();
        if (!SaveWater(Clip)) return;
        UE_LOG(LogTemp,Display,TEXT("HAND_CLIP_SAVED %s %d"),*Name,Frames);
    }
}
void WaterSystems()
{
    auto Data=ReadWater(TEXT("Recipe.json"));
    if (!Data) return;
    const auto UserParameters=ReadWater(TEXT("UserParameters.json"));
    if (!UserParameters)return;
    int Count=0;
    for (auto& Pair:Data->Values)
    {
        const FString Path(Pair.Key);
        const bool Hands=Path.Contains(TEXT("Water01_Charge")) || Path.Contains(TEXT("Water01_Holding"));
        const bool Flight=Path.EndsWith(TEXT("Water01_Release")) || Path.EndsWith(TEXT("Water01_Bullet")) || Path.EndsWith(TEXT("Water01_Hit"));
        const bool Anchor=Path.Contains(TEXT("/Water/02/")) && Path.Contains(TEXT("Water02_Anchor"));
        if (FParse::Param(FCommandLine::Get(),TEXT("WaterAnchorOnly")) && !Anchor)continue;
        if (FParse::Param(FCommandLine::Get(),TEXT("WaterContactSprayOnly")) && !Path.EndsWith(TEXT("Water02_Anchor_Hit")))continue;
        if (FParse::Param(FCommandLine::Get(),TEXT("WaterFlightOnly")) && !Flight)continue;
        if (FParse::Param(FCommandLine::Get(),TEXT("WaterHandsOnly")) && !Hands)continue;
        auto* System=LoadObject<UNiagaraSystem>(nullptr,*Pair.Key);
        if (!System) return;
        TArray<FString> Parameters;
        UserParameters->TryGetStringArrayField(Pair.Key,Parameters);
        auto& Store=System->GetExposedParameters();
        if (Flight && !Path.EndsWith(TEXT("_Release")))
            Store.SetParameterValue(FVector3f::ForwardVector,FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.FlightCameraForward")),true);
        if (Hands)
            for (const FName Input:{FName(TEXT("User.PalmOffset")),FName(TEXT("User.PalmUp"))})
                Store.SetParameterValue(Input==TEXT("User.PalmUp")?FVector3f::UpVector:FVector3f::ZeroVector,FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(),Input),true);
        if (Hands && Path.EndsWith(TEXT("_Arm")))
            Store.SetParameterValue(FVector3f::ZeroVector,FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.WristOffset")),true);
        for (const FString& Name:Parameters)
        {
            if (Name.EndsWith(TEXT("Position"))) Store.SetPositionParameterValue(FVector::ZeroVector,*Name,true);
            else if (Name.EndsWith(TEXT("Vector1")) || Name.EndsWith(TEXT("Direction")))
                Store.SetParameterValue(FVector3f::ZeroVector,FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(),*Name),true);
            else if (Name.EndsWith(TEXT("IsActive")))
                Store.SetParameterValue(FNiagaraBool(true),FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(),*Name),true);
            else Store.SetParameterValue(Name.EndsWith(TEXT("ChargeDuration"))?.5f:0.f,FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(),*Name),true);
        }
        const auto Rows=Pair.Value->AsObject();
        if (Rows->Values.Num()!=System->GetEmitterHandles().Num()) {UE_LOG(LogTemp,Error,TEXT("Water emitter count %s"),*Pair.Key); return;}
        for (const auto& H:System->GetEmitterHandles())
            if (!H.GetIsEnabled() || !H.GetEmitterData() || !Rows->HasField(H.GetName().ToString()))
            { UE_LOG(LogTemp,Error,TEXT("Water emitter identity %s/%s"),*Pair.Key,*H.GetName().ToString());return; }
        for (auto& H:System->GetEmitterHandles())
        {
            if (!ApplyWaterEmitterRecipe(*H.GetEmitterData(),Rows->GetObjectField(H.GetName().ToString()))) return;
            if (Hands && (H.GetName()==TEXT("ArmParticle") || H.GetName()==TEXT("Minimal005")))
                for (auto* Renderer:H.GetEmitterData()->GetRenderers())
                    if (auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(Renderer))
                    {
                        auto* Strand=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Materials/M_Hand_WaterFilament"));
                        if (!Strand) return;
                        Sprite->Material=Strand;
                    }
            if (Hands && (H.GetName()==TEXT("NE_Splash") || H.GetName()==TEXT("WaterShockWave") || H.GetName()==TEXT("WaterShockWave002")))
            {
                const TCHAR* Name=H.GetName()!=TEXT("NE_Splash")?TEXT("HoldShockWave"):
                    Path.EndsWith(TEXT("_Charge"))?TEXT("ChargeSplash"):TEXT("HoldSplash");
                auto* Mat=LoadObject<UMaterialInterface>(nullptr,*(FString(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Materials/M_Hand_"))+Name));
                if (!Mat) return;
                for (auto* R:H.GetEmitterData()->GetRenderers())
                    if (auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(R)) Sprite->Material=Mat;
            }
            if (Anchor)
            {
                const FName Handle=H.GetName();
                FString Surface;
                if (Handle=="WaterShockWave") Surface=TEXT("01/Materials/M_Hand_HoldShockWave");
                if (Handle=="NE_Splash") Surface=TEXT("02/Materials/M_Anchor_SurfaceSplash");
                if (Handle=="NE_Splash001") Surface=TEXT("02/Materials/M_Anchor_ImpactSplash");
                if (Handle=="NE_SplashShockwave") Surface=TEXT("02/Materials/M_Anchor_ImpactWave");
                if (Handle=="Splash003") Surface=TEXT("02/Materials/M_Anchor_ContactSpray");
                if (Handle=="Minimal014") Surface=TEXT("02/Materials/M_Anchor_ImpactDecal");
                if (!Surface.IsEmpty())
                {
                    auto* Mat=LoadObject<UMaterialInterface>(nullptr,*(FString(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/"))+Surface));
                    if (!Mat)return;
                    for (auto* R:H.GetEmitterData()->GetRenderers())
                        if (auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(R)) Sprite->Material=Mat;
                        else if (auto* Decal=Cast<UNiagaraDecalRendererProperties>(R)) Decal->Material=Mat;
                }
            }
            if (Flight)
            {
                const FName Handle=H.GetName();
                FString Surface;
                if (Handle=="Splash002" || Handle=="WaterShockWave") Surface="M_Hand_HoldShockWave";
                else if (Handle=="NE_Splash") Surface="M_Flight_BulletSplash";
                else if (Handle=="NE_Splash001") Surface="M_Flight_HitSplash";
                else if (Handle=="NE_SplashShockwave") Surface="M_Flight_HitSplashShockWave";
                if (!Surface.IsEmpty())
                {
                    auto* Material=LoadObject<UMaterialInterface>(nullptr,*(FString(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Materials/"))+Surface));
                    if (!Material)return;
                    for (auto* Renderer:H.GetEmitterData()->GetRenderers())
                        if (auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(Renderer)) Sprite->Material=Material;
                }
            }
            const bool FlightCore=Path.EndsWith(TEXT("Water01_Bullet")) && H.GetName()==TEXT("Minimal002");
            if (FlightCore || (Hands && (H.GetName()==TEXT("Minimal") || H.GetName()==TEXT("WaterShockWave001"))))
                for (auto* R:H.GetEmitterData()->GetRenderers())
                    if (auto* Mesh=Cast<UNiagaraMeshRendererProperties>(R))
                    {
                        auto* Mat=LoadObject<UMaterialInterface>(nullptr,FlightCore || H.GetName()==TEXT("Minimal")?
                            TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Materials/M_Hand_HoldCore"):
                            TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Materials/M_Hand_HoldMeshShockWave"));
                        auto* Sphere=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/01/Meshes/SM_Hand_HQSphere"));
                        if (!Mat || !Sphere || Mesh->Meshes.IsEmpty() || Mesh->OverrideMaterials.IsEmpty())return;
                        Mesh->Meshes[0].Mesh=Sphere;
                        Mesh->OverrideMaterials[0].ExplicitMat=Mat;
                    }
            ++Count;
        }
        for (const auto& Item:Rows->Values)
        {
            FString Peer;
            if (!Item.Value->AsObject()->TryGetStringField(TEXT("particle_source"),Peer))continue;
            for (auto& H:System->GetEmitterHandles())
                if (H.GetName()==FName(Peer)) PreserveWaterReaderSource(*H.GetEmitterData());
        }
        System->RequestCompile(true);
        System->WaitForCompilationComplete(true,false);
        if (!System->IsValid() || !SaveWater(System)) {UE_LOG(LogTemp,Error,TEXT("Water system failed %s"),*Pair.Key);return;}
        UE_LOG(LogTemp,Display,TEXT("WATER_SYSTEM_SAVED %s"),*Pair.Key);
    }
    UE_LOG(LogTemp,Display,TEXT("WATER_EMITTERS_SAVED %d"),Count);
}
void WaterCurves()
{
    auto Data=ReadWater(TEXT("ParentCurves.json"));
    if (!Data) return;
    const FString Path=TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/02/Niagara/Water02_AnchorDriver");
    UCurveTable* Table=nullptr;
    if (FPackageName::DoesPackageExist(Path)) Table=LoadObject<UCurveTable>(nullptr,*Path);
    if (!Table)
    {
        Table=NewObject<UCurveTable>(CreatePackage(*Path),TEXT("Water02_AnchorDriver"),RF_Public|RF_Standalone);
        FAssetRegistryModule::AssetCreated(Table);
    }
    Table->EmptyTable();
    for (auto& Pair:Data->Values)
    {
        const auto Row=Pair.Value->AsObject();
        const auto& V=Row->GetArrayField(TEXT("values"));
        auto& Curve=Table->AddRichCurve(FName(Pair.Key));
        for(int I=0;I<V.Num();++I)
        {
            const float T=FMath::Lerp(Row->GetNumberField(TEXT("min")),Row->GetNumberField(TEXT("max")),double(I)/FMath::Max(1,V.Num()-1));
            Curve.SetKeyInterpMode(Curve.AddKey(T,V[I]->AsNumber()),RCIM_Linear);
        }
    }
    if (SaveWater(Table)) UE_LOG(LogTemp,Display,TEXT("WATER_ANCHOR_CURVES_SAVED"));
}
FAutoConsoleCommand WaterAnimCmd(TEXT("RR.AuthorWaterClips"), TEXT("Restore the right-hand Lift sequences."),
    FConsoleCommandDelegate::CreateLambda([] { AuthorHandClips(WaterStudyDirectory); }));
FAutoConsoleCommand FirstPersonAnimCmd(TEXT("RR.AuthorFirstPersonClips"), TEXT("Restore the equip, jump and Orb hand sequences."),
    FConsoleCommandDelegate::CreateLambda([] { AuthorHandClips(TEXT("NatureTheoryStudy/FirstPerson")); }));
FAutoConsoleCommand WaterCurveCmd(TEXT("RR.AuthorWaterCurves"),TEXT("Restore the anchor parent particle's curves."),FConsoleCommandDelegate::CreateStatic(&WaterCurves));
FAutoConsoleCommand WaterFxCmd(TEXT("RR.AuthorWaterSystems"),TEXT("Set source emitter timing and water particle outputs."),FConsoleCommandDelegate::CreateStatic(&WaterSystems));
}
#endif

