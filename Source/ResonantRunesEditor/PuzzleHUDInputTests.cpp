#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "ImageUtils.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ShengLuGame.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleHUDInputTest, "ResonantRunes.Puzzle.Editor.HUDInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleBoardMarkerTest, "ResonantRunes.Puzzle.Editor.BoardMarkerDepth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

namespace
{
    class FCheckPuzzleHUDInput final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        const double Deadline = FPlatformTime::Seconds() + 30.;
        const bool bOffscreen = FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen"));
        const bool bScreens = bOffscreen && FParse::Param(FCommandLine::Get(), TEXT("RuneScreens"));
        enum class EStep { Load, Board, SkillHover, GoalHover, ZoomedBoard, ZoomedCharacter, Overview, Aim, MouseLook, Stop } Step = EStep::Load;
        double CaptureAt = 0.;
        float HomeWidth = 0.f;
        FVector2D GoalScreen{};
        FRotator InitialLook;

        void Stop()
        {
            GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
        }

        void Capture(FSceneViewport& Viewport, const TCHAR* FileName)
        {
            if (!bScreens) return;
            TArray<FColor> Pixels;
            const FIntPoint Size = Viewport.GetRenderTargetTextureSizeXY();
            if (!Test.TestTrue(TEXT("Read the play viewport for the HUD capture"),
                GetViewportScreenShot(&Viewport, Pixels) && Pixels.Num() == Size.X * Size.Y)) return;
            for (auto& Pixel : Pixels) Pixel.A = 255;
            const FString Path = FPaths::ProjectSavedDir() / TEXT("PuzzleVerification") / FileName;
            Test.TestTrue(TEXT("Save HUD capture"), FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Size.X, Size.Y)));
        }

    public:
        explicit FCheckPuzzleHUDInput(FAutomationTestBase& InTest) : Test(InTest) {}

        bool Update() override
        {
            if (Step == EStep::Stop) return GEditor->PlayWorld == nullptr;
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("HUD input did not settle at step %d"), int32(Step)));
                Stop();
                return false;
            }
            UWorld* World = GEditor->PlayWorld;
            auto* PC = World ? World->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            auto* Viewport = World && World->GetGameViewport() ? World->GetGameViewport()->GetGameViewport() : nullptr;
            if (!Board || !Board->HasActorBegunPlay() || !Viewport) return false;
            auto* Overview = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("BoardCamera")));
            auto* Eyes = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("CastCamera")));
            const auto Axis = [&](FKey Key, float Amount)
            { PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Axis, Amount)); };
            const auto ZoomIn = [&]
            {
                Axis(EKeys::MouseWheelAxis, 3.f);
                Step = EStep::ZoomedBoard;
            };
            const auto& Level = Board->Level();
            switch (Step)
            {
            case EStep::Load:
                if (bOffscreen)
                {
                    Viewport->SetFixedViewportSize(1600, 900);
                    Viewport->SetMouse(800, 625);
                }
                CaptureAt = World->GetRealTimeSeconds() + (bScreens ? 2.05 : 1.25);
                Step = EStep::Board;
                break;
            case EStep::Board:
            {
                if (World->GetRealTimeSeconds() < CaptureAt) return false;
                HomeWidth = Overview->OrthoWidth;
                Test.TestTrue(TEXT("The software cursor keeps overview pointer input enabled"),
                    PC->bShowMouseCursor && PC->GetMouseCursor() == EMouseCursor::None);
                const auto* Crate = Board->CrateMesh();
                if (!Test.TestTrue(TEXT("The starting level provides a goal and an ordinary box"), !Level.Goals.IsEmpty() && Crate))
                { Stop(); break; }
                const auto Goal = Level.Goals[0].GridPosition;
                Test.TestTrue(TEXT("The goal is inspectable before selecting the pawn"),
                    PC->ProjectWorldLocationToScreen(Board->Cell(Goal), GoalScreen) && Board->InspectCellAt(GoalScreen) == Goal);
                FVector2D BoxScreen;
                Test.TestTrue(TEXT("An ordinary box has no coordinates tooltip"),
                    PC->ProjectWorldLocationToScreen(Crate->Bounds.Origin, BoxScreen) && !Board->InspectCellAt(BoxScreen));
                Capture(*Viewport, TEXT("RuneHUD-Zoom-Overview.png"));
                if (bScreens)
                {
                    const FIntPoint Pixels = Viewport->GetSizeXY();
                    Viewport->SetMouse(Pixels.X / 5, FMath::RoundToInt(Pixels.Y * .89f));
                    CaptureAt = World->GetRealTimeSeconds() + .35;
                    Step = EStep::SkillHover;
                }
                else ZoomIn();
                break;
            }
            case EStep::SkillHover:
                if (World->GetRealTimeSeconds() < CaptureAt) return false;
                Capture(*Viewport, TEXT("RuneHUD-R2-SkillHover.png"));
                Viewport->SetMouse(FMath::RoundToInt(GoalScreen.X), FMath::RoundToInt(GoalScreen.Y));
                Step = EStep::GoalHover;
                break;
            case EStep::GoalHover:
                Capture(*Viewport, TEXT("RuneHUD-R2-GoalHover.png"));
                Viewport->SetMouse(12, 12);
                ZoomIn();
                break;
            case EStep::ZoomedBoard:
            {
                if (Overview->OrthoWidth > HomeWidth / 1.59f) return false;
                Test.TestTrue(TEXT("Wheel-in changes the board scale without advancing time or spending SP"),
                    Board->Runtime().Turn == 0 && Board->Runtime().SP == Level.InitialSP);
                const auto Goal = Level.Goals[0].GridPosition;
                Test.TestTrue(TEXT("Coordinate picking follows the zoomed overview camera"),
                    PC->ProjectWorldLocationToScreen(Board->Cell(Goal), GoalScreen) && Board->InspectCellAt(GoalScreen) == Goal);
                const FIntPoint Pixels = Viewport->GetSizeXY();
                const FIntVector Edge = Level.GridDimensions - FIntVector(1);
                for (int32 X : {0, Edge.X}) for (int32 Y : {0, Edge.Y})
                {
                    FVector2D Corner;
                    Test.TestTrue(TEXT("The four board corners stay in the viewport at the overview zoom"),
                        PC->ProjectWorldLocationToScreen(Board->Cell({X, Y, 0}), Corner) &&
                        Corner.X > 0. && Corner.X < Pixels.X && Corner.Y > 0. && Corner.Y < Pixels.Y);
                }
                Capture(*Viewport, TEXT("RuneHUD-Zoom-Board.png"));
                Axis(EKeys::MouseWheelAxis, 40.f);
                CaptureAt = World->GetRealTimeSeconds() + .72;
                Step = EStep::ZoomedCharacter;
                break;
            }
            case EStep::ZoomedCharacter:
            {
                if (World->GetRealTimeSeconds() < CaptureAt) return false;
                const auto* Player = CastChecked<UCapsuleComponent>(Board->GetDefaultSubobjectByName(TEXT("MannyPick")));
                const FVector Center = Player->GetComponentLocation();
                const FVector HalfHeight(0, 0, Player->GetScaledCapsuleHalfHeight());
                const FIntPoint Pixels = Viewport->GetSizeXY();
                FVector2D Middle{}, Feet{}, Head{};
                if (!Test.TestTrue(TEXT("Project the character hit capsule in the close overview"),
                    PC->ProjectWorldLocationToScreen(Center, Middle) &&
                    PC->ProjectWorldLocationToScreen(Center - HalfHeight, Feet) &&
                    PC->ProjectWorldLocationToScreen(Center + HalfHeight, Head)))
                { Stop(); break; }
                Test.TestTrue(TEXT("Wheel continues past the board zoom and frames the character"),
                    Overview->OrthoWidth < HomeWidth / 3.f &&
                    FMath::Abs(Middle.X / Pixels.X - .5) < .06 &&
                    FMath::Abs(Middle.Y / Pixels.Y - .5) < .06);
                Test.TestTrue(TEXT("The close overview shows a readable full-body silhouette"),
                    Feet.Y - Head.Y > .32 * Pixels.Y && Head.Y > .10 * Pixels.Y && Feet.Y < .82 * Pixels.Y);
                Capture(*Viewport, TEXT("RuneHUD-Zoom-Character.png"));
                Board->ClickAt(Middle);
                Test.TestTrue(TEXT("The character can still be selected in the close overview"), Board->IsPawnSelected());
                Test.TestTrue(TEXT("Framing and selection leave the turn and SP alone"),
                    Board->Runtime().Turn == 0 && Board->Runtime().SP == Level.InitialSP);
                Axis(EKeys::MouseWheelAxis, -40.f);
                Step = EStep::Overview;
                break;
            }
            case EStep::Overview:
                if (!FMath::IsNearlyEqual(Overview->OrthoWidth, HomeWidth, 1.f)) return false;
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::One, IE_Pressed, 1.f));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::One, IE_Released, 0.f));
                Step = EStep::Aim;
                break;
            case EStep::Aim:
                if (!Board->IsFirstPerson() || Board->AimBoxId() == INDEX_NONE) return false;
                Test.TestFalse(TEXT("The cursor is hidden while aiming in first person"), PC->bShowMouseCursor);
                Capture(*Viewport, TEXT("RuneHUD-R2-FirstPerson.png"));
                InitialLook = Eyes->GetComponentRotation();
                Axis(EKeys::MouseX, 25.f);
                Axis(EKeys::MouseY, -25.f);
                Axis(EKeys::MouseWheelAxis, 3.f);
                Step = EStep::MouseLook;
                break;
            case EStep::MouseLook:
            {
                const FRotator Look = Eyes->GetComponentRotation();
                const double MouseX = PC->GetInputAnalogKeyState(EKeys::MouseX);
                const double MouseY = PC->GetInputAnalogKeyState(EKeys::MouseY);
                Test.TestTrue(TEXT("First-person yaw and pitch use 0.6 degrees per processed mouse unit"),
                    MouseX > 0. && MouseY < 0. &&
                    FMath::IsNearlyEqual(FMath::FindDeltaAngleDegrees(InitialLook.Yaw, Look.Yaw), MouseX * .60, .012) &&
                    FMath::IsNearlyEqual(FMath::FindDeltaAngleDegrees(InitialLook.Pitch, Look.Pitch), MouseY * .60, .012));
                Test.TestTrue(TEXT("The wheel leaves the overview scale alone in first person"),
                    FMath::IsNearlyEqual(Overview->OrthoWidth, HomeWidth, 1.f));
                Stop();
                break;
            }
            case EStep::Stop: break;
            }
            return false;
        }
    };
}

