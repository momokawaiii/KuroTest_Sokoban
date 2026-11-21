#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ShengLuGame.h"
#include "TextureResource.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleHissPIETest, "ResonantRunes.Puzzle.Editor.HissOpticalFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
    const FIntVector Obstacle(2, 1, 0);

    class FPlayHiss final : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TStrongObjectPtr<UPuzzleLevelAsset> Asset;
        const double Deadline = FPlatformTime::Seconds() + 80.;
        double CaptureAfter = 0.;
        enum class EStep { AimWall, CastWall, Wall, Push, SharedCell, Cycle, Box, BoxFlow, WalkNorth, MoveBox, Overview, OverviewFlow, Cleanup, Stop };
        EStep Step = EStep::AimWall;

        void Stop()
        {
            AShengLuGameMode::SetPIELevel(nullptr);
            if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
        }

        void CheckFlow(UPostProcessComponent* Post, const TCHAR* Shot)
        {
            if (!Test.TestTrue(TEXT("The Hiss post process is enabled"), Post && Post->bEnabled)) return;
            if (!FParse::Param(FCommandLine::Get(), TEXT("RuneScreens"))) return;
            const auto& Blendables = Post->Settings.WeightedBlendables.Array;
            auto* Instance = Blendables.IsEmpty() ? nullptr : Cast<UMaterialInstanceDynamic>(Blendables[0].Object);
            auto* Composite = Instance ? Cast<UTextureRenderTarget2D>(Instance->K2_GetTextureParameterValue(TEXT("HissBuffer"))) : nullptr;
            TArray<FFloat16Color> Pixels;
            if (Test.TestTrue(TEXT("The Niagara composite can be read on the GPU"), Composite &&
                Composite->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels)))
            {
                int32 FlowPixels = 0;
                for (const auto& Pixel : Pixels) FlowPixels += Pixel.A.GetFloat() > .01f;
                Test.AddInfo(FString::Printf(TEXT("%s - %d/%d pixels carry Hiss density"), Shot, FlowPixels, Pixels.Num()));
                Test.TestTrue(TEXT("The selected shape generates optical flow"), FlowPixels > 64);
            }
            FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
                TEXT("PuzzleVerification") / Shot), false, false);
        }

    public:
        FPlayHiss(FAutomationTestBase& InTest, UPuzzleLevelAsset* InAsset) : Test(InTest), Asset(InAsset) {}

        bool Update() override
        {
            if (Step == EStep::Stop) return GEditor->PlayWorld == nullptr;
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Hiss playthrough timed out at step %d"), int32(Step)));
                Stop();
                return false;
            }
            auto* PC = GEditor->PlayWorld ? GEditor->PlayWorld->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            if (!Board || Board->GetLevelAsset() != Asset.Get() || !Board->HasActorBegunPlay() || Board->IsAnimating()) return false;
            const auto AimAt = [Board](FVector Point)
            {
                auto* Camera = CastChecked<UCameraComponent>(Board->GetDefaultSubobjectByName(TEXT("CastCamera")));
                Camera->SetWorldRotation((Point - Camera->GetComponentLocation()).Rotation());
            };
            const FVector WallAim = Board->Level().BlockBounds(Obstacle).GetCenter() + FVector(0, 0, 50);
            auto* Post = FindObjectFast<UPostProcessComponent>(Board, TEXT("HissPostProcess"));
            const auto& State = Board->Runtime();
            switch (Step)
            {
            case EStep::AimWall:
                Board->SelectSkill(ERuneAction::Hiss);
                AimAt(WallAim);
                Step = EStep::CastWall;
                break;
            case EStep::CastWall:
                Test.TestTrue(TEXT("Crosshair picks the wall volume and previews Hiss"),
                    Board->AimedAtObstacle() && Board->AimCell() == Obstacle && Board->Preview().IsValid());
                Board->ClickBoard();
                Test.TestTrue(TEXT("Click commits the wall phase through GAS"), State.HissObstacles.Contains(Obstacle));
                Test.TestEqual(TEXT("The live cast spends two SP"), State.SP, Asset->Definition.InitialSP - 2);
                CaptureAfter = FPlatformTime::Seconds() + .7;
                Step = EStep::Wall;
                break;
            case EStep::Wall:
                if (FPlatformTime::Seconds() < CaptureAfter) break;
                CheckFlow(Post, TEXT("HissObstacle.png"));
                Step = EStep::Push;
                break;
            case EStep::Push:
                Test.TestTrue(TEXT("Pulse moves a normal crate into the Hiss wall"), Board->TryPuzzleAction({ERuneAction::Pulse, 1}));
                Step = EStep::SharedCell;
                break;
            case EStep::SharedCell:
                Test.TestTrue(TEXT("The crate and wall occupy the same cell"), State.GetBoxesAt(Obstacle).Contains(1));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Tab, IE_Pressed, 1.f));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Tab, IE_Released, 0.f));
                Step = EStep::Cycle;
                break;
            case EStep::Cycle:
                Test.TestEqual(TEXT("Tab can pick the crate sharing the Hiss wall's cell"), Board->AimBoxId(), 1);
                Board->SelectSkill(ERuneAction::Pulse);
                AimAt(WallAim);
                Step = EStep::Box;
                break;
            case EStep::Box:
            {
                Test.TestEqual(TEXT("Aiming at the shared cell can still target the enclosed crate"), Board->AimBoxId(), 1);
                Board->Undo();
                Board->Undo();
                Test.TestTrue(TEXT("Undo removes the wall's phase and stops the composite"), State.HissObstacles.IsEmpty() && Post && !Post->bEnabled);
                const auto* Mesh = Board->BoxMesh(1);
                const FVector OriginalExtent = Mesh->Bounds.BoxExtent;
                Board->SelectSkill(ERuneAction::Hiss);
                AimAt(Mesh->GetComponentLocation());
                Board->ClickBoard();
                Test.TestTrue(TEXT("Hiss marks the targeted crate mesh"), State.FindBox(1)->bHiss && Mesh->bRenderCustomDepth);
                Test.TestTrue(TEXT("Casting Hiss preserves the box mesh dimensions"), Mesh->Bounds.BoxExtent.Equals(OriginalExtent));
                CaptureAfter = FPlatformTime::Seconds() + .7;
                Step = EStep::BoxFlow;
                break;
            }
            case EStep::BoxFlow:
                if (FPlatformTime::Seconds() < CaptureAfter) break;
                CheckFlow(Post, TEXT("HissBox.png"));
                Test.TestTrue(TEXT("Approach the north side of the crate"), Board->TryPuzzleAction({ERuneAction::North}));
                Step = EStep::WalkNorth;
                break;
            case EStep::WalkNorth:
                Test.TestTrue(TEXT("Line up on the north side of the crate"), Board->TryPuzzleAction({ERuneAction::East}));
                Step = EStep::MoveBox;
                break;
            case EStep::MoveBox:
                Test.TestTrue(TEXT("Pulse moves the phased crate"), Board->TryPuzzleAction({ERuneAction::Pulse, 1, {0, -1, 0}}));
                Step = EStep::Overview;
                break;
            case EStep::Overview:
                Test.TestTrue(TEXT("The moved crate still supplies its own stencil"), Board->BoxMesh(1)->bRenderCustomDepth &&
                    Board->BoxMesh(1)->GetComponentLocation().Equals(Board->Level().BoxBounds({1, 0, 0}, true).GetCenter()));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F, IE_Pressed, 1.f));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F, IE_Released, 0.f));
                CaptureAfter = FPlatformTime::Seconds() + .7;
                Step = EStep::OverviewFlow;
                break;
            case EStep::OverviewFlow:
                if (FPlatformTime::Seconds() < CaptureAfter) break;
                Test.TestFalse(TEXT("Returned to the orthographic board camera"), Board->IsFirstPerson());
                CheckFlow(Post, TEXT("HissBoxOverview.png"));
                Step = EStep::Cleanup;
                break;
            case EStep::Cleanup:
                Board->ResetPuzzle();
                Test.TestTrue(TEXT("Restart clears the crate stencil and disables the composite"),
                    !State.FindBox(1)->bHiss && !Board->BoxMesh(1)->bRenderCustomDepth && Post && !Post->bEnabled);
                Stop();
                break;
            case EStep::Stop: break;
            }
            return false;
        }
    };
}

bool FPuzzleHissPIETest::RunTest(const FString&)
{
    auto* Asset = NewObject<UPuzzleLevelAsset>();
    auto& Level = Asset->Definition;
    Level.LevelId = FGuid::NewGuid();
    Level.DisplayName = FText::FromString(TEXT("Hiss Optical Flow"));
    Level.GridDimensions = {4, 3, 2};
    Level.StaticCells.SetNum(FPuzzleLevelDefinition::CellCount(Level.GridDimensions));
    for (int32 Index = 0; Index < 12; ++Index) Level.StaticCells[Index].bHasFloor = true;
    Level.StaticCells[Level.GridToIndex(Obstacle)].bBlocked = true;
    Level.PlayerStart = {0, 1, 0};
    Level.InitialBoxes.Add({1, {1, 1, 0}});
    Level.Goals.Add({1, {3, 2, 0}, 1});

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
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPlayHiss>(*this, Asset));
    return true;
}

#endif
