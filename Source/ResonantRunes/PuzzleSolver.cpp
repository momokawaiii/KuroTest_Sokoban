#include "PuzzleSolver.h"
#include "Algo/AnyOf.h"

namespace
{
    const FIntVector Up(0, 0, 1);
    constexpr ERuneAction Steps[] = {ERuneAction::North, ERuneAction::East, ERuneAction::South, ERuneAction::West};

    bool MeetsState(const FRuneBoxState& Box, ERuneGoalState Requirement)
    {
        return Requirement == ERuneGoalState::Any || (Requirement == ERuneGoalState::Hiss) == Box.bHiss;
    }

    bool MatchesBoxId(int32 RequiredId, int32 ActualId)
    {
        return RequiredId == FRuneGoalData::AnyBoxId || RequiredId == ActualId;
    }

    bool MeetsGoalSlot(const FRuneBoxState& Box, int32 Id, ERuneGoalState Requirement, bool bWaterAnchor)
    {
        return MatchesBoxId(Id, Box.BoxId) && MeetsState(Box, Requirement) && (!bWaterAnchor || Box.bPhaseLocked);
    }

    ERuneGoalState PrimaryRequirement(const FRuneGoalData& Goal)
    {
        return Goal.bHissPair ? ERuneGoalState::Normal : Goal.RequiredState;
    }
}

int32 RuneSkills::Slot(ERuneAction Action)
{
    if (Action == ERuneAction::LevitateSelf) Action = ERuneAction::Levitate;
    for (int32 Index = 0; Index < Count; ++Index)
        if (Ordered[Index].Action == Action) return Index + 1;
    return 0;
}

bool RuneSkills::IsUnlocked(const FPuzzleLevelDefinition& Level, ERuneAction Action)
{
    const int32 Number = Slot(Action);
    return Number == 0 || Level.AvailableSkills.Contains(Ordered[Number - 1].Skill);
}

FString RuneSkills::LockedMessage(ERuneAction Action)
{
    const int32 Number = Slot(Action);
    return Number > 0 ? FString::Printf(TEXT("本关未开放  %d · %s"), Number, Ordered[Number - 1].Name) : FString();
}

FPuzzleRuntimeState FPuzzleRuntimeState::FromDefinition(const FPuzzleLevelDefinition& Level)
{
    FPuzzleRuntimeState State;
    State.PlayerPosition = Level.PlayerStart;
    State.Facing = Level.GetPlayerStartDirection();
    State.SP = Level.InitialSP;
    State.Boxes = Level.InitialBoxes;
    State.RebuildOccupancy();
    return State;
}

const FRuneBoxState* FPuzzleRuntimeState::FindBox(int32 BoxId) const
{
    return Boxes.FindByPredicate([BoxId](const FRuneBoxState& Box) { return Box.BoxId == BoxId; });
}

TArrayView<const int32> FPuzzleRuntimeState::GetBoxesAt(FIntVector Position) const
{
    if (const auto* Found = Occupancy.Find(Position)) return *Found;
    return {};
}

void FPuzzleRuntimeState::RebuildOccupancy()
{
    Occupancy.Reset();
    for (const auto& Box : Boxes) Occupancy.FindOrAdd(Box.GridPosition).Add(Box.BoxId);
}

void FPuzzleRuntimeState::MoveBox(FRuneBoxState& Box, FIntVector Position)
{
    if (Position == Box.GridPosition) return;
    if (auto* Ids = Occupancy.Find(Box.GridPosition))
    {
        Ids->RemoveSingle(Box.BoxId);
        if (Ids->IsEmpty()) Occupancy.Remove(Box.GridPosition);
    }
    Box.GridPosition = Position;
    Occupancy.FindOrAdd(Position).Add(Box.BoxId);
}

int32 FPuzzleSolver::Cost(ERuneAction Action)
{
    switch (Action)
    {
    case ERuneAction::LevitateSelf:
    case ERuneAction::ClearPlayerWind: return 0;
    case ERuneAction::Levitate: return 3;
    case ERuneAction::Pulse:
    case ERuneAction::WaterAnchor:
    case ERuneAction::Hiss: return 2;
    default: return 1;
    }
}

