#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraDataInterfaceParticleRead.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

// Both bursts on Minimal011 sample LineSource using emitter-owned interfaces.
void BindAir02LineSource(UNiagaraSystem& System,FVersionedNiagaraEmitterData& Emitter)
{
    const auto Type=FNiagaraTypeDefinition(UNiagaraDataInterfaceParticleRead::StaticClass());
    UNiagaraGraph* Graph=CastChecked<UNiagaraScriptSource>(Emitter.GraphSource)->NodeGraph;
    TArray<UNiagaraNodeFunctionCall*> Calls;
    Graph->GetNodesOfClass(Calls);
    TArray<UEdGraphPin*> Overrides;
    for (auto* Call : Calls)
    {
        if (!Call->GetFunctionName().StartsWith(TEXT("SpawnParticlesFromOtherEmitter"))) continue;
        // Both 5.7 calls produce their own counts. Only the final call supplies the
        // renderer-facing reader. Isolate the earlier helper output in a local copy
        // so UE 5.8 does not resolve two assignments to the same data interface.
        if (Call->GetFunctionName()==TEXT("SpawnParticlesFromOtherEmitter001") && Emitter.ScratchPads &&
            Call->FunctionScript && Call->FunctionScript->GetOuter()!=Emitter.ScratchPads)
        {
            auto* Local=DuplicateObject<UNiagaraScript>(Call->FunctionScript,Emitter.ScratchPads,TEXT("Air02FirstLineBurst"));
            auto* Source=CastChecked<UNiagaraScriptSource>(Local->GetSource(Call->SelectedScriptVersion));
            const FNiagaraVariable Output(Type,TEXT("Emitter.SpawnParticlesAttributeReader"));
            if (Source->NodeGraph->RenameParameter(Output,TEXT("Emitter.FirstLineBurstAttributeReader")))
            {
                Local->ClearFlags(RF_Public|RF_Standalone);
                Emitter.ScratchPads->AppendScripts({TObjectPtr<UNiagaraScript>(Local)});
                Call->FunctionScript=Local;
                Call->RefreshFromExternalChanges();
            }
        }
        if (Call->FunctionScript && Call->FunctionScript->GetOuter()==Emitter.ScratchPads)
        {
            ForEachObjectWithOuter(Call->FunctionScript,[](UObject* O)
            {
                if (auto* DI=Cast<UNiagaraDataInterfaceParticleRead>(O))
                    DI->EmitterBinding.EmitterName=TEXT("LineSource");
            },true);
        }
        auto& Pin=FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Call,
            FNiagaraParameterHandle(FName(Call->GetFunctionName()+TEXT(".Attribute Reader"))),Type,FGuid(),FGuid());
        Overrides.Add(&Pin);
    }
    if (Overrides.Num()!=2) return;
    for (int32 I=0;I<Overrides.Num();++I)
    {
        auto* Pin=Overrides[I];
        if (Pin->LinkedTo.Num()==1 && Pin->LinkedTo[0]->LinkedTo.Num()==1 &&
            Pin->LinkedTo[0]->GetOwningNode()->NodeComment==TEXT("Air02 source for this burst")) continue;
        Pin->BreakAllPinLinks();
        UNiagaraDataInterface* Data=nullptr;
        FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(*Pin,Type.GetClass(),I?TEXT("SecondLineSource"):TEXT("FirstLineSource"),Data);
        auto* Reader=CastChecked<UNiagaraDataInterfaceParticleRead>(Data);
        Reader->EmitterBinding.BindingMode=ENiagaraDataInterfaceEmitterBindingMode::Other;
        Reader->EmitterBinding.EmitterName=TEXT("LineSource");
        Pin->LinkedTo[0]->GetOwningNode()->NodeComment=TEXT("Air02 source for this burst");
    }
    System.GetExposedParameters().RemoveParameter(FNiagaraVariable(Type,TEXT("User.LineSourceReader")));
}
namespace
{
using FRow=TSharedPtr<FJsonObject>;
namespace Stack=FNiagaraStackGraphUtilities;
TArray<TSharedPtr<FJsonValue>> ReadRows(const TCHAR* Name)
{
    FString T;TArray<TSharedPtr<FJsonValue>> Out;
    if(FFileHelper::LoadFileToString(T,*(FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory")/Name)))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),Out);
    return Out;
}
void PreserveSourcePosition(FVersionedNiagaraEmitterData& Source)
{
    const auto Type=FNiagaraTypeDefinition::GetPositionDef();
    Source.AttributesToPreserve.AddUnique(TEXT("Particles.Previous.Position"));
    Source.AttributesToPreserve.AddUnique(TEXT("Particles.SampledFrameOrigin"));
    UNiagaraGraph* G=CastChecked<UNiagaraScriptSource>(Source.GraphSource)->NodeGraph;
    TArray<UNiagaraNodeOutput*> Outputs;
    G->GetNodesOfClass(Outputs);
    for(auto* Out:Outputs)
    {
        auto Stage=Out->GetUsage();
        if(Stage!=ENiagaraScriptUsage::ParticleUpdateScript) continue;
        const FString Marker=TEXT("Nature reader previous frame");
        if(G->Nodes.ContainsByPredicate([&](const UEdGraphNode* N){return N->NodeComment==Marker;}))continue;
        auto* Set=Stack::AddParameterModuleToStack({FNiagaraVariable(Type,TEXT("Particles.SampledFrameOrigin"))},*Out,0,{FString()});
        if(!Set)continue;
        Set->NodeComment=Marker;
        auto& Pin=Stack::GetOrCreateStackFunctionInputOverridePin(*Set,FNiagaraParameterHandle(FName(Set->GetFunctionName()+TEXT(".Particles.SampledFrameOrigin"))),Type,FGuid(),FGuid());
        Stack::SetLinkedParameterValueForFunctionInput(Pin,FNiagaraVariable(Type,TEXT("Particles.Previous.Position")),{});
    }
}
}
void PreserveWaterReaderSource(FVersionedNiagaraEmitterData& Source)
{
    PreserveSourcePosition(Source);
}
namespace
{
void AddMissingSourceModules()
{
    TMap<FString,FString> Scripts={
        {TEXT("SpawnParticlesFromOtherEmitter"),TEXT("AttributeReader/SpawnParticlesFromOtherEmitter")},
        {TEXT("SampleParticlesFromOtherEmitter"),TEXT("AttributeReader/SampleParticlesFromOtherEmitter")},
        {TEXT("UpdateParticlesFromOtherEmitter"),TEXT("AttributeReader/UpdateParticlesFromOtherEmitter")},
        {TEXT("UpdateMeshOrientation"),TEXT("Update/Orientation/V3/UpdateMeshOrientation")},
        {TEXT("InitialMeshOrientation"),TEXT("Spawn/Orientation/V3/InitialMeshOrientation")},
        {TEXT("PointAttractionForce"),TEXT("Update/Forces/V2/PointAttractionForce")},
        {TEXT("DynamicMaterialParameters"),TEXT("Update/Material/DynamicMaterialParameters")},
        {TEXT("SpriteFacingAndAlignment"),TEXT("Update/Renderers/Sprite/SpriteFacingAndAlignment")},
        {TEXT("SpriteRotationRate"),TEXT("Update/Orientation/SpriteRotationRate")},
        {TEXT("CameraOffset"),TEXT("Update/Camera/CameraOffset")},
        {TEXT("Light_Attributes"),TEXT("Light/Light_Attributes")},
        {TEXT("Decal_Attributes"),TEXT("Decal/Decal_Attributes")},
        {TEXT("SubUVAnimation"),TEXT("Update/SubUV/V2/SubUVAnimation")}
    };
    auto Readers=ReadRows(TEXT("NativeInterfaces.json"));
    auto Systems=ReadRows(TEXT("SourceStructure.json"));
    int32 Added=0,Linked=0;
    for(auto& Value:Systems)
    {
        const auto S=Value->AsObject();
        const FString P=S->GetStringField(TEXT("source"));
        auto* System=LoadObject<UNiagaraSystem>(nullptr,*(P+TEXT(".")+FPaths::GetCleanFilename(P)));
        if(!System)return;
        TMap<FString,FVersionedNiagaraEmitterData*> Emitters;
        for(auto& H:System->GetEmitterHandles()) Emitters.Add(H.GetName().ToString(),H.GetEmitterData());
        TSet<FString> Prepared;
        for(auto& Ref:Readers)
        {
            const auto R=Ref->AsObject();
            if(R->GetStringField(TEXT("system"))!=P || R->GetStringField(TEXT("cls"))!=TEXT("NiagaraDataInterfaceParticleRead")) continue;
            const TSharedPtr<FJsonObject>* Binding=nullptr;
            if(!R->GetObjectField(TEXT("fields"))->TryGetObjectField(TEXT("EmitterBinding"),Binding))continue;
            const FString Target=(*Binding)->GetStringField(TEXT("EmitterName"));
            if(auto* Source=Emitters.FindRef(Target);Source && !Prepared.Contains(Target)) { PreserveSourcePosition(*Source);Prepared.Add(Target); }
        }
        for(auto& Item:S->GetArrayField(TEXT("emitters")))
        {
            const auto Row=Item->AsObject();
            if(!Row->GetBoolField(TEXT("enabled")))continue;
            const FString Name=Row->GetStringField(TEXT("name"));
            auto* E=Emitters.FindRef(Name);
            if(!E)return;
            UNiagaraGraph* Graph=CastChecked<UNiagaraScriptSource>(E->GraphSource)->NodeGraph;
            TMap<ENiagaraScriptUsage,UNiagaraNodeOutput*> Outputs;
            TMap<FString,UNiagaraNodeFunctionCall*> Existing;
            for(UEdGraphNode* N:Graph->Nodes)
            {
                if(auto* O=Cast<UNiagaraNodeOutput>(N)) Outputs.Add(O->GetUsage(),O);
                if(auto* F=Cast<UNiagaraNodeFunctionCall>(N)) Existing.Add(F->GetFunctionName(),F);
            }
            TArray<FString> Spawn,Update,Emitter;
            auto Functions=Row->GetObjectField(TEXT("particle_functions"));
            Functions->TryGetStringArrayField(TEXT("SpawnScript"),Spawn);
            Functions->TryGetStringArrayField(TEXT("UpdateScript"),Update);
            Row->TryGetStringArrayField(TEXT("emitter_functions"),Emitter);
            TArray<FString> All=Emitter;All.Append(Spawn);All.Append(Update);
            for(const FString& Fn:All)
            {
                if(Existing.Contains(Fn)) continue;
                FString Base=Fn;
                while(Base.Len() && FChar::IsDigit(Base[Base.Len()-1])) Base.LeftChopInline(1);
                const FString* Rel=Scripts.Find(Base);
                if(!Rel)continue;
                ENiagaraScriptUsage Usage=Emitter.Contains(Fn)?ENiagaraScriptUsage::EmitterUpdateScript:Update.Contains(Fn)?ENiagaraScriptUsage::ParticleUpdateScript:ENiagaraScriptUsage::ParticleSpawnScript;
                auto* Out=Outputs.FindRef(Usage);if(!Out)continue;
                auto* Script=LoadObject<UNiagaraScript>(nullptr,*(TEXT("/Niagara/Modules/")+*Rel+TEXT(".")+Base));
                if(!Script)return;
                auto* Node=Stack::AddScriptModuleToStack(Script,*Out,INDEX_NONE,Fn);
                if(!Node)return;
                Existing.Add(Fn,Node);++Added;
                if(Base!=TEXT("SpawnParticlesFromOtherEmitter") && Base!=TEXT("UpdateParticlesFromOtherEmitter"))continue;
                const FString Binding=Base.StartsWith(TEXT("Spawn"))?Name+TEXT(".SpawnParticlesAttributeReader"):TEXT("__INTERNAL__.")+Name+TEXT(".")+Fn+TEXT(".Attribute Reader");
                for(auto& Ref:Readers)
                {
                    const auto R=Ref->AsObject();
                    if(R->GetStringField(TEXT("system"))!=P || R->GetStringField(TEXT("cls"))!=TEXT("NiagaraDataInterfaceParticleRead"))continue;
                    TArray<FString> B;R->TryGetStringArrayField(TEXT("bindings"),B);if(!B.Contains(Binding))continue;
                    FString Target=R->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("EmitterBinding"))->GetStringField(TEXT("EmitterName"));
                    if(auto* Source=Emitters.FindRef(Target))Source->bRequiresPersistentIDs=true;
                    const auto Type=FNiagaraTypeDefinition(UNiagaraDataInterfaceParticleRead::StaticClass());
                    auto& Override=Stack::GetOrCreateStackFunctionInputOverridePin(*Node,FNiagaraParameterHandle(FName(Fn+TEXT(".Attribute Reader"))),Type,FGuid(),FGuid());
                    UNiagaraDataInterface* DI=nullptr;
                    Stack::SetDataInterfaceValueForFunctionInput(Override,Type.GetClass(),TEXT("Attribute Reader"),DI);
                    auto* Reader=CastChecked<UNiagaraDataInterfaceParticleRead>(DI);
                    Reader->EmitterBinding.BindingMode=ENiagaraDataInterfaceEmitterBindingMode::Other;
                    Reader->EmitterBinding.EmitterName=FName(Target);
                    ++Linked;break;
                }
            }
            if (P.EndsWith(TEXT("_Tornado")) && Name==TEXT("Minimal011"))
                BindAir02LineSource(*System,*E);
        }
        System->RequestCompile(true);System->WaitForCompilationComplete(true,false);
        if(!System->IsValid()){UE_LOG(LogTemp,Error,TEXT("Nature modules failed %s"),*P);return;}
        System->MarkPackageDirty();
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        if(!UPackage::SavePackage(System->GetOutermost(),System,*FPackageName::LongPackageNameToFilename(P,FPackageName::GetAssetPackageExtension()),Args))return;
    }
    UE_LOG(LogTemp,Display,TEXT("NATURE_SOURCE_MODULES added=%d reader_links=%d"),Added,Linked);
}
FAutoConsoleCommand Cmd(TEXT("RR.RestoreSourceModules"),TEXT("Bind recovered Niagara module calls and cross-emitter readers."),FConsoleCommandDelegate::CreateStatic(&AddMissingSourceModules));
}
#endif
