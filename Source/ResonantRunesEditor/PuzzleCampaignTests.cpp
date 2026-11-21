#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "PuzzleAuthoring.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ShengLuGame.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleCampaignTest, "ResonantRunes.Puzzle.Editor.CampaignTravel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleSelectedTravelTest, "ResonantRunes.Puzzle.Editor.SelectedLevelTravel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
    class FPlayCampaign final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        FGuid First, Second, Third, Fourth;
        const double Deadline = FPlatformTime::Seconds() + 90.;
        double IntroFrame = 0.;
        double ResultFrame = 0.;
        double PoseCaptureUntil = 0.;
        FVector EyeStart, HeroStart;
        FQuat LookStart;
        FTransform WinningView;
        bool bSawStrafe = false;
        enum class EStep { Start, CheckMovementSelection, CheckSpace, CheckLocks, Locked, TryBlockedStep, StartStrafe, TrackStrafe, TrackForward, VerifyOverview,
            AimNorth, PushNorth, WalkWestOfBox, AimEast, PushEast, Win,
            EnterSecond, WindAim, WindRelease, WalkSecond, TurnEast, AimSecond, PushSecond, EnterThird,
            StartThird, AimSelf, VerifyFirstLift, AirborneHands, VerifySecondLift, TryFlightMovement, DispelFlight, VerifyDispel, VerifyUndo,
            ArmWaterThird, AimWaterThird, WalkUnderThird, AimUpThird, PushUpThird, EnterFourth, CheckFourth, CheckFourthLocks,
            CheckFourthHiss, Stop };
        EStep Step;

        void Stop()
        {
            if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
        }

        static void Shot(const TCHAR* Name)
        {
            if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
                    TEXT("PuzzleVerification") / Name), false, false);
        }

        bool CapturePose(const UWorld& World, const TCHAR* Name)
        {
            if (!FParse::Param(FCommandLine::Get(), TEXT("RuneScreens"))) return true;
            const double Now = World.GetRealTimeSeconds();
            if (PoseCaptureUntil == 0.)
            {
                Shot(Name);
                PoseCaptureUntil = Now + .2;
            }
            if (Now < PoseCaptureUntil) return false;
            PoseCaptureUntil = 0.;
            return true;
        }

    public:
        FPlayCampaign(FAutomationTestBase& InTest, FGuid InFirst, FGuid InSecond, FGuid InThird, FGuid InFourth, bool bFromSelected)
            : Test(InTest), First(InFirst), Second(InSecond), Third(InThird), Fourth(InFourth),
              Step(bFromSelected ? EStep::WindAim : EStep::Start) {}

        bool Update() override
        {
            if (Step == EStep::Stop) return GEditor->PlayWorld == nullptr;
            UWorld* World = GEditor->PlayWorld;
            auto* PC = World ? World->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Campaign timed out in step %d, board=%s, turn=%d, aiming at=%d"),
                    int32(Step), Board ? *Board->Level().DisplayName.ToString() : TEXT("none"),
                    Board ? Board->Runtime().Turn : -1, Board ? Board->AimBoxId() : -1));
                Stop();
                return false;
            }
            if (!Board || !Board->HasActorBegunPlay()) return false;
            auto Key = [PC](FKey K, EInputEvent Event)
            { PC->InputKey(FInputKeyEventArgs::CreateSimulated(K, Event, Event == IE_Pressed ? 1.f : 0.f)); };
            auto Tap = [&](FKey K) { Key(K, IE_Pressed); Key(K, IE_Released); };
            auto* Camera = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("CastCamera")));
            auto* Hero = CastChecked<USkeletalMeshComponent>(Board->GetDefaultSubobjectByName(TEXT("Manny")));
            auto* Hands = CastChecked<USkeletalMeshComponent>(Board->GetDefaultSubobjectByName(TEXT("CastHands")));
            const auto PlayingHands = [Hands](const TCHAR* Clip, bool bLooping)
            {
                const auto* Anim = Hands->GetSingleNodeInstance();
                return Hands->IsVisible() && Anim && Anim->IsPlaying() && Anim->IsLooping() == bLooping &&
                    Anim->GetAnimationAsset() && Anim->GetAnimationAsset()->GetName().EndsWith(Clip);
            };
            auto Aim = [Board, Camera]
            {
                Camera->SetWorldRotation((Board->BoxMesh(1)->GetComponentLocation() - Camera->GetComponentLocation()).Rotation());
                return Board->AimBoxId() == 1 && Board->Preview().IsValid();
            };
            const auto CaptureStepStart = [&]
            {
                EyeStart = Camera->GetComponentLocation();
                LookStart = Camera->GetComponentQuat();
                HeroStart = Hero->GetComponentLocation();
            };
            const auto CheckStep = [&]
            {
                Test.TestTrue(TEXT("The camera moves by the same amount as the third-person mesh"),
                    (Camera->GetComponentLocation() - EyeStart).Equals(Hero->GetComponentLocation() - HeroStart, 1.));
                Test.TestTrue(TEXT("A first-person step preserves the look rotation"), Camera->GetComponentQuat().Equals(LookStart));
            };
            const auto CheckWinningView = [&]
            {
                Test.TestTrue(TEXT("Victory leaves the same first-person camera and look direction"),
                    Board->IsFirstPerson() && Camera->GetComponentTransform().Equals(WinningView));
            };
            const auto& Level = Board->Level();
            const auto& State = Board->Runtime();
            const auto AimAtFeet = [&]
            {
                Camera->SetWorldRotation((Board->Cell(State.PlayerPosition) - Camera->GetComponentLocation()).Rotation());
                return Board->AimedAtSelf() && Board->Preview().IsValid();
            };
            auto Granted = [Board] { return Board->FindComponentByClass<UAbilitySystemComponent>()->GetActivatableAbilities().Num(); };
            switch (Step)
            {
            case EStep::Start:
                if (IntroFrame == 0.)
                {
                    IntroFrame = FPlatformTime::Seconds() + 1.5;
                    if (auto* Viewport = World->GetGameViewport()->GetGameViewport()) Viewport->SetFixedViewportSize(1600, 900);
                }
                if (FPlatformTime::Seconds() < IntroFrame) return false;
                if (!Test.TestEqual(TEXT("The shared map boots the first catalog level"), Level.LevelId, First)) { Stop(); break; }
                Test.TestEqual(TEXT("The first level has a single editing layer"), Level.GridDimensions, FIntVector(4, 3, 1));
                Test.TestEqual(TEXT("The player starts at the authored grid cell"), State.PlayerPosition, FIntVector(0, 1, 0));
                Test.TestEqual(TEXT("Initial budget"), State.SP, 9);
                Test.TestTrue(TEXT("The entry prompt teaches slot 1"), Level.EntryTip.ToString().Contains(TEXT("按 1")));
                Test.TestEqual(TEXT("Only Pulse is granted"), Granted(), 1);
                Test.TestTrue(TEXT("The visible overview model is the project's Paragon Drongo"), Hero->IsVisible() &&
                    Hero->GetSkeletalMeshAsset() && Hero->GetSkeletalMeshAsset()->GetName() == TEXT("Drongo_GDC"));
                Test.TestTrue(TEXT("The overview keeps pointer input with the native cursor suppressed"),
                    PC->bShowMouseCursor && PC->GetMouseCursor() == EMouseCursor::None);
                Shot(TEXT("RuneHUD-Overview.png"));
                Step = EStep::CheckMovementSelection;
                break;
            case EStep::CheckMovementSelection:
            {
                FVector2D Screen;
                const FVector TilePoint = Board->Cell(State.PlayerPosition) + FVector(0, -.32f * Level.GridCellSize, 0);
                if (!Test.TestTrue(TEXT("A point beside the model projects onto the player tile"), PC->ProjectWorldLocationToScreen(TilePoint, Screen)))
                { Stop(); break; }
                FHitResult Hit;
                Test.TestTrue(TEXT("The click hits the floor rather than the selection capsule"),
                    PC->GetHitResultAtScreenPosition(Screen, ECC_Visibility, false, Hit) && Hit.GetComponent() &&
                    Hit.GetComponent()->GetFName() == TEXT("PuzzleFloors"));
                Board->ClickAt(Screen);
                Test.TestTrue(TEXT("Clicking the occupied tile enables movement"), Board->IsPawnSelected());
                Board->Cancel();
                Tap(EKeys::SpaceBar);
                Step = EStep::CheckSpace;
                break;
            }
            case EStep::CheckSpace:
            {
                Test.TestTrue(TEXT("Space enables movement without spending SP or a turn"), Board->IsPawnSelected() && State.Turn == 0 && State.SP == 9);
                FVector2D Screen;
                if (!Test.TestTrue(TEXT("Project a reachable neighboring tile"), PC->ProjectWorldLocationToScreen(Board->Cell({0, 0, 0}), Screen)))
                { Stop(); break; }
                Board->ClickAt(Screen);
                Test.TestEqual(TEXT("After pressing Space, clicking a reachable tile moves the character"), State.PlayerPosition, FIntVector(0, 0, 0));
                Board->Undo();
                Step = EStep::CheckLocks;
                break;
            }
            case EStep::CheckLocks:
                Tap(EKeys::Two); Tap(EKeys::Three); Tap(EKeys::Four); Tap(EKeys::Enter);
                Step = EStep::Locked;
                break;
            case EStep::Locked:
                Test.TestFalse(TEXT("Locked keys never equip a skill"), Board->Selection().IsSet());
                Test.TestFalse(TEXT("A locked ability cannot be committed through Blueprint"), Board->TryPuzzleAction({ERuneAction::WaterAnchor, 1}));
                Test.TestFalse(TEXT("Cannot advance before winning"), Board->CanAdvanceLevel());
                Tap(EKeys::One);
                Step = EStep::TryBlockedStep;
                break;
            case EStep::TryBlockedStep:
                if (Board->Selection() != ERuneAction::Pulse || Board->AimBoxId() != 1) return false;
                Test.TestTrue(TEXT("Entering first person plays the original equip one-shot"), PlayingHands(TEXT("Equip_Retarget"), false));
                Tap(EKeys::W);
                Step = EStep::StartStrafe;
                break;
            case EStep::StartStrafe:
                Test.TestTrue(TEXT("W cannot walk into a box or charge SP"),
                    State.PlayerPosition == FIntVector(0, 1, 0) && State.Turn == 0 && State.SP == 9 && !Board->IsAnimating());
                CaptureStepStart();
                Tap(EKeys::A);
                Step = EStep::TrackStrafe;
                break;
            case EStep::TrackStrafe:
                if (State.Turn == 0) return false;
                if (Board->IsAnimating())
                {
                    if (!bSawStrafe && !Hero->GetComponentLocation().Equals(HeroStart, 1.))
                    {
                        bSawStrafe = true;
                        CheckStep();
                        Test.TestFalse(TEXT("The player model renders an intermediate position during the strafe"),
                            Hero->GetComponentLocation().Equals(Board->Cell(State.PlayerPosition), 1.));
                    }
                    return false;
                }
                Test.TestTrue(TEXT("A strafes a single grid step relative to the view and costs 1 SP"),
                    bSawStrafe && State.PlayerPosition == FIntVector(0, 0, 0) && State.Turn == 1 && State.SP == 8);
                CheckStep();
                CaptureStepStart();
                Tap(EKeys::W);
                Step = EStep::TrackForward;
                break;
            case EStep::TrackForward:
                if (State.Turn < 2 || Hero->GetComponentLocation().Equals(HeroStart, 1.)) return false;
                Test.TestTrue(TEXT("W moves forward relative to the view and costs 1 SP"),
                    State.PlayerPosition == FIntVector(1, 0, 0) && State.Turn == 2 && State.SP == 7);
                Test.TestTrue(TEXT("F is pressed during the step"), Board->IsAnimating());
                CheckStep();
                Tap(EKeys::F);
                Step = EStep::VerifyOverview;
                break;
            case EStep::VerifyOverview:
                if (Board->IsFirstPerson() || Board->IsAnimating()) return false;
                Test.TestTrue(TEXT("F reveals Drongo at the cell reached in first person"),
                    Hero->IsVisible() && Hero->GetComponentLocation().Equals(Board->Cell(State.PlayerPosition), 1.));
                Test.TestEqual(TEXT("Facing still follows the last eastward step"), State.Facing, FIntPoint(1, 0));
                Tap(EKeys::One);
                Step = EStep::AimNorth;
                break;
            case EStep::AimNorth:
            case EStep::AimEast:
                if (Board->Selection() != ERuneAction::Pulse || Board->AimBoxId() != 1) return false;
                if (!Test.TestTrue(*FString::Printf(TEXT("Native aim previews a push despite stale facing: %s"), *Board->Preview().Reason),
                    Board->Preview().IsValid())) { Stop(); break; }
                WinningView = Camera->GetComponentTransform();
                Key(EKeys::LeftMouseButton, IE_Pressed);
                Step = Step == EStep::AimNorth ? EStep::PushNorth : EStep::PushEast;
                break;
            case EStep::PushNorth:
                if (State.Turn < 3) return false;
                Key(EKeys::LeftMouseButton, IE_Released); Tap(EKeys::F);
                Test.TestEqual(TEXT("Aimed pulse pushes north from the south side"), State.FindBox(1)->GridPosition, FIntVector(1, 2, 0));
                Step = EStep::WalkWestOfBox;
                break;
            case EStep::WalkWestOfBox:
                if (Board->IsAnimating() || Board->IsFirstPerson()) return false;
                if (State.Turn < 6)
                {
                    if (!Test.TestTrue(TEXT("Walk to the west side"), Board->TryPuzzleAction({State.Turn == 3 ? ERuneAction::West : ERuneAction::North}))) Stop();
                    return false;
                }
                Test.TestEqual(TEXT("Reproduces the screenshot's position"), State.PlayerPosition, FIntVector(0, 2, 0));
                Test.TestEqual(TEXT("The screenshot has just enough SP for one pulse"), State.SP, 2);
                Test.TestEqual(TEXT("Facing is still north after walking"), State.Facing, FIntPoint(0, 1));
                Test.TestFalse(TEXT("The solver still rejects an explicit push to the north"),
                    Board->Evaluate({ERuneAction::Pulse, 1, {0, 1, 0}}).IsValid());
                Tap(EKeys::One);
                Step = EStep::AimEast;
                break;
            case EStep::PushEast:
                if (State.Turn < 7) return false;
                Key(EKeys::LeftMouseButton, IE_Released);
                Test.TestTrue(TEXT("Aiming at the crate from the screenshot position wins"), Board->IsRestored());
                Test.TestEqual(TEXT("The last pulse may spend the final 2 SP"), State.SP, 0);
                Test.TestEqual(TEXT("Target box reaches the normal-state Z0 goal"), State.FindBox(1)->GridPosition, FIntVector(2, 2, 0));
                Step = EStep::Win;
                break;
            case EStep::Win:
                if (!Board->CanAdvanceLevel()) return false;
                CheckWinningView();
                if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                {
                    if (ResultFrame == 0.) ResultFrame = World->GetRealTimeSeconds();
                    if (World->GetRealTimeSeconds() - ResultFrame < 1.25) return false;
                    Shot(TEXT("RuneHUD-ArrayRestored.png"));
                }
                Test.TestTrue(TEXT("Settlement enables the software pointer in the first-person view"),
                    PC->bShowMouseCursor && PC->GetMouseCursor() == EMouseCursor::None);
                Step = EStep::EnterSecond;
                break;
            case EStep::EnterSecond:
                if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                {
                    const FIntPoint ViewportSize = World->GetGameViewport()->GetGameViewport()->GetRenderTargetTextureSizeXY();
                    const float Scale = FMath::Min(ViewportSize.X / 1280.f, ViewportSize.Y / 720.f);
                    Board->ClickAt(FVector2D(ViewportSize) * .5 + FVector2D(0, 227 * Scale));
                }
                else Tap(EKeys::Enter);
                Step = EStep::WindAim;
                break;
            case EStep::WindAim:
                if (Level.LevelId != Second) return false;
                Test.TestEqual(TEXT("Next switches to fresh second-level SP"), State.SP, 8);
                Test.TestEqual(TEXT("Wind joins Pulse at level two"), Granted(), 2);
                Test.TestEqual(TEXT("The original second-level layout is loaded"), Level.GridDimensions, FIntVector(3, 3, 2));
                Test.TestFalse(TEXT("Water remains locked at level two"), RuneSkills::IsUnlocked(Level, ERuneAction::WaterAnchor));
                Test.TestTrue(TEXT("The second level teaches key 2"), Level.EntryTip.ToString().Contains(TEXT("按 2")));
                Test.TestFalse(TEXT("A pulse on the ground is blocked by the platform"),
                    Board->Evaluate({ERuneAction::Pulse, 1, {1, 0, 0}}).IsValid());
                Tap(EKeys::Two);
                Step = EStep::WindRelease;
                break;
            case EStep::WindRelease:
                if (Board->Selection() != ERuneAction::Levitate || !Aim()) return false;
                if (!PlayingHands(TEXT("2h_Beam_Loop_Retarget"), true)) return false;
                if (!CapturePose(*World, TEXT("FirstPerson-WindLoop.png"))) return false;
                Test.TestTrue(TEXT("Holding the original wind pose spends no SP or turns"), State.SP == 8 && State.Turn == 0);
                Tap(EKeys::LeftMouseButton);
                Step = EStep::WalkSecond;
                break;
            case EStep::WalkSecond:
            {
                if (State.Turn == 0 || Board->IsAnimating()) return false;
                Test.TestEqual(TEXT("Skill 2 lifts the first crate"), State.FindBox(1)->GridPosition, FIntVector(1, 1, 1));
                Tap(EKeys::F);
                if (State.PlayerPosition.Y != 1)
                {
                    const ERuneAction WalkToBoxRow = State.PlayerPosition.Y < 1 ? ERuneAction::North : ERuneAction::South;
                    if (!Test.TestTrue(TEXT("Approach the box row"), Board->TryPuzzleAction({WalkToBoxRow}))) { Stop(); break; }
                }
                Step = EStep::TurnEast;
                break;
            }
            case EStep::TurnEast:
                if (Board->IsAnimating() || Board->IsFirstPerson()) return false;
                Tap(EKeys::D); Tap(EKeys::One);
                Step = EStep::AimSecond;
                break;
            case EStep::AimSecond:
                if (Board->Selection() != ERuneAction::Pulse || !Aim()) return false;
                WinningView = Camera->GetComponentTransform();
                Key(EKeys::LeftMouseButton, IE_Pressed);
                Step = EStep::PushSecond;
                break;
            case EStep::PushSecond:
                if (State.FindBox(1)->GridPosition != FIntVector(2, 1, 1)) return false;
                Key(EKeys::LeftMouseButton, IE_Released);
                Test.TestTrue(TEXT("Arrival on the second-level platform restores the board immediately"), Board->IsRestored());
                if (State.Turn == 2)
                {
                    Test.TestEqual(TEXT("The winning box still has one wind turn remaining"), State.FindBox(1)->LevitationTurns, 1);
                    Test.TestEqual(TEXT("Wind and Pulse alone leave 3 SP"), State.SP, 3);
                }
                Step = EStep::EnterThird;
                break;
            case EStep::EnterThird:
                if (!Board->CanAdvanceLevel()) return false;
                CheckWinningView();
                Test.TestEqual(TEXT("The next entry is the authored third level"), Board->NextPuzzleLevel()->Definition.LevelId, Third);
                Shot(TEXT("SecondLevelClearFirstPerson.png"));
                Step = EStep::StartThird;
                Tap(EKeys::Enter);
                break;
            case EStep::StartThird:
                if (Level.LevelId != Third) return false;
                if (!Test.TestNotNull(TEXT("The third level has a successor"), Board->NextPuzzleLevel())) { Stop(); break; }
                Test.TestEqual(TEXT("The third level targets the fourth catalog entry"), Board->NextPuzzleLevel()->Definition.LevelId, Fourth);
                Test.TestTrue(TEXT("The third level teaches key 3 and requires an anchored crate"),
                    Level.EntryTip.ToString().Contains(TEXT("按 3")) && Level.Goals[0].bRequireWaterAnchor);
                Test.TestEqual(TEXT("The third level grants its configured skills"), Granted(), Level.AvailableSkills.Num());
                if (RuneSkills::IsUnlocked(Level, ERuneAction::Levitate))
                {
                    Tap(EKeys::Two);
                    Step = EStep::AimSelf;
                }
                else
                {
                    Test.TestFalse(TEXT("Self lift stays locked when slot 2 is omitted"), Board->Evaluate({ERuneAction::LevitateSelf}).IsValid());
                    Step = EStep::ArmWaterThird;
                }
                break;
            case EStep::AimSelf:
                if (Board->Selection() != ERuneAction::Levitate || !AimAtFeet()) return false;
                Test.TestEqual(TEXT("The foot ray previews one free layer"), Board->Preview().After.PlayerPosition, Level.PlayerStart + FIntVector(0, 0, 1));
                CaptureStepStart();
                Tap(EKeys::LeftMouseButton);
                Step = EStep::VerifyFirstLift;
                break;
            case EStep::VerifyFirstLift:
                if (State.Turn < 1 || Board->IsAnimating()) return false;
                if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")) &&
                    Hands->GetSingleNodeInstance() && Hands->GetSingleNodeInstance()->GetCurrentTime() < .75f) return false;
                if (!CapturePose(*World, TEXT("FirstPerson-WindJump.png"))) return false;
                CheckStep();
                Test.TestEqual(TEXT("Wind gives the camera one full layer of height"), Camera->GetComponentLocation().Z - EyeStart.Z, double(Level.LayerHeight()));
                Test.TestTrue(TEXT("A successful self lift plays the original jump start"), PlayingHands(TEXT("Jump_Start_Retarget"), false));
                Step = EStep::AirborneHands;
                break;
            case EStep::AirborneHands:
                if (!PlayingHands(TEXT("Jump_Loop_Retarget"), true)) return false;
                if (!AimAtFeet()) return false;
                CaptureStepStart();
                Tap(EKeys::LeftMouseButton);
                Step = EStep::VerifySecondLift;
                break;
            case EStep::VerifySecondLift:
                if (State.Turn < 2 || Board->IsAnimating()) return false;
                CheckStep();
                Test.TestEqual(TEXT("Consecutive casts reach Z2"), State.PlayerPosition, Level.PlayerStart + FIntVector(0, 0, 2));
                Test.TestEqual(TEXT("Both self lifts are free"), State.SP, Level.InitialSP);
                Test.TestEqual(TEXT("The second cast refreshes the wind to four turns"), State.PlayerWindTurns, 4);
                Test.TestTrue(TEXT("A consecutive self lift restarts the jump"), PlayingHands(TEXT("Jump_Start_Retarget"), false));
                Step = EStep::TryFlightMovement;
                break;
            case EStep::TryFlightMovement:
                Camera->SetWorldRotation(FRotator::ZeroRotator);
                Tap(EKeys::W); Tap(EKeys::S); Tap(EKeys::A); Tap(EKeys::D);
                Step = EStep::DispelFlight;
                break;
            case EStep::DispelFlight:
                Test.TestTrue(TEXT("WASD does not move or spend any turns while airborne"), State.PlayerPosition == Level.PlayerStart + FIntVector(0, 0, 2) &&
                    State.Turn == 2 && State.PlayerWindTurns == 4 && State.SP == Level.InitialSP);
                Test.TestTrue(TEXT("The self wind field is active beneath the avatar"),
                    CastChecked<UActorComponent>(Board->GetDefaultSubobjectByName(TEXT("PlayerWind")))->IsActive());
                CaptureStepStart();
                Tap(EKeys::G);
                Step = EStep::VerifyDispel;
                break;
            case EStep::VerifyDispel:
                if (State.PlayerWindTurns != 0 || Board->IsAnimating()) return false;
                CheckStep();
                Test.TestTrue(TEXT("G lands and clears the field without using SP or a turn"), State.PlayerPosition == Level.PlayerStart &&
                    State.Turn == 2 && State.SP == Level.InitialSP && Board->IsFirstPerson() &&
                    !CastChecked<UActorComponent>(Board->GetDefaultSubobjectByName(TEXT("PlayerWind")))->IsActive());
                Test.TestTrue(TEXT("Landing after G plays the original jump end"), PlayingHands(TEXT("Jump_End_Retarget"), false));
                Tap(EKeys::Z);
                Step = EStep::VerifyUndo;
                break;
            case EStep::VerifyUndo:
                if (State.PlayerWindTurns == 0) return false;
                Test.TestEqual(TEXT("Undo restores the airborne grid state"), State.PlayerPosition, Level.PlayerStart + FIntVector(0, 0, 2));
                Test.TestTrue(TEXT("Undo restores the airborne loop"), PlayingHands(TEXT("Jump_Loop_Retarget"), true));
                Tap(EKeys::F);
                Tap(EKeys::R);
                Step = EStep::ArmWaterThird;
                break;
            case EStep::ArmWaterThird:
                if (State.Turn != 0 || Board->IsFirstPerson()) return false;
                Test.TestTrue(TEXT("The water lesson starts with a box briefly held at Z1"),
                    State.FindBox(1)->GridPosition == FIntVector(1, 1, 1) && State.FindBox(1)->LevitationTurns == 2 &&
                    !State.FindBox(1)->bPhaseLocked);
                Tap(EKeys::Three);
                Step = EStep::AimWaterThird;
                break;
            case EStep::AimWaterThird:
                if (Board->Selection() != ERuneAction::WaterAnchor || !Aim()) return false;
                Tap(EKeys::LeftMouseButton);
                Step = EStep::WalkUnderThird;
                break;
            case EStep::WalkUnderThird:
                if (State.Turn == 0 || Board->IsAnimating()) return false;
                if (!Test.TestTrue(TEXT("The water projectile anchors the hovering crate"), State.FindBox(1)->bPhaseLocked)) { Stop(); break; }
                if (State.Turn < 3)
                {
                    Tap(EKeys::W);
                    return false;
                }
                Test.TestTrue(TEXT("The crate stays at Z1 after the initial wind has expired"),
                    State.FindBox(1)->LevitationTurns == 0 && State.FindBox(1)->GridPosition == FIntVector(1, 1, 1));
                Tap(EKeys::One);
                Step = EStep::AimUpThird;
                break;
            case EStep::AimUpThird:
                if (Board->Selection() != ERuneAction::Pulse || !Aim()) return false;
                Test.TestEqual(TEXT("The player stands directly under the anchored box"), State.PlayerPosition, FIntVector(1, 1, 0));
                Test.TestEqual(TEXT("Looking up sets the pulse direction without a toggle"), Board->PulseDirection(), FIntVector(0, 0, 1));
                Test.TestEqual(TEXT("The upward pulse previews the anchored Z2 goal"), Board->Preview().After.FindBox(1)->GridPosition, FIntVector(1, 1, 2));
                WinningView = Camera->GetComponentTransform();
                Key(EKeys::LeftMouseButton, IE_Pressed);
                Step = EStep::PushUpThird;
                break;
            case EStep::PushUpThird:
                if (State.Turn < 4) return false;
                Key(EKeys::LeftMouseButton, IE_Released);
                if (!Test.TestTrue(TEXT("Water anchor and an upward push complete the saved third level"), Board->IsRestored())) { Stop(); break; }
                Test.TestEqual(TEXT("Water, two steps and one pulse cost 6 SP"), Level.InitialSP - State.SP, 6);
                Step = EStep::EnterFourth;
                break;
            case EStep::EnterFourth:
                if (!Board->CanAdvanceLevel()) return false;
                CheckWinningView();
                Shot(TEXT("ThirdLevelClearFirstPerson.png"));
                Tap(EKeys::Enter);
                Step = EStep::CheckFourth;
                break;
            case EStep::CheckFourth:
                if (Level.LevelId != Fourth) return false;
                Test.TestEqual(TEXT("The fourth level starts with its authored SP"), State.SP, Level.InitialSP);
                Test.TestEqual(TEXT("The fourth level starts on turn zero"), State.Turn, 0);
                Test.TestEqual(TEXT("The sparse fourth-level list grants just Pulse and Hiss"), Granted(), 2);
                Test.TestTrue(TEXT("The fourth level teaches key 4"), Level.EntryTip.ToString().Contains(TEXT("按 4")));
                Tap(EKeys::Two);
                Tap(EKeys::Three);
                Step = EStep::CheckFourthLocks;
                break;
            case EStep::CheckFourthLocks:
                Test.TestFalse(TEXT("Keys 2 and 3 stay locked on a level with key 4"), Board->Selection().IsSet());
                Tap(EKeys::Four);
                Step = EStep::CheckFourthHiss;
                break;
            case EStep::CheckFourthHiss:
                if (Board->Selection() != ERuneAction::Hiss || !PlayingHands(TEXT("2h_Orb_Loop_Retarget"), true)) return false;
                if (!CapturePose(*World, TEXT("FirstPerson-HissLoop.png"))) return false;
                Test.TestTrue(TEXT("The original Orb loop after pressing 4 spends no SP or turns"),
                    State.Turn == 0 && State.SP == Level.InitialSP && Granted() == 2);
                Stop();
                break;
            case EStep::Stop: break;
            }
            return false;
        }
    };
    bool RunCampaign(FAutomationTestBase& Test, bool bFromSelected)
    {
        auto* Catalog = LoadObject<UPuzzleLevelCatalog>(nullptr, UPuzzleLevelCatalog::AssetPath);
        if (!Test.TestNotNull(TEXT("The saved campaign catalog"), Catalog)) return false;
        auto* First = Catalog->NextLevel();
        if (!Test.TestNotNull(TEXT("The starting level"), First)) return false;
        auto* Second = Catalog->NextLevel(First);
        if (!Test.TestNotNull(TEXT("The second level"), Second)) return false;
        auto* Third = Catalog->NextLevel(Second);
        if (!Test.TestNotNull(TEXT("The third level"), Third)) return false;
        auto* Fourth = Catalog->NextLevel(Third);
        if (!Test.TestNotNull(TEXT("The fourth level"), Fourth)) return false;
        for (const auto* Level : {Third, Fourth})
        {
            const auto Errors = FPuzzleSolver::Validate(Level->Definition);
            if (!Errors.IsEmpty()) { Test.AddError(Level->Definition.DisplayName.ToString() + TEXT(" · ") + FString::Join(Errors, TEXT(" / "))); return false; }
        }
        if (bFromSelected)
        {
            FPuzzleAuthoring Editor(Catalog);
            Editor.SetActive(Second);
            if (!Test.TestTrue(TEXT("Authoring starts at the selected second level"), Editor.TestLevel())) return false;
        }
        else
        {
            AShengLuGameMode::SetPIELevel(nullptr);
            FRequestPlaySessionParams Params;
            Params.WorldType = EPlaySessionWorldType::PlayInEditor;
            Params.GlobalMapOverride = UPuzzleLevelCatalog::PlayMap;
            auto* Settings = DuplicateObject(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
            Settings->SetPlayNetMode(PIE_Standalone);
            Settings->SetRunUnderOneProcess(true);
            Settings->SetPlayNumberOfClients(1);
            Settings->bLaunchSeparateServer = false;
            Params.EditorPlaySettings = Settings;
            GEditor->RequestPlaySession(Params);
        }
        FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPlayCampaign>(Test, First->Definition.LevelId,
            Second->Definition.LevelId, Third->Definition.LevelId, Fourth->Definition.LevelId, bFromSelected));
        return true;
    }
}

bool FPuzzleCampaignTest::RunTest(const FString&) { return RunCampaign(*this, false); }
bool FPuzzleSelectedTravelTest::RunTest(const FString&) { return RunCampaign(*this, true); }

#endif
