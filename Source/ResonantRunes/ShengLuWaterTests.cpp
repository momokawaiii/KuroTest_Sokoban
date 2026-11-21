#if WITH_DEV_AUTOMATION_TESTS
#include "ShengLuGame.h"
#include "Algo/Count.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraSimCache.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterSourceHandles,"ResonantRunes.ShengLu.Water.Assets",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FWaterSourceHandles::RunTest(const FString&)
{
    FString Text;TSharedPtr<FJsonObject> Json;
    if (!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectDir()/TEXT("Artifacts/NatureTheory/Water/Dependencies.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)) return false;
    int32 Count=0;
    for (const auto& Pair:Json->GetObjectField(TEXT("systems"))->Values)
    {
        auto Row=Pair.Value->AsObject();
        auto* Sys=LoadObject<UNiagaraSystem>(nullptr,*Row->GetStringField(TEXT("path")));
        if (!TestNotNull(FString(Pair.Key),Sys)) continue;
#if WITH_EDITOR
        Sys->WaitForCompilationComplete(true,false);
#endif
        TestTrue(FString(Pair.Key)+TEXT(" compiled"),Sys->IsValid());
        const auto& Handles=Sys->GetEmitterHandles();
        int Index=0;
        for (const auto& Value:Row->GetArrayField(TEXT("emitters")))
        {
            auto E=Value->AsObject();
            if (!E->GetBoolField(TEXT("enabled"))) continue;
            if (!TestTrue(TEXT("handle present"),Handles.IsValidIndex(Index))) break;
            const auto& H=Handles[Index++];
            TestEqual(FString(Pair.Key),H.GetName().ToString(),E->GetStringField(TEXT("name")));
            TestTrue(TEXT("enabled emitter instance"),H.GetIsEnabled() && H.GetEmitterData());
            if (H.GetName()==TEXT("Minimal") && (Sys->GetName().EndsWith(TEXT("Water01_Charge")) || Sys->GetName().EndsWith(TEXT("Water01_Holding"))))
            {
                bool HandSurface=false;
                for (auto* R:H.GetEmitterData()->GetRenderers())
                    if (const auto* Mesh=Cast<UNiagaraMeshRendererProperties>(R))
                        for (const auto& Override:Mesh->OverrideMaterials)
                            HandSurface |= Override.ExplicitMat && Override.ExplicitMat->GetName()==TEXT("M_Hand_HoldCore");
                TestTrue(TEXT("Hand core reloads its optical material"),HandSurface);
            }
            ++Count;
        }
        TestEqual(TEXT("no extra handles"),Index,Handles.Num());
    }
    TestEqual(TEXT("active source handles"),Count,78);
    return true;
}

namespace
{
class FWaterCast : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AShengLuBoard> Board;
    int Step=0; double Next=0;
    int SampleTick=0;
    double EquipTime=0;
    double ThrowTime=-1, ImpactTime=-1;
    int32 FlightFrame=0;
    int32 AnchorFrame=0;
    int32 OrbCards=0, ContactCards=0;
    double WidestCardRadius=0;
    double AnchorStart=-1;
    int32 HandFrame=0;
    TArray<FString> StrandSamples{TEXT("age,system,emitter,x_px,y_px,along_to_core")};
    TArray<FString> OpticalSamples{TEXT("age,system,emitter,live,group0,group1,alpha,wpo_strength,wpo_clock")};
    TArray<FString> Samples{TEXT("time,step,system,emitter,state,particles,spawned")};
    void Sample(AShengLuBoard& B,double Now)
    {
        TInlineComponentArray<UNiagaraComponent*> Components(&B);
        for (auto* FX:Components)
        {
            if (!FX->IsActive() || !FX->GetName().StartsWith(TEXT("Water"))) continue;
            auto Controller=FX->GetSystemInstanceController();
            if (!Controller) continue;
            Controller->WaitForConcurrentTickAndFinalize();
            auto* Instance=Controller->GetSystemInstance_Unsafe();
            if (!Instance) continue;
            auto* Cache=NewObject<UNiagaraSimCache>();
            const bool Valid=Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),FX) && Cache->WriteFrame(FX) && Cache->EndWrite();
            TMap<FName,int32> Counts;
            if (Valid && Cache->GetNumFrames())
                for (int I=0;I<Cache->GetNumEmitters();++I)
                {
                    const FName Emitter=Cache->GetEmitterName(I);
                    Counts.Add(Emitter,Cache->GetEmitterNumInstances(I,0));
                    if (Emitter=="Splash003" && FParse::Param(FCommandLine::Get(),TEXT("RuneWaterAnchorGallery")))
                    {
                        if (FX->GetName()==TEXT("WaterAnchorHit"))
                        {
                            TArray<FVector> Local;
                            TArray<FVector2D> Size;
                            Cache->ReadPositionAttribute(Local,TEXT("Position"),Emitter,false);
                            Cache->ReadVector2Attribute(Size,TEXT("SpriteSize"),Emitter);
                            for (int32 J=0;J<FMath::Min(Local.Num(),Size.Num());++J)
                                if (FMath::Min(FMath::Abs(Size[J].X),FMath::Abs(Size[J].Y))>.01)
                                {
                                    ++ContactCards;
                                    WidestCardRadius=FMath::Max(WidestCardRadius,Local[J].Size2D());
                                }
                        }
                        else OrbCards+=Cache->GetEmitterNumInstances(I,0);
                    }
                    if (Step>2 || !FParse::Param(FCommandLine::Get(),TEXT("RuneWaterStrands"))) continue;
                    if (FX->GetName()==TEXT("WaterHold") || FX->GetName()==TEXT("WaterCharge"))
                    {
                        TArray<int32> Groups;
                        TArray<FLinearColor> Colors;
                        TArray<FVector4> Dynamic;
                        if (Emitter=="WaterShockWave" || Emitter=="WaterShockWave002")
                            Cache->ReadIntAttribute(Groups,TEXT("SpawnGroup"),Emitter);
                        if (Emitter=="Minimal")
                        {
                            Cache->ReadColorAttribute(Colors,TEXT("Color"),Emitter);
                            Cache->ReadVector4Attribute(Dynamic,TEXT("DynamicMaterialParameter"),Emitter);
                        }
                        if (Emitter=="Minimal" || Emitter=="Runes001" || Emitter=="NE_BasicEmitter_Light" || Groups.Num())
                        {
                            const FVector4 D=Dynamic.IsEmpty()?FVector4(0,0,0,0):Dynamic[0];
                            OpticalSamples.Add(FString::Printf(TEXT("%.4f,%s,%s,%d,%d,%d,%.4f,%.4f,%.5f"),
                                Now-EquipTime,*FX->GetName(),*Emitter.ToString(),Counts[Emitter],
                                Algo::Count(Groups,0),Algo::Count(Groups,1),Colors.IsEmpty()?0.f:Colors[0].A,D.X,D.Y));
                        }
                    }
                    if (Emitter!="ArmParticle" && Emitter!="Minimal005" && Emitter!="ArmRibbon_Source" &&
                        Emitter!="RibbonSource001" && Emitter!="Runes" && Emitter!="Minimal") continue;
                    const auto* Core=CastChecked<UNiagaraComponent>(B.GetDefaultSubobjectByName(TEXT("WaterHold")));
                    const auto* Arm=CastChecked<UNiagaraComponent>(B.GetDefaultSubobjectByName(TEXT("WaterHoldArm")));
                    const FVector Axis=Core->GetComponentLocation()-Arm->GetComponentLocation();
                    TArray<FVector> Positions;
                    Cache->ReadPositionAttribute(Positions,TEXT("Position"),Emitter,true,0);
                    for (const FVector& P:Positions)
                    {
                        FVector2D Pixel;
                        if (CastChecked<APlayerController>(B.GetController())->ProjectWorldLocationToScreen(P,Pixel))
                            StrandSamples.Add(FString::Printf(TEXT("%.4f,%s,%s,%.2f,%.2f,%.3f"),Now-EquipTime,
                                *FX->GetName(),*Emitter.ToString(),Pixel.X,Pixel.Y,
                                FVector::DotProduct(P-Arm->GetComponentLocation(),Axis)/FMath::Max(1.,Axis.SizeSquared())));
                    }
                }
            for (const auto& E:Instance->GetEmitters())
            {
                FName Name=E->GetEmitterHandle().GetName();
                const auto* N=Counts.Find(Name);
                Samples.Add(FString::Printf(TEXT("%.4f,%d,%s,%s,%d,%d,%d"),Now,Step,*FX->GetAsset()->GetName(),
                    *Name.ToString(),int(E->GetExecutionState()),N?*N:-1,E->GetTotalSpawnedParticles()));
            }
        }
    }
public:
    FWaterCast(FAutomationTestBase& T,AShengLuBoard* B):Test(T),Board(B){}
    bool Update() override
    {
        auto* B=Board.Get(); if (!B) {Test.AddError(TEXT("No board"));return true;}
        auto* PC=CastChecked<APlayerController>(B->GetController());
        double Now=B->GetWorld()->GetTimeSeconds();
        const bool AnchorGallery=FParse::Param(FCommandLine::Get(),TEXT("RuneWaterAnchorGallery"));
        if (AnchorGallery && ThrowTime>=0 && AnchorFrame<11)
        {
            auto* Anchor=CastChecked<UNiagaraComponent>(B->GetDefaultSubobjectByName(TEXT("WaterAnchor")));
            if (AnchorStart<0 && Anchor->IsActive()) AnchorStart=Now;
            constexpr double Times[]={.06,.13,.23,.34,.47,.59,.72,.86,.98,1.115,1.19};
            if (AnchorStart>=0 && Now-AnchorStart>=Times[AnchorFrame])
            {
                const auto* Range=CastChecked<UStaticMeshComponent>(B->GetDefaultSubobjectByName(TEXT("WaterRange")));
                Test.TestFalse(TEXT("Water aim sphere stays hidden throughout the anchor shot"),Range->IsVisible());
                const FString File=FString::Printf(TEXT("Screenshots/WaterAnchor/%02d.png"),++AnchorFrame);
                FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/File),false,false);
                UE_LOG(LogTemp,Display,TEXT("WATER_ANCHOR_FRAME %d age=%.4f z=%.2f"),AnchorFrame,Now-AnchorStart,Anchor->GetComponentLocation().Z);
            }
        }
        if (ThrowTime>=0 && FlightFrame<7 && FParse::Param(FCommandLine::Get(),TEXT("RuneWaterFlight")))
        {
            auto* Hit=CastChecked<UNiagaraComponent>(B->GetDefaultSubobjectByName(TEXT("WaterBurst")));
            if (ImpactTime<0 && Hit->IsActive()) ImpactTime=Now;
            constexpr double Times[]={0.,.085,.18,0.,.085,.28,.65};
            double Start=FlightFrame<3?ThrowTime:ImpactTime;
            if (Start>=0 && Now-Start>=Times[FlightFrame])
            {
                const TCHAR* Folder=FParse::Param(FCommandLine::Get(),TEXT("RuneWaterFlightBox"))?TEXT("WaterFlightBox"):TEXT("WaterFlight");
                const FString File=FString::Printf(TEXT("Screenshots/%s/%02d.png"),Folder,++FlightFrame);
                FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/File),false,false);
                UE_LOG(LogTemp,Display,TEXT("WATER_FLIGHT_FRAME %d throw=%.4f impact=%.4f"),FlightFrame,Now-ThrowTime,ImpactTime<0?-1.:Now-ImpactTime);
            }
        }
        if (Step>0 && Step<3 && FParse::Param(FCommandLine::Get(),TEXT("RuneWaterHands")))
        {
            constexpr float Times[]={.16f,.27f,.45f,.56f,.62f,.70f,.80f,.95f,1.16f,1.55f};
            if (HandFrame<UE_ARRAY_COUNT(Times) && Now-EquipTime>=Times[HandFrame])
            {
                const FString File=FString::Printf(TEXT("Screenshots/WaterHands/%02d.png"),++HandFrame);
                FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/File),false,false);
                UE_LOG(LogTemp,Display,TEXT("WATER_HAND_FRAME %d age=%.4f"),HandFrame,Now-EquipTime);
            }
        }
        if (FParse::Param(FCommandLine::Get(),TEXT("RuneEmitterAudit")) && Step>0 && ++SampleTick%3==1) Sample(*B,Now);
        if (Now<Next) return false;
        auto Key=[&](FKey K){PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,IE_Pressed,1));
                              PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,IE_Released,0));};
        auto* Cam=CastChecked<UCameraComponent>(B->GetDefaultSubobjectByName(TEXT("CastCamera")));
        auto FX=[&](FName N){return CastChecked<UNiagaraComponent>(B->GetDefaultSubobjectByName(N));};
        auto Aim=[&]{Cam->SetWorldRotation((B->CrateMesh()->GetComponentLocation()-Cam->GetComponentLocation()).Rotation());};
        const bool FlightGallery=FParse::Param(FCommandLine::Get(),TEXT("RuneWaterFlightGallery"));
        auto AimAtFloor=[&]{
            const auto& Level=B->Level();
            const FVector Eye=Cam->GetComponentLocation();
            double Range=0;
            for (int32 I=0;I<Level.StaticCells.Num();++I)
            {
                const FIntVector Cell=Level.IndexToGrid(I);
                if (!Level.StaticCells[I].bHasFloor || Level.StaticCells[I].bBlocked || !B->Runtime().GetBoxesAt(Cell).IsEmpty())continue;
                const FBox Floor=Level.FloorBounds(Cell);
                const FVector End(Floor.GetCenter().X,Floor.GetCenter().Y,Floor.Max.Z-1.);
                FHitResult Hit;
                FCollisionQueryParams Params(SCENE_QUERY_STAT(WaterFlightGallery),false);
                if (!B->GetWorld()->LineTraceSingleByChannel(Hit,Eye,End,ECC_Visibility,Params) || FVector::Dist(Hit.ImpactPoint,End)>5.)continue;
                const double D=FVector::Dist(Eye,End);
                if (D>Range && D<7000.) {Range=D;Cam->SetWorldRotation((End-Eye).Rotation());}
            }
            Test.TestTrue(TEXT("Gallery has a visible landing tile"),Range>0);
        };
        const bool CaptureHands=FParse::Param(FCommandLine::Get(),TEXT("RuneWaterHands"));
        auto Shot=[&](const TCHAR* Name){if(FParse::Param(FCommandLine::Get(),TEXT("RuneScreens")))
            FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Screenshots/Water")/Name)+TEXT(".png"),false,false);};
        float Wait=.06f;
        switch(Step++)
        {
        case 0:
            Key(EKeys::R);Key(EKeys::Three);Aim();EquipTime=Now;
            if (CaptureHands) Cam->SetWorldRotation(FRotator(0,Cam->GetComponentRotation().Yaw+65.f,0));
            Wait=.34f;break;
        case 1: Test.TestTrue(TEXT("Water selection gathers water"),B->Selection()==ERuneAction::WaterAnchor && FX("WaterCharge")->IsActive());Shot(TEXT("01-Charge"));Wait=1.6f;break;
        case 2: Test.TestTrue(TEXT("Water orb holds without mouse input"),FX("WaterHold")->IsActive() && !FX("WaterCharge")->IsActive());Shot(TEXT("02-Holding"));break;
        case 3: if (FlightGallery && !FParse::Param(FCommandLine::Get(),TEXT("RuneWaterFlightBox"))) AimAtFloor();else Aim();Key(EKeys::LeftMouseButton);ThrowTime=Now;Wait=.065f;break;
        case 4:
            Test.TestTrue(TEXT("Projectile active before the water impact"),FX("WaterBullet")->IsActive() && !FX("WaterBurst")->IsActive());
            if (!FlightGallery) Test.TestFalse(TEXT("Fixture box still falls before impact"),B->Board().bHeightAnchored);
            Shot(TEXT("03-Flight"));Wait=.29f;break;
        case 5:
            if (FlightGallery) {Wait=1.8f;break;}
            Test.TestTrue(TEXT("Burst starts the falling anchor"),FX("WaterBurst")->IsActive() && FX("WaterAnchor")->IsActive());Shot(TEXT("04-Burst"));Wait=.91f;break;
        case 6:
            if (FlightGallery)
            {
                if (AnchorGallery)
                {
                    Test.TestEqual(TEXT("Eleven anchor stages from one cast"),AnchorFrame,11);
                    if (FParse::Param(FCommandLine::Get(),TEXT("RuneEmitterAudit")))
                    {
                        Test.TestEqual(TEXT("Projectile and orb burst omit ground splash cards"),OrbCards,0);
                        Test.TestTrue(TEXT("Contact produces local secondary spray"),ContactCards>0);
                        Test.TestTrue(TEXT("Secondary spray stays within anchor width"),WidestCardRadius<=149.);
                        UE_LOG(LogTemp,Display,TEXT("WATER_CARD_AUDIT orb=%d contactSamples=%d maxLocalRadius=%.2f"),OrbCards,ContactCards,WidestCardRadius);
                    }
                    if (Samples.Num()>1) FFileHelper::SaveStringArrayToFile(Samples,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/AnchorRuntime.csv")));
                    return true;
                }
                Test.TestEqual(TEXT("Seven frames from one cast"),FlightFrame,7);
                const TCHAR* Csv=FParse::Param(FCommandLine::Get(),TEXT("RuneWaterFlightBox"))?TEXT("RuntimeBox.csv"):TEXT("Runtime.csv");
                if (Samples.Num()>1) FFileHelper::SaveStringArrayToFile(Samples,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/Flight")/Csv));
                return true;
            }
            Shot(TEXT("05-Anchor"));Wait=.17f;break;
        case 7: Shot(TEXT("06-Contact"));Test.TestTrue(TEXT("ground box anchored"),B->Board().bHeightAnchored);Wait=.4f;break;
        case 8: Key(EKeys::R);Key(EKeys::Three);Wait=.6f;break;
        case 9: Key(EKeys::RightMouseButton);break;
        case 10:
            Test.TestTrue(TEXT("Cancel clears water from the hands for free"),!FX("WaterHold")->IsActive() && B->Board().Turn==0);
            Test.TestFalse(TEXT("Cancel clears the palm flash"),CastChecked<UPointLightComponent>(B->GetDefaultSubobjectByName(TEXT("WaterPalmFlash")))->IsVisible());
            Key(EKeys::Two);Aim();Key(EKeys::LeftMouseButton);Wait=.65f;break;
        case 11: Test.TestEqual(TEXT("wind lifted crate"),B->Board().Box.Z,1);Key(EKeys::Three);Aim();Wait=.7f;break;
        case 12: Key(EKeys::LeftMouseButton);Wait=.6f;break;
        case 13: Key(EKeys::One);Wait=.9f;break;
        case 14: Test.TestTrue(TEXT("Impact survives switching spell and locks the raised crate once"),B->Board().bHeightAnchored && B->Board().SP==3 && B->Board().Turn==2 && B->Board().Box.Z==1 && FX("WaterAnchorHit")->IsActive());Wait=.45f;break;
        case 15: Test.TestTrue(TEXT("gravity skips anchored crate when wind expires"),B->Apply(ERuneAction::North) && B->Board().Wind==0 && B->Board().Box.Z==1);Wait=.45f;break;
        case 16:
        {
            const auto Push=FPuzzleSolver::Evaluate(B->Level(),B->Runtime(),{ERuneAction::Pulse,1,{1,0,0}});
            Test.TestTrue(TEXT("anchored crate can be pushed horizontally at fixed height"),Push.IsValid() &&
                Push.After.FindBox(1)->GridPosition==FIntVector(2,1,1));
            Key(EKeys::Z);break;
        }
        case 17: Key(EKeys::Z);break;
        case 18: Test.TestTrue(TEXT("undo removes anchor and restores wind and SP"),!B->Board().bHeightAnchored && B->Board().Box.Z==1 && B->Board().Wind==2 && B->Board().SP==5);Key(EKeys::R);Key(EKeys::Three);Wait=.6f;break;
        case 19:
            Cam->SetWorldRotation((B->GetActorTransform().TransformPosition(FVector(ShengLuLayout::CellSize,0,0))-Cam->GetComponentLocation()).Rotation());
            break;
        case 20: Key(EKeys::LeftMouseButton);Wait=.09f;break;
        case 21: Test.TestTrue(TEXT("empty tile accepts aimed projectile"),FX("WaterBullet")->IsActive());Wait=1.5f;break;
        case 22:
            Test.TestTrue(TEXT("empty landing has impact without locking a box or charging SP"),FX("WaterAnchorHit")->IsActive() && !B->Board().bHeightAnchored && B->Board().SP==8 && B->Board().Turn==0);
            if (Samples.Num()>1) FFileHelper::SaveStringArrayToFile(Samples,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/Runtime.csv")));
            if (StrandSamples.Num()>1) FFileHelper::SaveStringArrayToFile(StrandSamples,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/StrandPositions.csv")));
            if (OpticalSamples.Num()>1) FFileHelper::SaveStringArrayToFile(OpticalSamples,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/Water/OpticalRuntime.csv")));
            Key(EKeys::R);Key(EKeys::F);return true;
        }
        Next=Now+Wait;return false;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterCastTest,"ResonantRunes.ShengLu.Water.Cast",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FWaterCastTest::RunTest(const FString&)
{
    for (const auto& Context:GEngine->GetWorldContexts())
    {
        auto* W=Context.World();
        if (!W || !W->IsGameWorld())continue;
        auto* PC=W->GetFirstPlayerController();auto* B=PC?Cast<AShengLuBoard>(PC->GetPawn()):nullptr;
        if(B){FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FWaterCast(*this,B)));return true;}
    }
    AddError(TEXT("Water cast requires the ShengLu game world"));return false;
}
#endif