FIntPoint FPuzzleSolver::Direction(ERuneAction Action)
{
    switch (Action)
    {
    case ERuneAction::North: return {0, 1};
    case ERuneAction::East: return {1, 0};
    case ERuneAction::South: return {0, -1};
    case ERuneAction::West: return {-1, 0};
    default: return FIntPoint::ZeroValue;
    }
}

FIntVector FPuzzleSolver::PushFromLook(FIntVector PlayerPosition, FIntVector BoxPosition, FVector GridLook)
{
    const FIntVector Delta = BoxPosition - PlayerPosition;
    FIntVector Push = FIntVector::ZeroValue;
    double Best = 0.;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (Delta[Axis] == 0) continue;
        const int32 Sign = FMath::Sign(Delta[Axis]);
        const double Alignment = GridLook[Axis] * Sign;
        if (Alignment <= Best) continue;
        Best = Alignment;
        Push = FIntVector::ZeroValue;
        Push[Axis] = Sign;
    }
    return Push;
}

bool FPuzzleSolver::IsCellBlocked(const FPuzzleLevelDefinition& Level, FIntVector Position)
{
    const auto* Cell = Level.CellAt(Position);
    return !Cell || Cell->bBlocked;
}

bool FPuzzleSolver::HasStaticSupport(const FPuzzleLevelDefinition& Level, FIntVector Position)
{
    const auto* Cell = Level.CellAt(Position);
    const auto* Below = Level.CellAt(Position - Up);
    return Cell && (Cell->bHasFloor || (Below && Below->bBlocked));
}

bool FPuzzleSolver::CanOverlap(const FRuneBoxState& A, const FRuneBoxState& B)
{
    return A.BoxId != B.BoxId && A.bHiss != B.bHiss;
}

bool FPuzzleSolver::CanEnterCell(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State,
    const FRuneBoxState& Box, FIntVector Target)
{
    const auto* Cell = Level.CellAt(Target);
    if (!Cell || State.PlayerPosition == Target) return false;
    if (Cell->bBlocked && !Box.bHiss && !State.HissObstacles.Contains(Target)) return false;
    int32 OtherCount = 0;
    for (int32 Id : State.GetBoxesAt(Target))
    {
        if (Id == Box.BoxId) continue;
        const auto* Other = State.FindBox(Id);
        if (!Other || ++OtherCount > 1 || !CanOverlap(Box, *Other)) return false;
    }
    return true;
}

bool FPuzzleSolver::HasSupport(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State, const FRuneBoxState& Box)
{
    if (HasStaticSupport(Level, Box.GridPosition)) return true;
    int32 BelowCount = 0;
    for (int32 Id : State.GetBoxesAt(Box.GridPosition - Up))
    {
        const auto* Below = State.FindBox(Id);
        if (Below && (++BelowCount > 1 || !CanOverlap(Box, *Below))) return true;
    }
    return false;
}

FString FPuzzleSolver::SettleGravity(const FPuzzleLevelDefinition& Level, FPuzzleRuntimeState& State)
{
    // Low boxes finish first, so no higher box rests on a box that is about to fall.
    TArray<int32> Order;
    for (int32 I = 0; I < State.Boxes.Num(); ++I) Order.Add(I);
    Order.Sort([&State](int32 A, int32 B)
    {
        const auto& First = State.Boxes[A];
        const auto& Second = State.Boxes[B];
        return First.GridPosition.Z != Second.GridPosition.Z ? First.GridPosition.Z < Second.GridPosition.Z : First.BoxId < Second.BoxId;
    });

    for (int32 Index : Order)
    {
        auto& Box = State.Boxes[Index];
        if (Box.bPhaseLocked || Box.LevitationTurns > 0) continue;
        while (!HasSupport(Level, State, Box))
        {
            const FIntVector Below = Box.GridPosition - Up;
            if (!Level.IsValidGridCoordinate(Below)) return FString::Printf(TEXT("箱 #%d 会跌出棋盘"), Box.BoxId);
            if (Below == State.PlayerPosition) return TEXT("落点有角色，请先让开");
            if (!CanEnterCell(Level, State, Box, Below)) return TEXT("箱子的下落路径被阻挡");
            State.MoveBox(Box, Below);
        }
    }
    if (State.PlayerWindTurns == 0)
    {
        while (!HasStaticSupport(Level, State.PlayerPosition) && State.GetBoxesAt(State.PlayerPosition - Up).IsEmpty())
        {
            const FIntVector Below = State.PlayerPosition - Up;
            if (IsCellBlocked(Level, Below)) return TEXT("玩家下方没有落脚点");
            State.PlayerPosition = Below;
        }
    }
    return {};
}

