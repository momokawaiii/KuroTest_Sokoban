#if WITH_DEV_AUTOMATION_TESTS
#include "ShengLuGame.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "NiagaraComponent.h"

namespace
{
    class FShengLuCastChain final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TWeakObjectPtr<AShengLuBoard> Board;
        int32 Step=0;
        double Next=0, Deadline=0;
        FTransform AimBeforeSwitch;
        float FovBeforeSwitch=0;

    public:
        FShengLuCastChain(FAutomationTestBase& InTest, AShengLuBoard* Game) : Test(InTest), Board(Game) {}

        bool Update() override
        {
            auto* Game=Board.Get();
            if (!Game) { Test.AddError(TEXT("Board disappeared")); return true; }
            const double Now=Game->GetWorld()->GetTimeSeconds();
            if (Now<Next) return false;
            if (Deadline && Now>Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Cast chain stalled at step %d, phase %d, turn %d, SP %d, contact %.2f"),
                    Step,int32(Game->Phase()),Game->Board().Turn,Game->Board().SP,Game->BeamProgress()));
                return true;
            }
            auto* PC=CastChecked<APlayerController>(Game->GetController());
            auto* Eyes=CastChecked<UCameraComponent>(Game->GetDefaultSubobjectByName(TEXT("CastCamera")));
            auto* Hands=CastChecked<USkeletalMeshComponent>(Game->GetDefaultSubobjectByName(TEXT("CastHands")));
            auto* Tornado=CastChecked<UNiagaraComponent>(Game->GetDefaultSubobjectByName(TEXT("NatureTornado")));
            auto* Range=CastChecked<UNiagaraComponent>(Game->GetDefaultSubobjectByName(TEXT("WindRange")));
            auto Key=[&](FKey K,EInputEvent Event)
            {
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,Event,Event==IE_Pressed?1.f:0.f));
            };
            auto Tap=[&](FKey K){ Key(K,IE_Pressed); Key(K,IE_Released); };
            const auto& State=Game->Board();
            const bool InFirstPerson=Eyes->IsActive() && Hands->IsVisible() && !PC->bShowMouseCursor;
            float Delay=.1f;
            switch (Step)
            {
            case 0:
                Game->ResetPuzzle();
                if (Game->IsFirstPerson()) Tap(EKeys::F);
                Test.TestTrue(TEXT("Set up an adjacent firing lane"),Game->Apply(ERuneAction::North));
                Delay=.5f; break;
            case 1: Tap(EKeys::D); break;
            case 2: Tap(EKeys::Two); Tap(EKeys::Enter); break;
            case 3:
                Test.TestTrue(TEXT("Selecting wind and pressing the old Enter key leaves the preview free"),InFirstPerson && Game->Preview().IsValid() && State.SP==7 && State.Turn==1);
                Tap(EKeys::LeftMouseButton); Delay=.06f; break;
            case 4:
                Test.TestTrue(TEXT("Lift is committed while the box is still rising"),InFirstPerson && Game->IsAnimating() &&
                    Game->Phase()==ECastPhase::End && State.SP==4 && State.Turn==2 && State.Box.Z==1 && State.Wind==2);
                AimBeforeSwitch=Eyes->GetComponentTransform();
                FovBeforeSwitch=Eyes->FieldOfView;
                Tap(EKeys::One); Delay=.06f; break;
            case 5:
                Test.TestTrue(TEXT("Switch during lift preserves the exact view, the paid tornado and the remaining turns"),
                    InFirstPerson && Eyes->GetComponentTransform().Equals(AimBeforeSwitch,.001f) && Eyes->FieldOfView==FovBeforeSwitch &&
                    Game->IsAnimating() && Game->Selection()==ERuneAction::Pulse && Game->Phase()==ECastPhase::Ready &&
                    Tornado->IsActive() && !Range->IsActive() && State.Wind==2 && State.SP==4);
                Delay=.45f; break;
            case 6:
                Eyes->SetWorldRotation((Game->CrateMesh()->GetComponentLocation() - Eyes->GetComponentLocation()).Rotation());
                break;
            case 7:
                Test.TestTrue(TEXT("Lifted box can be targeted from the same cell"),Game->Preview().IsValid());
                Key(EKeys::LeftMouseButton,IE_Pressed); break;
            case 8:
                if (Game->BeamProgress()<.18f) return false;
                Test.TestTrue(TEXT("Partial contact is still free"),InFirstPerson && Game->IsFiring() && State.Turn==2);
                Tap(EKeys::Two); break;
            case 9:
                Test.TestTrue(TEXT("Switching away from a held beam clears its contact and starts wind"),
                    InFirstPerson && Game->Selection()==ERuneAction::Levitate && Game->Phase()==ECastPhase::Start &&
                    !Game->IsFiring() && !Game->ActiveContact() && Game->BeamProgress()==0 && Tornado->IsActive() && State.SP==4);
                Tap(EKeys::One); Delay=.8f; break;
            case 10:
                Test.TestTrue(TEXT("Returning to pulse with the old button still down cannot fire it"),
                    InFirstPerson && Game->Selection()==ERuneAction::Pulse && Game->Phase()==ECastPhase::Ready &&
                    !Game->IsFiring() && Game->BeamProgress()==0 && State.Turn==2);
                Key(EKeys::LeftMouseButton,IE_Released); break;
            case 11: Key(EKeys::LeftMouseButton,IE_Pressed); break;
            case 12:
                if (State.Turn<3) return false;
                Test.TestTrue(TEXT("A fresh continuous beam pushes once after wind without leaving first person"),
                    InFirstPerson && State.Box==FIntVector(2,1,1) && State.Wind==1 && State.SP==2 && State.Turn==3);
                Key(EKeys::LeftMouseButton,IE_Released); break;
            case 13:
                if (Game->Phase()!=ECastPhase::Ready) return false;
                Test.TestTrue(TEXT("Natural recovery leaves pulse equipped"),InFirstPerson && Game->Selection()==ERuneAction::Pulse);
                Tap(EKeys::Z); break;
            case 14:
                Test.TestTrue(TEXT("Undo keeps the view and restores only the paid push"),InFirstPerson && State.Turn==2 &&
                    State.SP==4 && State.Box==FIntVector(1,1,1) && State.Wind==2 && !Game->IsFiring());
                Tap(EKeys::R); break;
            case 15:
                Test.TestTrue(TEXT("Retry retains first person and refunds the board"),InFirstPerson && State.SP==8 && State.Turn==0);
                Tap(EKeys::Two); break;
            case 16: Tap(EKeys::Escape); break;
            case 17:
                Test.TestTrue(TEXT("Cancel keeps the selected wind in first person and costs nothing"),
                    InFirstPerson && Game->Selection()==ERuneAction::Levitate && Game->Phase()==ECastPhase::Ready && State.SP==8);
                Tap(EKeys::Two); break;
            case 18:
                Test.TestTrue(TEXT("The same wind key starts a fresh charge"),InFirstPerson && Game->Phase()==ECastPhase::Start);
                Tap(EKeys::F); break;
            case 19:
                Test.TestTrue(TEXT("F exits even while charging and restores board interaction for free"),!Eyes->IsActive() &&
                    !Hands->IsVisible() && PC->bShowMouseCursor && !Game->Selection() && !Range->IsActive() && State.SP==8 && State.Turn==0);
                return true;
            }
            Next=Now+Delay; Deadline=Next+4.; ++Step;
            return false;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShengLuCastChainTest,"ResonantRunes.ShengLu.CastChaining",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)

bool FShengLuCastChainTest::RunTest(const FString&)
{
    for (const auto& Context:GEngine->GetWorldContexts())
    {
        auto* World=Context.World();
        if (!World || !World->IsGameWorld()) continue;
        auto* PC=World->GetFirstPlayerController();
        if (auto* Game=PC?Cast<AShengLuBoard>(PC->GetPawn()):nullptr)
        {
            FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FShengLuCastChain(*this,Game)));
            return true;
        }
    }
    AddError(TEXT("Run in the ShengLu game world")); return false;
}
#endif
