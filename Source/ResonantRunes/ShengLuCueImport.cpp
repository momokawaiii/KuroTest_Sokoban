#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/SavePackage.h"

namespace
{
void ImportSourceCues()
{
    FString Text;
    TArray<TSharedPtr<FJsonValue>> Data;
    const TCHAR* Source=FParse::Param(FCommandLine::Get(),TEXT("WaterAssets"))?TEXT("Artifacts/NatureTheory/Water/CueNodes.json"):TEXT("Artifacts/NatureTheory/CueNodes.json");
    if(!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectDir()/Source)) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Data)) return;
    FModuleManager::LoadModuleChecked<IModuleInterface>("AudioEditor");
    for(auto& Entry:Data)
    {
        const auto Row=Entry->AsObject();
        const FString P=Row->GetStringField(TEXT("path"));
        if(FPackageName::DoesPackageExist(P)) continue;
        auto* Package=CreatePackage(*P);
        auto* Cue=NewObject<USoundCue>(Package,FName(FPaths::GetCleanFilename(P)),RF_Public|RF_Standalone|RF_Transactional);
        Cue->CreateGraph();
        const auto& Exports=Row->GetArrayField(TEXT("nodes"));
        TArray<USoundNode*> Nodes;
        for(auto& Export:Exports)
        {
            const auto E=Export->AsObject();const auto F=E->GetObjectField(TEXT("fields"));
            const FString Type=E->GetStringField(TEXT("cls"));
            USoundNode* Node=nullptr;
            if(Type==TEXT("SoundNodeWavePlayer"))
            {
                auto* Wave=Cue->ConstructSoundNode<USoundNodeWavePlayer>();
                const FString Path=F->GetStringField(TEXT("wave"));
                auto* Sound=LoadObject<USoundWave>(nullptr,*(Path+TEXT(".")+FPaths::GetCleanFilename(Path)));
                if(!Sound) return;
                Wave->SetSoundWave(Sound);Wave->bLooping=F->GetBoolField(TEXT("loop"));Node=Wave;
            }
            else if(Type==TEXT("SoundNodeLooping")) Node=Cue->ConstructSoundNode<USoundNodeLooping>();
            else if(Type==TEXT("SoundNodeAttenuation"))
            {
                auto* A=Cue->ConstructSoundNode<USoundNodeAttenuation>();Node=A;
                A->bOverrideAttenuation=true;
                auto V=F->GetObjectField(TEXT("AttenuationOverrides"));
                double Mode=0;
                if(V->TryGetNumberField(TEXT("DistanceAlgorithm"),Mode)) A->AttenuationOverrides.DistanceAlgorithm=static_cast<EAttenuationDistanceModel>(int32(Mode));
                A->AttenuationOverrides.FalloffDistance=V->GetNumberField(TEXT("FalloffDistance"));
                const TArray<TSharedPtr<FJsonValue>>* Extents=nullptr;
                if(V->TryGetArrayField(TEXT("AttenuationShapeExtents"),Extents))
                    A->AttenuationOverrides.AttenuationShapeExtents=FVector((*Extents)[0]->AsNumber(),(*Extents)[1]->AsNumber(),(*Extents)[2]->AsNumber());
            }
            Nodes.Add(Node);
        }
        for(int32 I=0;I<Exports.Num();++I)
        {
            const auto F=Exports[I]->AsObject()->GetObjectField(TEXT("fields"));
            if(!Nodes[I]) { Cue->FirstNode=Nodes[int32(F->GetObjectField(TEXT("FirstNode"))->GetNumberField(TEXT("export")))];continue; }
            const TArray<TSharedPtr<FJsonValue>>* Children=nullptr;
            if(F->TryGetArrayField(TEXT("ChildNodes"),Children))
            {
                while(Nodes[I]->ChildNodes.Num()<Children->Num()) Nodes[I]->InsertChildNode(Nodes[I]->ChildNodes.Num());
                for(int32 Child=0;Child<Children->Num();++Child)
                    Nodes[I]->ChildNodes[Child]=Nodes[int32((*Children)[Child]->AsObject()->GetNumberField(TEXT("export")))];
            }
            if(UEdGraphNode* EditorNode=Nodes[I]->GetGraphNode()) EditorNode->NodePosX=-250*I;
        }
        Cue->LinkGraphNodesFromSoundNodes();
        Cue->PostEditChange();
        FAssetRegistryModule::AssetCreated(Cue);
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        if(!UPackage::SavePackage(Package,Cue,*FPackageName::LongPackageNameToFilename(P,FPackageName::GetAssetPackageExtension()),Args)) return;
    }
    UE_LOG(LogTemp,Display,TEXT("NATURE_NATIVE_CUES %d"),Data.Num());
}
FAutoConsoleCommand Cmd(TEXT("RR.ImportSourceCues"),TEXT("Restore NatureTheory cue graphs."),FConsoleCommandDelegate::CreateStatic(&ImportSourceCues));
}
#endif
