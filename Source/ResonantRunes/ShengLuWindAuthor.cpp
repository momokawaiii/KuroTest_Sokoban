#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraDataInterfaceParticleRead.h"
#include "NiagaraDataInterfaceCurlNoise.h"
#include "NiagaraDataInterfaceVectorField.h"
#include "NiagaraDataInterfaceTexture.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

void BindAir02LineSource(UNiagaraSystem& System,FVersionedNiagaraEmitterData& Emitter);
bool BindWaterSpawnRate(UNiagaraNodeFunctionCall&,const TSharedPtr<FJsonObject>&);

namespace
{
    const FNiagaraTypeDefinition& AttributeType(const FString& Name)
    {
        using T=FNiagaraTypeDefinition;
        if (Name==TEXT("Position")) return T::GetPositionDef();
        if (Name.StartsWith(TEXT("DynamicMaterialParameter"))) return T::GetVec4Def();
        if (Name==TEXT("Color") || Name==TEXT("Color2")) return T::GetColorDef();
        if (Name==TEXT("SpriteSize") || Name==TEXT("SpriteSize2")) return T::GetVec2Def();
        if (Name==TEXT("Scale") || Name==TEXT("DecalSize") || Name==TEXT("Velocity") ||
            ((Name.StartsWith(TEXT("Air02Smoke")) || Name.StartsWith(TEXT("Air02Spark"))) && !Name.EndsWith(TEXT("Ready"))) ||
            Name==TEXT("SpriteFacing") || Name==TEXT("SpriteAlignment")) return T::GetVec3Def();
        if (Name==TEXT("MeshOrientation") || Name==TEXT("DecalOrientation")) return T::GetQuatDef();
        if (Name==TEXT("MeshIndex")) return T::GetIntDef();
        return T::GetFloatDef();
    }

    UEdGraphPin& Override(UNiagaraNodeFunctionCall& Node,const FString& Name,const FNiagaraTypeDefinition& Type)
    {
        return FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(Node,
            FNiagaraParameterHandle(FName(Node.GetFunctionName()+TEXT(".")+Name)),Type,FGuid(),FGuid());
    }

    bool ModuleNumber(UNiagaraNodeFunctionCall& Call,const TCHAR* Input,double Value,bool Integer=false)
    {
        for (const auto& Entry : Call.GetCalledGraph()->GetAllMetaData())
        {
            FString Name=Entry.Key.GetName().ToString();
            if (!Name.RemoveFromStart(TEXT("Module.")) || Name.Replace(TEXT(" "),TEXT(""))!=Input) continue;
            auto& Pin=Override(Call,Name,Entry.Key.GetType());
            Pin.DefaultValue=Integer?FString::FromInt(FMath::RoundToInt(Value)):FString::SanitizeFloat(Value);
            return true;
        }
        UE_LOG(LogTemp,Error,TEXT("Missing Air02 Niagara input %s on %s"),Input,*Call.GetFunctionName());
        return false;
    }

