#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

namespace
{
    namespace Stack=FNiagaraStackGraphUtilities;
    using T=FNiagaraTypeDefinition;
    using FRow=TSharedPtr<FJsonObject>;

    UEdGraphPin& Override(UNiagaraNodeFunctionCall& Call,const FString& Name,const T& Type)
    {
        return Stack::GetOrCreateStackFunctionInputOverridePin(Call,
            FNiagaraParameterHandle(FName(Call.GetFunctionName()+TEXT(".")+Name)),Type,FGuid(),FGuid());
    }

    UEdGraphPin* ModuleInput(UNiagaraNodeFunctionCall& Call,const FString& Requested)
    {
        // Stock module versions use both "Spawn Rate" and "SpawnRate".
        for (const auto& Pair : Call.GetCalledGraph()->GetAllMetaData())
        {
            FString Name=Pair.Key.GetName().ToString();
            if (Name.RemoveFromStart(TEXT("Module.")) && Name.Replace(TEXT(" "),TEXT("")).Equals(Requested,ESearchCase::IgnoreCase))
                return &Override(Call,Name,Pair.Key.GetType());
        }
        UE_LOG(LogTemp,Error,TEXT("Electric01 input %s missing on %s"),*Requested,*Call.GetFunctionName());
        return nullptr;
    }

    bool Number(UNiagaraNodeFunctionCall& Call,const TCHAR* Name,double Value,bool Integer=false)
    {
        auto* Pin=ModuleInput(Call,Name);
        if (!Pin) return false;
        Pin->DefaultValue=Integer?FString::FromInt(FMath::RoundToInt(Value)):FString::SanitizeFloat(Value);
        return true;
    }

    bool UserInput(UNiagaraSystem& System,UNiagaraNodeFunctionCall& Call,const TCHAR* Input,const TCHAR* Source,const T& Type)
    {
        auto* Pin=ModuleInput(Call,Input);
        if (!Pin) return false;
        const FNiagaraVariable V(Type,Source);
        System.GetExposedParameters().AddParameter(V);
        if (!Pin->LinkedTo.Num()) Stack::SetLinkedParameterValueForFunctionInput(*Pin,V,{});
        return true;
    }

    bool WriteParticleExpressions(UNiagaraGraph& Graph,UNiagaraNodeOutput& Stage,const FRow& Row,float DiameterScale,const FString& Label)
    {
        TArray<FString> Names,Defaults;
        Row->TryGetStringArrayField(TEXT("attributes"),Names);
        TArray<FNiagaraVariable> Vars;
        for (const auto& N:Names)
        {
            const auto Type=N==TEXT("Color")?T::GetColorDef():N.EndsWith(TEXT("SpriteSize"))?T::GetVec2Def():T::GetFloatDef();
            Vars.Emplace(Type,FName(TEXT("Particles.")+N));
            Defaults.Add(FString());
        }
        UNiagaraNodeAssignment* Assign=nullptr;
        UNiagaraNodeCustomHlsl* Hlsl=nullptr;
        for (UEdGraphNode* N:Graph.Nodes)
        {
            if (N->NodeComment==Label+TEXT(" outputs")) Assign=Cast<UNiagaraNodeAssignment>(N);
            if (N->NodeComment==Label+TEXT(" expressions")) Hlsl=Cast<UNiagaraNodeCustomHlsl>(N);
        }
        if (!Assign) Assign=Stack::AddParameterModuleToStack(Vars,Stage,INDEX_NONE,Defaults);
        if (!Assign) return false;
        Assign->NodeComment=Label+TEXT(" outputs");
        if (!Hlsl)
        {
            FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(Graph);
            Hlsl=Creator.CreateNode();
            Hlsl->NodeComment=Label+TEXT(" expressions");
            Hlsl->Signature.Inputs.Emplace(T::GetParameterMapDef(),TEXT("Map"));
            for (int I=0;I<Names.Num();++I)
                Hlsl->Signature.Outputs.Emplace(Vars[I].GetType(),FName(TEXT("Out")+Names[I].Replace(TEXT("."),TEXT(""))));
            Creator.Finalize();
        }
        FString Code=Row->GetStringField(TEXT("hlsl"));
        for (int I=0;I<Names.Num();++I)
        {
            const FString OutName=TEXT("Out")+Names[I].Replace(TEXT("."),TEXT(""));
            if (Names[I].EndsWith(TEXT("SpriteSize")) || Names[I]==TEXT("RibbonWidth"))
                Code+=FString::Printf(TEXT("\n%s *= %.6ff;"),*OutName,DiameterScale);
            auto& Pin=Override(*Assign,Vars[I].GetName().ToString(),Vars[I].GetType());
            auto* Output=Hlsl->FindPin(FName(OutName),EGPD_Output);
            auto* Map=Hlsl->FindPin(TEXT("Map"),EGPD_Input);
            if (!Output || !Map) return false;
            if (!Map->LinkedTo.Num())
                for (auto* P:Pin.GetOwningNode()->Pins)
                    if (P->Direction==EGPD_Input && P->PinType.PinSubCategoryObject==T::GetParameterMapDef().GetStruct() && P->LinkedTo.Num())
                    { P->LinkedTo[0]->MakeLinkTo(Map);break; }
            if (!Map->LinkedTo.Num()) return false;
            if (!Pin.LinkedTo.Contains(Output)) { Pin.BreakAllPinLinks();Output->MakeLinkTo(&Pin); }
        }
        FindFProperty<FStrProperty>(Hlsl->GetClass(),TEXT("CustomHlsl"))->SetPropertyValue_InContainer(Hlsl,Code);
        Hlsl->MarkNodeRequiresSynchronization(Label,true);
        return true;
    }

