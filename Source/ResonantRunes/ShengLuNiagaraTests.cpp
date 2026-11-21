#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeCustomHlsl.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElectricScheduleReadback,"ResonantRunes.Electric01.SavedEmitterInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FElectricScheduleReadback::RunTest(const FString&)
{
    const FString Directory=FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory/Electric01-Branches");
    FString Text;
    TSharedPtr<FJsonObject> Recipes;
    if (!FFileHelper::LoadFileToString(Text,*(Directory/TEXT("Schedules.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Recipes)) return false;
    int32 ModulesRead=0, BirthGraphs=0, SizeGraphs=0, ColorGraphs=0;
    for (const auto& Pair:Recipes->Values)
    {
        const FString Name(*Pair.Key);
        const FString Path=TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/")+Name;
        auto* System=LoadObject<UNiagaraSystem>(nullptr,*(Path+TEXT(".")+Name));
        if (!TestNotNull(Name,System)) continue;
        for (const auto& Handle:System->GetEmitterHandles())
        {
            auto* Data=Handle.GetEmitterData();
            if (!TestNotNull(Handle.GetName().ToString(),Data)) continue;
            const FString Emitter=Handle.GetName().ToString();
            const auto Row=Pair.Value->AsObject()->GetObjectField(Emitter);
            const FString Id=Name+TEXT(".")+Emitter+TEXT(".");
            const auto Source=Row->GetObjectField(TEXT("instance"));
            TestEqual(Id+TEXT("local space"),int32(Data->bLocalSpace),Source->GetIntegerField(TEXT("bLocalSpace")));
            TestEqual(Id+TEXT("determinism"),int32(Data->bDeterminism),Source->GetIntegerField(TEXT("bDeterminism")));
            TestEqual(Id+TEXT("seed"),Data->RandomSeed,Source->GetIntegerField(TEXT("RandomSeed")));
            TestEqual(Id+TEXT("persistent IDs"),int32(Data->bRequiresPersistentIDs),Source->GetIntegerField(TEXT("bRequiresPersistentIDs")));
            TestEqual(Id+TEXT("simulation target"),int32(Data->SimTarget),Source->GetIntegerField(TEXT("SimTarget")));
            TestEqual(Id+TEXT("bounds mode"),int32(Data->CalculateBoundsMode),Source->GetIntegerField(TEXT("CalculateBoundsMode")));
            TestEqual(Id+TEXT("interpolated spawn"),int32(Data->InterpolatedSpawnMode),Source->GetIntegerField(TEXT("InterpolatedSpawnMode")));
            TestEqual(Id+TEXT("allocation"),int32(Data->AllocationMode),Source->GetIntegerField(TEXT("AllocationMode")));
            TestEqual(Id+TEXT("preallocate"),Data->PreAllocationCount,Source->GetIntegerField(TEXT("PreAllocationCount")));
            TestEqual(Id+TEXT("GPU limit"),Data->MaxGPUParticlesSpawnPerFrame,Source->GetIntegerField(TEXT("MaxGPUParticlesSpawnPerFrame")));
            for (const FString Key:{FString(TEXT("min")),FString(TEXT("max"))})
            {
                const auto& B=Source->GetObjectField(TEXT("FixedBounds"))->GetArrayField(Key);
                const FVector V(B[0]->AsNumber(),B[1]->AsNumber(),B[2]->AsNumber());
                TestTrue(Id+Key,(Key==TEXT("min")?Data->FixedBounds.Min:Data->FixedBounds.Max).Equals(V));
            }
            const auto* Graph=CastChecked<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph.Get();
            TMap<FString,UNiagaraNodeFunctionCall*> Calls;
            UNiagaraNodeCustomHlsl* Birth=nullptr;
            UNiagaraNodeCustomHlsl* SizeUpdate=nullptr;
            UNiagaraNodeCustomHlsl* ColorUpdate=nullptr;
            for (UEdGraphNode* N:Graph->Nodes)
            {
                if (N->NodeComment==TEXT("Electric01 cooked color update expressions")) ColorUpdate=Cast<UNiagaraNodeCustomHlsl>(N);
                if (auto* Call=Cast<UNiagaraNodeFunctionCall>(N)) Calls.Add(Call->GetFunctionName(),Call);
                if (N->NodeComment==TEXT("Electric01 cooked birth expressions")) Birth=Cast<UNiagaraNodeCustomHlsl>(N);
                if (N->NodeComment==TEXT("Electric01 cooked size update expressions")) SizeUpdate=Cast<UNiagaraNodeCustomHlsl>(N);
            }
            auto Input=[&](UNiagaraNodeFunctionCall& Node,const FString& Key)
            {
                for (const auto& K:Node.GetCalledGraph()->GetAllMetaData())
                {
                    FString Field=K.Key.GetName().ToString();
                    if (Field.RemoveFromStart(TEXT("Module.")) && Field.Replace(TEXT(" "),TEXT("")).Equals(Key,ESearchCase::IgnoreCase))
                    {
                        const FName Alias(Node.GetFunctionName()+TEXT(".")+Field);
                        for (UEdGraphPin* Pin : Node.Pins)
                            if (Pin->Direction==EGPD_Input && Pin->LinkedTo.Num()==1 &&
                                Pin->PinType.PinSubCategoryObject==FNiagaraTypeDefinition::GetParameterMapDef().GetStruct())
                                return Pin->LinkedTo[0]->GetOwningNode()->FindPin(Alias,EGPD_Input);
                    }
                }
                return static_cast<UEdGraphPin*>(nullptr);
            };
            auto Numeric=[&](UNiagaraNodeFunctionCall& Node,const TCHAR* Field,double Expected)
            {
                auto* P=Input(Node,Field);
                const FString Label=Id+Node.GetFunctionName()+TEXT(".")+Field;
                if (TestNotNull(Label,P))
                    TestTrue(Label,P->LinkedTo.IsEmpty() && P->DefaultValue.IsNumeric()
                             && FMath::IsNearlyEqual(FCString::Atod(*P->DefaultValue),Expected,.0001));
            };
            auto Link=[&](UNiagaraNodeFunctionCall& Node,const TCHAR* Field,FName Target)
            {
                auto* Pin=Input(Node,Field);
                TestTrue(Id+Field,Pin && Pin->LinkedTo.ContainsByPredicate([Target](auto* P){return P->PinName==Target;}));
            };
            for (auto& S:Row->GetArrayField(TEXT("spawns")))
            {
                const auto Spawn=S->AsObject();
                const auto Fn=Spawn->GetStringField(TEXT("function"));
                auto* Node=Calls.FindRef(Fn);
                if (Fn==TEXT("NM_Codi_SpawnPerUnit") && !Node) Node=Calls.FindRef(TEXT("SpawnPerUnit"));
                if (!TestNotNull(Id+Fn,Node)) continue;
                ++ModulesRead;
                double Value;
                if (Spawn->TryGetNumberField(TEXT("burst"),Value)) Numeric(*Node,TEXT("SpawnCount"),Value);
                if (Spawn->TryGetNumberField(TEXT("time"),Value)) Numeric(*Node,TEXT("SpawnTime"),Value);
                if (Spawn->TryGetNumberField(TEXT("group"),Value) && (Value!=0 || Fn.StartsWith(TEXT("SpawnBurst")))) Numeric(*Node,TEXT("SpawnGroup"),Value);
                if (Spawn->TryGetNumberField(TEXT("rate"),Value)) Numeric(*Node,TEXT("SpawnRate"),Value);
                if (Fn==TEXT("NM_Codi_SpawnPerUnit"))
                {
                    int32 SpacingModules=0;
                    for (auto& Call:Calls)
                        if (Node->FunctionScript && Call.Value->FunctionScript==Node->FunctionScript) ++SpacingModules;
                    TestEqual(Id+TEXT("distance-driven spawn modules"),SpacingModules,1);
                    Numeric(*Node,TEXT("SpawnSpacing"),5);
                    Link(*Node,TEXT("VelocityVector"),TEXT("User.ElectricSurfaceVelocity"));
                }
                if (Name.EndsWith(TEXT("_Hit")) && Emitter==TEXT("LightningSource"))
                    Link(*Node,TEXT("SpawnRate"),TEXT("User.ElectricBranchRate"));
            }
            if (auto* State=Calls.FindRef(TEXT("EmitterState")))
            {
                double Value;
                if (Row->TryGetNumberField(TEXT("CurrentLoopDuration"),Value)) Numeric(*State,TEXT("LoopDuration"),Value);
                if (Row->TryGetNumberField(TEXT("CurrentLoopDelay"),Value) && Value>0) Numeric(*State,TEXT("LoopDelay"),Value);
            }
            const TSharedPtr<FJsonObject>* Expected=nullptr;
            if (Row->TryGetObjectField(TEXT("color_update"),Expected) && TestNotNull(Id+TEXT("color update"),ColorUpdate))
            {
                ++ColorGraphs;
                const auto Code=FindFProperty<FStrProperty>(ColorUpdate->GetClass(),TEXT("CustomHlsl"))->GetPropertyValue_InContainer(ColorUpdate);
                const auto* Out=ColorUpdate->FindPin(TEXT("OutColor"),EGPD_Output);
                TestTrue(Id+TEXT("source RGBA update"),Out && !Out->LinkedTo.IsEmpty() && Code==(*Expected)->GetStringField(TEXT("hlsl")));
            }
            if (Row->TryGetObjectField(TEXT("size_update"),Expected) && TestNotNull(Id+TEXT("size update"),SizeUpdate))
            {
                ++SizeGraphs;
                const auto Code=FindFProperty<FStrProperty>(SizeUpdate->GetClass(),TEXT("CustomHlsl"))->GetPropertyValue_InContainer(SizeUpdate);
                const auto* Size=SizeUpdate->FindPin(TEXT("OutSpriteSize"),EGPD_Output);
                TestTrue(Id+TEXT("speed stretch"),Code.StartsWith((*Expected)->GetStringField(TEXT("hlsl"))) && Size && !Size->LinkedTo.IsEmpty());
            }
            if (Row->TryGetObjectField(TEXT("birth"),Expected) && TestNotNull(Id+TEXT("birth"),Birth))
            {
                // The custom expressions must still feed the assignment after saving and reloading.
                ++BirthGraphs;
                const auto Shader=FindFProperty<FStrProperty>(Birth->GetClass(),TEXT("CustomHlsl"))->GetPropertyValue_InContainer(Birth);
                TestTrue(Id+TEXT("birth expressions"),Shader.StartsWith((*Expected)->GetStringField(TEXT("hlsl"))));
                for (const auto& V:(*Expected)->GetArrayField(TEXT("attributes")))
                {
                    const auto* Out=Birth->FindPin(FName(TEXT("Out")+V->AsString().Replace(TEXT("."),TEXT(""))),EGPD_Output);
                    TestTrue(Id+V->AsString(),Out && !Out->LinkedTo.IsEmpty());
                }
            }
        }
    }
    AddInfo(FString::Printf(TEXT("Read %d saved spawn modules, %d particle birth graphs, %d GPU size updates and %d CPU color updates."),
        ModulesRead,BirthGraphs,SizeGraphs,ColorGraphs));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShengLuNiagaraStructure, "ResonantRunes.ShengLu.NiagaraStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FShengLuNiagaraStructure::RunTest(const FString&)
{
    const FString Dir = FPaths::ProjectDir() / TEXT("Artifacts/NatureTheory");
    FString Source;
    TArray<TSharedPtr<FJsonValue>> Systems;
    if (!FFileHelper::LoadFileToString(Source, *(Dir / TEXT("SourceStructure.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Source), Systems))
    {
        AddError(TEXT("Missing NatureTheory source manifest"));
        return false;
    }
    FString MaterialManifest;
    TArray<TSharedPtr<FJsonValue>> PhaseMaterials;
    if (!FFileHelper::LoadFileToString(MaterialManifest,*(Dir / TEXT("PhaseMaterials.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MaterialManifest),PhaseMaterials))
    {
        AddError(TEXT("Missing phase renderer material manifest"));
        return false;
    }
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const auto& Entry : Systems)
    {
        const auto& Row = Entry->AsObject();
        const FString Path = Row->GetStringField(TEXT("source"));
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *(Path + TEXT(".") + FPaths::GetCleanFilename(Path)));
        if (!TestNotNull(Path, System)) continue;
        System->WaitForCompilationComplete(true, false);
        TArray<FString> Expected;
        for (const auto& Value : Row->GetArrayField(TEXT("emitters")))
        {
            const auto& Handle = Value->AsObject();
            if (Handle->GetBoolField(TEXT("enabled")) || !Handle->HasTypedField<EJson::Null>(TEXT("source_export")))
                Expected.Add(Handle->GetStringField(TEXT("name")));
        }
        const auto& Handles = System->GetEmitterHandles();
        TestEqual(Path + TEXT(" handle count"), Handles.Num(), Expected.Num());
        TArray<TSharedPtr<FJsonValue>> Actual;
        for (int32 Index = 0; Index < Handles.Num(); ++Index)
        {
            const FNiagaraEmitterHandle& Handle = Handles[Index];
            const FString Name = Handle.GetName().ToString();
            if (Expected.IsValidIndex(Index)) TestEqual(Path + TEXT(" ordered name"), Name, Expected[Index]);
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Name);
            Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
            auto* Data = Handle.GetEmitterData();
            Item->SetBoolField(TEXT("instance"), Data != nullptr);
            TestTrue(Path + TEXT(".") + Name, Handle.GetIsEnabled() && Data);
            TArray<TSharedPtr<FJsonValue>> Renderers;
            TArray<TSharedPtr<FJsonValue>> Meshes, Materials;
            if (Data)
                for (auto* Renderer : Data->GetRenderers())
                {
                    if (!Renderer) continue;
                    Renderers.Add(MakeShared<FJsonValueString>(Renderer->GetClass()->GetName()));
                    if (const auto* Mesh=Cast<UNiagaraMeshRendererProperties>(Renderer))
                    {
                        for (const auto& Slot:Mesh->Meshes)
                            if (Slot.Mesh) Meshes.Add(MakeShared<FJsonValueString>(Slot.Mesh->GetOutermost()->GetName()));
                        if (Mesh->bOverrideMaterials)
                            for (const auto& Slot:Mesh->OverrideMaterials)
                                if (Slot.ExplicitMat) Materials.Add(MakeShared<FJsonValueString>(Slot.ExplicitMat->GetOutermost()->GetName()));
                    }
                    if (const auto* Ribbon=Cast<UNiagaraRibbonRendererProperties>(Renderer); Ribbon && Ribbon->Material)
                        Materials.Add(MakeShared<FJsonValueString>(Ribbon->Material->GetOutermost()->GetName()));
                    if (const auto* Decal=Cast<UNiagaraDecalRendererProperties>(Renderer); Decal && Decal->Material)
                        Materials.Add(MakeShared<FJsonValueString>(Decal->Material->GetOutermost()->GetName()));
                    if (const auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(Renderer); Sprite && Sprite->Material)
                        Materials.Add(MakeShared<FJsonValueString>(Sprite->Material->GetOutermost()->GetName()));
                }
            for (const auto& Record:PhaseMaterials)
            {
                const auto& Mat=Record->AsObject();
                if (Mat->GetStringField(TEXT("system"))!=System->GetName() || Mat->GetStringField(TEXT("emitter"))!=Name) continue;
                FString Package,Object;
                Mat->GetStringField(TEXT("material")).Split(TEXT("."),&Package,&Object);
                TestTrue(Path+TEXT(".")+Name+TEXT(" phase material ")+Package,Materials.ContainsByPredicate(
                    [&](const TSharedPtr<FJsonValue>& Value){return Value->AsString()==Package;}));
            }
            Item->SetArrayField(TEXT("renderers"), Renderers);
            Item->SetArrayField(TEXT("meshes"), Meshes);
            Item->SetArrayField(TEXT("materials"), Materials);
            Actual.Add(MakeShared<FJsonValueObject>(Item));
        }
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("system"), System->GetName());
        Result->SetBoolField(TEXT("valid"), System->IsValid());
        Result->SetArrayField(TEXT("emitters"), Actual);
        Results.Add(MakeShared<FJsonValueObject>(Result));
        TestTrue(Path + TEXT(" compiled"), System->IsValid());
    }
    FString Report;
    FJsonSerializer::Serialize(Results, TJsonWriterFactory<>::Create(&Report));
    TestTrue(TEXT("Write native readback"), FFileHelper::SaveStringToFile(Report, *(Dir / TEXT("TargetReadback.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    return !HasAnyErrors();
}
#endif