TArray<FString> FPuzzleSolver::Validate(const FPuzzleLevelDefinition& Level)
{
    TArray<FString> Errors;
    if (!Level.LevelId.IsValid()) Errors.Add(TEXT("关卡缺少稳定 ID"));
    if (Level.DisplayName.IsEmptyOrWhitespace()) Errors.Add(TEXT("请设置关卡名称"));
    if (!Level.HasValidGrid())
    {
        Errors.Add(TEXT("网格尺寸、坐标系或静态格数量无效"));
        return Errors;
    }
    if (Level.InitialSP < 0) Errors.Add(TEXT("初始 SP 不能为负数"));
    TSet<ERuneSkill> Skills;
    for (const ERuneSkill Skill : Level.AvailableSkills)
    {
        if (!Algo::AnyOf(RuneSkills::Ordered, [Skill](const auto& Entry) { return Entry.Skill == Skill; }) || Skills.Contains(Skill))
            Errors.Add(TEXT("可用技能包含无效值或重复项"));
        Skills.Add(Skill);
    }
    const auto State = FPuzzleRuntimeState::FromDefinition(Level);
    if (IsCellBlocked(Level, Level.PlayerStart) || !HasStaticSupport(Level, Level.PlayerStart) ||
        !State.GetBoxesAt(Level.PlayerStart).IsEmpty())
    {
        Errors.Add(TEXT("出生点必须在有支撑且无箱子、无障碍的格子"));
    }

    TSet<int32> BoxIds;
    for (const auto& Box : Level.InitialBoxes)
    {
        if (Box.BoxId <= 0 || BoxIds.Contains(Box.BoxId)) Errors.Add(FString::Printf(TEXT("箱 ID #%d 无效或重复"), Box.BoxId));
        BoxIds.Add(Box.BoxId);
        if (Box.LevitationTurns < 0 || Box.LevitationTurns > LevitationDuration)
            Errors.Add(FString::Printf(TEXT("箱 #%d 的风托回合数超出范围"), Box.BoxId));
        if (!CanEnterCell(Level, State, Box, Box.GridPosition))
            Errors.Add(FString::Printf(TEXT("箱 #%d 越界、遇到障碍或占用冲突"), Box.BoxId));
    }

    TSet<int32> GoalIds;
    TMap<int32, FIntVector> RequiredPositions;
    TMap<int32, ERuneGoalState> RequiredStates;
    TMap<FIntVector, TArray<int32>> RequiredAtCell;
    const bool bCanChangePhase = RuneSkills::IsUnlocked(Level, ERuneAction::Hiss);
    const bool bCanAnchor = RuneSkills::IsUnlocked(Level, ERuneAction::WaterAnchor);
    const auto IsCandidate = [&](const FRuneBoxState& Box, int32 Id, ERuneGoalState Requirement, bool bNeedsAnchor)
    {
        return MatchesBoxId(Id, Box.BoxId) &&
            (MeetsState(Box, Requirement) || (Requirement == ERuneGoalState::Hiss && bCanChangePhase)) &&
            (!bNeedsAnchor || Box.bPhaseLocked || bCanAnchor);
    };
    const auto RequireBox = [&](const FRuneGoalData& Goal, int32 BoxId, ERuneGoalState Requirement)
    {
        if (BoxId == FRuneGoalData::AnyBoxId) return;
        if (!BoxIds.Contains(BoxId)) Errors.Add(FString::Printf(TEXT("阵眼 #%d 引用的箱 #%d 不存在"), Goal.GoalId, BoxId));
        if (const auto* Previous = RequiredPositions.Find(BoxId); Previous && *Previous != Goal.GridPosition)
            Errors.Add(FString::Printf(TEXT("箱 #%d 被要求同时位于不同阵眼"), BoxId));
        RequiredPositions.Add(BoxId, Goal.GridPosition);
        RequiredAtCell.FindOrAdd(Goal.GridPosition).AddUnique(BoxId);
        if (Requirement != ERuneGoalState::Any)
        {
            if (const auto* Previous = RequiredStates.Find(BoxId); Previous && *Previous != Requirement)
                Errors.Add(FString::Printf(TEXT("箱 #%d 的阵眼状态要求冲突"), BoxId));
            if (const auto* Box = State.FindBox(BoxId))
            {
                if (Box->bHiss && Requirement == ERuneGoalState::Normal)
                    Errors.Add(FString::Printf(TEXT("箱 #%d 初始已是 Hiss，无法满足普通态阵眼"), BoxId));
                if (!bCanChangePhase && !Box->bHiss && Requirement == ERuneGoalState::Hiss)
                    Errors.Add(FString::Printf(TEXT("本关未开放镜花水月，箱 #%d 无法转为 Hiss"), BoxId));
            }
            RequiredStates.Add(BoxId, Requirement);
        }
    };
    if (Level.Goals.IsEmpty()) Errors.Add(TEXT("至少需要一个阵眼"));
    for (const auto& Goal : Level.Goals)
    {
        if (Goal.GoalId <= 0 || GoalIds.Contains(Goal.GoalId)) Errors.Add(FString::Printf(TEXT("阵眼 ID #%d 无效或重复"), Goal.GoalId));
        GoalIds.Add(Goal.GoalId);
        if (IsCellBlocked(Level, Goal.GridPosition)) Errors.Add(FString::Printf(TEXT("阵眼 #%d 越界或在硬障碍内"), Goal.GoalId));
        const auto Requirement = PrimaryRequirement(Goal);
        RequireBox(Goal, Goal.BoxId, Requirement);
        if (const auto* Box = State.FindBox(Goal.BoxId); Goal.bRequireWaterAnchor && Box && !Box->bPhaseLocked && !bCanAnchor)
            Errors.Add(FString::Printf(TEXT("阵眼 #%d 要求水锚，请开放沦灭潭渊"), Goal.GoalId));
        if (Goal.bHissPair)
        {
            if (Goal.BoxId == Goal.HissBoxId && Goal.BoxId != FRuneGoalData::AnyBoxId)
                Errors.Add(FString::Printf(TEXT("阵眼 #%d 必须指定两个不同的箱 ID"), Goal.GoalId));
            RequireBox(Goal, Goal.HissBoxId, ERuneGoalState::Hiss);
        }
        if (Goal.BoxId == FRuneGoalData::AnyBoxId || (Goal.bHissPair && Goal.HissBoxId == FRuneGoalData::AnyBoxId))
        {
            const auto CanFillPrimary = [&](const FRuneBoxState& Box)
            { return IsCandidate(Box, Goal.BoxId, Requirement, Goal.bRequireWaterAnchor); };
            const auto* Primary = State.Boxes.FindByPredicate(CanFillPrimary);
            if (!Goal.bHissPair)
            {
                if (!Primary) Errors.Add(FString::Printf(TEXT("阵眼 #%d 没有符合状态和水锚条件的箱子"), Goal.GoalId));
                continue;
            }
            const auto CanFillHiss = [&](const FRuneBoxState& Box)
            { return IsCandidate(Box, Goal.HissBoxId, ERuneGoalState::Hiss, false); };
            const auto* Hiss = State.Boxes.FindByPredicate(CanFillHiss);
            if (!Primary || !Hiss || (Primary == Hiss && !Algo::AnyOf(State.Boxes, [&](const FRuneBoxState& Box)
                { return &Box != Primary && (CanFillPrimary(Box) || CanFillHiss(Box)); })))
                Errors.Add(FString::Printf(TEXT("阵眼 #%d 需要两个不同的箱子分别满足普通态和 Hiss 态条件"), Goal.GoalId));
        }
    }
    for (const auto& Pair : RequiredAtCell)
    {
        if (Pair.Value.Num() > 2) Errors.Add(FString::Printf(TEXT("%s 的阵眼需要超过两个箱"), *Pair.Key.ToString()));
        if (Pair.Value.Num() != 2) continue;
        int32 Normal = 0, Hiss = 0;
        for (int32 Id : Pair.Value)
        {
            const auto* Box = State.FindBox(Id);
            const ERuneGoalState Requirement = RequiredStates.FindRef(Id);
            Normal += Requirement == ERuneGoalState::Normal;
            Hiss += Requirement == ERuneGoalState::Hiss || (Box && Box->bHiss);
        }
        if (Normal > 1 || Hiss > 1) Errors.Add(FString::Printf(TEXT("%s 的阵眼无法形成普通态与 Hiss 叠合"), *Pair.Key.ToString()));
    }

    if (Errors.IsEmpty())
    {
        auto Settled = State;
        FString GravityError = SettleGravity(Level, Settled);
        if (!GravityError.IsEmpty()) Errors.Add(MoveTemp(GravityError));
    }
    return Errors;
}