    bool SetSwitch(UNiagaraNodeFunctionCall& Node,FString Name,const FString& Choice)
    {
        for (auto* P:Node.Pins)
        {
            if (P->Direction!=EGPD_Input || P->PinName.ToString().Replace(TEXT(" "),TEXT(""))!=Name) continue;
            if (auto* Enum=Cast<UEnum>(P->PinType.PinSubCategoryObject.Get()))
            {
                for (int I=0;I<Enum->NumEnums();++I)
                    if (Enum->GetDisplayNameTextByIndex(I).ToString().Equals(Choice,ESearchCase::IgnoreCase))
                    { P->DefaultValue=Enum->GetNameStringByIndex(I);return true; }
            }
            else { P->DefaultValue=Choice;return true; }
        }
        return false;
    }

    bool Configure(UNiagaraSystem& System,FNiagaraEmitterHandle& Handle,const FRow& Row)
    {
        auto* E=Handle.GetEmitterData();
        const FRow Settings=Row->GetObjectField(TEXT("instance"));
        E->bLocalSpace=Settings->GetIntegerField(TEXT("bLocalSpace"))!=0;
        E->bDeterminism=Settings->GetIntegerField(TEXT("bDeterminism"))!=0;
        E->RandomSeed=Settings->GetIntegerField(TEXT("RandomSeed"));
        E->InterpolatedSpawnMode=ENiagaraInterpolatedSpawnMode(Settings->GetIntegerField(TEXT("InterpolatedSpawnMode")));
        E->SimTarget=ENiagaraSimTarget(Settings->GetIntegerField(TEXT("SimTarget")));
        E->CalculateBoundsMode=ENiagaraEmitterCalculateBoundMode(Settings->GetIntegerField(TEXT("CalculateBoundsMode")));
        E->bRequiresPersistentIDs=Settings->GetIntegerField(TEXT("bRequiresPersistentIDs"))!=0;
        E->AllocationMode=EParticleAllocationMode(Settings->GetIntegerField(TEXT("AllocationMode")));
        E->MaxGPUParticlesSpawnPerFrame=Settings->GetIntegerField(TEXT("MaxGPUParticlesSpawnPerFrame"));
        E->PreAllocationCount=Settings->GetIntegerField(TEXT("PreAllocationCount"));
        const FRow Bounds=Settings->GetObjectField(TEXT("FixedBounds"));
        const auto Vector=[&](const TCHAR* Key){
            const auto& V=Bounds->GetArrayField(Key);
            return FVector(V[0]->AsNumber(),V[1]->AsNumber(),V[2]->AsNumber());
        };
        E->FixedBounds=FBox(Vector(TEXT("min")),Vector(TEXT("max")));
        UNiagaraGraph* Graph=CastChecked<UNiagaraScriptSource>(E->GraphSource)->NodeGraph;
        TMap<FString,UNiagaraNodeFunctionCall*> Modules;
        UNiagaraNodeOutput *Spawn=nullptr,*Update=nullptr,*ParticleUpdate=nullptr;
        for (UEdGraphNode* N:Graph->Nodes)
        {
            if (auto* F=Cast<UNiagaraNodeFunctionCall>(N)) Modules.Add(F->GetFunctionName(),F);
            if (auto* O=Cast<UNiagaraNodeOutput>(N))
            {
                if (O->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScript) Spawn=O;
                if (O->GetUsage()==ENiagaraScriptUsage::ParticleUpdateScript) ParticleUpdate=O;
                if (O->GetUsage()==ENiagaraScriptUsage::EmitterUpdateScript) Update=O;
            }
        }
        if (!Spawn || !Update) return false;
        const bool Hit=System.GetName().EndsWith(TEXT("_Hit"));
        const FRow* Birth;
        const bool FitSurface=Hit && Handle.GetName()!=TEXT("LightningSource");
        if (Row->TryGetObjectField(TEXT("birth"),Birth) &&
            !WriteParticleExpressions(*Graph,*Spawn,*Birth,FitSurface?.10f:1.f,TEXT("Electric01 cooked birth"))) return false;
        const FRow* SizeUpdate;
        if (Row->TryGetObjectField(TEXT("size_update"),SizeUpdate) &&
            (!ParticleUpdate || !WriteParticleExpressions(*Graph,*ParticleUpdate,*SizeUpdate,1.f,TEXT("Electric01 cooked size update")))) return false;
        const FRow* ColorUpdate;
        if (Row->TryGetObjectField(TEXT("color_update"),ColorUpdate) &&
            (!ParticleUpdate || !WriteParticleExpressions(*Graph,*ParticleUpdate,*ColorUpdate,1.f,TEXT("Electric01 cooked color update")))) return false;
        if (auto* State=Modules.FindRef(TEXT("EmitterState")))
        {
            double Duration,Delay=0;
            if (Row->TryGetNumberField(TEXT("CurrentLoopDuration"),Duration))
            {
                SetSwitch(*State,TEXT("LifeCycleMode"),TEXT("Self"));
                if (!Number(*State,TEXT("LoopDuration"),Duration)) return false;
                if (Row->TryGetNumberField(TEXT("CurrentLoopDelay"),Delay) && Delay>0)
                {
                    SetSwitch(*State,TEXT("UseLoopDelay"),TEXT("true"));
                    if (!Number(*State,TEXT("LoopDelay"),Delay)) return false;
                }
            }
        }
        for (auto& Entry : Row->GetArrayField(TEXT("spawns")))
        {
            const auto S=Entry->AsObject();
            const auto Fn=S->GetStringField(TEXT("function"));
            auto* Module=Modules.FindRef(Fn);
            if (Fn==TEXT("NM_Codi_SpawnPerUnit"))
            {
                // Older saved copies used the stock script's automatic name and could
                // accumulate a second spacing module when the recipe was reapplied.
                auto* Script=LoadObject<UNiagaraScript>(nullptr,TEXT("/Niagara/Modules/Emitter/SpawnPerUnit.SpawnPerUnit"));
                if (!Script) return false;
                if (!Module) Module=Modules.FindRef(Script->GetName());
                for (const auto& Item:Modules)
                {
                    if (Item.Value==Module || Item.Value->FunctionScript!=Script) continue;
                    FNiagaraExternalEditContext Context(&System);
                    UNiagaraExternalEditUtilities::RemoveModule({&System,Handle.GetName(),TEXT("EmitterUpdateScript"),FName(Item.Key)},Context);
                    if (Context.HasErrors()) return false;
                }
                if (!Module) Module=Stack::AddScriptModuleToStack(Script,*Update);
                if (!Module || !Number(*Module,TEXT("SpawnSpacing"),5.f) ||
                    !UserInput(System,*Module,TEXT("VelocityVector"),TEXT("User.ElectricSurfaceVelocity"),T::GetVec3Def())) return false;
                continue;
            }
            if (!Module) {UE_LOG(LogTemp,Error,TEXT("Missing Electric01 module %s/%s/%s"),*System.GetName(),*Handle.GetName().ToString(),*Fn);return false;}
            double V;
            if (S->TryGetNumberField(TEXT("burst"),V) && !Number(*Module,TEXT("SpawnCount"),V,true)) return false;
            if (S->TryGetNumberField(TEXT("time"),V) && !Number(*Module,TEXT("SpawnTime"),V)) return false;
            if (S->TryGetNumberField(TEXT("group"),V) && (V!=0 || Fn.StartsWith(TEXT("SpawnBurst"))) && !Number(*Module,TEXT("SpawnGroup"),V,true)) return false;
            if (S->TryGetNumberField(TEXT("rate"),V))
            {
                if (!Number(*Module,TEXT("SpawnRate"),V)) return false;
            }
            else if (Hit && Handle.GetName()==TEXT("LightningSource"))
            {
                if (!UserInput(System,*Module,TEXT("SpawnRate"),TEXT("User.ElectricBranchRate"),T::GetFloatDef())) return false;
            }
            Module->MarkNodeRequiresSynchronization(TEXT("Recovered Electric01 emitter inputs"),true);
        }
        return true;
    }