bool FPuzzleHUDInputTest::RunTest(const FString&)
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
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCheckPuzzleHUDInput>(*this));
    return true;
}

namespace
{
    const FIntVector OpenGoal(0, 1, 0);
    const FIntVector CoveredGoal(2, 1, 0);

    struct FTrailSample
    {
        int32 Count = 0;
        FVector2D Center = FVector2D::ZeroVector;
    };

    FTrailSample SampleTrail(const APlayerController& PC, const AShengLuBoard& Board, FIntVector Cell,
        const TArray<FColor>& Pixels, FIntPoint Size)
    {
        FBox2D Bounds(ForceInit);
        const double Half = Board.Level().GridCellSize * .45;
        for (int32 X : {-1, 1})
            for (int32 Y : {-1, 1})
            {
                FVector2D Point;
                if (!PC.ProjectWorldLocationToScreen(Board.Cell(Cell) + FVector(X * Half, Y * Half, 0), Point)) return {};
                Bounds += Point;
            }
        FTrailSample Sample;
        double Sum = 0.;
        const FIntPoint Min(FMath::Max(0, FMath::CeilToInt(Bounds.Min.X)), FMath::Max(0, FMath::CeilToInt(Bounds.Min.Y)));
        const FIntPoint Max(FMath::Min(Size.X, FMath::FloorToInt(Bounds.Max.X)), FMath::Min(Size.Y, FMath::FloorToInt(Bounds.Max.Y)));
        for (int32 Y = Min.Y; Y < Max.Y; ++Y)
            for (int32 X = Min.X; X < Max.X; ++X)
            {
                const FColor Color = Pixels[Y * Size.X + X];
                const int32 Weight = int32(Color.R) - Color.B - 30;
                if (Color.R < 90 || int32(Color.R) - Color.G < 9 || Weight <= 0) continue;
                Sample.Center += FVector2D(X, Y) * Weight;
                ++Sample.Count;
                Sum += Weight;
            }
        if (Sum > 0.) Sample.Center /= Sum;
        return Sample;
    }

