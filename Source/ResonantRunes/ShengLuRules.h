#pragma once

#include "PuzzleSolver.h"

struct FRuneBoard
{
    FIntPoint Player{0, 0};
    FIntPoint Facing{1, 0};
    FIntVector Box{1, 1, 0};
    int32 Wind = 0;
    int32 SP = 8;
    int32 Turn = 0;
    bool bHeightAnchored = false;

    static inline const FIntVector Goal{2, 1, 1};
    bool IsRestored() const { return Box == Goal; }
};

class FShengLuRules
{
public:
    static FRuneBoard View(const FPuzzleRuntimeState& State);
};

