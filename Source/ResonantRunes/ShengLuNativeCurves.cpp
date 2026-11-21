#if WITH_EDITOR
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "Engine/CurveTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
using FEntry=TSharedPtr<FJsonObject>;
namespace FStack=FNiagaraStackGraphUtilities;

bool SaveCurvesObject(UObject* Obj)
{
    UPackage* Package=Obj->GetOutermost();
    Package->MarkPackageDirty();
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Package,Obj,*FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}

FRichCurve ReadChannel(const FEntry& F,int32 Channels,int32 Channel)
{
    const auto& Samples=F->GetArrayField(TEXT("ShaderLUT"));
    double Min=0,Max=1;
    F->TryGetNumberField(TEXT("LUTMinTime"),Min);
    F->TryGetNumberField(TEXT("LUTMaxTime"),Max);
    const int32 Count=Samples.Num()/Channels;
    FRichCurve Result;
    for(int32 I=0;I<Count;++I)
    {
        const float T=Count>1?FMath::Lerp(Min,Max,double(I)/(Count-1)):Min;
        auto K=Result.AddKey(T,Samples[I*Channels+Channel]->AsNumber());
        Result.SetKeyInterpMode(K,RCIM_Linear);
    }
    return Result;
}

UEdGraphPin& CurvePin(UNiagaraNodeFunctionCall& Node,const FString& Name,const FNiagaraTypeDefinition& Type)
{
    return FStack::GetOrCreateStackFunctionInputOverridePin(Node,FNiagaraParameterHandle(FName(Node.GetFunctionName()+TEXT(".")+Name)),Type,FGuid(),FGuid());
}

UNiagaraDataInterfaceCurveBase* CurveInput(UNiagaraNodeFunctionCall& Node,const FString& Name,UClass* Class)
{
    auto& Pin=CurvePin(Node,Name,FNiagaraTypeDefinition(Class));
    if (!Pin.LinkedTo.IsEmpty())
    {
        if (auto* N=Cast<UNiagaraNodeInput>(Pin.LinkedTo[0]->GetOwningNode()))
            if (auto* P=FindFProperty<FObjectPropertyBase>(N->GetClass(),TEXT("DataInterface")))
                return Cast<UNiagaraDataInterfaceCurveBase>(P->GetObjectPropertyValue_InContainer(N));
        return nullptr;
    }
    UNiagaraDataInterface* DI=nullptr;
    FStack::SetDataInterfaceValueForFunctionInput(Pin,Class,Name,DI);
    return Cast<UNiagaraDataInterfaceCurveBase>(DI);
}

UNiagaraNodeFunctionCall* ColorReader(UNiagaraNodeOutput& Update)
{
    UNiagaraNodeAssignment* Set=nullptr;
    for (UEdGraphNode* Node:Update.GetGraph()->Nodes)
        if (Node->NodeComment==TEXT("NatureTheory particle color LUT")) Set=Cast<UNiagaraNodeAssignment>(Node);
    if(!Set)
    {
        FNiagaraVariable Color(FNiagaraTypeDefinition::GetColorDef(),TEXT("Particles.Color"));
        Set=FStack::AddParameterModuleToStack({Color},Update,INDEX_NONE,{FString()});
        if (!Set) return nullptr;
        Set->NodeComment=TEXT("NatureTheory particle color LUT");
    }
    auto& Pin=CurvePin(*Set,TEXT("Particles.Color"),FNiagaraTypeDefinition::GetColorDef());
    if (!Pin.LinkedTo.IsEmpty()) return Cast<UNiagaraNodeFunctionCall>(Pin.LinkedTo[0]->GetOwningNode());
    auto* Script=LoadObject<UNiagaraScript>(nullptr,TEXT("/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"));
    if (!Script) return nullptr;
    UNiagaraNodeFunctionCall* Call=nullptr;
    FStack::SetDynamicInputForFunctionInput(Pin,Script,Call);
    return Call;
}