    bool SetStateChoice(UNiagaraNodeFunctionCall& Call,const TCHAR* Input,const FString& Choice)
    {
        for (auto* Pin : Call.Pins)
        {
            if (Pin->Direction!=EGPD_Input || !Pin->PinName.ToString().Replace(TEXT(" "),TEXT("")).Equals(Input,ESearchCase::IgnoreCase)) continue;
            if (const auto* Enum=Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
            {
                for (int32 I=0;I<Enum->NumEnums();++I)
                {
                    if (!Enum->GetDisplayNameTextByIndex(I).BuildSourceString().Equals(Choice,ESearchCase::IgnoreCase)) continue;
                    Pin->DefaultValue=Enum->GetNameStringByIndex(I);
                    return true;
                }
            }
            else { Pin->DefaultValue=Choice; return true; }
        }
        UE_LOG(LogTemp,Error,TEXT("Missing Air02 state switch %s=%s"),Input,*Choice);
        return false;
    }

    bool ApplySchedule(UNiagaraNodeFunctionCall& Call,const FJsonObject& Schedule,bool bWater)
    {
        const FString Function=Call.GetFunctionName();
        double Value;
        if (Function==TEXT("EmitterState"))
        {
            if (!SetStateChoice(Call,TEXT("LifeCycleMode"),TEXT("Self")) ||
                !SetStateChoice(Call,TEXT("LoopBehavior"),Schedule.GetStringField(TEXT("loop_behavior"))) ||
                !ModuleNumber(Call,TEXT("LoopDuration"),Schedule.GetNumberField(TEXT("CurrentLoopDuration")))) return false;
            if (Schedule.TryGetNumberField(TEXT("CurrentLoopDelay"),Value) && Value>0.)
                if (!SetStateChoice(Call,TEXT("UseLoopDelay"),TEXT("true")) || !ModuleNumber(Call,TEXT("LoopDelay"),Value)) return false;
            if (bWater && Schedule.TryGetNumberField(TEXT("loop_count"),Value))
                if (!ModuleNumber(Call,TEXT("LoopCount"),Value,true))return false;
        }
        for (const auto& Entry : Schedule.GetArrayField(TEXT("spawns")))
        {
            const auto Spawn=Entry->AsObject();
            if (Function!=Spawn->GetStringField(TEXT("function"))) continue;
            if (Spawn->TryGetNumberField(TEXT("burst"),Value) && !ModuleNumber(Call,TEXT("SpawnCount"),Value,true)) return false;
            if (Spawn->TryGetNumberField(TEXT("time"),Value) && !ModuleNumber(Call,TEXT("SpawnTime"),Value)) return false;
            if (Spawn->TryGetNumberField(TEXT("rate"),Value))
            {
                if (bWater && Function==TEXT("SpawnParticlesFromOtherEmitter"))
                    Override(Call,TEXT("Spawn Rate"),FNiagaraTypeDefinition::GetFloatDef()).DefaultValue=FString::SanitizeFloat(Value);
                else if (!ModuleNumber(Call,TEXT("SpawnRate"),Value)) return false;
            }
            if (Spawn->TryGetNumberField(TEXT("group"),Value) && (Value!=0 || Function.StartsWith(TEXT("SpawnBurst"))))
                if (!ModuleNumber(Call,TEXT("SpawnGroup"),Value,true)) return false;
        }
        Call.MarkNodeRequiresSynchronization(TEXT("Air02 source emitter schedule"),true);
        return true;
    }

    bool BindField(UNiagaraGraph& G,UNiagaraNodeCustomHlsl& Hlsl,const FName Name,UClass* Class)
    {
        const FNiagaraVariable Field(FNiagaraTypeDefinition(Class),Name);
        if (!Hlsl.Signature.Inputs.Contains(Field))
        {
            Hlsl.Signature.Inputs.Add(Field);
            Hlsl.RefreshFromExternalChanges();
        }
        auto* InputPin=Hlsl.FindPin(Name,EGPD_Input);
        if (!InputPin) return false;
        if (!InputPin->LinkedTo.IsEmpty()) return true;
        FGraphNodeCreator<UNiagaraNodeInput> Creator(G);
        auto* Node=Creator.CreateNode();
        Node->Input=FNiagaraVariable(Field.GetType(),FName(TEXT("Air02")+Name.ToString()));
        Node->Usage=ENiagaraInputNodeUsage::Parameter;
        Node->ExposureOptions.bExposed=false;
        auto* Default=NewObject<UNiagaraDataInterface>(Node,Class);
        if (Name==TEXT("AnchorSurface"))
        {
            auto* Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Water/02/Misc/Anchor02/source/anchor"));
            auto* FieldMesh=FindFProperty<FObjectPropertyBase>(Class,TEXT("DefaultMesh"));
            if (!Mesh || !FieldMesh)return false;
            FieldMesh->SetObjectPropertyValue_InContainer(Default,Mesh);
            auto* Mode=FindFProperty<FEnumProperty>(Class,TEXT("SourceMode"));
            if (!Mode)return false;
            Mode->GetUnderlyingProperty()->SetIntPropertyValue(Mode->ContainerPtrToValuePtr<void>(Default),Mode->GetEnum()->GetValueByNameString(TEXT("DefaultMeshOnly")));
        }
        if (auto* Texture=Cast<UNiagaraDataInterfaceTexture>(Default))
        {
            const TCHAR* Tile=Name==TEXT("AnchorBroad")?TEXT("T_CVFX_Tile14"):TEXT("T_CVFX_Tile18");
            auto* Asset=LoadObject<UTexture2D>(nullptr,*(FString(TEXT("/Game/VFX/CVFXKits/Textures/Tile/"))+Tile));
            if (!Asset)return false;
            Texture->SetTexture(Asset);
        }
        if (auto* Vector=Cast<UNiagaraDataInterfaceVectorField>(Default))
        {
            Vector->Field=LoadObject<UVectorField>(nullptr,TEXT("/Niagara/VectorFields/Assets/TilingCurl32.TilingCurl32"));
            if (!Vector->Field) return false;
            Vector->bTileX=Vector->bTileY=Vector->bTileZ=true;
        }
        auto* Property=FindFProperty<FObjectPropertyBase>(Node->GetClass(),TEXT("DataInterface"));
        if (!Property) return false;
        Property->SetObjectPropertyValue_InContainer(Node,Default);
        Creator.Finalize();
        for (auto* Pin : Node->Pins)
            if (Pin->Direction==EGPD_Output) { Pin->MakeLinkTo(InputPin); break; }
        return !InputPin->LinkedTo.IsEmpty();
    }

    // Append to the recovered source stack. The output names and native renderer bindings
    // stay in the emitter, so the effect also previews in the Niagara editor.
    bool AddWindLayers(FVersionedNiagaraEmitterData& E,const TSharedPtr<FJsonObject>& Recipe,const FString& Family=TEXT("Air02"))
    {
        const auto Settings=Recipe->GetObjectField(TEXT("instance"));
        E.bLocalSpace=Settings->GetIntegerField(TEXT("bLocalSpace"))!=0;
        E.bDeterminism=Settings->GetIntegerField(TEXT("bDeterminism"))!=0;
        E.RandomSeed=Settings->GetIntegerField(TEXT("RandomSeed"));
        E.InterpolatedSpawnMode=ENiagaraInterpolatedSpawnMode(Settings->GetIntegerField(TEXT("InterpolatedSpawnMode")));
        E.SimTarget=ENiagaraSimTarget(Settings->GetIntegerField(TEXT("SimTarget")));
        E.CalculateBoundsMode=ENiagaraEmitterCalculateBoundMode(Settings->GetIntegerField(TEXT("CalculateBoundsMode")));
        E.bRequiresPersistentIDs=Settings->GetIntegerField(TEXT("bRequiresPersistentIDs"))!=0;
        E.MaxGPUParticlesSpawnPerFrame=Settings->GetIntegerField(TEXT("MaxGPUParticlesSpawnPerFrame"));
        E.AllocationMode=EParticleAllocationMode(Settings->GetIntegerField(TEXT("AllocationMode")));
        E.PreAllocationCount=Settings->GetIntegerField(TEXT("PreAllocationCount"));
        const auto Box=Settings->GetObjectField(TEXT("FixedBounds"));
        auto Corner=[&](const TCHAR* Key)
        {
            const auto& V=Box->GetArrayField(Key);
            return FVector(V[0]->AsNumber(),V[1]->AsNumber(),V[2]->AsNumber());
        };
        E.FixedBounds=FBox(Corner(TEXT("min")),Corner(TEXT("max")));
        UNiagaraGraph* G=CastChecked<UNiagaraScriptSource>(E.GraphSource)->NodeGraph;
        UNiagaraNodeOutput* Spawn=nullptr;
        UNiagaraNodeOutput* Update=nullptr;
        UNiagaraNodeAssignment* Lifetime=nullptr;
        UNiagaraNodeAssignment* Set=nullptr;
        UNiagaraNodeCustomHlsl* Hlsl=nullptr;
        const auto InitialNodes=G->Nodes;
        for (UEdGraphNode* N : InitialNodes)
        {
            if (auto* O=Cast<UNiagaraNodeOutput>(N))
            {
                if (O->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScript) Spawn=O;
                if (O->GetUsage()==ENiagaraScriptUsage::ParticleUpdateScript) Update=O;
            }
            if (N->NodeComment==Family+TEXT(" phase particle lifetime")) Lifetime=Cast<UNiagaraNodeAssignment>(N);
            if (N->NodeComment==Family+TEXT(" reference sequence")) Set=Cast<UNiagaraNodeAssignment>(N);
            if (N->NodeComment==Family+TEXT(" phase expressions")) Hlsl=Cast<UNiagaraNodeCustomHlsl>(N);
            auto* F=Cast<UNiagaraNodeFunctionCall>(N);
            if (!F) continue;
            bool AlwaysAlive=false;
            if (F->GetFunctionName()==TEXT("ParticleState") && Recipe->TryGetBoolField(TEXT("source_always_alive"),AlwaysAlive) && AlwaysAlive)
                if (!SetStateChoice(*F,TEXT("KillParticlesWhenLifetimeHasElapsed"),TEXT("false"))) return false;
            if (Family==TEXT("Water") && F->GetFunctionName()==TEXT("Collision"))
            {
                // Tile and crate surfaces expose the same query channel used by the player's aim.
                for (const auto& Input:F->GetCalledGraph()->GetAllMetaData())
                    if (Input.Key.GetName()==TEXT("Module.CPU Collision Trace Channel"))
                        Override(*F,TEXT("CPU Collision Trace Channel"),Input.Key.GetType()).DefaultValue=TEXT("ECC_Visibility");
            }
            const TSharedPtr<FJsonObject>* SpawnCurve=nullptr;
            if (F->GetFunctionName()==TEXT("SpawnRate") && Recipe->TryGetObjectField(TEXT("spawn_rate_curve"),SpawnCurve))
                if (!BindWaterSpawnRate(*F,*SpawnCurve))return false;
            FString SourceName;
            bool bInheritDirection=false;
            if (F->GetFunctionName()==TEXT("SampleParticlesFromOtherEmitter") &&
                Recipe->TryGetBoolField(TEXT("inherit_donor_direction"),bInheritDirection) && bInheritDirection)
            {
                if (!SetStateChoice(*F,TEXT("VelocitySampling"),TEXT("Apply to Attribute")) ||
                    !SetStateChoice(*F,TEXT("VelocityApplication"),TEXT("Overwrite")) ||
                    !ModuleNumber(*F,TEXT("AppliedVelocityScale"),1.f)) return false;
            }
            if (F->GetFunctionName()==TEXT("SpawnParticlesFromOtherEmitter") && Recipe->TryGetStringField(TEXT("particle_source"),SourceName))
            {
                const auto Type=FNiagaraTypeDefinition(UNiagaraDataInterfaceParticleRead::StaticClass());
                auto& Pin=Override(*F,TEXT("Attribute Reader"),Type);
                if (Pin.LinkedTo.IsEmpty())
                {
                    UNiagaraDataInterface* DI=nullptr;
                    FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(Pin,Type.GetClass(),TEXT("Source Particles"),DI);
                    auto* Reader=CastChecked<UNiagaraDataInterfaceParticleRead>(DI);
                    Reader->EmitterBinding.BindingMode=ENiagaraDataInterfaceEmitterBindingMode::Other;
                    Reader->EmitterBinding.EmitterName=FName(SourceName);
                }
            }
            if (!ApplySchedule(*F,*Recipe->GetObjectField(TEXT("schedule")),Family==TEXT("Water"))) return false;
        }
        if (!Recipe->HasField(TEXT("hlsl"))) return true;
        if (!Spawn || !Update) return false;
        const FNiagaraVariable Life(FNiagaraTypeDefinition::GetFloatDef(),TEXT("Particles.Lifetime"));
        if (!Lifetime) Lifetime=FNiagaraStackGraphUtilities::AddParameterModuleToStack({Life},*Spawn,INDEX_NONE,{FString()});
        if (!Lifetime) return false;
        Lifetime->NodeComment=Family+TEXT(" phase particle lifetime");
        Override(*Lifetime,Life.GetName().ToString(),Life.GetType()).DefaultValue=FString::SanitizeFloat(Recipe->GetNumberField(TEXT("life")));
        if (Family==TEXT("Water") && Recipe->GetStringField(TEXT("hlsl")).Contains(TEXT("Particles.SpawnGroup")))
        {
            const FNiagaraVariable Group(FNiagaraTypeDefinition::GetIntDef(),TEXT("Particles.SpawnGroup"));
            if (Lifetime->FindAssignmentTarget(Group.GetName(),Group.GetType())==INDEX_NONE)
            {
                Lifetime->AddAssignmentTarget(Group);
                Lifetime->RefreshFromExternalChanges();
            }
            auto& Pin=Override(*Lifetime,Group.GetName().ToString(),Group.GetType());
            if (Pin.LinkedTo.IsEmpty())
            {
                const FNiagaraVariable SourceGroup(Group.GetType(),TEXT("Emitter.SpawnGroup"));
                FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Pin,SourceGroup,{SourceGroup});
            }
        }

        TArray<FString> Attrs,Defaults;
        Recipe->TryGetStringArrayField(TEXT("attrs"),Attrs);
        if (Attrs.Contains(TEXT("Air02SmokeReady")) || Attrs.Contains(TEXT("Air02SparkReady")) || Attrs.Contains(TEXT("WaterStrandReady")) || Attrs.Contains(TEXT("WaterFlightReady")))
        {
            for (const FString& A : Attrs)
            {
                if (!A.StartsWith(TEXT("Air02Smoke")) && !A.StartsWith(TEXT("Air02Spark")) && A!=TEXT("WaterStrandReady") && A!=TEXT("WaterFlightReady")) continue;
                const FNiagaraVariable State(AttributeType(A),FName(TEXT("Particles.")+A));
                if (Lifetime->FindAssignmentTarget(State.GetName(),State.GetType())==INDEX_NONE)
                {
                    Lifetime->AddAssignmentTarget(State);
                    Lifetime->RefreshFromExternalChanges();
                }
                Override(*Lifetime,State.GetName().ToString(),State.GetType()).DefaultValue=
                    A.EndsWith(TEXT("Ready"))?TEXT("0.0"):TEXT("(X=0,Y=0,Z=0)");
            }
        }
        TArray<FNiagaraVariable> Vars;
        for (auto& A : Attrs) { Vars.Emplace(AttributeType(A),FName(TEXT("Particles.")+A)); Defaults.Add(FString()); }
        if (!Set) Set=FNiagaraStackGraphUtilities::AddParameterModuleToStack(Vars,*Update,INDEX_NONE,Defaults);
        if (!Set) return false;
        bool AddedTarget=false;
        for (const auto& Var : Vars)
        {
            if (Set->FindAssignmentTarget(Var.GetName(),Var.GetType())!=INDEX_NONE) continue;
            Set->AddAssignmentTarget(Var);
            AddedTarget=true;
        }
        if (AddedTarget) Set->RefreshFromExternalChanges();
        Set->NodeComment=Family+TEXT(" reference sequence");
        if (!Hlsl)
        {
            FGraphNodeCreator<UNiagaraNodeCustomHlsl> Creator(*G);
            Hlsl=Creator.CreateNode();
            Hlsl->Signature.Inputs.Emplace(FNiagaraTypeDefinition::GetParameterMapDef(),TEXT("Map"));
            for (const auto& A : Attrs) Hlsl->Signature.Outputs.Emplace(AttributeType(A),FName(TEXT("Out")+A));
            Hlsl->NodeComment=Family+TEXT(" phase expressions");
            Creator.Finalize();
        }
        else
        {
            bool AddedOutput=false;
            for (const auto& A : Attrs)
            {
                const FNiagaraVariable Var(AttributeType(A),FName(TEXT("Out")+A));
                if (Hlsl->Signature.Outputs.Contains(Var)) continue;
                Hlsl->Signature.Outputs.Add(Var);
                AddedOutput=true;
            }
            if (AddedOutput) Hlsl->RefreshFromExternalChanges();
        }
        FString Code=Recipe->GetStringField(TEXT("hlsl"));
        if (Code.Contains(TEXT("AnchorSurface.GetTriangle")))
        {
            auto* Class=LoadClass<UNiagaraDataInterface>(nullptr,TEXT("/Script/Niagara.NiagaraDataInterfaceStaticMesh"));
            if (!Class || !BindField(*G,*Hlsl,TEXT("AnchorSurface"),Class) ||
                !BindField(*G,*Hlsl,TEXT("AnchorBroad"),UNiagaraDataInterfaceTexture::StaticClass()) ||
                !BindField(*G,*Hlsl,TEXT("AnchorFine"),UNiagaraDataInterfaceTexture::StaticClass())) return false;
        }
        if (!E.bLocalSpace && Attrs.Contains(TEXT("Position")) && !Attrs.Contains(TEXT("WaterFlightReady")))
            Code+=TEXT("\nOutPosition+=Engine.Owner.Position;");
        if (Code.Contains(TEXT("CurlField.SampleNoiseField")) && !BindField(*G,*Hlsl,TEXT("CurlField"),UNiagaraDataInterfaceCurlNoise::StaticClass())) return false;
        if (Code.Contains(TEXT("WindField.SampleField")) && !BindField(*G,*Hlsl,TEXT("WindField"),UNiagaraDataInterfaceVectorField::StaticClass())) return false;
        FindFProperty<FStrProperty>(Hlsl->GetClass(),TEXT("CustomHlsl"))->SetPropertyValue_InContainer(Hlsl,Code);
        for (int32 I=0;I<Attrs.Num();++I)
        {
            auto& Pin=Override(*Set,Vars[I].GetName().ToString(),Vars[I].GetType());
            auto* Output=Hlsl->FindPin(FName(TEXT("Out")+Attrs[I]),EGPD_Output);
            auto* Map=Hlsl->FindPin(TEXT("Map"),EGPD_Input);
            if (!Output || !Map) return false;
            if (Map->LinkedTo.IsEmpty())
            {
                for (auto* P : Pin.GetOwningNode()->Pins)
                    if (P->Direction==EGPD_Input && P->PinType.PinSubCategoryObject==FNiagaraTypeDefinition::GetParameterMapDef().GetStruct() && P->LinkedTo.Num())
                    { P->LinkedTo[0]->MakeLinkTo(Map); break; }
                if (Map->LinkedTo.IsEmpty()) return false;
            }
            if (!Pin.LinkedTo.Contains(Output)) { Pin.BreakAllPinLinks(); Output->MakeLinkTo(&Pin); }
        }
        Hlsl->MarkNodeRequiresSynchronization(Family+TEXT(" particle expressions"),true);
        return true;
    }