    class FCheckBoardMarkerDepth final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TStrongObjectPtr<UPuzzleLevelAsset> Asset;
        const double Deadline = FPlatformTime::Seconds() + 35.;
        enum class EStep { FrameBoard, Default, Orbit, Held, Released, Stop } Step = EStep::FrameBoard;
        double CaptureAt = 0.;
        FVector2D InitialLight = FVector2D::ZeroVector;

        void Stop()
        {
            AShengLuGameMode::SetPIELevel(nullptr);
            if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
        }

    public:
        FCheckBoardMarkerDepth(FAutomationTestBase& InTest, UPuzzleLevelAsset* InAsset) : Test(InTest), Asset(InAsset) {}

        bool Update() override
        {
            if (Step == EStep::Stop) return GEditor->PlayWorld == nullptr;
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Marker visibility did not settle at step %d"), int32(Step)));
                Stop();
                return false;
            }
            UWorld* World = GEditor->PlayWorld;
            auto* PC = World ? World->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            auto* Viewport = World && World->GetGameViewport() ? World->GetGameViewport()->GetGameViewport() : nullptr;
            if (!Board || !Board->HasActorBegunPlay() || Board->GetLevelAsset() != Asset.Get() || !Viewport) return false;

            const double Now = World->GetTimeSeconds();
            if (Step == EStep::FrameBoard)
            {
                if (FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen"))) Viewport->SetFixedViewportSize(1600, 900);
                if (auto* HUD = PC->GetHUD()) HUD->bShowHUD = false;
                auto* Camera = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("BoardCamera")));
                const FVector Center = (Board->Cell({0, 0, 0}) + Board->Cell({3, 2, 0})) * .5;
                Camera->SetWorldLocationAndRotation(Center + FVector(0, 0, 3200), FRotator(-90, 0, 0));
                CaptureAt = Now + 1.2;
                Step = EStep::Default;
                return false;
            }
            if (Now < CaptureAt || (Step == EStep::Held) != Board->IsDepthViewActive()) return false;

            TArray<FColor> Pixels;
            const FIntPoint Size = Viewport->GetRenderTargetTextureSizeXY();
            if (!Test.TestTrue(TEXT("Read the rendered board markers from the game viewport"),
                GetViewportScreenShot(Viewport, Pixels) && Pixels.Num() == Size.X * Size.Y))
            { Stop(); return false; }
            const auto Open = SampleTrail(*PC, *Board, OpenGoal, Pixels, Size);
            const auto Hidden = SampleTrail(*PC, *Board, CoveredGoal, Pixels, Size);
            FVector2D CoveredScreen;
            PC->ProjectWorldLocationToScreen(Board->Cell(CoveredGoal), CoveredScreen);
            const bool bCanInspect = Board->InspectCellAt(CoveredScreen) == CoveredGoal;
            Test.AddInfo(FString::Printf(TEXT("Step %d: %d warm pixels at the open goal, %d at the covered goal"),
                int32(Step), Open.Count, Hidden.Count));

            const auto Save = [&](const TCHAR* Name)
            {
                if (!FParse::Param(FCommandLine::Get(), TEXT("RuneScreens"))) return;
                for (auto& Pixel : Pixels) Pixel.A = 255;
                const FString Path = FPaths::ProjectSavedDir() / TEXT("PuzzleVerification") / Name;
                Test.TestTrue(TEXT("Save the board-marker capture"),
                    FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Size.X, Size.Y)));
            };
            switch (Step)
            {
            case EStep::Default:
                Test.TestTrue(TEXT("A visible goal has a lit perimeter"), Open.Count > 24);
                Test.TestTrue(TEXT("An opaque overhead block hides the goal and its tooltip"), Hidden.Count < 6 && !bCanInspect);
                InitialLight = Open.Center;
                Save(TEXT("BoardMarkers-Depth.png"));
                CaptureAt = Now + .40;
                Step = EStep::Orbit;
                break;
            case EStep::Orbit:
                Test.TestTrue(TEXT("The light travels around the goal perimeter"), Open.Count > 24 &&
                    FVector2D::Distance(Open.Center, InitialLight) > 5.);
                Save(TEXT("BoardMarkers-Orbit.png"));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::E, IE_Pressed, 1.f));
                CaptureAt = Now + .25;
                Step = EStep::Held;
                break;
            case EStep::Held:
                Test.TestTrue(TEXT("Holding E reveals the moving goal and its tooltip through the opaque block"),
                    Hidden.Count > 24 && bCanInspect);
                Save(TEXT("BoardMarkers-HoldE.png"));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::E, IE_Released, 0.f));
                CaptureAt = Now + .25;
                Step = EStep::Released;
                break;
            case EStep::Released:
                Test.TestTrue(TEXT("Releasing E restores full occlusion"), Hidden.Count < 6 && !bCanInspect);
                Test.TestTrue(TEXT("Inspection leaves the puzzle turn and SP alone"),
                    Board->Runtime().Turn == 0 && Board->Runtime().SP == Board->Level().InitialSP);
                Stop();
                break;
            default: break;
            }
            return false;
        }
    };
}

bool FPuzzleBoardMarkerTest::RunTest(const FString&)
{
    auto* Asset = NewObject<UPuzzleLevelAsset>();
    auto& Level = Asset->Definition;
    Level.LevelId = FGuid::NewGuid();
    Level.DisplayName = FText::FromString(TEXT("Board Marker Depth"));
    Level.GridDimensions = {4, 3, 3};
    Level.StaticCells.SetNum(FPuzzleLevelDefinition::CellCount(Level.GridDimensions));
    for (int32 Index = 0; Index < 12; ++Index) Level.StaticCells[Index].bHasFloor = true;
    Level.StaticCells[Level.GridToIndex(CoveredGoal + FIntVector(0, 0, 1))].bBlocked = true;
    Level.PlayerStart = {0, 0, 0};
    Level.InitialBoxes = {{1, {1, 0, 0}}, {2, {3, 2, 0}}};
    Level.Goals = {{1, OpenGoal, 1}, {2, CoveredGoal, 2}};

    AShengLuGameMode::SetPIELevel(Asset);
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
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCheckBoardMarkerDepth>(*this, Asset));
    return true;
}

#endif