FPuzzleActionResult FPuzzleSolver::Evaluate(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& Current, FPuzzleAction Request)
{
    const ERuneAction Action = Request.Action;
    const FRuneBoxState* CurrentBox = Current.FindBox(Request.BoxId);
    const FIntVector OriginalBox = CurrentBox ? CurrentBox->GridPosition : FPuzzleLevelDefinition::InvalidPosition;
    const auto Reject = [&](FString Message) { return FPuzzleActionResult{Current, OriginalBox, MoveTemp(Message)}; };
    if (!Level.HasValidGrid()) return Reject(TEXT("棋盘数据无效"));
    if (IsRestored(Level, Current)) return Reject(TEXT("阵列已复原"));
    if (!RuneSkills::IsUnlocked(Level, Action)) return Reject(RuneSkills::LockedMessage(Action));
    if (Current.SP < Cost(Action)) return Reject(TEXT("SP 不足"));

    FPuzzleActionResult Result{Current, OriginalBox, {}};
    auto& Next = Result.After;
    if (Action == ERuneAction::ClearPlayerWind)
    {
        if (Current.PlayerWindTurns == 0) return Reject(TEXT("脚下没有风场"));
        Next.PlayerWindTurns = 0;
        if (FString Error = SettleGravity(Level, Next); !Error.IsEmpty()) return Reject(MoveTemp(Error));
        return Result;
    }

    const FIntPoint Step = Direction(Action);
    if (Step != FIntPoint::ZeroValue)
    {
        FIntVector Target = Current.PlayerPosition + FIntVector(Step.X, Step.Y, 0);
        if (!Level.IsValidGridCoordinate(Target)) return Reject(TEXT("已到棋盘边缘"));
        if (Current.PlayerWindTurns == 0 && !IsCellBlocked(Level, Target) && !HasStaticSupport(Level, Target))
        {
            Target -= Up;
        }
        if (IsCellBlocked(Level, Target) || !HasStaticSupport(Level, Target) || !Next.GetBoxesAt(Target).IsEmpty())
            return Reject(TEXT("前方格子被占用或无支撑"));
        Next.Facing = Step;
        Next.PlayerPosition = Target;
        Next.PlayerWindTurns = 0;
    }
    else if (Action == ERuneAction::LevitateSelf)
    {
        const FIntVector Target = Current.PlayerPosition + Up;
        const auto* Cell = Level.CellAt(Target);
        if (!Cell) return Reject(TEXT("已到最高一层"));
        if (Cell->bBlocked || Cell->bHasFloor || !Next.GetBoxesAt(Target).IsEmpty())
            return Reject(TEXT("上方有箱子、地板或障碍物"));
        Next.PlayerPosition = Target;
        Next.PlayerWindTurns = PlayerWindDuration;
    }
    else if (Action == ERuneAction::Hiss && Request.ObstacleCell != FPuzzleLevelDefinition::InvalidPosition)
    {
        const auto* Cell = Level.CellAt(Request.ObstacleCell);
        if (Request.BoxId != INDEX_NONE || !Cell || !Cell->bBlocked) return Reject(TEXT("请选择一个障碍物"));
        if (Next.HissObstacles.Contains(Request.ObstacleCell)) return Reject(TEXT("障碍物已处于 Hiss"));
        Next.HissObstacles.Add(Request.ObstacleCell);
        Result.BeforeGravity = Request.ObstacleCell;
    }
    else
    {
        if (!CurrentBox) return Reject(Action == ERuneAction::Hiss ? TEXT("请选择符文箱或障碍物") : TEXT("请选择符文箱"));
        auto& Box = Next.Boxes[CurrentBox - Current.Boxes.GetData()];
        const FIntVector Delta = OriginalBox - Current.PlayerPosition;
        const int32 Distance = FMath::Abs(Delta.X) + FMath::Abs(Delta.Y);
        FIntVector Target = OriginalBox;
        switch (Action)
        {
        case ERuneAction::WaterAnchor:
            if (Box.bPhaseLocked) return Reject(TEXT("符文箱已锚定"));
            Box.bPhaseLocked = true;
            break;
        case ERuneAction::Levitate:
            if (Box.LevitationTurns > 0) return Reject(TEXT("风托仍在生效"));
            if (FMath::Max(FMath::Abs(Delta.X), FMath::Abs(Delta.Y)) > 1) return Reject(TEXT("请靠近符文箱（相邻或斜邻）"));
            Target += Up;
            Box.LevitationTurns = LevitationDuration;
            break;
        case ERuneAction::Pulse:
        {
            FIntVector Push = Request.Direction;
            if (Push == FIntVector::ZeroValue) Push = {Current.Facing.X, Current.Facing.Y, 0};
            if (FMath::Abs(int64(Push.X)) + FMath::Abs(int64(Push.Y)) + FMath::Abs(int64(Push.Z)) != 1)
                return Reject(TEXT("电推必须沿轴线移动一格"));
            if (Push.Z && !Box.bPhaseLocked && Box.LevitationTurns <= 0)
                return Reject(TEXT("竖推需要风托或水锚"));
            if (Distance > 2 || FMath::Abs(Delta.Z) > 2) return Reject(TEXT("符文箱超出电推范围"));
            if (Push.X * Delta.X + Push.Y * Delta.Y + Push.Z * Delta.Z <= 0)
                return Reject(TEXT("请站到施力侧，沿视线朝箱子推"));
            Target += Push;
            break;
        }
        case ERuneAction::Hiss:
            if (Box.bHiss) return Reject(TEXT("符文箱已处于 Hiss"));
            Box.bHiss = true;
            break;
        default: return Reject(TEXT("未知的操作"));
        }

        if (!Level.IsValidGridCoordinate(Target)) return Reject(TEXT("目标格超出棋盘"));
        if (!CanEnterCell(Level, Next, Box, Target)) return Reject(TEXT("目标格被占用或无法叠合"));
        Next.MoveBox(Box, Target);
        Result.BeforeGravity = Target;
    }

    Next.SP -= Cost(Action);
    ++Next.Turn;
    if (Action != ERuneAction::LevitateSelf && Next.PlayerWindTurns > 0) --Next.PlayerWindTurns;
    const int32 LiftedBoxId = Action == ERuneAction::Levitate ? Request.BoxId : INDEX_NONE;
    const auto HasLastWindAtGoal = [&](const FRuneBoxState& Box)
    {
        return Box.LevitationTurns == 1 && !Box.bPhaseLocked && !HasStaticSupport(Level, Box.GridPosition) &&
            Level.Goals.ContainsByPredicate([&](const FRuneGoalData& Goal)
            {
                return Goal.GridPosition == Box.GridPosition &&
                    (MeetsGoalSlot(Box, Goal.BoxId, PrimaryRequirement(Goal), Goal.bRequireWaterAnchor) ||
                        (Goal.bHissPair && MeetsGoalSlot(Box, Goal.HissBoxId, ERuneGoalState::Hiss, false)));
            });
    };
    const auto ResolveGravity = [&](FPuzzleRuntimeState& After, bool bFinishOnLastWind)
    {
        for (auto& Box : After.Boxes)
            if (Box.LevitationTurns > 0 && Box.BoxId != LiftedBoxId && !(bFinishOnLastWind && HasLastWindAtGoal(Box)))
                --Box.LevitationTurns;
        return SettleGravity(Level, After);
    };
    if (Next.Boxes.ContainsByPredicate(HasLastWindAtGoal))
    {
        // Match the final arrival while its wind still supports it. All other boxes settle normally.
        auto Arrival = Next;
        if (ResolveGravity(Arrival, true).IsEmpty() && IsRestored(Level, Arrival))
        {
            Next = MoveTemp(Arrival);
            return Result;
        }
    }
    if (FString Error = ResolveGravity(Next, false); !Error.IsEmpty()) return Reject(MoveTemp(Error));
    return Result;
}

