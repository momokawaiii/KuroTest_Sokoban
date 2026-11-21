#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "PuzzleAuthoring.h"
#include "ShengLuGame.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleWindPlatformTest, "ResonantRunes.Puzzle.Editor.FifthLevelWindRoute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
    const FIntVector SpawnCell(0, 0, 0);
    const FIntVector Approach(0, 1, 0);
    const FIntVector Platform(0, 2, 1);
    const FIntVector Up(0, 0, 1);
    const FIntVector East(1, 0, 0);

    class FPlayFifthLevelWindRoute final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        const FGuid LevelId;
        const double Deadline = FPlatformTime::Seconds() + 45.;
        FVector ApproachWindCenter = FVector::ZeroVector;
        bool bViewportReady = false;
        bool bVerifiedLandingUndo = false;
        enum class EStep { Start, LiftInPlace, LiftBox, AnchorBox, ArmPulse, PushForward, ClearWind, ReturnToBoard,
            WalkNorth, TryHighClick, LiftToPlatformHeight, ShowPlatform, StepOntoPlatform, Hiss, Finish, Stop };
        EStep Step = EStep::Start;

        void Stop()
        {
            AShengLuGameMode::SetPIELevel(nullptr);
            if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
        }

    public:
        FPlayFifthLevelWindRoute(FAutomationTestBase& InTest, FGuid InLevelId) : Test(InTest), LevelId(InLevelId) {}

        bool Update() override
        {
            if (Step == EStep::Stop) return GEditor->PlayWorld == nullptr;
            auto* PC = GEditor->PlayWorld ? GEditor->PlayWorld->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Fifth-level route timed out at stage %d"), int32(Step)));
                Stop();
                return false;
            }
            if (!Board || !Board->HasActorBegunPlay() || Board->Level().LevelId != LevelId || Board->IsAnimating()) return false;

            const auto Tap = [PC](FKey Key)
            {
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.f));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.f));
            };
            auto* Eyes = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("CastCamera")));
            const auto* Wind = CastChecked<USceneComponent>(Board->GetDefaultSubobjectByName(TEXT("PlayerWind")));
            const auto WindTagCount = [Board]
            {
                const auto Tag = FGameplayTag::RequestGameplayTag(TEXT("Puzzle.Player.Levitating"));
                return Board->GetAbilitySystemComponent()->GetGameplayTagCount(Tag);
            };
            const auto AimAt = [Eyes](FVector Point)
            {
                Eyes->SetWorldRotation((Point - Eyes->GetComponentLocation()).Rotation());
            };
            const auto TileScreen = [PC, Board](FIntVector Cell) -> TOptional<FVector2D>
            {
                const auto& Level = Board->Level();
                const FVector Center = Board->Cell(Cell);
                const float Margin = Level.GridCellSize * .3f;
                // Use the terrain hit that drives ClickAt. The hover inspector also picks floating goal markers.
                for (float X : {0.f, -Margin, Margin})
                    for (float Y : {0.f, -Margin, Margin})
                    {
                        FVector2D Screen;
                        FHitResult Hit;
                        if (PC->ProjectWorldLocationToScreen(Center + FVector(X, Y, 0), Screen) &&
                            PC->GetHitResultAtScreenPosition(Screen, ECC_Visibility, false, Hit) &&
                            Hit.GetActor() == Board && Cast<UInstancedStaticMeshComponent>(Hit.GetComponent()) &&
                            Hit.ImpactNormal.Z > .5 && Level.WorldToGrid(Hit.ImpactPoint) == Cell)
                            return Screen;
                    }
                return {};
            };
            const auto Commit = [&](FPuzzleAction Action, EStep Next)
            {
                if (Board->TryPuzzleAction(Action)) Step = Next;
                else
                {
                    Test.AddError(TEXT("The recorded fifth-level action was rejected. ") + Board->Message());
                    Stop();
                }
            };
            const auto& State = Board->Runtime();
            switch (Step)
            {
            case EStep::Start:
                if (!bViewportReady && FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen")))
                {
                    auto* GameViewport = Board->GetWorld()->GetGameViewport();
                    if (!GameViewport || !GameViewport->GetGameViewport()) return false;
                    GameViewport->GetGameViewport()->SetFixedViewportSize(1600, 900);
                    bViewportReady = true;
                    return false;
                }
                if (!Test.TestEqual(TEXT("Selected fifth-level spawn"), State.PlayerPosition, SpawnCell)) { Stop(); break; }
                Tap(EKeys::Two);
                Step = EStep::LiftInPlace;
                break;
            case EStep::LiftInPlace:
            case EStep::LiftToPlatformHeight:
                if (Board->Selection() != ERuneAction::Levitate) return false;
                AimAt(Board->Cell(State.PlayerPosition));
                if (!Board->AimedAtSelf()) return false;
                Board->ClickBoard();
                Step = Step == EStep::LiftInPlace ? EStep::LiftBox : EStep::ShowPlatform;
                break;
            case EStep::LiftBox:
                Test.TestTrue(TEXT("Slot 2 raised the player in place for free"),
                    State.PlayerPosition == SpawnCell + Up && State.SP == Board->Level().InitialSP && State.PlayerWindTurns == 4);
                Test.TestTrue(TEXT("Self lift grants one player levitation tag and anchors the wind at the launch cell"),
                    WindTagCount() == 1 && Wind->IsActive() && Wind->GetComponentLocation().Equals(Board->Cell(SpawnCell) + FVector(0, 0, 100)));
                Commit({ERuneAction::Levitate, 1}, EStep::AnchorBox);
                break;
            case EStep::AnchorBox:
                Commit({ERuneAction::WaterAnchor, 1}, EStep::ArmPulse);
                break;
            case EStep::ArmPulse:
                Tap(EKeys::One);
                Step = EStep::PushForward;
                break;
            case EStep::PushForward:
                if (Board->Selection() != ERuneAction::Pulse) return false;
                AimAt(Board->BoxMesh(1)->GetComponentLocation());
                if (Board->AimBoxId() != 1 || !Board->Preview().IsValid()) return false;
                Test.TestEqual(TEXT("Looking forward while floating previews an eastward push"), Board->PulseDirection(), East);
                Board->ClickBoard();
                Step = EStep::ClearWind;
                break;
            case EStep::ClearWind:
                if (State.FindBox(1)->GridPosition != FIntVector(2, 0, 1)) return false;
                Test.TestTrue(TEXT("First-person Pulse pushes the box while the player stays airborne in the same cell"),
                    State.PlayerPosition == SpawnCell + Up && State.PlayerWindTurns == 1);
                Board->ReleaseClick();
                Tap(EKeys::G);
                Step = EStep::ReturnToBoard;
                break;
            case EStep::ReturnToBoard:
                if (State.PlayerWindTurns != 0) return false;
                Test.TestTrue(TEXT("G removes the player's levitation tag and wind"), WindTagCount() == 0 && !Wind->IsActive());
                Tap(EKeys::F);
                Step = EStep::WalkNorth;
                break;
            case EStep::WalkNorth:
            {
                if (Board->IsFirstPerson()) return false;
                if (!Board->IsPawnSelected()) { Tap(EKeys::SpaceBar); return false; }
                const auto Mouse = TileScreen(Approach);
                if (!Mouse) return false;
                Board->ClickAt(*Mouse);
                if (!Test.TestEqual(TEXT("Walk to the platform approach from the overview"), State.PlayerPosition, Approach))
                { Stop(); break; }
                Step = EStep::TryHighClick;
                break;
            }
            case EStep::TryHighClick:
            case EStep::StepOntoPlatform:
            {
                if (Board->IsFirstPerson()) return false;
                if (!Board->IsPawnSelected()) { Tap(EKeys::SpaceBar); return false; }
                const bool bLifted = Step == EStep::StepOntoPlatform;
                const auto Mouse = TileScreen(Platform);
                if (!Mouse) return false;
                TOptional<FVector2D> PlayerScreen;
                if (bLifted)
                {
                    const auto* Player = CastChecked<UCapsuleComponent>(Board->GetDefaultSubobjectByName(TEXT("MannyPick")));
                    const float Offset = Player->GetScaledCapsuleHalfHeight() * .45f;
                    for (float Z : {0.f, -Offset, Offset})
                    {
                        FVector2D Screen;
                        FHitResult Hit;
                        if (PC->ProjectWorldLocationToScreen(Player->GetComponentLocation() + FVector(0, 0, Z), Screen) &&
                            PC->GetHitResultAtScreenPosition(Screen, ECC_Visibility, false, Hit) && Hit.GetComponent() == Player)
                        {
                            PlayerScreen = Screen;
                            break;
                        }
                    }
                    if (!PlayerScreen) return false;
                }
                Test.TestEqual(TEXT("The northern platform is reachable only from its own layer"),
                    Board->StepDestination(ERuneAction::North) == Platform, bLifted);
                if (bLifted)
                {
                    Test.TestTrue(TEXT("Space selects airborne landing points while the player has the tag"), WindTagCount() == 1);
                    Board->Cancel();
                    Board->ClickAt(*PlayerScreen);
                    Test.TestTrue(TEXT("Clicking the airborne character also selects the landing points"),
                        Board->IsPawnSelected() && Board->StepDestination(ERuneAction::North) == Platform);
                }
                const int32 BeforeSP = State.SP, BeforeTurn = State.Turn;
                Board->ClickAt(*Mouse);
                Test.TestEqual(TEXT("Clicking the platform uses the previewed position"), State.PlayerPosition, bLifted ? Platform : Approach);
                Test.TestTrue(TEXT("Only the successful horizontal step consumes SP and time"),
                    State.SP == BeforeSP - int32(bLifted) && State.Turn == BeforeTurn + int32(bLifted));
                if (bLifted)
                {
                    Test.TestTrue(TEXT("The landing clears the tag and stops wind at its source before the movement animation"),
                        State.PlayerWindTurns == 0 && WindTagCount() == 0 && !Wind->IsActive() &&
                        Wind->GetComponentLocation().Equals(ApproachWindCenter));
                    Step = EStep::Hiss;
                }
                else
                {
                    Tap(EKeys::Two);
                    Step = EStep::LiftToPlatformHeight;
                }
                break;
            }
            case EStep::ShowPlatform:
                Test.TestEqual(TEXT("The second lift reaches the neighbouring Z1 cell"), State.PlayerPosition, Approach + Up);
                ApproachWindCenter = Board->Cell(Approach) + FVector(0, 0, 100);
                Test.TestTrue(TEXT("The field stays at the foot of the launch column"),
                    Wind->GetComponentLocation().Equals(ApproachWindCenter));
                Tap(EKeys::F);
                Step = EStep::StepOntoPlatform;
                break;
            case EStep::Hiss:
                Test.TestTrue(TEXT("The player stays on the pedestal without the tag and the field does not follow the movement"),
                    State.PlayerPosition == Platform && WindTagCount() == 0 && !Wind->IsActive() &&
                    Wind->GetComponentLocation().Equals(ApproachWindCenter));
                if (!bVerifiedLandingUndo)
                {
                    Board->Undo();
                    Test.TestTrue(TEXT("Undo restores the tag, the airborne cell, and the source field"),
                        State.PlayerPosition == Approach + Up && WindTagCount() == 1 && Wind->IsActive() &&
                        Wind->GetComponentLocation().Equals(ApproachWindCenter));
                    bVerifiedLandingUndo = true;
                    Step = EStep::StepOntoPlatform;
                    break;
                }
                Commit({ERuneAction::Hiss, 2}, EStep::Finish);
                break;
            case EStep::Finish:
                if (Test.TestTrue(TEXT("Push the Hiss box through the aerial obstruction"), Board->TryPuzzleAction({ERuneAction::Pulse, 2, East})))
                {
                    Test.TestTrue(TEXT("Both saved fifth-level goals are met"), Board->IsRestored());
                    Test.TestEqual(TEXT("The full route leaves 2 SP"), State.SP, 2);
                }
                Stop();
                break;
            case EStep::Stop: break;
            }
            return false;
        }
    };
}

bool FPuzzleWindPlatformTest::RunTest(const FString&)
{
    auto* Catalog = LoadObject<UPuzzleLevelCatalog>(nullptr, UPuzzleLevelCatalog::AssetPath);
    if (!TestNotNull(TEXT("The saved campaign"), Catalog)) return false;
    const FGuid LevelId(0x6CC9B521, 0x4A726C53, 0xCFBB4EA0, 0xB858D29F);
    auto* Level = Catalog->FindLevel(LevelId);
    if (!TestNotNull(TEXT("The authored wind-platform level"), Level)) return false;
    FPuzzleAuthoring Editor(Catalog);
    Editor.SetActive(Level);
    if (!TestTrue(TEXT("Start the selected asset in PIE"), Editor.TestLevel())) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPlayFifthLevelWindRoute>(*this, LevelId));
    return true;
}

#endif
