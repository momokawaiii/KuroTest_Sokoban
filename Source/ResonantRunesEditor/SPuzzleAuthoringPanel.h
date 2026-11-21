#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "PuzzleAuthoring.h"

class SVerticalBox;
class SInlineEditableTextBlock;
class SPuzzleViewport;

class SPuzzleAuthoringPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SPuzzleAuthoringPanel) {}
        SLATE_ARGUMENT(TSharedPtr<FPuzzleAuthoring>, Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;

private:
    struct FLevelListItem
    {
        TWeakObjectPtr<UPuzzleLevelAsset> Asset;
        TWeakPtr<SInlineEditableTextBlock> InlineName;
    };
    using FLevelRow = TSharedPtr<FLevelListItem>;
    void Refresh();
    void RefreshInspector();
    void ApplyGridSize();
    void OpenNewDialog();
    void OpenRenameAssetDialog();
    TSharedRef<SWidget> MakeLevelMenu();
    TSharedRef<ITableRow> GenerateRow(FLevelRow Item, const TSharedRef<STableViewBase>& Owner);
    void SelectRow(FLevelRow Item, ESelectInfo::Type How);
    void BeginRename(FLevelRow Item);
    TSharedPtr<FPuzzleAuthoring> Model;
    TArray<FLevelRow> Rows;
    TSharedPtr<SListView<FLevelRow>> List;
    TSharedPtr<SVerticalBox> Details;
    TSharedPtr<SPuzzleViewport> Viewport;
    TWeakObjectPtr<UPuzzleLevelAsset> FramedAsset;
    FIntVector FramedDimensions = FIntVector::ZeroValue;
    FIntVector GridSizeDraft = FIntVector::ZeroValue;
    FPuzzleElement InspectedElement;
    bool bRefreshing = false;
    bool bInspectLevel = true;
    bool bTopView = false;
};