bool AttachCurve(UNiagaraGraph* G,const FString& Binding,const TArray<FRichCurve>& Curves)
{
    TArray<FString> Parts;
    Binding.ParseIntoArray(Parts,TEXT("."));
    if (Parts.Num()<4) return false;
    const auto& Function=Parts[2];
    const auto& Input=Parts[3];
    const bool bColor=Curves.Num()==4;
    if(!bColor && Function!=TEXT("ScaleSpriteSize") && Function!=TEXT("ScaleSpriteSizeBySpeed")) return false;
    UNiagaraNodeOutput* Update=nullptr;
    UNiagaraNodeFunctionCall* Call=nullptr;
    for(UEdGraphNode* Node:G->Nodes)
    {
        if(auto* Out=Cast<UNiagaraNodeOutput>(Node);Out && Out->GetUsage()==ENiagaraScriptUsage::ParticleUpdateScript) Update=Out;
        if(auto* Func=Cast<UNiagaraNodeFunctionCall>(Node);Func && Func->GetFunctionName()==Function) Call=Func;
    }
    if(!Update)return false;
    if(bColor)
    {
        Call=ColorReader(*Update);
        if(!Call) return false;
        auto* DI=Cast<UNiagaraDataInterfaceColorCurve>(CurveInput(*Call,TEXT("ColorCurve"),UNiagaraDataInterfaceColorCurve::StaticClass()));
        if(!DI)return false;
        DI->RedCurve=Curves[0];DI->GreenCurve=Curves[1];DI->BlueCurve=Curves[2];DI->AlphaCurve=Curves[3];
        DI->UpdateLUT();
        return true;
    }
    if(!Call)
    {
        FString P=TEXT("/Niagara/Modules/Update/Size/")+Function+TEXT(".")+Function;
        if(auto* Script=LoadObject<UNiagaraScript>(nullptr,*P)) Call=FStack::AddScriptModuleToStack(Script,*Update);
    }
    if(!Call)return false;
    auto* DI=Cast<UNiagaraDataInterfaceCurve>(CurveInput(*Call,Input,UNiagaraDataInterfaceCurve::StaticClass()));
    if(!DI)return false;
    DI->Curve=Curves[0];DI->UpdateLUT();
    return true;
}

