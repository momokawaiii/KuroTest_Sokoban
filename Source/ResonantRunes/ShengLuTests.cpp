#if WITH_DEV_AUTOMATION_TESTS

#include "ShengLuGame.h"
#include "ShengLuHUD.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/DecalComponent.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "UnrealClient.h"
#include "NiagaraComponent.h"
#include "NiagaraSimCache.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShengLuRulesTest,"ResonantRunes.ShengLu.Rules",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FShengLuRulesTest::RunTest(const FString&)
{
    const auto Level = FPuzzleLevelDefinition::MakeShengLu();
    const auto Start = FPuzzleRuntimeState::FromDefinition(Level);
    const FPuzzleAction Wind{ERuneAction::Levitate, 1};
    const FPuzzleAction PushEast{ERuneAction::Pulse, 1, {1, 0, 0}};
    const FPuzzleAction PushNorth{ERuneAction::Pulse, 1, {0, 1, 0}};
    const auto Lift = FPuzzleSolver::Evaluate(Level, Start, Wind);
    TestTrue(TEXT("Preview leaves current state untouched"), Start.SP == 8 && Start.FindBox(1)->GridPosition.Z == 0);
    TestTrue(TEXT("Wind gets two future actions"), Lift.IsValid() && Lift.After.SP == 5 &&
        Lift.After.FindBox(1)->LevitationTurns == 2);
    const auto Step = FPuzzleSolver::Evaluate(Level, Lift.After, {ERuneAction::North});
    TestFalse(TEXT("A push still has to go away from the player"), FPuzzleSolver::Evaluate(Level, Step.After, PushNorth).IsValid());
    const auto Finish = FPuzzleSolver::Evaluate(Level, Step.After, PushEast);
    TestTrue(TEXT("Restore on the platform"), Finish.IsValid() && Finish.After.SP == 2 && FPuzzleSolver::IsRestored(Level, Finish.After));
    TestTrue(TEXT("Passing second turn without support drops box"),
        FPuzzleSolver::Evaluate(Level, Step.After, {ERuneAction::South}).After.FindBox(1)->GridPosition.Z == 0);
    TestFalse(TEXT("Plinth blocks ground push"), FPuzzleSolver::Evaluate(Level, Start, PushEast).IsValid());
    const auto Diagonal = FPuzzleSolver::Evaluate(Level, Start, PushNorth);
    TestTrue(TEXT("The second board accepts an axial push from diagonal player position"), Diagonal.IsValid() &&
        Diagonal.After.FindBox(1)->GridPosition == FIntVector(1, 2, 0));

    const auto Anchored = FPuzzleSolver::Evaluate(Level, Start, {ERuneAction::WaterAnchor, 1});
    const auto LiftAnchored = FPuzzleSolver::Evaluate(Level, Anchored.After, Wind);
    TestTrue(TEXT("Water anchor allows wind to raise the crate on the second board"), LiftAnchored.IsValid() &&
        LiftAnchored.After.FindBox(1)->bPhaseLocked && LiftAnchored.After.FindBox(1)->GridPosition.Z == 1);
    const auto WindTick = FPuzzleSolver::Evaluate(Level, LiftAnchored.After, {ERuneAction::North});
    const auto WindEnd = FPuzzleSolver::Evaluate(Level, WindTick.After, {ERuneAction::South});
    TestTrue(TEXT("The anchor keeps its new height after wind expires"), WindEnd.IsValid() &&
        WindEnd.After.FindBox(1)->LevitationTurns == 0 && WindEnd.After.FindBox(1)->GridPosition.Z == 1);

    auto Fall = Step.After;
    Fall.PlayerPosition = {1, 0, 0};
    const auto Slide = FPuzzleSolver::Evaluate(Level, Fall, PushNorth);
    TestTrue(TEXT("Preview separates travel and landing"), Slide.IsValid() && Slide.BeforeGravity == FIntVector(1, 2, 1) &&
        Slide.After.FindBox(1)->GridPosition == FIntVector(1, 2, 0));
    Fall.PlayerPosition = {1, 0, 0};
    TestFalse(TEXT("Rejected when landing would crush player"),
        FPuzzleSolver::Evaluate(Level, Fall, {ERuneAction::North}).IsValid());
    Fall.SP = 1;
    TestFalse(TEXT("Cannot overspend"), FPuzzleSolver::Evaluate(Level, Fall, PushNorth).IsValid());
    auto Exact = Step.After;
    Exact.SP = 2;
    const auto Last = FPuzzleSolver::Evaluate(Level, Exact, PushEast);
    TestTrue(TEXT("Last SP can win"), Last.IsValid() && Last.After.SP == 0 && FPuzzleSolver::IsRestored(Level, Last.After));
    return true;
}

