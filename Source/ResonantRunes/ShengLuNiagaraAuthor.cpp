#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "HAL/IConsoleManager.h"

namespace
{
    UEdGraphPin& Input(UNiagaraNodeFunctionCall& Node,const TCHAR* Name,const FNiagaraTypeDefinition& Type)
    {
        const FName Aliased(*(Node.GetFunctionName()+TEXT(".")+Name));
        return FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
            Node,FNiagaraParameterHandle(Aliased),Type,FGuid(),FGuid());
    }

    void BindNatureInputs()
    {
        const FString Root=TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/");
        for (const FString& Rel : {FString(TEXT("Air/02/Niagara/NS_Player_Skill_Air02_Tornado")),
                                   FString(TEXT("Electric/01/Niagara/NS_Player_Skill_Electric01_Trail")),
                                   FString(TEXT("Electric/01/Niagara/NS_Player_Skill_Electric01_Hit"))})
        {
            const FString Path=Root+Rel;
            UNiagaraSystem* System=LoadObject<UNiagaraSystem>(nullptr,*(Path+TEXT(".")+FPaths::GetCleanFilename(Path)));
            if (!System) return;
            bool Changed=false;
            for (auto& Handle : System->GetEmitterHandles())
            {
                const FName Name=Handle.GetName();
                auto* Data=Handle.GetEmitterData();
                if (!Data) continue;
                if (Rel.StartsWith(TEXT("Air/")) && (Name==TEXT("Minimal003") || Name==TEXT("Minimal008")))
                {
                    Data->bLocalSpace=true;
                    Changed=true;
                    continue;
                }
                if (Rel.EndsWith(TEXT("_Hit")) && Name==TEXT("NE_BasicEmitter_SimpleFlare002"))
                {
                    Data->bLocalSpace=true;
                    Changed=true;
                    continue;
                }
                if (Name!=TEXT("Lightning")) continue;
                auto* Source=CastChecked<UNiagaraScriptSource>(Data->GraphSource);
                TArray<UNiagaraNodeFunctionCall*> Nodes;
                Source->NodeGraph->GetNodesOfClass(Nodes);
                for (auto* Node : Nodes)
                {
                    if (Node->GetFunctionName()!=TEXT("BeamEmitterSetup")) continue;
                    for (bool End : {false,true})
                    {
                        const auto& Type=FNiagaraTypeDefinition::GetPositionDef();
                        FNiagaraVariable Var(Type, End ? TEXT("User.TargetPosition") : TEXT("User.CenterPosition"));
                        System->GetExposedParameters().AddParameter(Var);
                        auto& Pin=Input(*Node,End?TEXT("Beam End"):TEXT("Beam Start"),Type);
                        if (!Pin.LinkedTo.Num())
                            FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Pin,Var,{});
                        Input(*Node,End?TEXT("Absolute Beam End"):TEXT("Absolute Beam Start"),
                              FNiagaraTypeDefinition::GetBoolDef()).DefaultValue=TEXT("true");
                    }
                    Changed=true;
                }
            }
            if (!Changed) { UE_LOG(LogTemp, Error,TEXT("NatureTheory graph binding missing %s"),*Path); return; }
            System->RequestCompile(true);
            System->WaitForCompilationComplete(true,false);
            if (!System->IsValid()) { UE_LOG(LogTemp,Error,TEXT("NatureTheory compile failed %s"),*Path); return; }
            System->MarkPackageDirty();
            FSavePackageArgs Args;
            Args.TopLevelFlags=RF_Public|RF_Standalone;
            const bool Saved=UPackage::SavePackage(System->GetOutermost(), System,
                *FPackageName::LongPackageNameToFilename(Path,FPackageName::GetAssetPackageExtension()), Args);
            UE_LOG(LogTemp,Display,TEXT("NatureTheory graph binding saved=%d %s"),Saved,*Path);
        }
    }
    FAutoConsoleCommand Command(TEXT("RR.BindNatureInputs"),TEXT("Bind the ShengLu board to the recovered Niagara main layers."),
                                FConsoleCommandDelegate::CreateStatic(&BindNatureInputs));
}
#endif
