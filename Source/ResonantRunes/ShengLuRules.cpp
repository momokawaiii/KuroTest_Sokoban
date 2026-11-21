#include "ShengLuRules.h"

FRuneBoard FShengLuRules::View(const FPuzzleRuntimeState& State)
{
    const auto* Box = State.Boxes.IsEmpty() ? nullptr : &State.Boxes[0];
    return {{State.PlayerPosition.X, State.PlayerPosition.Y}, State.Facing,
        Box ? Box->GridPosition : FPuzzleLevelDefinition::InvalidPosition,
        Box ? Box->LevitationTurns : 0, State.SP, State.Turn, Box && Box->bPhaseLocked};
}

