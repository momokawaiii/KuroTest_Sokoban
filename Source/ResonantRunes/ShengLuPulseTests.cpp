#if WITH_DEV_AUTOMATION_TESTS
#include "ShengLuGame.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSimCache.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"

namespace
{
    class FShengLuSurfaceDrag final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TWeakObjectPtr<AShengLuBoard> Board;
        int Step=0;
        double Next=0, Deadline=0;
        int ScarIndices=0;
        double SweepStart=0;
        double CaptureStart=-1;
        double FirstDischargeStart=-1;
        double CloseStart=-1;
        int LastCapture=-1;
        int AuditTick=0;
        TArray<FString> EmitterSamples;
        // An optical isolation run changes only transient renderer visibility in its own game process.
        int32 MaterialProbe=0;
        bool ProbeWaiting=false;
        FString SelectMaterialProbe()
        {
            static const TCHAR* Groups[]={TEXT("None"),TEXT("Flare"),TEXT("Spark"),TEXT("Smoke"),TEXT("Rune"),
                TEXT("Wave"),TEXT("Ribbon"),TEXT("Other"),TEXT("All")};
            const FString Group=Groups[MaterialProbe];
            for (const TCHAR* Phase:{TEXT("Arm"),TEXT("Charge"),TEXT("Charge_Arm"),TEXT("End"),TEXT("Trail"),TEXT("Hit")})
            {
                const FString Name=FString(TEXT("NS_Player_Skill_Electric01_"))+Phase;
                auto* System=LoadObject<UNiagaraSystem>(nullptr,*(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/")+Name+TEXT(".")+Name));
                for (const auto& E:System->GetEmitterHandles())
                    for (auto* R:E.GetEmitterData()->GetRenderers())
                    {
                        FString Family=TEXT("Other");
                        if (auto* S=Cast<UNiagaraSpriteRendererProperties>(R);S && S->Material)
                        {
                            const auto M=S->Material->GetName();
                            for (const auto* Class:{TEXT("Flare"),TEXT("Spark"),TEXT("Smoke"),TEXT("Rune"),TEXT("Wave")})
                                if (M.Contains(Class)) Family=Class;
                        }
                        else if (Cast<UNiagaraRibbonRendererProperties>(R)) Family=TEXT("Ribbon");
                        R->SetIsEnabled(Group==TEXT("All") || Family==Group);
                    }
            }
            return Group;
        }
        void SampleEmitters(AShengLuBoard& Game,double Now)
        {
            TInlineComponentArray<UNiagaraComponent*> Components(&Game);
            for (auto* Component : Components)
            {
                auto* Asset=Component->GetAsset();
                if (!Asset || !Asset->GetName().StartsWith(TEXT("NS_Player_Skill_Electric01_"))) continue;
                const auto Controller=Component->GetSystemInstanceController();
                if (!Controller) continue;
                Controller->WaitForConcurrentTickAndFinalize();
                auto* Instance=Controller->GetSystemInstance_Unsafe();
                if (!Instance) continue;
                auto* Cache=NewObject<UNiagaraSimCache>();
                const bool Captured=Component->IsActive() && Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),Component)
                    && Cache->WriteFrame(Component) && Cache->EndWrite() && Cache->GetNumFrames()==1;
                TMap<FName,int32> LiveCounts;
                if (Captured)
                    for (int I=0;I<Cache->GetNumEmitters();++I)
                        LiveCounts.Add(Cache->GetEmitterName(I),Cache->GetEmitterNumInstances(I,0));
                for (const auto& Emitter : Instance->GetEmitters())
                {
                    const auto& Handle=Emitter->GetEmitterHandle();
                    const FName Name=Handle.GetName();
                    const int32* Live=LiveCounts.Find(Name);
                    EmitterSamples.Add(FString::Printf(TEXT("%.4f,%d,%s,%s,%s,%s,%d,%d,%d"),Now,Step,*Component->GetName(),
                        *Asset->GetName(),*Name.ToString(),Emitter->GetSimTarget()==ENiagaraSimTarget::GPUComputeSim?TEXT("GPU"):TEXT("CPU"),
                        int(Emitter->GetExecutionState()),Live?*Live:-1,Emitter->GetTotalSpawnedParticles()));
                }
            }
        }
        TWeakObjectPtr<const UNiagaraComponent> OldContact;
        FVector FirstScar, OriginalCrate;
    public:
        FShengLuSurfaceDrag(FAutomationTestBase& InTest, AShengLuBoard* Game) : Test(InTest), Board(Game) {}
        bool Update() override
        {
            auto* G=Board.Get();
            if (!G) { Test.AddError(TEXT("Board disappeared")); return true; }
            auto* PC=CastChecked<APlayerController>(G->GetController());
            const double Now=G->GetWorld()->GetTimeSeconds();
            const bool Audit=FParse::Param(FCommandLine::Get(),TEXT("RuneEmitterAudit"));
            if (Audit && Step>0 && (++AuditTick%3==1))
                SampleEmitters(*G,Now);
            if (FirstDischargeStart>=0 && Now-FirstDischargeStart<=.73 && FParse::Param(FCommandLine::Get(),TEXT("RuneFirstDischarge"))
                    && !FScreenshotRequest::IsScreenshotRequested())
            {
                const int Frame=FMath::RoundToInt((Now-FirstDischargeStart)*60);
                const FString Name=FString::Printf(TEXT("ElectricFirstDischarge/Frame%03d.png"),Frame);
                FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()/Name),false,false);
            }
            if (CloseStart>=0 && Step>=20 && Step<=22 && FParse::Param(FCommandLine::Get(),TEXT("RuneFirstDischarge")))
            {
                const int Frame=FMath::RoundToInt((Now-CloseStart)*60);
                if (Frame%3==0 && !FScreenshotRequest::IsScreenshotRequested())
                {
                    const FString Name=FString::Printf(TEXT("ElectricCloseDischarge/Frame%03d.png"),Frame);
                    FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()/Name),false,false);
                }
            }
            if (CaptureStart>=0 && Now-CaptureStart<3.0)
            {
                const int Frame=FMath::FloorToInt((Now-CaptureStart)*24);
                if (Frame!=LastCapture && !FScreenshotRequest::IsScreenshotRequested())
                {
                    const FString Name=FString::Printf(TEXT("Electric01Frames/Frame%03d.png"),Frame);
                    FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()/Name),false,false);
                    LastCapture=Frame;
                }
            }
            if (Now<Next) return false;
            if (Step==20 && FParse::Param(FCommandLine::Get(),TEXT("RuneMaterialProbe")))
            {
                if (!ProbeWaiting)
                {
                    if (MaterialProbe>=9) {G->ResetPuzzle();return true;}
                    const auto Group=SelectMaterialProbe();
                    UE_LOG(LogTemp,Display,TEXT("Electric01 isolating renderer group %s"),*Group);
                    auto* Camera=CastChecked<UCameraComponent>(G->GetDefaultSubobjectByName(TEXT("CastCamera")));
                    Camera->SetWorldRotation((G->GetActorTransform().TransformPosition(FVector(131,185,2))-Camera->GetComponentLocation()).Rotation());
                    Next=Now+.45;
                }
                else
                {
                    const FString Path=FPaths::ProjectIntermediateDir()/FString::Printf(TEXT("ElectricIsolation/Group%02d.png"),MaterialProbe);
                    FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(Path),false,false);
                    Next=Now+.08;
                    ++MaterialProbe;
                }
                ProbeWaiting=!ProbeWaiting;
                return false;
            }
            if (Deadline && Now>Deadline) {Test.AddError(FString::Printf(TEXT("Timeout at step %d, phase %d, SP %d, aim %d, progress %.2f"),Step,int(G->Phase()),G->Board().SP,G->AimedAtBox(),G->BeamProgress()));return true;}
            auto Key=[&](FKey K,EInputEvent E=IE_Pressed){PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,E,E==IE_Pressed?1.f:0.f));};
            auto Tap=[&](FKey K){Key(K);Key(K,IE_Released);};
            auto Click=[&](FVector World)
            {
                FVector2D Screen;
                Test.TestTrue(TEXT("Point projects onto board"),PC->ProjectWorldLocationToScreen(World,Screen));
                G->ClickAt(Screen);
            };
            auto Shot=[&](const TCHAR* Name)
            {
                if (FParse::Param(FCommandLine::Get(),TEXT("RuneScreens")))
                    FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Screenshots/Casting")/Name)+TEXT(".png"),false,false);
            };
            const auto& B=G->Board();
            auto* Eye=CastChecked<UCameraComponent>(G->GetDefaultSubobjectByName(TEXT("CastCamera")));
            const auto* Impact=G->ActiveContact();
            auto* FX=CastChecked<UProceduralMeshComponent>(G->GetDefaultSubobjectByName(TEXT("ResonanceFX")));
            auto ScarCount=[&](){const auto* S=FX->GetProcMeshSection(2); return S?S->ProcIndexBuffer.Num():0;};
            auto Aim=[&](FVector Point)
            {
                Eye->SetWorldRotation((Point-Eye->GetComponentLocation()).Rotation());
            };
            const FVector BoxFoot=G->Cell(B.Box);
            constexpr float HalfBox=ShengLuLayout::CrateSide*.5f;
            const FVector Floor=G->Cell({0,1,0});
            float Delay=.12f;
            switch(Step)
            {
            case 0:
                G->ResetPuzzle();
                Tap(EKeys::One);
                break;
            case 1:
                if (FParse::Param(FCommandLine::Get(),TEXT("RunePulseSequence"))) CaptureStart=Now;
                Aim(BoxFoot+FVector(-40,-HalfBox,32));Key(EKeys::LeftMouseButton);Delay=.14f;break;
            case 2:
                Shot(TEXT("Pulse-01-Windup"));Delay=.21f;break;
            case 3:
                Test.TestTrue(TEXT("Palm charges before the independent impact starts"),G->Phase()==ECastPhase::Start && !Impact && ScarCount()==0);
                Shot(TEXT("Pulse-02-Raise"));Delay=.30f;break;
            case 4:
                Test.TestTrue(TEXT("A stationary hit on an ineligible box lights it without painting a line"),G->AimedAtBox() && Impact && Impact->IsActive() && G->BeamProgress()==0 && !ScarCount());
                Shot(TEXT("Pulse-03-Contact"));SweepStart=Now;break;
            case 5:
            {
                const float T=FMath::Min(1.,(Now-SweepStart)/.62);
                Aim(BoxFoot+FVector(-40+78*T,-HalfBox,32+37*T+8*FMath::Sin(PI*T)));
                if (T<1) return false;
                break;
            }
            case 6:
                ScarIndices=ScarCount();Test.TestTrue(TEXT("Sliding along one face draws a continuous ribbon without spending"),ScarIndices>=48 && B.SP==8);
                Shot(TEXT("Pulse-04-Drag"));break;
            case 7:
                Test.TestEqual(TEXT("Stationary contact adds no samples"),ScarCount(),ScarIndices);
                Aim(Floor);break;
            case 8:
                Test.TestTrue(TEXT("The floor has a contact effect and starts a new stroke without bridging"),Impact && Impact->IsActive() && !G->AimedAtBox() && ScarCount()==ScarIndices);
                Aim(Floor+FVector(20,-15,0));break;
            case 9:
                Test.TestTrue(TEXT("Dragging on a tile paints its surface"),ScarCount()>ScarIndices && B.Turn==0);
                ScarIndices=ScarCount(); OldContact=Impact; Eye->SetWorldRotation(FRotator(75,0,0));break;
            case 10:
                Test.TestTrue(TEXT("An air gap turns off contact and breaks the stroke"),!Impact && ScarCount()==ScarIndices);
                Aim(BoxFoot+FVector(0,-HalfBox,52));break;
            case 11:
                Test.TestTrue(TEXT("Reacquiring the box starts a distinct effect"),Impact && Impact->IsActive() && Impact!=OldContact.Get() && ScarCount()==ScarIndices);
                Key(EKeys::LeftMouseButton,IE_Released);Delay=.09f;break;
            case 12:
                Test.TestTrue(TEXT("Release stops the beam while the surface scar cools"),!G->IsFiring() && !Impact && ScarCount()>0);
                if (const auto* Active=FX->GetProcMeshSection(1))
                    Test.TestTrue(TEXT("Previously emitted contact smoke survives release"),Active->ProcVertexBuffer.ContainsByPredicate(
                        [](const FProcMeshVertex& V){return FMath::IsNearlyEqual(V.UV1.X,3.f);}));
                else Test.AddError(TEXT("All discharge particles vanished on release"));
                Shot(TEXT("Pulse-05-Scar"));Delay=2.45f;break;
            case 13:
                Test.TestTrue(TEXT("Released surface ribbons fade while the caster stays in first person"),
                    G->IsFirstPerson() && G->Phase()==ECastPhase::Ready && ScarCount()==0 && B.SP==8);
                Tap(EKeys::F);break;
            case 14:
                Click(G->Cell({0,0,0})+FVector(0,0,92));
                Click(G->Cell({1,0,0})+FVector(0,0,2));Delay=.55f;break;
            case 15: Tap(EKeys::W);break;
            case 16: Tap(EKeys::One);break;
            case 17:
                Eye->SetWorldLocation(BoxFoot+FVector(-HalfBox-112.f,0,154));
                Aim(BoxFoot+FVector(-HalfBox,-28,48));Key(EKeys::LeftMouseButton);CloseStart=Now;Delay=.12f;break;
            case 18: Shot(TEXT("Electric-Close-Windup"));Delay=.27f;break;
            case 19:
                Shot(TEXT("Electric-Close-Raise"));Delay=.43f;break;
            case 20:
                if (!Test.TestTrue(TEXT("Close impact originates on the box within 180 cm"),
                    Impact && FVector::Dist(Impact->GetComponentLocation(),Eye->GetComponentLocation())<180.f)) return true;
                Shot(TEXT("Electric-Close-Hit"));SweepStart=Now;break;
            case 21:
            {
                const float T=FMath::Min(1.,(Now-SweepStart)/.56);
                // Sweep back upwards along the near face like the reference: the
                // groove already cut into the receiver should trail below the tip.
                const float Bow=FMath::Sin(PI*T);
                Aim(BoxFoot+FVector(-HalfBox,-28+26*Bow-14*T,48+37*T-8*Bow));
                if (T<1) return false;
                Delay=.025f;break;
            }
            case 22:
                Shot(TEXT("Electric-Close-Drag"));
                if (!Test.TestTrue(TEXT("An aligned box receives a stroke before the push"),G->AimedAtBox() && ScarCount()>0)) return true;
                FirstScar=FX->GetProcMeshSection(2)->ProcVertexBuffer[0].Position;
                OriginalCrate=G->CrateMesh()->GetComponentLocation();
                break;
            case 23:
                if (B.Turn<2 || G->IsAnimating()) return false;
                if (Test.TestTrue(TEXT("Scored push retains the marked surface"),B.Box==FIntVector(1,2,0) && B.SP==5 && ScarCount()>0))
                {
                    const FVector Delta=FX->GetComponentTransform().TransformVector(FX->GetProcMeshSection(2)->ProcVertexBuffer[0].Position-FirstScar);
                    Test.TestTrue(TEXT("Existing scar moves the same distance as its receiver"),Delta.Equals(G->CrateMesh()->GetComponentLocation()-OriginalCrate,1.f));
                }
                Tap(EKeys::Z);break;
            case 24:
                Test.TestTrue(TEXT("Undo clears old surface marks and active discharge"),ScarCount()==0 && !G->IsFiring());
                G->ResetPuzzle();
                Tap(EKeys::One);Key(EKeys::LeftMouseButton);Delay=.12f;break;
            case 25:
                Key(EKeys::LeftMouseButton,IE_Released);break;
            case 26:
                Test.TestTrue(TEXT("Releasing during the windup skips charged release FX"),!G->IsFiring() && !Impact &&
                    !CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("PulseEnd")))->IsActive() && B.SP==8);
                G->ResetPuzzle();
                Tap(EKeys::One);break;
            case 27:
                // Aim at the far face of the raised tile. The camera sees over the unlifted crate.
                Aim(G->Cell({2,1,0})+FVector(-16,-92.5,113)*ShengLuLayout::Scale);Key(EKeys::LeftMouseButton);FirstDischargeStart=Now;Delay=.47f;break;
            case 28:
            {
                const auto* Light=FX->GetProcMeshSection(1);
                Test.TestTrue(TEXT("The raised palm has glyphs before the first beam"),!G->IsFiring() && Light &&
                    Light->ProcVertexBuffer.ContainsByPredicate([](const FProcMeshVertex& V){return V.UV1.X==8;}));
                Delay=.085f;break;
            }
            case 29:
            {
                const auto* Wave=FX->GetProcMeshSection(3);
                Test.TestTrue(TEXT("The first beam opens a refractive wave at the hand"),G->IsFiring() && Impact && Wave && !Wave->ProcIndexBuffer.IsEmpty());
                Test.TestTrue(TEXT("Each hand renderer issues both source spawn groups with separate curve samples"), Wave &&
                    Wave->ProcVertexBuffer.Num()==16 && Wave->ProcVertexBuffer[0].UV1.X==0 &&
                    Wave->ProcVertexBuffer[4].UV1.X==.25f && Wave->ProcVertexBuffer[8].UV1.X==1 &&
                    Wave->ProcVertexBuffer[12].UV1.X==1.25f && Wave->ProcVertexBuffer[12].UV2.X>0);
                Delay=.48f;break;
            }
            case 30:
            {
                const auto* Wave=FX->GetProcMeshSection(3);
                Test.TestTrue(TEXT("The wide first-discharge film expires while the smaller hand wave keeps emitting"),G->IsFiring() &&
                    Wave && !Wave->ProcVertexBuffer.IsEmpty() &&
                    !Wave->ProcVertexBuffer.ContainsByPredicate([](const FProcMeshVertex& V){return V.UV1.X>=1.f;}));
                const auto* HitWaves=FX->GetProcMeshSection(4);
                for (float Mode : {2.f,2.25f,3.f,3.25f,4.f,4.25f})
                    Test.TestTrue(TEXT("Each impact renderer continues issuing both source spawn groups"),HitWaves &&
                        HitWaves->ProcVertexBuffer.ContainsByPredicate([Mode](const FProcMeshVertex& V){return V.UV1.X==Mode;}));
                if (Audit)
                {
                    EmitterSamples.Insert(TEXT("world_seconds,test_step,component,system,emitter,sim,execution_state,live_buffer,total_spawned"),0);
                    Test.TestTrue(TEXT("Save real Electric01 emitter observations"),EmitterSamples.Num()>1 &&
                        FFileHelper::SaveStringArrayToFile(EmitterSamples,*(FPaths::ProjectSavedDir()/TEXT("Automation/Electric01Emitters.csv")),
                            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
                }
                Key(EKeys::LeftMouseButton,IE_Released); G->ResetPuzzle();Tap(EKeys::F);
                return true;
            }
            }
            Next=Now+Delay; Deadline=Next+4.; ++Step; return false;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShengLuSurfaceTest,"ResonantRunes.Electric01.SurfaceDrag",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)

bool FShengLuSurfaceTest::RunTest(const FString&)
{
    for (const auto& Context:GEngine->GetWorldContexts())
    {
        UWorld* World=Context.World();
        if (!World || !World->IsGameWorld()) continue;
        auto* PC=World->GetFirstPlayerController();
        if (auto* Board=PC?Cast<AShengLuBoard>(PC->GetPawn()):nullptr)
        {
            FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FShengLuSurfaceDrag(*this,Board)));
            return true;
        }
    }
    AddError(TEXT("Run in the ShengLu game world"));
    return false;
}
#endif