    void Restore()
    {
        FString Text;
        FRow Root;
        if (!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory/Electric01-Branches/Schedules.json"))) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root)) return;
        for (const auto& Pair:Root->Values)
        {
            const FString Name(*Pair.Key);
            const FString Path=TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/")+Name;
            auto* System=LoadObject<UNiagaraSystem>(nullptr,*(Path+TEXT(".")+Name));
            if (!System) return;
            for (auto& H:System->GetEmitterHandles())
                if (!Configure(*System,H,Pair.Value->AsObject()->GetObjectField(H.GetName().ToString())))
                { UE_LOG(LogTemp,Error,TEXT("Electric01 recipe failed %s/%s"),*Name,*H.GetName().ToString());return; }
            System->RequestCompile(true);System->WaitForCompilationComplete(true,false);
            if (!System->IsValid()) {UE_LOG(LogTemp,Error,TEXT("Electric01 compilation failed %s"),*Name);return;}
            System->MarkPackageDirty();
            FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
            if (!UPackage::SavePackage(System->GetOutermost(),System,*FPackageName::LongPackageNameToFilename(Path,FPackageName::GetAssetPackageExtension()),Args)) return;
            UE_LOG(LogTemp,Display,TEXT("ELECTRIC_SCHEDULE_SAVED %s"),*Name);
        }
    }
    FAutoConsoleCommand Cmd(TEXT("RR.RestoreElectricSchedules"),TEXT("Restore Electric01 cooked emitter timing and particle birth values."),FConsoleCommandDelegate::CreateStatic(&Restore));
}
#endif