bool FPuzzleSolver::IsRestored(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State)
{
    const auto HasMatchingBox = [&](FIntVector Position, int32 Id, ERuneGoalState Requirement, bool bRequireWaterAnchor)
    {
        return Algo::AnyOf(State.Boxes, [&](const FRuneBoxState& Box)
        {
            return Box.GridPosition == Position && MeetsGoalSlot(Box, Id, Requirement, bRequireWaterAnchor);
        });
    };
    for (const auto& Goal : Level.Goals)
    {
        if (!HasMatchingBox(Goal.GridPosition, Goal.BoxId, PrimaryRequirement(Goal),
            Goal.bRequireWaterAnchor)) return false;
        if (Goal.bHissPair && !HasMatchingBox(Goal.GridPosition, Goal.HissBoxId, ERuneGoalState::Hiss, false)) return false;
    }
    return !Level.Goals.IsEmpty();
}

bool FPuzzleSolver::CanContinue(const FPuzzleLevelDefinition& Level, const FPuzzleRuntimeState& State)
{
    if (IsRestored(Level, State)) return false;
    if (Evaluate(Level, State, {ERuneAction::LevitateSelf}).IsValid() ||
        Evaluate(Level, State, {ERuneAction::ClearPlayerWind}).IsValid()) return true;
    for (ERuneAction Step : Steps)
        if (Evaluate(Level, State, {Step}).IsValid()) return true;
    if (State.SP < Cost(ERuneAction::Pulse)) return false;
    for (const auto& Box : State.Boxes)
    {
        for (ERuneAction Action : {ERuneAction::Levitate, ERuneAction::WaterAnchor, ERuneAction::Hiss})
            if (Evaluate(Level, State, {Action, Box.BoxId}).IsValid()) return true;
        for (const FIntVector Push : {FIntVector(0, 1, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0), FIntVector(-1, 0, 0), Up, FIntVector(0, 0, -1)})
            if (Evaluate(Level, State, {ERuneAction::Pulse, Box.BoxId, Push}).IsValid()) return true;
    }
    if (RuneSkills::IsUnlocked(Level, ERuneAction::Hiss))
    {
        for (int32 Index = 0; Index < Level.StaticCells.Num(); ++Index)
        {
            if (!Level.StaticCells[Index].bBlocked) continue;
            const FIntVector Position = Level.IndexToGrid(Index);
            if (!State.HissObstacles.Contains(Position))
                return Evaluate(Level, State, {ERuneAction::Hiss, INDEX_NONE, FIntVector::ZeroValue, Position}).IsValid();
        }
    }
    return false;
}
