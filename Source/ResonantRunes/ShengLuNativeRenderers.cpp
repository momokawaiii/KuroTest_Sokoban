#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "ShengLuPostComponent.h"
#include "Components/PointLightComponent.h"
#include "Camera/CameraShakeSourceComponent.h"
#include "Camera/CameraShakeBase.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/AsyncLoading2.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/SavePackage.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/IConsoleManager.h"

namespace
{
using FJsonPtr=TSharedPtr<FJsonObject>;
using FImports=TMap<int32,UObject*>;

class FRendererStream : public FMemoryReader
{
public:
    FRendererStream(const TArray<uint8>& Bytes,const TArray<FString>& InNames,const FImports& InImports)
        : FMemoryReader(Bytes,true),Names(InNames),Imports(InImports)
    {
        SetFilterEditorOnly(true);
        SetUseUnversionedPropertySerialization(true);
        SetCustomVersions(FCurrentCustomVersions::GetAll());
    }
    using FMemoryReader::operator<<;
    virtual FArchive& operator<<(FName& Out) override
    {
        int32 Index=0,Number=0;
        *this<<Index<<Number;
        if (!Names.IsValidIndex(Index) || Number<0) { SetError(); return *this; }
        Out=FName(*Names[Index],Number);
        return *this;
    }
    virtual FArchive& operator<<(UObject*& Out) override
    {
        int32 Index=0;
        *this<<Index;
        Out=Imports.FindRef(Index);
        if (Index && !Out) { UE_LOG(LogTemp,Error,TEXT("Nature renderer unresolved index %d"),Index); SetError(); }
        return *this;
    }
    virtual FArchive& operator<<(FObjectPtr& Out) override
    {
        UObject* Value=nullptr;
        *this<<Value;
        Out=Value;
        return *this;
    }
private:
    const TArray<FString>& Names;
    const FImports& Imports;
};

UObject* GameAsset(const FString& Path)
{
    const FString Full=Path+TEXT(".")+FPaths::GetCleanFilename(Path);
    if (Path.Contains(TEXT("/BP_")))
    {
        if (auto* Class=LoadClass<UObject>(nullptr,*(Full+TEXT("_C")))) return Class;
        return nullptr;
    }
    return LoadObject<UObject>(nullptr,*Full);
}

void RecoverRenderers()
{
    FString Text;
    TArray<TSharedPtr<FJsonValue>> Systems;
    const bool Water=FParse::Param(FCommandLine::Get(),TEXT("WaterAssets"));
    const FString Source=Water?FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/RenderStreams.json"):FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory/NativeRenderStreams.json");
    if (!FFileHelper::LoadFileToString(Text,*Source) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Systems)) return;
    int32 Total=0;
    for (const auto& Value:Systems)
    {
        const auto& S=Value->AsObject();
        FString Path=S->GetStringField(TEXT("path"));
        auto* System=Cast<UNiagaraSystem>(GameAsset(Path));
        if (!System) return;
        TArray<FString> Names;
        S->TryGetStringArrayField(TEXT("names"),Names);
        FImports Resolved;
        for (const auto& Pair:S->GetObjectField(TEXT("imports"))->Values)
        {
            const FString Ref=Pair.Value->AsString();
            UObject* Obj=nullptr;
            if (Ref.StartsWith(TEXT("/Script/"))) Obj=LoadObject<UObject>(nullptr,*Ref);
            else if ((Ref.StartsWith(TEXT("/Game/")) || Ref.StartsWith(TEXT("/Niagara/")) || Ref.StartsWith(TEXT("/Engine/"))) && !Ref.Contains(TEXT("/NS_")) && !Ref.Contains(TEXT("/SFX/"))) Obj=GameAsset(Ref);
            Resolved.Add(FCString::Atoi(*Pair.Key),Obj);
        }
        // Associate each original instance with the matching handle and class occurrence.
        // The order in the cooked export table is verified against the native target renderer array.
        TMap<FString,TArray<UNiagaraRendererProperties*>> ByHandle;
        for(auto& Handle:System->GetEmitterHandles())
            if (auto* E=Handle.GetEmitterData())
                for(auto* R:E->GetRenderers())
                    ByHandle.FindOrAdd(Handle.GetName().ToString()+TEXT("/")+R->GetClass()->GetName()).Add(R);
        for (const auto& Record:S->GetArrayField(TEXT("renderers")))
        {
            const auto& Row=Record->AsObject();
            const FString Key=Row->GetStringField(TEXT("emitter"))+TEXT("/")+Row->GetStringField(TEXT("cls"));
            if (!ByHandle.Contains(Key)) { UE_LOG(LogTemp,Error,TEXT("Missing native renderer %s %s"),*Path,*Key); return; }
            auto& Options=ByHandle.FindChecked(Key);
            if (Options.IsEmpty()) { UE_LOG(LogTemp,Error,TEXT("Missing renderer %s"),*Key); return; }
            auto* R=Options[0];Options.RemoveAt(0);
            FImports Imports=Resolved;
            for (const auto& Pair:S->GetObjectField(TEXT("local"))->Values)
            {
                const auto& Local=Pair.Value->AsObject();
                const auto& JsonPath=Local->Values[TEXT("path")];
                FString SourcePath;
                UObject* Obj=nullptr;
                if(Local->GetStringField(TEXT("role"))==TEXT("Material"))
                    Obj=GameAsset(JsonPath->AsString());
                else if (Row->GetStringField(TEXT("cls"))==TEXT("NiagaraComponentRendererProperties"))
                {
                    if(JsonPath->TryGetString(SourcePath))
                    {
                        auto* Class=Cast<UClass>(GameAsset(SourcePath));
                        if (!Class || !Class->IsChildOf(UShengLuPostComponent::StaticClass())) return;
                        auto* Post=NewObject<UShengLuPostComponent>(R,Class,NAME_None,RF_Transactional);
                        const auto& Mats=Local->GetArrayField(TEXT("materials"));
                        if (Mats.Num()) Post->PostMaterial=Cast<UMaterialInterface>(GameAsset(Mats[0]->AsString()));
                        Obj=Post;
                    }
                    else if (Local->GetStringField(TEXT("object")).StartsWith(TEXT("CameraShakeSource")))
                    {
                        auto* Shake=NewObject<UCameraShakeSourceComponent>(R,NAME_None,RF_Transactional);
                        const auto Settings=Local->GetObjectField(TEXT("settings"));
                        Shake->CameraShake=Cast<UClass>(GameAsset(Settings->GetStringField(TEXT("CameraShake"))));
                        if (!Shake->CameraShake)return;
                        Shake->InnerAttenuationRadius=Settings->GetNumberField(TEXT("InnerAttenuationRadius"));
                        Shake->OuterAttenuationRadius=Settings->GetNumberField(TEXT("OuterAttenuationRadius"));
                        Shake->bAutoStart=Settings->GetIntegerField(TEXT("bAutoStart"))!=0;
                        Shake->SetAbsolute(true,true,true);
                        Shake->SetVisibility(false);
                        Obj=Shake;
                    }
                    else Obj=NewObject<UPointLightComponent>(R,NAME_None,RF_Transactional);
                }
                Imports.Add(FCString::Atoi(*Pair.Key),Obj);
            }
            TArray<uint8> Bytes;
            if (!FBase64::Decode(Row->GetStringField(TEXT("data")),Bytes)) return;
            FRendererStream Ar(Bytes,Names,Imports);
            // Property serialization only. Source and target GUIDs identify different editor emitter instances.
            const FGuid TargetVersion=R->OuterEmitterVersion;
            R->GetClass()->SerializeTaggedProperties(Ar,reinterpret_cast<uint8*>(R),R->GetClass(),reinterpret_cast<const uint8*>(R->GetClass()->GetDefaultObject()));
            R->OuterEmitterVersion=TargetVersion;
            if (Ar.IsError() || Ar.Tell()!=Bytes.Num())
            {
                UE_LOG(LogTemp,Error,TEXT("Nature renderer stream %s/%s consumed %lld of %d"),*Path,*Key,Ar.Tell(),Bytes.Num());
                return;
            }
            ++Total;
        }
        System->RequestCompile(true);
        System->WaitForCompilationComplete(true,false);
        if(!System->IsValid()) { UE_LOG(LogTemp,Error,TEXT("Nature invalid %s"),*Path); return; }
        System->MarkPackageDirty();
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        if(!UPackage::SavePackage(System->GetOutermost(),System,*FPackageName::LongPackageNameToFilename(Path,FPackageName::GetAssetPackageExtension()),Args)) return;
        UE_LOG(LogTemp,Display,TEXT("Nature renderer streams saved %s"),*Path);
    }
    UE_LOG(LogTemp,Display,TEXT("NATURE_NATIVE_RENDERERS %d"),Total);
}
FAutoConsoleCommand Cmd(TEXT("RR.RecoverNativeRenderers"),TEXT("Restore audited NatureTheory renderer properties."),FConsoleCommandDelegate::CreateStatic(&RecoverRenderers));
}
#endif
