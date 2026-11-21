#if WITH_DEV_AUTOMATION_TESTS

#include "PuzzleAuthoring.h"
#include "PuzzleViewportMode.h"
#include "RuneBoxPawn.h"
#include "ShengLuGame.h"
#include "SPuzzleViewport.h"

#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "ImageUtils.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Slate/SceneViewport.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleAuthoringTest, "ResonantRunes.Puzzle.Editor.Transactions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleAuthoringTest::RunTest(const FString&)
{
    auto* Catalog = NewObject<UPuzzleLevelCatalog>(GetTransientPackage(), NAME_None, RF_Transactional);
    auto* Level = NewObject<UPuzzleLevelAsset>(GetTransientPackage(), NAME_None, RF_Transactional);
    auto* Other = NewObject<UPuzzleLevelAsset>(GetTransientPackage(), NAME_None, RF_Transactional);
    Level->Definition = Other->Definition = FPuzzleLevelDefinition::MakeShengLu();
    Other->Definition.LevelId = FGuid::NewGuid();
    Other->Definition.GridCellSize = 450.f;
    FPuzzleAuthoring Editor(Catalog);
    Editor.AddExisting(Level);
    Editor.AddExisting(Other);
    const FGuid OtherId = Other->Definition.LevelId;
    TestTrue(TEXT("Row can be dropped before another level"), Editor.Reorder(Other, Level, false));
    TestTrue(TEXT("Reorder preserves permanent ID and puts selected level first"),
        Catalog->Levels[0] == Other && Other->Definition.LevelId == OtherId);
    TestEqual(TEXT("New levels default to the first row's cell size"), Editor.DefaultCellSize(), 450.f);
    TestTrue(TEXT("Catalog reorder can be undone"), GEditor->UndoTransaction() && Catalog->Levels[0] == Level);
    TestEqual(TEXT("Undo also restores the default cell size"), Editor.DefaultCellSize(), Level->Definition.GridCellSize);
    TestTrue(TEXT("Catalog reorder can be redone"), GEditor->RedoTransaction() && Catalog->Levels[0] == Other);
    TestTrue(TEXT("Row can be dropped after another level"), Editor.Reorder(Other, Level, true) && Catalog->Levels[1] == Other);
    const FText OriginalName = Other->Definition.DisplayName;
    TestTrue(TEXT("Rename stores the display name without touching its asset path or ID"),
        Editor.Rename(Other, FText::FromString(TEXT(" Renamed level "))) &&
        Other->Definition.DisplayName.ToString() == TEXT("Renamed level") && Other->Definition.LevelId == OtherId);
    TestTrue(TEXT("Rename uses editor undo"), GEditor->UndoTransaction() && Other->Definition.DisplayName.EqualTo(OriginalName));
    TestTrue(TEXT("Rename uses editor redo"), GEditor->RedoTransaction() && Other->Definition.DisplayName.ToString() == TEXT("Renamed level"));
    Editor.SetActive(Other);
    const auto OriginalSkills = Other->Definition.AvailableSkills;
    const TArray<ERuneSkill> SelectedSkills{ERuneSkill::Levitate, ERuneSkill::Hiss};
    Editor.UpdateLevel([&](auto& Definition) { Definition.AvailableSkills = SelectedSkills; });
    TestTrue(TEXT("Level authoring keeps a nonconsecutive selection"), Other->Definition.AvailableSkills == SelectedSkills);
    TestTrue(TEXT("Skill selection can be undone"), GEditor->UndoTransaction() && Other->Definition.AvailableSkills == OriginalSkills);
    TestTrue(TEXT("Skill selection can be redone"), GEditor->RedoTransaction() && Other->Definition.AvailableSkills == SelectedSkills);
    Editor.SetActive(Level);
    Editor.Select({EPuzzleTool::Player, Level->Definition.PlayerStart});
    TestEqual(TEXT("The default spawn direction is positive grid X"), Level->Definition.GetPlayerStartDirection(), FIntPoint(1, 0));
    Editor.RotatePlayerStart(true);
    TestTrue(TEXT("Clockwise rotation respects swapped axes and reaches the initial runtime state"),
        Level->Definition.GetPlayerStartDirection() == FIntPoint(0, -1) &&
        FPuzzleRuntimeState::FromDefinition(Level->Definition).Facing == FIntPoint(0, -1));
    TestTrue(TEXT("Undo restores the previous spawn direction"), GEditor->UndoTransaction() &&
        Level->Definition.GetPlayerStartDirection() == FIntPoint(1, 0));
    TestTrue(TEXT("Redo restores the rotated spawn direction"), GEditor->RedoTransaction() &&
        Level->Definition.GetPlayerStartDirection() == FIntPoint(0, -1));
    Editor.UpdateLevel([](auto& Definition) { Definition.StaticCells[Definition.GridToIndex({0, 2, 0})].bHasFloor = false; });
    TestTrue(TEXT("Grow remaps static cells through grid coordinates"), Editor.Resize({4, 3, 3}) &&
        Level->Definition.CellAt({2, 1, 0})->bBlocked);
    TestTrue(TEXT("Growth fills new Z0 cells and preserves existing holes and empty upper layers"),
        Level->Definition.CellAt({3, 2, 0})->bHasFloor && !Level->Definition.CellAt({0, 2, 0})->bHasFloor &&
        !Level->Definition.CellAt({3, 2, 1})->bHasFloor);
    const auto P = FPuzzleViewportMode::OnLayer(Level->Definition, 2, FVector(132, 572, 2200), -FVector::UpVector);
    if (!TestTrue(TEXT("Ray picks an empty upper layer with swapped axes"), P == FIntVector(1, 0, 2))) return false;
    Editor.SetTool(EPuzzleTool::Goal);
    TestTrue(TEXT("Floating goal can be placed"), Editor.Place(*P));
    const FPuzzleElement Goal = Editor.Selection;
    const auto DragTarget = Level->Definition.WorldToGrid(Level->Definition.GridToWorld({1, 2, 1}) + FVector(93, 81, 55));
    TestTrue(TEXT("Drag or coordinate edit commits snapped grid location"), Editor.Move(DragTarget));
    TestEqual(TEXT("Goal retains ID after move"), Level->Definition.Goals.Last().GoalId, Goal.Id);
    TestEqual(TEXT("Goal uses snapped XYZ"), Level->Definition.Goals.Last().GridPosition, FIntVector(1, 2, 1));
    TestTrue(TEXT("Editor undo restores coordinates"), GEditor->UndoTransaction() && Level->Definition.Goals.Last().GridPosition == *P);
    TestTrue(TEXT("Editor redo restores moved coordinates"), GEditor->RedoTransaction() &&
        Level->Definition.Goals.Last().GridPosition == FIntVector(1, 2, 1));
    TestTrue(TEXT("Delete resolves by goal ID"), Editor.Erase(Goal) && Level->Definition.Goals.Num() == 1);
    TestTrue(TEXT("Editor undo restores deleted element with same ID"), GEditor->UndoTransaction() &&
        Level->Definition.Goals.Last().GoalId == Goal.Id);
    TestEqual(TEXT("New goals accept any box by default"), Level->Definition.Goals.Last().BoxId, FRuneGoalData::AnyBoxId);
    Editor.SetTool(EPuzzleTool::Box);
    TestTrue(TEXT("An extra box occupies a newly floored cell"), Editor.Place({3, 0, 0}));
    const int32 ExtraId = Editor.Selection.Id;
    Editor.Select({EPuzzleTool::Box, {1, 1, 0}, 1});
    TestFalse(TEXT("A box cannot take a live box number"), Editor.RenumberBox(ExtraId));
    TestTrue(TEXT("Renumbering keeps the corresponding goal reference"), Editor.RenumberBox(31) &&
        Level->Definition.InitialBoxes[0].BoxId == 31 && Level->Definition.Goals[0].BoxId == 31);
    TestTrue(TEXT("Undo restores the box number and goal reference together"), GEditor->UndoTransaction() &&
        Level->Definition.InitialBoxes[0].BoxId == 1 && Level->Definition.Goals[0].BoxId == 1);
    TestTrue(TEXT("A deleted box number becomes available for an explicit renumber"),
        Editor.Erase({EPuzzleTool::Box, {3, 0, 0}, ExtraId}));
    Editor.Select({EPuzzleTool::Box, {1, 1, 0}, 1});
    TestTrue(TEXT("An author can reuse the deleted number"), Editor.RenumberBox(ExtraId) &&
        Level->Definition.Goals[0].BoxId == ExtraId);
    TestFalse(TEXT("Resize cannot silently discard terrain or goals"), Editor.Resize({1, 1, 1}));
    const auto OriginalWorld = Level->Definition.GridToWorld({1, 1, 0});
    if (!TestTrue(TEXT("A smaller size enters crop preview"), Editor.BeginCrop({2, 3, 2}))) return false;
    TestFalse(TEXT("The crop requires a completed range"), Editor.ApplyCrop());
    Editor.Crop->Range = FIntRect(1, 0, 3, 3);
    TestTrue(TEXT("Confirmed crop rebases coordinates and preserves retained world positions"), Editor.ApplyCrop() &&
        Level->Definition.GridDimensions == FIntVector(2, 3, 2) &&
        Level->Definition.InitialBoxes[0].GridPosition == FIntVector(0, 1, 0) &&
        Level->Definition.GridToWorld({0, 1, 0}) == OriginalWorld &&
        Level->Definition.CellAt({1, 1, 0})->bBlocked &&
        Level->Definition.PlayerStart == FPuzzleLevelDefinition::InvalidPosition);
    TestTrue(TEXT("Undo restores the cells and spawn removed by the crop"), GEditor->UndoTransaction() &&
        Level->Definition.GridDimensions == FIntVector(4, 3, 3) &&
        Level->Definition.PlayerStart == FIntVector::ZeroValue && Level->Definition.CellAt({3, 2, 0})->bHasFloor);
    TestEqual(TEXT("Authoring the first asset does not affect another level"), Other->Definition.Goals.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleEditorPreviewTest, "ResonantRunes.Puzzle.Editor.IndependentViewport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleEditorPreviewTest::RunTest(const FString&)
{
    auto* Catalog = NewObject<UPuzzleLevelCatalog>();
    auto* Asset = NewObject<UPuzzleLevelAsset>();
    Asset->Definition = FPuzzleLevelDefinition::MakeShengLu();
    auto Author = MakeShared<FPuzzleAuthoring>(Catalog);
    Author->AddExisting(Asset);
    auto Widget = SNew(SPuzzleViewport).Model(Author);
    Widget->Refresh(true);
    UWorld* PreviewWorld = Widget->GetViewportClient()->GetWorld();
    TestTrue(TEXT("The viewport renders in an editor preview world"), PreviewWorld && PreviewWorld->WorldType == EWorldType::EditorPreview);
    TestTrue(TEXT("The level editor remains in its own world"), PreviewWorld != GEditor->GetEditorWorldContext().World());
    TestTrue(TEXT("The viewport has its own mode manager"), Widget->GetViewportClient()->GetModeTools() != &GLevelEditorModeTools());
    const auto ArrowPointsAlong = [PreviewWorld](const FVector& Direction)
    {
        for (TObjectIterator<UArrowComponent> Arrow; Arrow; ++Arrow)
        {
            if (Arrow->IsRegistered() && Arrow->GetWorld() == PreviewWorld)
                return Arrow->GetForwardVector().Equals(Direction);
        }
        return false;
    };
    TestTrue(TEXT("The preview uses a native arrow in the starting world direction"), ArrowPointsAlong(FVector::RightVector));
    Author->Select({EPuzzleTool::Player, Asset->Definition.PlayerStart});
    Author->RotatePlayerStart(true);
    Widget->Refresh(false);
    TestTrue(TEXT("Rotating the spawn refreshes the arrow's world direction"), ArrowPointsAlong(-FVector::ForwardVector));
    Author->SetLayer(1);
    Author->SetTool(EPuzzleTool::Floor);
    TestTrue(TEXT("Authoring refreshes the asset even without an open level in the preview"), Author->Place({0, 0, 1}));
    Widget->Refresh(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzlePIETest, "ResonantRunes.Puzzle.Editor.SavedLevelPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleCatalogVictoryTest, "ResonantRunes.Puzzle.Editor.VictoryFollowsCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
    class FPlayAuthoredLevel : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TSharedRef<FPuzzleAuthoring> Editor;
        UPuzzleLevelAsset* ExtraLevel;
        TArray<TObjectPtr<UPuzzleLevelAsset>> OriginalOrder;
        const FGuid Id;
        const double Deadline = FPlatformTime::Seconds() + 55.;
        FIntVector ExpectedBox;
        int32 ExpectedSP = 0;
        int32 Index = 0;
        double AimDeadline = 0.;
        double ResultStarted = -1.;
        bool bShotFormation = false;
        enum class EStep { Load, Aim, Confirm, Cast, Settle, Finish, Inspect, OpenExtra, CompleteExtra, AwaitExit, Stop };
        EStep Step = EStep::Load;

        static FSceneViewport* PlayViewport(const UWorld& World)
        {
            auto* Client = World.GetGameViewport();
            return Client ? Client->GetGameViewport() : nullptr;
        }

        static void Shot(const UWorld& World, const TCHAR* Name)
        {
            if (!FParse::Param(FCommandLine::Get(), TEXT("RuneScreens"))) return;
            auto* Viewport = PlayViewport(World);
            if (!Viewport) return;
            TArray<FColor> Pixels;
            if (!GetViewportScreenShot(Viewport, Pixels)) return;
            for (FColor& Pixel : Pixels) Pixel.A = 255;
            const FIntPoint Size = Viewport->GetRenderTargetTextureSizeXY();
            const FString Path = FPaths::ProjectSavedDir() / TEXT("PuzzleVerification") / Name;
            FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Size.X, Size.Y));
        }

        static void AimAtBox(AShengLuBoard& Board)
        {
            auto* Camera = CastChecked<UCameraComponent>(Board.GetDefaultSubobjectByName(TEXT("CastCamera")));
            Camera->SetWorldRotation((Board.BoxMesh(2)->GetComponentLocation() - Camera->GetComponentLocation()).Rotation());
        }
    public:
        FPlayAuthoredLevel(FAutomationTestBase& InTest, TSharedRef<FPuzzleAuthoring> InEditor,
            UPuzzleLevelAsset* InExtraLevel, TArray<TObjectPtr<UPuzzleLevelAsset>> InOriginalOrder)
            : Test(InTest), Editor(MoveTemp(InEditor)), ExtraLevel(InExtraLevel), OriginalOrder(MoveTemp(InOriginalOrder)),
              Id(Editor->Level()->LevelId) {}

        ~FPlayAuthoredLevel() override
        {
            if (ExtraLevel) Editor->Catalog()->Levels = MoveTemp(OriginalOrder);
        }

        bool Update() override
        {
            if (FPlatformTime::Seconds() > Deadline)
            {
                Test.AddError(FString::Printf(TEXT("Timed out in PIE at action %d step %d"), Index, int32(Step)));
                if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
                return true;
            }
            if (Step == EStep::Stop)
            {
                if (GEditor->PlayWorld) return false;
                Test.TestEqual(TEXT("PIE returns to the same authored asset"), Editor->Level()->LevelId, Id);
                Test.TestEqual(TEXT("Saved box position is unchanged by PIE"), Editor->Level()->InitialBoxes.Last().GridPosition, FIntVector(1, 1, 0));
                Test.TestFalse(TEXT("Playing did not dirty the asset"), Editor->Asset()->GetOutermost()->IsDirty());
                return true;
            }
            if (Step == EStep::AwaitExit)
            {
                if (GEditor->PlayWorld) return false;
                Test.TestTrue(TEXT("The campaign finish action exits the play session"), true);
                Step = EStep::Stop;
                return false;
            }
            UWorld* World = GEditor->PlayWorld;
            APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
            auto* Board = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
            if (!Board || !Board->HasActorBegunPlay()) return false;
            const auto Tap = [PC](FKey Key)
            {
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.f));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.f));
            };
            if (Step == EStep::OpenExtra)
            {
                if (Board->Level().LevelId != ExtraLevel->Definition.LevelId) return false;
                Test.TestFalse(TEXT("Entering the newly appended final level does not show victory"), Board->ShouldShowCampaignVictory());
                Test.TestTrue(TEXT("Solve the appended level"), Board->TryPuzzleAction({ERuneAction::Pulse, 1, {1, 0, 0}}));
                Test.TestFalse(TEXT("The victory prompt waits for the final box animation"), Board->ShouldShowCampaignVictory());
                Step = EStep::CompleteExtra;
                return false;
            }
            if (Step == EStep::CompleteExtra)
            {
                if (Board->IsAnimating()) return false;
                if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                {
                    const double Now = World->GetRealTimeSeconds();
                    if (ResultStarted < 0.)
                    {
                        ResultStarted = Now;
                        if (auto* Viewport = PlayViewport(*World))
                        {
                            Viewport->SetFixedViewportSize(1600, 900);
                            Viewport->SetMouse(1030, 590);
                        }
                    }
                    if (Now - ResultStarted < 1.2) return false;
                    Shot(*World, TEXT("RuneHUD-Cursor-Victory.png"));
                }
                Test.TestTrue(TEXT("The newly appended catalog tail shows full victory when cleared"), Board->ShouldShowCampaignVictory());
                Test.TestFalse(TEXT("The catalog tail has no next level"), Board->CanAdvanceLevel());
                Test.TestTrue(TEXT("Full victory leaves the software cursor available"),
                    PC->bShowMouseCursor && PC->GetMouseCursor() == EMouseCursor::None);
                Tap(EKeys::Enter);
                Step = EStep::AwaitExit;
                return false;
            }
            const FIntVector GoalPosition = Editor->Level()->Goals[0].GridPosition;
            auto* Mesh = Board->BoxMesh(2);
            const auto* BoxPawn = Mesh ? Cast<ARuneBoxPawn>(Mesh->GetOwner()) : nullptr;
            if (!Test.TestNotNull(TEXT("Each authored box uses its own Pawn"), BoxPawn))
            {
                GEditor->RequestEndPlayMap();
                Step = EStep::Stop;
                return false;
            }
            const auto CheckTags = [&]
            {
                const auto* State = Board->Runtime().FindBox(2);
                const auto* ASC = BoxPawn->GetAbilitySystemComponent();
                const auto Check = [&](const TCHAR* Name, bool bExpected)
                {
                    Test.TestEqual(Name, ASC->GetTagCount(FGameplayTag::RequestGameplayTag(FName(Name))), bExpected ? 1 : 0);
                };
                Check(TEXT("Puzzle.Box.Levitating"), State->LevitationTurns > 0);
                Check(TEXT("Puzzle.Box.PhaseLocked"), State->bPhaseLocked);
                Check(TEXT("Puzzle.Box.Hiss"), State->bHiss);
                Test.TestEqual(TEXT("Each status tag comes from one active Gameplay Effect"), ASC->GetActiveEffects(FGameplayEffectQuery()).Num(),
                    int32(State->LevitationTurns > 0) + int32(State->bPhaseLocked) + int32(State->bHiss));
            };
            if (Step == EStep::Load)
            {
                if (!ExtraLevel && FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                    if (auto* Viewport = PlayViewport(*World)) Viewport->SetFixedViewportSize(1920, 1080);
                if (!Test.TestEqual(TEXT("PIE game uses the selected saved asset"), Board->Level().LevelId, Id))
                {
                    GEditor->RequestEndPlayMap();
                    Step = EStep::Stop;
                    return false;
                }
                Test.TestEqual(TEXT("Authored spawn reaches runtime"), Board->Runtime().PlayerPosition, Editor->Level()->PlayerStart);
                Test.TestEqual(TEXT("Viewport and game share world positions"), Board->Cell(GoalPosition), Editor->Level()->GridToWorld(GoalPosition));
                Test.TestEqual(TEXT("Runtime mesh uses authored box transform"), Board->BoxMesh(2)->GetComponentLocation(),
                    Editor->Level()->BoxBounds({1, 1, 0}).GetCenter());
                Test.TestEqual(TEXT("The box Pawn keeps its authored ID"), BoxPawn->GetBoxId(), 2);
                Test.TestNull(TEXT("The box Pawn is never possessed"), BoxPawn->GetController());
                const auto* OtherBox = CastChecked<ARuneBoxPawn>(Board->BoxMesh(1)->GetOwner());
                Test.TestTrue(TEXT("The other box has its own ability system"),
                    OtherBox->GetAbilitySystemComponent() != BoxPawn->GetAbilitySystemComponent());
                Test.TestTrue(TEXT("The saved tutorial enables just slots 1 and 4"),
                    Board->Level().AvailableSkills == TArray<ERuneSkill>{ERuneSkill::Pulse, ERuneSkill::Hiss});
                Test.TestFalse(TEXT("The ordinary pair cannot overlap before casting Hiss"),
                    Board->Evaluate({ERuneAction::Pulse, 2, {1, 0, 0}}).IsValid());
                CheckTags();
                Step = EStep::Aim;
                return false;
            }
            constexpr ERuneAction Actions[] = {ERuneAction::Hiss, ERuneAction::Pulse};
            if (Step == EStep::Aim)
            {
                if (Board->IsAnimating()) return false;
                Board->SelectSkill(Actions[Index]);
                AimAtBox(*Board);
                AimDeadline = FPlatformTime::Seconds() + 5.;
                Step = EStep::Confirm;
                return false;
            }
            if (Step == EStep::Confirm)
            {
                const auto Preview = Board->Preview();
                if (Board->AimBoxId() != 2 && FPlatformTime::Seconds() < AimDeadline)
                {
                    AimAtBox(*Board);
                    return false;
                }
                if (!Preview.IsValid() || Board->AimBoxId() != 2)
                {
                    Test.AddError(FString::Printf(TEXT("Could not preview action %d on #2, aim #%d: %s"),
                        Index, Board->AimBoxId(), *Preview.Reason));
                    GEditor->RequestEndPlayMap();
                    Step = EStep::Stop;
                    return false;
                }
                ExpectedBox = Preview.After.FindBox(2)->GridPosition;
                ExpectedSP = Preview.After.SP;
                CheckTags();
                Step = EStep::Cast;
                return false;
            }
            if (Step == EStep::Cast)
            {
                if (!ExtraLevel)
                {
                    const TCHAR* Shots[] = {TEXT("RuneHUD-Hiss.png"), TEXT("RuneHUD-Pulse.png")};
                    Shot(*World, Shots[Index]);
                }
                if (Actions[Index] == ERuneAction::Pulse) Board->ClickBoard();
                else Board->Confirm();
                Step = EStep::Settle;
                return false;
            }
            if (Step == EStep::Settle)
            {
                if (Board->Runtime().Turn <= Index || Board->IsAnimating()) return false;
                Test.TestEqual(TEXT("GAS commit matches predicted SP"), Board->Runtime().SP, ExpectedSP);
                Test.TestEqual(TEXT("GAS commit matches predicted landing cell"), Board->Runtime().FindBox(2)->GridPosition, ExpectedBox);
                CheckTags();
                if (++Index < UE_ARRAY_COUNT(Actions))
                {
                    Test.TestTrue(TEXT("The new Hiss box is ready to push, and the goal is still open"),
                        Board->Runtime().FindBox(2)->bHiss && !Board->IsRestored());
                    Step = EStep::Aim;
                    return false;
                }
                Board->ReleaseClick();
                Test.TestTrue(TEXT("The Hiss lesson clears by converting one box and pushing it into the other"), Board->IsRestored());
                Test.TestEqual(TEXT("Hiss and Pulse spend 4 SP"), Board->Level().InitialSP - Board->Runtime().SP, 4);
                Test.TestEqual(TEXT("Both independent boxes occupy the goal"), Board->Runtime().GetBoxesAt(GoalPosition).Num(), 2);
                const bool bIsCatalogTail = Editor->Catalog()->NextLevel(Editor->Asset()) == nullptr;
                Test.TestEqual(TEXT("The solved fourth level opens the prompt if it is the catalog tail"),
                    Board->ShouldShowCampaignVictory(), bIsCatalogTail);
                Test.TestTrue(TEXT("Completing the board retains the first-person camera"), Board->IsFirstPerson());
                Step = EStep::Finish;
                return false;
            }
            if (ExtraLevel)
            {
                Test.TestEqual(TEXT("The fourth level now leads to the appended entry"), Board->NextPuzzleLevel(),
                    static_cast<const UPuzzleLevelAsset*>(ExtraLevel));
                Test.TestTrue(TEXT("The fourth level can advance instead of showing full victory"),
                    Board->CanAdvanceLevel() && !Board->ShouldShowCampaignVictory());
                Board->AdvanceLevel();
                Step = EStep::OpenExtra;
                return false;
            }
            if (Step == EStep::Finish && Board->ShouldShowCampaignVictory())
            {
                if (FParse::Param(FCommandLine::Get(), TEXT("RuneScreens")))
                {
                    if (ResultStarted < 0.) ResultStarted = World->GetRealTimeSeconds();
                    const double Age = World->GetRealTimeSeconds() - ResultStarted;
                    if (Age > .40 && !bShotFormation)
                    {
                        Shot(*World, TEXT("RuneHUD-Formation.png"));
                        bShotFormation = true;
                    }
                    if (Age < 1.30) return false;
                    Shot(*World, TEXT("RuneHUD-Campaign.png"));
                    const FIntPoint Size = PlayViewport(*World)->GetRenderTargetTextureSizeXY();
                    const float Scale = FMath::Min(Size.X / 1280.f, Size.Y / 720.f);
                    Board->ClickAt(FVector2D(Size) * .5 + FVector2D(248 * Scale, 227 * Scale));
                }
                else Tap(EKeys::Escape);
                Step = EStep::Inspect;
                return false;
            }
            if (Step == EStep::Inspect && Board->ShouldShowCampaignVictory()) return false;
            if (Step == EStep::Inspect)
                Test.TestTrue(TEXT("Inspect keeps the completed board and first-person camera"), Board->IsRestored() && Board->IsFirstPerson());
            Board->Undo();
            Test.TestFalse(TEXT("Undo removes the completed overlap"), Board->IsRestored());
            Test.TestEqual(TEXT("Undo refreshes the occupancy cache"), Board->Runtime().GetBoxesAt(GoalPosition).Num(), 1);
            Board->Undo();
            CheckTags();
            Board->ResetPuzzle();
            Test.TestEqual(TEXT("Restart restores initial SP"), Board->Runtime().SP, Editor->Level()->InitialSP);
            Test.TestEqual(TEXT("Restart restores the authored position"), Board->Runtime().FindBox(2)->GridPosition, FIntVector(1, 1, 0));
            Test.TestFalse(TEXT("Restart restores initial phase"), Board->Runtime().FindBox(2)->bHiss);
            CheckTags();
            GEditor->RequestEndPlayMap();
            Step = EStep::Stop;
            return false;
        }
    };
}