// The board brings the beam up to a box within one cell. Size the camera-near
// particles in centimeters, preserving the recovered normalized-time scale curves.
void FitElectricSprites(UNiagaraSystem& System)
{
    const bool Impact=System.GetName().EndsWith(TEXT("_Hit"));
    for(auto& H:System.GetEmitterHandles())
    {
        auto* E=H.GetEmitterData();
        if(!E)continue;
        bool HasSprite=false;
        for(auto* R:E->GetRenderers())
            if(R && R->GetClass()->GetFName()==TEXT("NiagaraSpriteRendererProperties")) HasSprite=true;
        if(!HasSprite)continue;
        const FString Name=H.GetName().ToString();
        float Span=Impact?14.f:3.f;
        if(Name.Contains(TEXT("Shockwave")) || Name.Contains(TEXT("Ring"))) Span=Impact?10.f:2.6f;
        else if(Name.Contains(TEXT("Flare"))) Span=Impact?23.f:6.f;
        else if(Name.Contains(TEXT("Spark")) || Name.Contains(TEXT("ArmParticle"))) Span=Impact?2.f:.6f;
        else if(Name.Contains(TEXT("Smoke"))) Span=25.f;
        UNiagaraGraph* G=CastChecked<UNiagaraScriptSource>(E->GraphSource)->NodeGraph;
        UNiagaraNodeOutput* Spawn=nullptr;
        UNiagaraNodeAssignment* Set=nullptr;
        for(UEdGraphNode* N:G->Nodes)
        {
            if(auto* O=Cast<UNiagaraNodeOutput>(N);O && O->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScript) Spawn=O;
            if(N->NodeComment==TEXT("ShengLu impact size") || N->NodeComment==TEXT("ShengLu first-person sprite scale")) Set=Cast<UNiagaraNodeAssignment>(N);
        }
        if(!Spawn)continue;
        if(!Set)Set=FStack::AddParameterModuleToStack({FNiagaraVariable(FNiagaraTypeDefinition::GetVec2Def(),TEXT("Particles.SpriteSize"))},*Spawn,INDEX_NONE,{FString()});
        if(!Set)continue;
        Set->NodeComment=TEXT("ShengLu first-person sprite scale");
        CurvePin(*Set,TEXT("Particles.SpriteSize"),FNiagaraTypeDefinition::GetVec2Def()).DefaultValue=FString::Printf(TEXT("(X=%f,Y=%f)"),Span,Span);
    }
}
void RecoverNativeCurves()
{
    FString Text;
    TArray<TSharedPtr<FJsonValue>> Entries;
    if(!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory/NativeCurves.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Entries)) return;
    TMap<FString,TArray<FEntry>> BySystem;
    for(auto& V:Entries) BySystem.FindOrAdd(V->AsObject()->GetStringField(TEXT("system"))).Add(V->AsObject());
    int32 Imported=0,Attached=0;
    for(auto& Pair:BySystem)
    {
        const FString P=Pair.Key;
        auto* S=LoadObject<UNiagaraSystem>(nullptr,*(P+TEXT(".")+FPaths::GetCleanFilename(P)));
        if(!S) return;
        const FString Name=FPaths::GetCleanFilename(P)+TEXT("_BakedCurves");
        const FString TablePath=FPaths::GetPath(P)+TEXT("/")+Name;
        UPackage* Package=CreatePackage(*TablePath);
        auto* Table=FindObject<UCurveTable>(Package,*Name);
        if(!Table && FPackageName::DoesPackageExist(TablePath)) Table=LoadObject<UCurveTable>(nullptr,*(TablePath+TEXT(".")+Name));
        if(!Table)
        {
            Table=NewObject<UCurveTable>(Package,*Name,RF_Public|RF_Standalone|RF_Transactional);
            FAssetRegistryModule::AssetCreated(Table);
        }
        Table->EmptyTable();
        TMap<FString,UNiagaraGraph*> Graphs;
        for(auto& H:S->GetEmitterHandles())
            if(auto* E=H.GetEmitterData()) Graphs.Add(H.GetName().ToString(),CastChecked<UNiagaraScriptSource>(E->GraphSource)->NodeGraph);
        TSet<FString> Bound;
        for(auto& Row:Pair.Value)
        {
            const FString Binding=Row->GetArrayField(TEXT("bindings"))[0]->AsString();
            const FString Emitter=Row->GetStringField(TEXT("emitter"));
            const FString Script=Row->GetStringField(TEXT("script"));
            const FString Key=FString::Printf(TEXT("%s.%s.%d"),*Binding,*Script,int32(Row->GetNumberField(TEXT("source_export"))));
            const int32 Channels=Row->GetStringField(TEXT("cls")).Contains(TEXT("ColorCurve"))?4:1;
            TArray<FRichCurve> Curves;
            for(int32 C=0;C<Channels;++C)
            {
                Curves.Add(ReadChannel(Row->GetObjectField(TEXT("fields")),Channels,C));
                const FString Suffix=Channels==1?FString():FString::Printf(TEXT(".%c"),TEXT("RGBA")[C]);
                Table->AddRichCurve(FName(Key+Suffix))=Curves.Last();
            }
            ++Imported;
            if(!Bound.Contains(Binding))
                if(auto* G=Graphs.FindRef(Emitter);G && AttachCurve(G,Binding,Curves)) { Bound.Add(Binding);++Attached; }
        }
        if(P.Contains(TEXT("/Electric/")))FitElectricSprites(*S);
        S->RequestCompile(true);S->WaitForCompilationComplete(true,false);
        if(!S->IsValid() || !SaveCurvesObject(Table) || !SaveCurvesObject(S)) { UE_LOG(LogTemp,Error,TEXT("Failed saving curves %s"),*P);return; }
    }
    UE_LOG(LogTemp,Display,TEXT("NATURE_NATIVE_CURVES imported=%d graph_inputs=%d"),Imported,Attached);
}
FAutoConsoleCommand Cmd(TEXT("RR.RecoverNativeCurves"),TEXT("Import original LUT samples and restore Niagara color and size inputs."),FConsoleCommandDelegate::CreateStatic(&RecoverNativeCurves));
}
bool BindWaterSpawnRate(UNiagaraNodeFunctionCall& Spawn,const TSharedPtr<FJsonObject>& Samples)
{
    const auto Float=FNiagaraTypeDefinition::GetFloatDef();
    auto& Pin=CurvePin(Spawn,TEXT("SpawnRate"),Float);
    if (auto* Old=Pin.GetOwningNode()->FindPin(FName(Spawn.GetFunctionName()+TEXT(".Spawn Rate")),EGPD_Input);
        Old && Old->LinkedTo.Num()==1 && Pin.LinkedTo.IsEmpty())
    {
        auto* Driver=Old->LinkedTo[0];
        Old->BreakAllPinLinks();Driver->MakeLinkTo(&Pin);
        Old->GetOwningNode()->RemovePin(Old);
    }
    UNiagaraNodeFunctionCall* Fn=nullptr;
    if (Pin.LinkedTo.Num()) Fn=Cast<UNiagaraNodeFunctionCall>(Pin.LinkedTo[0]->GetOwningNode());
    else
    {
        auto* Script=LoadObject<UNiagaraScript>(nullptr,TEXT("/Niagara/DynamicInputs/ValueFromCurve/FloatFromCurve.FloatFromCurve"));
        if (!Script)return false;
        FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(Pin,Script,Fn);
    }
    if (!Fn)return false;
    auto& Time=CurvePin(*Fn,TEXT("CurveIndex"),Float);
    if (Time.LinkedTo.IsEmpty()) FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Time,FNiagaraVariable(Float,TEXT("Emitter.NormalizedLoopAge")),{});
    auto* DI=Cast<UNiagaraDataInterfaceCurve>(CurveInput(*Fn,TEXT("FloatCurve"),UNiagaraDataInterfaceCurve::StaticClass()));
    if (!DI)return false;
    DI->Curve=ReadChannel(Samples,1,0);
    DI->UpdateLUT();
    return true;
}
#endif