namespace
{
    class FShengLuPlaythrough final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TWeakObjectPtr<AShengLuBoard> Board;
        int Step=0;
        double Next=0, Deadline=0;
    public:
        FShengLuPlaythrough(FAutomationTestBase& InTest,AShengLuBoard* Game) : Test(InTest),Board(Game) {}
        bool Update() override
        {
            auto* G=Board.Get();
            if (!G) { Test.AddError(TEXT("Board disappeared")); return true; }
            auto* PC=CastChecked<APlayerController>(G->GetController());
            const double Now=G->GetWorld()->GetTimeSeconds();
            if (Now<Next) return false;
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
            auto* Eyes=CastChecked<UCameraComponent>(G->GetDefaultSubobjectByName(TEXT("CastCamera")));
            auto* Hands=CastChecked<USkeletalMeshComponent>(G->GetDefaultSubobjectByName(TEXT("CastHands")));
            auto ClipIs=[&](const TCHAR* Clip)
            {
                auto* Instance=Hands->GetSingleNodeInstance();
                return Instance && Instance->GetCurrentAsset() && Instance->GetCurrentAsset()->GetName().EndsWith(Clip);
            };
            auto* Rune=CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("WindRange")));
            auto VoiceIs=[&](FName Voice,const TCHAR* Wave)
            {
                auto* Sound=CastChecked<UAudioComponent>(G->GetDefaultSubobjectByName(Voice))->GetSound();
                if (!Sound) return false;
                FString SourceName=Sound->GetName();
                SourceName.RemoveFromEnd(TEXT("_Cue"));
                return SourceName.EndsWith(Wave);
            };
            float Delay=.12f;
            switch(Step)
            {
            case 0: Tap(EKeys::R); break;
            case 1: Tap(EKeys::W); break;
            case 2:
                Test.TestTrue(TEXT("W turns without spending or walking"),B.Player==FIntPoint(0,0) && B.Facing==FIntPoint(0,1) && B.SP==8 && B.Turn==0);
                Click(G->Cell({0,0,0})+FVector(0,0,92)); break;
            case 3:
                Test.TestTrue(TEXT("The selected player cell shows both initial reachable cells"),G->IsPawnSelected() && G->CanStepTo({1,0}) && G->CanStepTo({0,1}) && !G->CanStepTo({1,1}));
                Shot(TEXT("01-Neighbors")); break;
            case 4: Tap(EKeys::Two); Delay=1.3f; break;
            case 5:
                if (G->Phase()!=ECastPhase::Loop) return false;
                Test.TestTrue(TEXT("Wind enters two-hand loop and previews actual crate"),G->Selection()==ERuneAction::Levitate && G->Preview().IsValid());
                Test.TestTrue(TEXT("Original two-hand loop and ground rune are visible"),Hands->IsVisible() && ClipIs(TEXT("2h_Beam_Loop_Retarget")) && Rune->IsActive() && Rune->GetAsset());
                Test.TestTrue(TEXT("Original range sound is selected"),VoiceIs(TEXT("CastingLoop"),TEXT("Air02_Range_Loop")));
                Shot(TEXT("02-WindAim")); break;
            case 6: Eyes->AddWorldRotation(FRotator(0,180,0)); break;
            case 7: Tap(EKeys::LeftMouseButton); break;
            case 8:
                Test.TestTrue(TEXT("Confirming empty aim spends nothing"),!G->AimedAtBox() && B.SP==8 && B.Turn==0);
                Tap(EKeys::RightMouseButton);break;
            case 9:
            {
                Test.TestTrue(TEXT("Cancelling wind keeps the first-person camera and clears the preview"),
                    G->IsFirstPerson() && G->Phase()==ECastPhase::Ready && !Rune->IsActive() && !PC->bShowMouseCursor);
                Eyes->SetWorldRotation((G->CrateMesh()->GetComponentLocation()-Eyes->GetComponentLocation()).Rotation());
                Tap(EKeys::Two); Delay=.65f;break;
            }
            case 10: Tap(EKeys::LeftMouseButton); Delay=.35f; break;
            case 11:
                Shot(TEXT("02b-WindRelease"));
                if(FParse::Param(FCommandLine::Get(),TEXT("RuneWindTrace")))
                {
                    auto* Wind=CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("NatureTornado")));
                    auto* Cache=NewObject<UNiagaraSimCache>();
                    if(Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),Wind) && Cache->WriteFrame(Wind) && Cache->EndWrite())
                    {
                        for(FName Name:Cache->GetEmitterNames())
                        {
                            TArray<FVector> P,Scale;
                            TArray<FLinearColor> C;
                            Cache->ReadPositionAttribute(P,TEXT("Position"),Name);
                            if(P.IsEmpty())continue;
                            Cache->ReadVectorAttribute(Scale,TEXT("Scale"),Name);
                            Cache->ReadColorAttribute(C,TEXT("Color"),Name);
                            UE_LOG(LogTemp,Display,TEXT("NATURE_WIND_FRAME %s count=%d xyz=%s scale=%s color=%s"),*Name.ToString(),P.Num(),*P[0].ToString(),Scale.Num()?*Scale[0].ToString():TEXT("-"),C.Num()?*C[0].ToString():TEXT("-"));
                        }
                    }
                }
                Test.TestTrue(TEXT("Original wind end pose and native tornado on release"),ClipIs(TEXT("2h_Beam_End_Retarget")) && CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("NatureTornado")))->IsActive());
                Delay=1.4f;break;
            case 12:
                Test.TestTrue(TEXT("Wind finishes in first person with 2 turns of lift"),G->IsFirstPerson() && G->Selection()==ERuneAction::Levitate &&
                    G->Phase()==ECastPhase::Ready && !Rune->IsActive() && Hands->IsVisible() && B.SP==5 && B.Wind==2 && B.Box.Z==1);
                Tap(EKeys::F);break;
            case 13:
                Test.TestTrue(TEXT("F restores the overview, cursor and board picking"),!G->IsFirstPerson() && PC->bShowMouseCursor);
                Shot(TEXT("03-Lifted"));break;
            case 14:
                Click(G->Cell({0,0,0})+FVector(0,0,92));
                Click(G->Cell({0,1,0})+FVector(0,0,2)); Delay=.5f;break;
            case 15:
                Test.TestTrue(TEXT("Click to move and face destination"),B.Player==FIntPoint(0,1) && B.Facing==FIntPoint(0,1) && B.Turn==2 && B.SP==4 && B.Wind==1);
                Tap(EKeys::D);break;
            case 16: Tap(EKeys::One);break;
            case 17: Tap(EKeys::Enter);break;
            case 18:
                Test.TestTrue(TEXT("Pulse waits for left mouse, Enter never fires"),G->Phase()==ECastPhase::Ready && B.SP==4);
                Key(EKeys::LeftMouseButton);Delay=.21f;break;
            case 19:
                Test.TestTrue(TEXT("Electric has an independent starting pose"),G->Phase()==ECastPhase::Start && ClipIs(TEXT("R_Beam_Start_Retarget")));
                Test.TestTrue(TEXT("Electric charge audio selected"),VoiceIs(TEXT("CastingOneShot"),TEXT("Electric01_Charge")));
                Shot(TEXT("04-Charge"));break;
            case 20:
                if(G->BeamProgress()<.28f) return false;
                Test.TestTrue(TEXT("Electric trail and hit loops selected"),VoiceIs(TEXT("CastingLoop"),TEXT("Electric01_TrailLoop")) && VoiceIs(TEXT("ContactLoop"),TEXT("Electric01_HitLoop")));
                Shot(TEXT("05-BeamProgress"));
                Test.TestTrue(TEXT("Continuous partial hit has not spent a turn"),G->IsFiring() && ClipIs(TEXT("R_Beam_Loop_Retarget")) && B.Turn==2);
                break;
            case 21: Eyes->AddWorldRotation(FRotator(0,180,0)); break;
            case 22:
                Test.TestTrue(TEXT("Losing target clears dwell"),G->BeamProgress()==0 && B.SP==4);
                Key(EKeys::LeftMouseButton,IE_Released);Delay=1.55f;break;
            case 23:
                Test.TestTrue(TEXT("Early release is free and leaves the beam equipped"),G->IsFirstPerson() && G->Selection()==ERuneAction::Pulse &&
                    G->Phase()==ECastPhase::Ready && B.SP==4 && B.Turn==2);
                Eyes->SetWorldRotation((G->CrateMesh()->GetComponentLocation()-Eyes->GetComponentLocation()).Rotation());
                break;
            case 24: Tap(EKeys::One);break;
            case 25: Key(EKeys::LeftMouseButton);Delay=.95f;break;
            case 26:
                Test.TestTrue(TEXT("Charge time is excluded from the contact timer"),G->BeamProgress()>0 && G->BeamProgress()<1 && B.Turn==2);
                Delay=.8f;break;
            case 27:
                Test.TestTrue(TEXT("One continuous hit pushes east, drops to goal and charges 2 SP"),B.IsRestored() && B.Turn==3 && B.SP==2 && B.Wind==0 && G->IsFiring());
                Key(EKeys::LeftMouseButton,IE_Released);Delay=.32f;break;
            case 28: Test.TestTrue(TEXT("Right-hand end pose releases the beam"),ClipIs(TEXT("R_Beam_End_Retarget")) && !G->IsFiring() && VoiceIs(TEXT("CastingOneShot"),TEXT("Electric01_TrailEnd")));Shot(TEXT("06-End"));Delay=1.35f;break;
            case 29:
                Test.TestTrue(TEXT("A winning release keeps the caster in first person"),G->IsFirstPerson() &&
                    G->Selection()==ERuneAction::Pulse && G->Phase()==ECastPhase::Ready && B.IsRestored());
                Tap(EKeys::Two);break;
            case 30:
                Test.TestTrue(TEXT("The player can still change equipped hands on victory without paying another turn"),
                    G->IsFirstPerson() && G->Selection()==ERuneAction::Levitate && G->Phase()==ECastPhase::Ready &&
                    B.IsRestored() && B.Turn==3 && B.SP==2);
                Tap(EKeys::F);break;
            case 31: Shot(TEXT("07-Won"));break;
            case 32: Tap(EKeys::Z);break;
            case 33: Test.TestTrue(TEXT("Undo push restores wind, crate, facing and SP"),B.Box==FIntVector(1,1,1) && B.Facing==FIntPoint(1,0) && B.Wind==1 && B.SP==4);Tap(EKeys::Z);break;
            case 34: Test.TestTrue(TEXT("Undo move restores starting cell and lift"),B.Player==FIntPoint(0,0) && B.Wind==2 && B.SP==5);Tap(EKeys::Z);break;
            case 35: Test.TestTrue(TEXT("Undo lift restores initial world"),B.Box==FIntVector(1,1,0) && B.Turn==0 && B.SP==8 && !G->HasHistory());return true;
            }
            Next=Now+Delay; Deadline=Next+4.; ++Step; return false;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShengLuPlayTest,"ResonantRunes.ShengLu.Playthrough",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)

bool FShengLuPlayTest::RunTest(const FString&)
{
    for (auto& Context:GEngine->GetWorldContexts())
    {
        UWorld* World=Context.World();
        if (!World || !World->IsGameWorld()) continue;
        auto* PC=World->GetFirstPlayerController();
        auto* Game=PC? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
        if (!Game) continue;
        int32 BoardCount=0;
        for (TActorIterator<AShengLuBoard> It(World); It; ++It) ++BoardCount;
        TestEqual(TEXT("Exactly one playable board in the map"),BoardCount,1);
        auto* Hero=Cast<USkeletalMeshComponent>(Game->GetDefaultSubobjectByName(TEXT("Manny")));
        TestTrue(TEXT("Paragon Drongo loaded"),Hero && Hero->GetSkeletalMeshAsset() && Hero->GetSkeletalMeshAsset()->GetName()==TEXT("Drongo_GDC"));
        TestTrue(TEXT("Gameplay HUD installed"),PC->GetHUD() && PC->GetHUD()->IsA<AShengLuHUD>());
        FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FShengLuPlaythrough(*this,Game)));
        return true;
    }
    AddError(TEXT("Run in the ShengLu game world")); return false;
}
#endif