static bool PlaySavedBoard(FAutomationTestBase& Test, bool bAppendLevel)
{
    const auto Authoring = MakeShared<FPuzzleAuthoring>();
    auto* Catalog = Authoring->Catalog();
    if (!Test.TestNotNull(TEXT("Catalog reopens from disk"), Catalog)) return false;
    const auto* First = Catalog->NextLevel();
    if (!Test.TestNotNull(TEXT("The catalog provides a first level as the tile reference"), First)) return false;
    for (const UPuzzleLevelAsset* Level : Catalog->Levels)
        if (Level && Level->Definition.LevelId.IsValid())
            Test.TestEqual(*FString::Printf(TEXT("%s uses the first level's floor tile dimensions"), *Level->Definition.DisplayName.ToString()),
                Level->Definition.FloorBounds(FIntVector::ZeroValue).GetSize(), First->Definition.FloorBounds(FIntVector::ZeroValue).GetSize());
    const auto* Imported = Catalog->FindLevel(FPuzzleLevelDefinition::MakeShengLu().LevelId);
    if (!Test.TestNotNull(TEXT("Migrated second level reopens from disk"), Imported)) return false;
    Test.TestEqual(TEXT("Imported level has the original box"), Imported->Definition.InitialBoxes[0].GridPosition, FIntVector(1, 1, 0));
    Test.TestEqual(TEXT("Imported level has the original goal"), Imported->Definition.Goals[0].GridPosition, FIntVector(2, 1, 1));
    Test.TestEqual(TEXT("Imported level has the original SP"), Imported->Definition.InitialSP, 8);
    // The bundled example keeps its identity when the author changes its display name.
    const FGuid ExampleId(0x7E4C4A8E, 0x480CB651, 0xBD6EC8A8, 0x5A3BDA78);
    UPuzzleLevelAsset* Example = Catalog->FindLevel(ExampleId);
    if (!Example)
    {
        for (UPuzzleLevelAsset* Level : Catalog->Levels)
        {
            if (!Level || Level->Definition.DisplayName.ToString() != TEXT("Z1 叠相练习")) continue;
            Example = Level;
            break;
        }
    }
    if (!Test.TestNotNull(TEXT("The saved Hiss tutorial"), Example)) return false;
    UPuzzleLevelAsset* Extra = nullptr;
    TArray<TObjectPtr<UPuzzleLevelAsset>> OriginalOrder;
    if (bAppendLevel)
    {
        Extra = NewObject<UPuzzleLevelAsset>();
        auto& Added = Extra->Definition;
        Added.LevelId = FGuid::NewGuid();
        Added.DisplayName = FText::FromString(TEXT("Appended victory fixture"));
        Added.GridDimensions = {3, 1, 1};
        Added.GridCellSize = First->Definition.GridCellSize;
        Added.StaticCells.SetNum(3);
        for (auto& Cell : Added.StaticCells) Cell.bHasFloor = true;
        Added.PlayerStart = {0, 0, 0};
        Added.InitialBoxes.Add({1, {1, 0, 0}});
        Added.Goals.Add({1, {2, 0, 0}, 1});
        OriginalOrder = Catalog->Levels;
        Catalog->Levels.RemoveSingle(Example);
        Catalog->Levels.Add(Example);
        Catalog->Levels.Add(Extra);
    }
    Authoring->SetActive(Example);
    const auto Play = MakeShared<FPlayAuthoredLevel>(Test, Authoring, Extra, MoveTemp(OriginalOrder));
    if (!Test.TestTrue(TEXT("Test Level starts a valid saved definition"), Authoring->TestLevel())) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(Play);
    return true;
}

bool FPuzzlePIETest::RunTest(const FString&) { return PlaySavedBoard(*this, false); }
bool FPuzzleCatalogVictoryTest::RunTest(const FString&) { return PlaySavedBoard(*this, true); }

#endif
