#if WITH_DEV_AUTOMATION_TESTS

#include "PuzzleSolver.h"
#include "Misc/AutomationTest.h"

namespace
{
    FPuzzleLevelDefinition TestLevel()
    {
        FPuzzleLevelDefinition Level;
        Level.LevelId = FGuid::NewGuid();
        Level.DisplayName = FText::FromString(TEXT("Rules fixture"));
        Level.GridDimensions = {5, 4, 4};
        Level.GridOrigin = FVector(-420, 650, 110);
        Level.GridCellSize = 175;
        Level.StaticCells.SetNum(FPuzzleLevelDefinition::CellCount(Level.GridDimensions));
        for (int32 Index = 0; Index < 20; ++Index) Level.StaticCells[Index].bHasFloor = true;
        Level.PlayerStart = {0, 0, 0};
        Level.InitialSP = 20;
        Level.InitialBoxes.Add({23, {1, 1, 0}});
        Level.Goals.Add({8, {4, 3, 2}, 23});
        return Level;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleGridTest, "ResonantRunes.Puzzle.GridAndValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleGridTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    TestTrue(TEXT("A suspended goal has a valid layout"), FPuzzleSolver::Validate(Level).IsEmpty());
    for (int32 Index = 0; Index < Level.StaticCells.Num(); ++Index)
    {
        const auto Position = Level.IndexToGrid(Index);
        TestEqual(TEXT("Dense index round trip"), Level.GridToIndex(Position), Index);
        TestEqual(TEXT("Grid and world round trip"), Level.WorldToGrid(Level.GridToWorld(Position)), Position);
        TestEqual(TEXT("Offset still snaps to the same XYZ"), Level.WorldToGrid(Level.GridToWorld(Position) + FVector(80, -80, 80)), Position);
    }
    TestEqual(TEXT("X is the fastest axis, followed by Y and Z"), Level.GridToIndex({2, 1, 2}), 47);
    TestEqual(TEXT("Outside X cannot alias the next row"), Level.GridToIndex({5, 0, 0}), INDEX_NONE);
    TestEqual(TEXT("Outside Z cannot index dense cells"), Level.GridToIndex({0, 0, 4}), INDEX_NONE);
    TestNull(TEXT("Invalid coordinates cannot access dense cells"), Level.CellAt({-1, 2, 2}));
    TestEqual(TEXT("Huge dimensions cannot overflow"), FPuzzleLevelDefinition::CellCount({MAX_int32, MAX_int32, MAX_int32}), 0);
    Level.bSwapXY = true;
    Level.LayerHeightOverride = 90.f;
    TestEqual(TEXT("Migrated map dimensions round trip"), Level.WorldToGrid(Level.GridToWorld({3, 1, 2})), FIntVector(3, 1, 2));
    Level.StaticCells.Pop();
    TestFalse(TEXT("Incomplete dense data is rejected"), FPuzzleSolver::Validate(Level).IsEmpty());

    Level = TestLevel();
    Level.AvailableSkills = {ERuneSkill::Hiss, ERuneSkill::Pulse};
    TestTrue(TEXT("A sparse skill list is valid"), FPuzzleSolver::Validate(Level).IsEmpty());
    TestTrue(TEXT("List order does not remap the number keys"),
        RuneSkills::Slot(ERuneAction::Hiss) == 4 && RuneSkills::IsUnlocked(Level, ERuneAction::Hiss) &&
        RuneSkills::Slot(ERuneAction::Pulse) == 1 && RuneSkills::IsUnlocked(Level, ERuneAction::Pulse));
    TestFalse(TEXT("A listed skill does not grant the omitted wind ability"), RuneSkills::IsUnlocked(Level, ERuneAction::LevitateSelf));
    TestFalse(TEXT("A listed skill does not grant the omitted water ability"), RuneSkills::IsUnlocked(Level, ERuneAction::WaterAnchor));
    Level.AvailableSkills.Add(ERuneSkill::Hiss);
    TestFalse(TEXT("Duplicate skill entries are invalid"), FPuzzleSolver::Validate(Level).IsEmpty());
    Level.AvailableSkills = {static_cast<ERuneSkill>(255)};
    TestFalse(TEXT("Unknown skill entries are invalid"), FPuzzleSolver::Validate(Level).IsEmpty());
    Level.AvailableSkills.Reset();
    TestTrue(TEXT("An explicit empty skill list is valid"), FPuzzleSolver::Validate(Level).IsEmpty());
    TestTrue(TEXT("Walking is available without any skills"),
        FPuzzleSolver::Evaluate(Level, FPuzzleRuntimeState::FromDefinition(Level), {ERuneAction::North}).IsValid());

    Level = TestLevel();
    Level.InitialBoxes.Add({23, {3, 3, 0}});
    Level.InitialBoxes[0].LevitationTurns = 3;
    Level.InitialSP = -1;
    Level.PlayerStart = {5, 0, 0};
    Level.Goals.Add({8, {1, 1, 2}, 99});
    const auto Errors = FPuzzleSolver::Validate(Level);
    TestTrue(TEXT("Invalid spawn, SP, duplicate IDs, missing target and illegal wind are collected"), Errors.Num() >= 6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleOverlapTest, "ResonantRunes.Puzzle.OccupancyAndGoals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleOverlapTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    Level.PlayerStart = {0, 1, 0};
    Level.InitialBoxes.Add({5, {2, 1, 0}});
    Level.InitialBoxes.Add({99, {3, 1, 0}, true});
    Level.Goals = {{2, {2, 1, 0}, 5, ERuneGoalState::Any, true, 23}};
    auto State = FPuzzleRuntimeState::FromDefinition(Level);
    const FPuzzleAction Push{ERuneAction::Pulse, 23, {1, 0, 0}};
    auto Result = FPuzzleSolver::Evaluate(Level, State, Push);
    TestFalse(TEXT("Ordinary box cannot move into another ordinary box"), Result.IsValid());
    TestEqual(TEXT("Rejected action keeps SP"), Result.After.SP, State.SP);
    TestEqual(TEXT("Rejected action keeps turn"), Result.After.Turn, State.Turn);

    const auto Hiss = FPuzzleSolver::Evaluate(Level, State, {ERuneAction::Hiss, 23});
    if (!TestTrue(TEXT("Hiss can target a box by its non-index ID"), Hiss.IsValid())) return false;
    TestFalse(TEXT("Preview leaves original initial box normal"), State.FindBox(23)->bHiss);
    TestTrue(TEXT("Target instance becomes Hiss"), Hiss.After.FindBox(23)->bHiss);
    Result = FPuzzleSolver::Evaluate(Level, Hiss.After, Push);
    if (!TestTrue(TEXT("Hiss can move into an occupied normal cell"), Result.IsValid())) return false;
    TestEqual(TEXT("Both boxes are in the same occupancy bucket"), Result.After.GetBoxesAt({2, 1, 0}).Num(), 2);
    const FBox NormalBounds = Level.BoxBounds(Result.After.FindBox(5)->GridPosition);
    const FBox HissBounds = Level.BoxBounds(Result.After.FindBox(23)->GridPosition, true);
    TestTrue(TEXT("A phased box retains full size"), HissBounds.GetSize().Equals(NormalBounds.GetSize()));
    TestTrue(TEXT("Overlapping boxes still intersect without coplanar faces"), NormalBounds.Intersect(HissBounds) &&
        (HissBounds.GetCenter() - NormalBounds.GetCenter()).GetAbsMin() > 0.);
    TestEqual(TEXT("Moving the first box clears its original bucket"), Result.After.GetBoxesAt({1, 1, 0}).Num(), 0);
    TestTrue(TEXT("Goal matches the specified normal/Hiss pair"), FPuzzleSolver::IsRestored(Level, Result.After));
    TestFalse(TEXT("Position alone cannot satisfy the pair"), FPuzzleSolver::IsRestored(Level, Hiss.After));

    Level.Goals[0].GridPosition = {4, 3, 1};
    const auto Overlap = Result.After;
    TestFalse(TEXT("Two Hiss boxes cannot share"), FPuzzleSolver::Evaluate(Level, Overlap, Push).IsValid());
    TestFalse(TEXT("Changing a shared normal box into Hiss is illegal"), FPuzzleSolver::Evaluate(Level, Overlap, {ERuneAction::Hiss, 5}).IsValid());
    auto FromSide = Overlap;
    FromSide.PlayerPosition = {4, 1, 0};
    TestFalse(TEXT("A third box cannot enter a shared cell"), FPuzzleSolver::Evaluate(Level, FromSide, {ERuneAction::Pulse, 99, {-1, 0, 0}}).IsValid());
    FromSide.PlayerPosition = {2, 0, 0};
    const auto MoveNormal = FPuzzleSolver::Evaluate(Level, FromSide, {ERuneAction::Pulse, 5, {0, 1, 0}});
    if (!TestTrue(TEXT("One box can leave a shared cell without moving the other"), MoveNormal.IsValid())) return false;
    TestEqual(TEXT("Hiss box retains its independent position"), MoveNormal.After.FindBox(23)->GridPosition, FIntVector(2, 1, 0));
    TestEqual(TEXT("Normal box moves separately"), MoveNormal.After.FindBox(5)->GridPosition, FIntVector(2, 2, 0));
    Level.Goals.Add({3, {0, 3, 0}, 5, ERuneGoalState::Hiss});
    TestFalse(TEXT("Contradictory required states and positions are invalid"), FPuzzleSolver::Validate(Level).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleHissObstacleTest, "ResonantRunes.Puzzle.HissObstacle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleHissObstacleTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    const FIntVector Wall(2, 1, 0);
    Level.StaticCells[Level.GridToIndex(Wall)].bBlocked = true;
    const auto State = FPuzzleRuntimeState::FromDefinition(Level);
    const FPuzzleAction Push{ERuneAction::Pulse, 23, {1, 0, 0}};
    const FPuzzleAction PhaseWall{ERuneAction::Hiss, INDEX_NONE, FIntVector::ZeroValue, Wall};
    TestFalse(TEXT("A solid obstacle blocks an ordinary box"), FPuzzleSolver::Evaluate(Level, State, Push).IsValid());
    TestFalse(TEXT("A floor is not an obstacle target"),
        FPuzzleSolver::Evaluate(Level, State, {ERuneAction::Hiss, INDEX_NONE, FIntVector::ZeroValue, {3, 1, 0}}).IsValid());

    const auto WallPreview = FPuzzleSolver::Evaluate(Level, State, PhaseWall);
    if (!TestTrue(TEXT("Mirror casts on an obstacle cell"), WallPreview.IsValid())) return false;
    TestTrue(TEXT("Only the predicted state phases the wall"), WallPreview.After.HissObstacles.Contains(Wall) && State.HissObstacles.IsEmpty());
    TestEqual(TEXT("Obstacle cast costs two SP"), WallPreview.After.SP, State.SP - 2);
    TestEqual(TEXT("Obstacle cast spends one turn"), WallPreview.After.Turn, State.Turn + 1);
    TestFalse(TEXT("Repeating the obstacle cast is rejected"), FPuzzleSolver::Evaluate(Level, WallPreview.After, PhaseWall).IsValid());
    const auto IntoPhaseWall = FPuzzleSolver::Evaluate(Level, WallPreview.After, Push);
    TestTrue(TEXT("An ordinary box can share a phased obstacle cell"),
        IntoPhaseWall.IsValid() && IntoPhaseWall.After.GetBoxesAt(Wall).Contains(23) && !IntoPhaseWall.After.FindBox(23)->bHiss);
    TestTrue(TEXT("The wall top still supports the layer above"), FPuzzleSolver::HasStaticSupport(Level, Wall + FIntVector(0, 0, 1)));

    const auto BoxPreview = FPuzzleSolver::Evaluate(Level, State, {ERuneAction::Hiss, 23});
    const auto IntoSolidWall = FPuzzleSolver::Evaluate(Level, BoxPreview.After, Push);
    TestTrue(TEXT("A phased box can share a solid obstacle cell"),
        BoxPreview.IsValid() && IntoSolidWall.IsValid() && IntoSolidWall.After.GetBoxesAt(Wall).Contains(23));
    Level.AvailableSkills.Remove(ERuneSkill::Hiss);
    TestFalse(TEXT("The skill lock also applies to obstacle casts"), FPuzzleSolver::Evaluate(Level, State, PhaseWall).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleGravityTest, "ResonantRunes.Puzzle.GravityAndTurns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleGravityTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    const auto State = FPuzzleRuntimeState::FromDefinition(Level);
    const auto Lift = FPuzzleSolver::Evaluate(Level, State, {ERuneAction::Levitate, 23});
    if (!TestTrue(TEXT("Lift succeeds"), Lift.IsValid())) return false;
    TestEqual(TEXT("Levitate moves up by one"), Lift.BeforeGravity, FIntVector(1, 1, 1));
    TestEqual(TEXT("Levitate keeps two later turns"), Lift.After.FindBox(23)->LevitationTurns, 2);
    const auto Wait = FPuzzleSolver::Evaluate(Level, Lift.After, {ERuneAction::North});
    const auto Fall = FPuzzleSolver::Evaluate(Level, Wait.After, {ERuneAction::North});
    TestTrue(TEXT("Wind expires and the box settles at Z0"), Fall.IsValid() && Fall.After.FindBox(23)->GridPosition.Z == 0);
    const FPuzzleAction PushUp{ERuneAction::Pulse, 23, {0, 0, 1}};
    TestFalse(TEXT("A box without wind or an anchor cannot be pulsed vertically"), FPuzzleSolver::Evaluate(Level, State, PushUp).IsValid());
    const auto WindUp = FPuzzleSolver::Evaluate(Level, Lift.After, PushUp);
    TestTrue(TEXT("An active wind allows a vertical pulse and one more action"), WindUp.IsValid() &&
        WindUp.After.FindBox(23)->GridPosition == FIntVector(1, 1, 2) && WindUp.After.FindBox(23)->LevitationTurns == 1);
    const FPuzzleAction PushEast{ERuneAction::Pulse, 23, {1, 0, 0}};
    Level.Goals[0].GridPosition = {2, 1, 2};
    Level.Goals[0].BoxId = FRuneGoalData::AnyBoxId;
    const auto WindArrival = FPuzzleSolver::Evaluate(Level, WindUp.After, PushEast);
    TestTrue(TEXT("The last wind turn can complete an airborne goal that accepts any box"), WindArrival.IsValid() &&
        WindArrival.After.FindBox(23)->GridPosition == Level.Goals[0].GridPosition && FPuzzleSolver::IsRestored(Level, WindArrival.After));
    Level.Goals[0].GridPosition = {3, 1, 2};
    const auto Miss = FPuzzleSolver::Evaluate(Level, WindUp.After, PushEast);
    TestTrue(TEXT("A push on the last wind turn still drops off-goal"), Miss.IsValid() &&
        Miss.After.FindBox(23)->GridPosition == FIntVector(2, 1, 0) && Miss.After.FindBox(23)->LevitationTurns == 0);
    auto Simultaneous = Level;
    Simultaneous.InitialBoxes = {{23, {1, 1, 2}, false, false, 1}, {41, {3, 1, 1}, false, false, 1}};
    Simultaneous.Goals = {{8, {2, 1, 2}, 23}, {9, {3, 1, 0}, 41}};
    const auto DoubleArrival = FPuzzleSolver::Evaluate(Simultaneous, FPuzzleRuntimeState::FromDefinition(Simultaneous), PushEast);
    TestTrue(TEXT("One box can arrive under the last wind turn while another falls into its own goal"),
        DoubleArrival.IsValid() && FPuzzleSolver::IsRestored(Simultaneous, DoubleArrival.After));
    const auto Lock = FPuzzleSolver::Evaluate(Level, Lift.After, {ERuneAction::WaterAnchor, 23});
    const auto Up = FPuzzleSolver::Evaluate(Level, Lock.After, PushUp);
    TestTrue(TEXT("An anchored box accepts vertical push and remains lifted"), Up.IsValid() && Up.After.FindBox(23)->GridPosition.Z == 2);
    const FPuzzleAction PushDown{ERuneAction::Pulse, 23, {0, 0, -1}};
    TestFalse(TEXT("Vertical pull from below is rejected"), FPuzzleSolver::Evaluate(Level, Up.After, PushDown).IsValid());
    auto Above = Up.After;
    Above.PlayerPosition = {1, 1, 3};
    Above.PlayerWindTurns = 4;
    const auto Down = FPuzzleSolver::Evaluate(Level, Above, PushDown);
    TestTrue(TEXT("Pushing from above can press an anchored box down"), Down.IsValid() && Down.After.FindBox(23)->GridPosition.Z == 1);

    Level.StaticCells[Level.GridToIndex({1, 1, 1})].bBlocked = true;
    Level.InitialBoxes[0] = {23, {1, 1, 3}, true};
    auto High = FPuzzleRuntimeState::FromDefinition(Level);
    const auto Land = FPuzzleSolver::Evaluate(Level, High, {ERuneAction::North});
    TestTrue(TEXT("An obstacle top supports a falling Hiss box"), Land.IsValid() && Land.After.FindBox(23)->GridPosition.Z == 2);
    Level.InitialBoxes[0].GridPosition.Z = 2;
    Level.InitialBoxes[0].bPhaseLocked = true;
    High = FPuzzleRuntimeState::FromDefinition(Level);
    High.PlayerPosition = {1, 1, 3};
    High.PlayerWindTurns = 4;
    const auto Inside = FPuzzleSolver::Evaluate(Level, High, PushDown);
    TestTrue(TEXT("Anchored Hiss can enter a hard obstacle with a vertical push"),
        Inside.IsValid() && Inside.After.FindBox(23)->GridPosition.Z == 1);

    Level = TestLevel();
    Level.InitialBoxes[0].GridPosition = {1, 1, 2};
    Level.InitialBoxes.Add({41, {1, 1, 3}});
    const auto First = FPuzzleSolver::Evaluate(Level, FPuzzleRuntimeState::FromDefinition(Level), {ERuneAction::North});
    Swap(Level.InitialBoxes[0], Level.InitialBoxes[1]);
    const auto Second = FPuzzleSolver::Evaluate(Level, FPuzzleRuntimeState::FromDefinition(Level), {ERuneAction::North});
    TestTrue(TEXT("Falling stack settles bottom-first regardless of array order"), First.IsValid() && Second.IsValid() &&
        First.After.FindBox(23)->GridPosition.Z == 0 && First.After.FindBox(41)->GridPosition.Z == 1 &&
        Second.After.FindBox(23)->GridPosition == First.After.FindBox(23)->GridPosition &&
        Second.After.FindBox(41)->GridPosition == First.After.FindBox(41)->GridPosition);

    Level = TestLevel();
    Level.Goals[0].GridPosition = Level.InitialBoxes[0].GridPosition;
    Level.Goals[0].RequiredState = ERuneGoalState::Normal;
    Level.Goals[0].bRequireWaterAnchor = true;
    const auto WaterStart = FPuzzleRuntimeState::FromDefinition(Level);
    TestFalse(TEXT("A normal box on the target does not satisfy a water-anchor goal by position alone"),
        FPuzzleSolver::IsRestored(Level, WaterStart));
    const auto Anchored = FPuzzleSolver::Evaluate(Level, WaterStart, {ERuneAction::WaterAnchor, 23});
    TestTrue(TEXT("A genuine water anchor satisfies the additional goal condition"),
        Anchored.IsValid() && FPuzzleSolver::IsRestored(Level, Anchored.After));
    Level.AvailableSkills.Remove(ERuneSkill::WaterAnchor);
    TestFalse(TEXT("A water goal without the skill or an initial anchor is invalid"), FPuzzleSolver::Validate(Level).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzleAnyBoxTest, "ResonantRunes.Puzzle.AnyBoxGoal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzleAnyBoxTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    const FIntVector GoalCell(2, 1, 0);
    const int32 AnyBox = FRuneGoalData::AnyBoxId;
    Level.InitialBoxes.Add({41, GoalCell, true, true});
    Level.Goals = {{8, GoalCell, AnyBox, ERuneGoalState::Hiss, false, INDEX_NONE, true}};
    const auto Start = FPuzzleRuntimeState::FromDefinition(Level);
    TestTrue(TEXT("An unnumbered goal accepts a matching anchored Hiss crate"), FPuzzleSolver::IsRestored(Level, Start));
    Level.Goals[0].BoxId = 23;
    TestFalse(TEXT("A numbered goal still demands its own crate"), FPuzzleSolver::IsRestored(Level, Start));

    Level.Goals = {{8, GoalCell, AnyBox, ERuneGoalState::Any, true, AnyBox, true}};
    Level.AvailableSkills.Remove(ERuneSkill::WaterAnchor);
    TestFalse(TEXT("An anchor on the Hiss slot cannot satisfy the normal slot"),
        FPuzzleSolver::Validate(Level).IsEmpty() || FPuzzleSolver::IsRestored(Level, Start));
    Level.InitialBoxes[0].bPhaseLocked = true;
    TestTrue(TEXT("Both wildcard slots have distinct candidates"), FPuzzleSolver::Validate(Level).IsEmpty());
    const auto Result = FPuzzleSolver::Evaluate(Level, FPuzzleRuntimeState::FromDefinition(Level), {ERuneAction::Pulse, 23, {1, 0, 0}});
    TestTrue(TEXT("The ordinary box can be pushed from a diagonal position to complete the pair"),
        Result.IsValid() && FPuzzleSolver::IsRestored(Level, Result.After));
    Level.InitialBoxes.RemoveAt(1);
    TestFalse(TEXT("One box cannot fill both slots even when it can become Hiss"), FPuzzleSolver::Validate(Level).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPuzzlePlayerWindTest, "ResonantRunes.Puzzle.PlayerWindAndAim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FPuzzlePlayerWindTest::RunTest(const FString&)
{
    auto Level = TestLevel();
    Level.InitialBoxes.Add({24, {1, 0, 0}});
    Level.InitialBoxes.Add({25, {0, 1, 0}});
    Level.InitialBoxes.Add({26, {2, 0, 0}});
    const auto Start = FPuzzleRuntimeState::FromDefinition(Level);
    const FPuzzleAction Lift{ERuneAction::LevitateSelf};
    const auto First = FPuzzleSolver::Evaluate(Level, Start, Lift);
    const auto Second = FPuzzleSolver::Evaluate(Level, First.After, Lift);
    if (!TestTrue(TEXT("Two free casts raise the player by two cells"), First.IsValid() && Second.IsValid() &&
        Second.After.PlayerPosition == FIntVector(0, 0, 2) && Second.After.SP == Start.SP && Second.After.Turn == 2)) return false;
    TestEqual(TEXT("Recasting refreshes four subsequent turns"), Second.After.PlayerWindTurns, 4);
    TestFalse(TEXT("The solver blocks an unsupported horizontal step while airborne"), FPuzzleSolver::Evaluate(Level, Second.After, {ERuneAction::East}).IsValid());
    const auto Dispel = FPuzzleSolver::Evaluate(Level, Second.After, {ERuneAction::ClearPlayerWind});
    TestTrue(TEXT("Dispel lands without spending SP or a turn"), Dispel.IsValid() && Dispel.After.PlayerPosition == Start.PlayerPosition &&
        Dispel.After.PlayerWindTurns == 0 && Dispel.After.SP == Start.SP && Dispel.After.Turn == 2);

    auto InFlight = Second.After;
    for (int32 Id = 23; Id <= 26; ++Id)
    {
        const auto Cast = FPuzzleSolver::Evaluate(Level, InFlight, {ERuneAction::WaterAnchor, Id});
        if (!TestTrue(TEXT("The player can still cast while airborne"), Cast.IsValid())) return false;
        InFlight = Cast.After;
        TestEqual(TEXT("Each other committed action spends one flight turn"), InFlight.PlayerWindTurns, 26 - Id);
        TestEqual(TEXT("The fourth action lands the player"), InFlight.PlayerPosition.Z, Id == 26 ? 0 : 2);
    }
    Level.StaticCells[Level.GridToIndex({0, 0, 1})].bHasFloor = true;
    TestFalse(TEXT("The player cannot levitate through an overhead floor"), FPuzzleSolver::Evaluate(Level, Start, Lift).IsValid());
    TestFalse(TEXT("The player cannot levitate above the top layer"),
        FPuzzleSolver::Evaluate(Level, FPuzzleSolver::Evaluate(Level, Second.After, Lift).After, Lift).IsValid());
    Level.StaticCells[Level.GridToIndex({0, 0, 1})].bHasFloor = false;
    Level.InitialSP = 0;
    TestTrue(TEXT("Self lift is available at zero SP"), FPuzzleSolver::Evaluate(Level, FPuzzleRuntimeState::FromDefinition(Level), Lift).IsValid());
    Level.AvailableSkills = {ERuneSkill::Pulse};
    TestFalse(TEXT("Self lift requires the Wind skill"), FPuzzleSolver::Evaluate(Level, Start, Lift).IsValid());
    TestEqual(TEXT("A same-level box is pushed horizontally even when aiming down at it"),
        FPuzzleSolver::PushFromLook({0, 0, 0}, {1, 0, 0}, {.6, 0, -.8}), FIntVector(1, 0, 0));
    TestEqual(TEXT("Aiming from directly below uses the vertical grid axis"),
        FPuzzleSolver::PushFromLook({0, 0, 0}, {0, 0, 1}, {.25, 0, .9}), FIntVector(0, 0, 1));
    TestFalse(TEXT("The solver rejects a horizontal pull toward the player"),
        FPuzzleSolver::Evaluate(Level, Start, {ERuneAction::Pulse, 23, {-1, 0, 0}}).IsValid());

    Level = TestLevel();
    Level.PlayerStart = {0, 1, 0};
    Level.StaticCells[Level.GridToIndex({0, 2, 0})].bBlocked = true;
    Level.InitialBoxes[0].GridPosition = {1, 1, 1};
    Level.InitialBoxes[0].bPhaseLocked = true;
    const auto Approach = FPuzzleRuntimeState::FromDefinition(Level);
    const auto GroundStep = FPuzzleSolver::Evaluate(Level, Approach, {ERuneAction::North});
    TestTrue(TEXT("A raised platform cannot be reached directly from the ground"),
        !GroundStep.IsValid() && GroundStep.After.PlayerPosition == Approach.PlayerPosition && GroundStep.After.SP == Approach.SP);
    const auto Raised = FPuzzleSolver::Evaluate(Level, Approach, Lift);
    if (!TestTrue(TEXT("Wind raises the player in place"), Raised.IsValid() && Raised.After.PlayerPosition == FIntVector(0, 1, 1))) return false;
    TestFalse(TEXT("Wind cannot move the player into a box"), FPuzzleSolver::Evaluate(Level, Raised.After, {ERuneAction::East}).IsValid());
    TestFalse(TEXT("Wind cannot move the player into an unsupported cell"), FPuzzleSolver::Evaluate(Level, Raised.After, {ERuneAction::South}).IsValid());
    const auto Push = FPuzzleSolver::Evaluate(Level, Raised.After, {ERuneAction::Pulse, 23, {1, 0, 0}});
    TestTrue(TEXT("The raised player can push a box forward without moving"),
        Push.IsValid() && Push.After.PlayerPosition == Raised.After.PlayerPosition && Push.After.FindBox(23)->GridPosition == FIntVector(2, 1, 1));
    const auto Cross = FPuzzleSolver::Evaluate(Level, Raised.After, {ERuneAction::North});
    if (!TestTrue(TEXT("A supported landing costs one SP and clears any remaining wind"),
        Cross.IsValid() && Cross.After.PlayerPosition == FIntVector(0, 2, 1) && Cross.After.SP == Approach.SP - 1 &&
        Cross.After.Turn == 2 && Cross.After.PlayerWindTurns == 0)) return false;
    TestFalse(TEXT("There is no wind to dispel after landing"), FPuzzleSolver::Evaluate(Level, Cross.After, {ERuneAction::ClearPlayerWind}).IsValid());
    const auto StepDown = FPuzzleSolver::Evaluate(Level, Cross.After, {ERuneAction::South});
    TestTrue(TEXT("Grounded descent from the platform remains available"), StepDown.IsValid() && StepDown.After.PlayerPosition == Approach.PlayerPosition);
    return true;
}

#endif