    bool RestoreWindSheetRenderer(FVersionedNiagaraEmitterData& Emitter)
    {
        const auto& Renderers=Emitter.GetRenderers();
        auto* Mesh=Renderers.Num()==1?Cast<UNiagaraMeshRendererProperties>(Renderers[0]):nullptr;
        if (!Mesh) return false;

        // Minimal003 and Minimal012 already face the camera in MeshOrientation.
        // The cooked renderer uses Default, so the vertex factory must not apply another look-at matrix.
        Mesh->FacingMode=ENiagaraMeshFacingMode::Default;
        Mesh->bLockedAxisEnable=false;

        auto* Graph=CastChecked<UNiagaraScriptSource>(Emitter.GraphSource)->NodeGraph.Get();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->NodeComment==TEXT("Air02 wind sheet orientation"))
                FNiagaraStackGraphUtilities::SetModuleIsEnabled(*CastChecked<UNiagaraNodeAssignment>(Node),false);
        }
        return true;
    }

    void AuthorWind()
    {
        FString Text;
        TSharedPtr<FJsonObject> All;
        if (!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/WindRecipe.json"))) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),All)) return;
        for (const TCHAR* Name : {TEXT("Range"),TEXT("Tornado")})
        {
            const bool bTornado=FCString::Strcmp(Name,TEXT("Tornado"))==0;
            if (!bTornado && FParse::Param(FCommandLine::Get(),TEXT("Air02TornadoOnly"))) continue;
            FString Path=FString(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Air/02/Niagara/NS_Player_Skill_Air02_"))+Name;
            auto* Sys=LoadObject<UNiagaraSystem>(nullptr,*(Path+TEXT(".")+FPaths::GetCleanFilename(Path)));
            if (!Sys) return;
            // Both source systems leave system-level fixed bounds disabled.
            Sys->bFixedBounds=false;
            const auto& Rows=All->GetObjectField(Name);
            Sys->GetExposedParameters().SetParameterValue(FVector4f(-1,1,1,1),
                FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(),TEXT("User.Air02Phase")),true);
            Sys->GetExposedParameters().SetParameterValue(FNiagaraBool(true),
                FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(),TEXT("User.IsActive")),true);
            if (FCString::Strcmp(Name,TEXT("Range"))==0)
                Sys->GetExposedParameters().SetParameterValue(FNiagaraBool(true),
                    FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(),TEXT("User.StartCharge")),true);
            for (FName Parameter : {FName(TEXT("User.Air02FadeAge")),FName(TEXT("User.Air02ViewYaw"))})
                Sys->GetExposedParameters().SetParameterValue(0.f,FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(),Parameter),true);
            Sys->GetExposedParameters().SetParameterValue(0.f,
                FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(),TEXT("User.Air02AgeOffset")),true);
            Sys->GetExposedParameters().SetPositionParameterValue(FVector::ZeroVector,TEXT("User.Air02CameraWorld"),true);
            Sys->GetExposedParameters().RemoveParameter(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.Air02Screen")));
            int32 Count=0;
            for (auto& H : Sys->GetEmitterHandles())
            {
                if (!H.GetIsEnabled()) continue;
                if (bTornado && H.GetName()==TEXT("Minimal011"))
                    BindAir02LineSource(*Sys,*H.GetEmitterData());
                if (bTornado && (H.GetName()==TEXT("Minimal003") || H.GetName()==TEXT("Minimal012")) &&
                    !RestoreWindSheetRenderer(*H.GetEmitterData()))
                {
                    UE_LOG(LogTemp,Error,TEXT("Air02 sheet renderer restore failed for %s"),*H.GetName().ToString());
                    return;
                }
                const TSharedPtr<FJsonObject>* Row=nullptr;
                if (!Rows->TryGetObjectField(H.GetName().ToString(),Row)) continue;
                if (!AddWindLayers(*H.GetEmitterData(),*Row)) {UE_LOG(LogTemp,Error,TEXT("Air02 author failed at %s"),*H.GetName().ToString()); return;}
                ++Count;
            }
            Sys->RequestCompile(true);
            Sys->WaitForCompilationComplete(true,false);
            if (Count!=Rows->Values.Num() || !Sys->IsValid())
            { UE_LOG(LogTemp,Error,TEXT("Air02 compilation failed for %s (%d layers)"),Name,Count); return; }
            for (auto& H : Sys->GetEmitterHandles())
                if (H.GetIsEnabled())
                    for (auto* S : {H.GetEmitterData()->SpawnScriptProps.Script.Get(),H.GetEmitterData()->UpdateScriptProps.Script.Get()})
                        if (S && S->GetLastCompileStatus()==ENiagaraScriptCompileStatus::NCS_Error)
                        { UE_LOG(LogTemp,Error,TEXT("Air02 compiled script error %s %s"),Name,*H.GetName().ToString());return; }
            TArray<UNiagaraScript*> Scripts={Sys->GetSystemSpawnScript(),Sys->GetSystemUpdateScript()};
            for (auto& H : Sys->GetEmitterHandles())
                if (H.GetIsEnabled()) H.GetEmitterData()->GetScripts(Scripts,false);
            for (auto* Script : Scripts)
                if (Script) for (const auto& Def : Script->GetCachedDefaultDataInterfaces())
                    if (const auto* DI=Cast<UNiagaraDataInterfaceParticleRead>(Def.DataInterface); DI && DI->EmitterBinding.EmitterName.IsNone())
                        UE_LOG(LogTemp,Display,TEXT("AIR02_DEFAULT_READER %s %s read=%s write=%s"),*Script->GetPathName(),*Def.Name.ToString(),*Def.RegisteredParameterMapRead.ToString(),*Def.RegisteredParameterMapWrite.ToString());
            Sys->MarkPackageDirty();
            FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
            FString Filename=FPackageName::LongPackageNameToFilename(Path,FPackageName::GetAssetPackageExtension());
            if (FParse::Param(FCommandLine::Get(),TEXT("Air02StagePackages")))
            {
                const FString Directory=FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Air02Packages");
                IFileManager::Get().MakeDirectory(*Directory,true);
                Filename=Directory/FPaths::GetCleanFilename(Filename);
            }
            if (!UPackage::SavePackage(Sys->GetOutermost(),Sys,*Filename,Args)) return;
            UE_LOG(LogTemp,Display,TEXT("AIR02_PHASE_SAVED %s %d"),Name,Count);
        }
    }
    FAutoConsoleCommand WindCommand(TEXT("RR.AuthorWindSequence"),TEXT("Restore Air02 per-emitter staging."),FConsoleCommandDelegate::CreateStatic(&AuthorWind));
}
bool ApplyWaterEmitterRecipe(FVersionedNiagaraEmitterData& E,const TSharedPtr<FJsonObject>& Recipe)
{
    return AddWindLayers(E,Recipe,TEXT("Water"));
}
#endif
