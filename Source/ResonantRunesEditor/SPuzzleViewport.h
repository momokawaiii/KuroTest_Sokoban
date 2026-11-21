#pragma once

#include "SEditorViewport.h"

class FPuzzleAuthoring;

class SPuzzleViewport : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(SPuzzleViewport) {}
        SLATE_ARGUMENT(TSharedPtr<FPuzzleAuthoring>, Model)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    void Refresh(bool bFrame);
    void SetTopView(bool bTopView);
    void OnFocusViewportToSelection() override;

private:
    TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
    TSharedPtr<FPuzzleAuthoring> Model;
};
