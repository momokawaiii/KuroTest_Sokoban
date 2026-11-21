#include "SPuzzleAuthoringPanel.h"

#include "ContentBrowserModule.h"
#include "AssetRegistry/AssetData.h"
#include "DragAndDrop/DecoratedDragDropOp.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "PuzzleEditorStyle.h"
#include "SPuzzleViewport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"

namespace
{
    const FPuzzleEditorStyle& Look() { return FPuzzleEditorStyle::Get(); }
    FText Txt(const TCHAR* Text) { return FText::FromString(Text); }

    constexpr const TCHAR* GridDimensionsTip = TEXT("X、Y 是横向格数，Z 是层数。例如 4 × 3 × 1 只有 Z0。各轴可填 1–128，总格数最多 65536。");
    constexpr const TCHAR* GridCoordinatesTip = TEXT("移动选中的元素。X、Y 是棋盘上的两条轴，Z 是层号，底层从 0 开始。");
    constexpr const TCHAR* GridOriginTip = TEXT("网格 (0,0,0) 底面中心的 UE 世界坐标。修改会平移整张棋盘。");
    constexpr const TCHAR* CellSizeTip = TEXT("单格的世界宽度，单位是厘米。600 cm 等于 6 米。调大后格间实际距离也会增大。");
    constexpr const TCHAR* InitialSPTip = TEXT("玩家开局的行动预算。走一格需 1 SP，电推、水锚、Hiss 各需 2 SP，风抬箱需 3 SP。");

    TSharedRef<SWidget> Icon(FName Name, float Size = 16.f, FSlateColor Tint = FSlateColor::UseForeground())
    {
        return SNew(SBox).WidthOverride(Size).HeightOverride(Size)
        [SNew(SImage).Image(FAppStyle::GetBrush(Name)).ColorAndOpacity(Tint)];
    }

    TSharedRef<SWidget> ButtonContent(FText Label, FName IconName)
    {
        auto Line = SNew(SHorizontalBox);
        if (!IconName.IsNone())
            Line->AddSlot().AutoWidth().VAlign(VAlign_Center)[Icon(IconName, 14.f)];
        if (!Label.IsEmpty())
            Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(IconName.IsNone() ? 0.f : 6.f, 0, 0, 0)
            [SNew(STextBlock).Text(Label).Font(Look().Body).ColorAndOpacity(FSlateColor::UseForeground())];
        return Line;
    }

    TSharedRef<SWidget> Button(FText Label, TFunction<void()> Click, FName IconName = NAME_None,
        const TCHAR* Tip = TEXT(""), bool bPrimary = false, TAttribute<bool> Enabled = true)
    {
        return SNew(SButton).ButtonStyle(bPrimary ? &Look().Primary : &Look().Quiet)
        .ContentPadding(FMargin(Label.IsEmpty() ? 7.f : 10.f, 0)).HAlign(HAlign_Center).VAlign(VAlign_Center)
        .ToolTipText(*Tip ? Txt(Tip) : Label).AccessibleText(Label.IsEmpty() ? Txt(Tip) : Label).IsEnabled(Enabled)
        .OnClicked_Lambda([Click = MoveTemp(Click)]
        {
            Click();
            return FReply::Handled();
        })
        [SNew(SBox).MinDesiredHeight(30.f).VAlign(VAlign_Center)[ButtonContent(Label, IconName)]];
    }

    TSharedRef<SWidget> Field(FText Label, TSharedRef<SWidget> Control, const TCHAR* Tip = TEXT(""))
    {
        return SNew(SVerticalBox).ToolTipText(Txt(Tip))
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
            [SNew(STextBlock).Text(Label).Font(Look().Small).ColorAndOpacity(Look().Muted)]
            + SVerticalBox::Slot().AutoHeight()[Control];
    }

    template<typename T>
    TSharedRef<SWidget> Number(TFunction<T()> Read, TFunction<void(T)> Write, const TCHAR* Tip = TEXT(""))
    {
        return SNew(SNumericEntryBox<T>)
            .EditableTextBoxStyle(&Look().Entry).Font(Look().Body)
            .ToolTipTextFormat(*Tip ? TOptional<FTextFormat>(FTextFormat(Txt(Tip))) : TOptional<FTextFormat>{})
            .Value_Lambda([Read = MoveTemp(Read)]() -> TOptional<T> { return Read(); })
            .OnValueCommitted_Lambda([Write = MoveTemp(Write)](T Value, ETextCommit::Type Reason)
            {
                if (Reason != ETextCommit::OnCleared) Write(Value);
            });
    }

    template<typename T>
    TSharedRef<SWidget> XYZ(TFunction<T()> Read, TFunction<void(T)> Write, const TCHAR* Tip = TEXT(""))
    {
        auto Fields = SNew(SUniformGridPanel).SlotPadding(FMargin(3, 0));
        const TCHAR* Labels[] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
        using Component = std::decay_t<decltype(Read()[0])>;
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            Fields->AddSlot(Axis, 0)
            [
                Field(Txt(Labels[Axis]), Number<Component>([Read, Axis] { return Read()[Axis]; }, [Read, Write, Axis](Component Value)
                {
                    T Vector = Read();
                    if (Vector[Axis] == Value) return;
                    Vector[Axis] = Value;
                    Write(Vector);
                }, Tip), Tip)
            ];
        }
        return Fields;
    }

    TSharedRef<SWidget> Check(TFunction<bool()> Read, TFunction<void(bool)> Write, FText Label, const TCHAR* Tip = TEXT(""))
    {
        return SNew(SCheckBox).ToolTipText(Txt(Tip))
            .IsChecked_Lambda([Read = MoveTemp(Read)] { return Read() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([Write = MoveTemp(Write)](ECheckBoxState State) { Write(State == ECheckBoxState::Checked); })
            [SNew(STextBlock).Text(Label).Font(Look().Body).ColorAndOpacity(Look().Text)];
    }

    const TCHAR* SkillTip(ERuneAction Action)
    {
        switch (Action)
        {
        case ERuneAction::Pulse:
            return TEXT("2 SP。沿视线选一个轴，把箱子推一格。可从斜侧瞄准，竖推需要箱子有风托或水锚。");
        case ERuneAction::Levitate:
            return TEXT("抬箱一层需 3 SP，并给箱子两回合风托。水锚箱也可以抬升。向脚下施放可免费抬升角色。");
        case ERuneAction::WaterAnchor:
            return TEXT("2 SP。命中箱子后使它免于重力下落。水锚箱仍可电推或用风抬升。");
        case ERuneAction::Hiss:
            return TEXT("2 SP。使箱子或硬障碍进入 Hiss。普通箱和 Hiss 箱可以同格，箱子与障碍任一为 Hiss 时也可同格。");
        default: return TEXT("");
        }
    }

    TSharedRef<SWidget> Toggle(FText Label, TFunction<bool()> Read, TFunction<void(bool)> Write,
        FName IconName = NAME_None, const TCHAR* Tip = TEXT(""))
    {
        return SNew(SCheckBox).Style(&Look().Toggle).ToolTipText(*Tip ? Txt(Tip) : Label)
            .AccessibleText(Label.IsEmpty() ? Txt(Tip) : Label)
            .IsChecked_Lambda([Read = MoveTemp(Read)] { return Read() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([Write = MoveTemp(Write)](ECheckBoxState State) { Write(State == ECheckBoxState::Checked); })
            [
                SNew(SBox).MinDesiredHeight(30.f).Padding(7, 0).HAlign(HAlign_Center).VAlign(VAlign_Center)
                [ButtonContent(Label, IconName)]
            ];
    }

    FName ToolIcon(EPuzzleTool Tool)
    {
        switch (Tool)
        {
        case EPuzzleTool::Erase: return TEXT("Icons.Delete");
        case EPuzzleTool::Floor: return TEXT("BlueprintEditor.ShowFloor");
        case EPuzzleTool::Wall: return TEXT("ContentBrowser.PrimitiveCube");
        case EPuzzleTool::Box: return TEXT("Icons.box-perspective");
        case EPuzzleTool::Goal: return TEXT("Icons.Star.Outline");
        case EPuzzleTool::Player: return TEXT("Icons.ArrowRight");
        default: return TEXT("Icons.SelectInViewport");
        }
    }

    class FPuzzleLevelDrag : public FDecoratedDragDropOp
    {
    public:
        DRAG_DROP_OPERATOR_TYPE(FPuzzleLevelDrag, FDecoratedDragDropOp)
        TWeakObjectPtr<UPuzzleLevelAsset> Asset;
        TWeakObjectPtr<UPuzzleLevelCatalog> Catalog;

        static TSharedRef<FPuzzleLevelDrag> Begin(UPuzzleLevelAsset* Level, UPuzzleLevelCatalog* InCatalog)
        {
            auto Operation = MakeShared<FPuzzleLevelDrag>();
            Operation->Asset = Level;
            Operation->Catalog = InCatalog;
            Operation->DefaultHoverText = Level->Definition.DisplayName;
            Operation->CurrentHoverText = Operation->DefaultHoverText;
            Operation->Construct();
            return Operation;
        }
    };

    TSharedRef<SWidget> Section(const TCHAR* Name)
    {
        return SNew(STextBlock).Text(Txt(Name)).Font(Look().Section).ColorAndOpacity(Look().Text);
    }
}

void SPuzzleAuthoringPanel::Construct(const FArguments& Args)
{
    Model = Args._Model;
    const auto Editable = TAttribute<bool>::CreateLambda([this] { return !Model->bPlaying && Model->Asset(); });

    auto Tools = SNew(SUniformGridPanel).SlotPadding(FMargin(3.f, 3.f));
    const EPuzzleTool Palette[] = {EPuzzleTool::Select, EPuzzleTool::Erase, EPuzzleTool::Floor,
        EPuzzleTool::Wall, EPuzzleTool::Box, EPuzzleTool::Goal, EPuzzleTool::Player};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Palette); ++Index)
    {
        const EPuzzleTool Tool = Palette[Index];
        Tools->AddSlot(Index % 2, Index / 2)
        [
            Toggle(PuzzleToolName(Tool), [this, Tool] { return Model->Tool == Tool; },
                [this, Tool](bool) { Model->SetTool(Tool); }, ToolIcon(Tool))
        ];
    }

    auto Play = SNew(SButton).ButtonStyle(&Look().Primary).ContentPadding(FMargin(16, 2))
        .IsEnabled_Lambda([this] { return Model->bPlaying || (Model->Asset() && !Model->Crop); })
        .ToolTipText(Txt(TEXT("校验当前编辑内容并从所选关卡开始试玩。通关后按目录顺序接关。")))
        .OnClicked_Lambda([this]
        {
            if (Model->bPlaying) GEditor->RequestEndPlayMap();
            else Model->TestLevel();
            return FReply::Handled();
        })
        [
            SNew(SBox).MinDesiredHeight(32.f).VAlign(VAlign_Center)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 7, 0)
                [
                    SNew(SImage).Image_Lambda([this] { return FAppStyle::GetBrush(Model->bPlaying ? "PlayWorld.StopPlaySession.Small" : "Icons.Play"); })
                    .DesiredSizeOverride(FVector2D(14, 14)).ColorAndOpacity(FSlateColor::UseForeground())
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(STextBlock).Font(Look().Section).ColorAndOpacity(FSlateColor::UseForeground())
                    .Text_Lambda([this] { return Txt(Model->bPlaying ? TEXT("停止试玩") : TEXT("试玩")); })
                ]
            ]
        ];

    auto ViewControls = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 3, 0)
        [
            Toggle(Txt(TEXT("等距")), [this] { return !bTopView; },
                [this](bool) { if (!Model->Crop) { bTopView = false; Viewport->SetTopView(false); } }, TEXT("EditorViewport.Perspective"),
                TEXT("等距取景\n右键平移 · 滚轮缩放 · Alt + 左键旋转"))
        ]
        + SHorizontalBox::Slot().AutoWidth()
        [
            Toggle(Txt(TEXT("顶视")), [this] { return bTopView; },
                [this](bool) { bTopView = true; Viewport->SetTopView(true); }, TEXT("EditorViewport.Top"), TEXT("正交顶视"))
        ];

    auto LayerControls = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
        [
            Toggle(FText::GetEmpty(), [this] { return Model->bShowGrid; },
                [this](bool Value) { Model->bShowGrid = Value; Model->Changed(); }, TEXT("ViewportToolbar.SetShowGrid"), TEXT("显示编辑网格"))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 3, 0)
        [
            Toggle(FText::GetEmpty(), [this] { return Model->bSoloLayer; },
                [this](bool Value) { Model->bSoloLayer = Value; Model->Changed(); }, TEXT("Icons.Visible"), TEXT("仅显示当前 Z 层"))
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5, 0, 5, 0)
        [SNew(SBox).WidthOverride(1.f).HeightOverride(16.f)[SNew(SImage).Image(&Look().Divider)]]
        + SHorizontalBox::Slot().AutoWidth()
        [
            Button(FText::GetEmpty(), [this] { Model->SetLayer(Model->Layer - 1); }, TEXT("Icons.ChevronLeft"), TEXT("上一层（PgDn）"), false,
                TAttribute<bool>::CreateLambda([this] { return Model->Layer > 0; }))
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(3, 0)
        [
            SNew(STextBlock).Text(Txt(TEXT("Z"))).Font(Look().Body).ColorAndOpacity(Look().Muted)
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(36.f)
            [Number<int32>([this] { return Model->Layer; }, [this](int32 Z) { Model->SetLayer(Z); })]
        ]
        + SHorizontalBox::Slot().AutoWidth()
        [
            Button(FText::GetEmpty(), [this] { Model->SetLayer(Model->Layer + 1); }, TEXT("Icons.ChevronRight"), TEXT("下一层（PgUp）"), false,
                TAttribute<bool>::CreateLambda([this] { return Model->Level() && Model->Layer + 1 < Model->Level()->GridDimensions.Z; }))
        ];

    ChildSlot
    [
        SNew(SBorder).BorderImage(&Look().Base).Padding(0).ForegroundColor(Look().Text)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder).BorderImage(&Look().Panel).Padding(16, 10)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                    [
                        SNew(SBox).WidthOverride(36.f).HeightOverride(36.f)
                        [
                            SNew(SBorder).BorderImage(&Look().GlyphTile).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0)
                            [Icon(TEXT("Icons.box-perspective"), 18.f, Look().Accent)]
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                    [
                        SNew(STextBlock).Font(Look().Title).ColorAndOpacity(Look().Text).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                        .Text_Lambda([this] { return Model->Level() ? Model->Level()->DisplayName : Txt(TEXT("符文关卡")); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [Button(Txt(TEXT("保存")), [this] { Model->Save(); }, TEXT("Icons.Save"), TEXT("保存关卡资产和目录（Ctrl+S）"), false, Editable)]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5, 0)
                    [SNew(SBox).WidthOverride(1.f).HeightOverride(20.f)[SNew(SImage).Image(&Look().Divider)]]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [Button(Txt(TEXT("撤销")), [] { GEditor->UndoTransaction(); }, NAME_None, TEXT("撤销（Ctrl+Z）"), false, Editable)]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [Button(Txt(TEXT("重做")), [] { GEditor->RedoTransaction(); }, NAME_None, TEXT("重做（Ctrl+Y）"), false, Editable)]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
                    [Button(Txt(TEXT("校验")), [this] { Model->Validate(); }, TEXT("Icons.Check"),
                        TEXT("检查网格、出生点、箱子占用和阵眼条件。可否在 SP 预算内通关由试玩验证。"), false, Editable)]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)[Play]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SBox).HeightOverride(1.f)[SNew(SImage).Image(&Look().Divider)]]
            + SVerticalBox::Slot().FillHeight(1.f)
            [
                SNew(SSplitter).Style(&Look().Splitter).Orientation(Orient_Horizontal)
                .PhysicalSplitterHandleSize(1.f).HitDetectionSplitterHandleSize(7.f)
                + SSplitter::Slot().Value(.15f).MinSize(180.f)
                [
                    SNew(SBorder).BorderImage(&Look().Panel).Padding(12.f)
                    [
                        SNew(SVerticalBox).IsEnabled_Lambda([this] { return !Model->bPlaying; })
                        + SVerticalBox::Slot().AutoHeight().Padding(3, 0, 0, 8)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[Section(TEXT("关卡"))]
                            + SHorizontalBox::Slot().AutoWidth()
                            [Button(Txt(TEXT("新建")), [this] { OpenNewDialog(); }, TEXT("Icons.Plus"))]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SComboButton).ButtonStyle(&Look().Quiet).HasDownArrow(false).ContentPadding(FMargin(6, 3))
                                .ToolTipText(Txt(TEXT("关卡操作"))).AccessibleText(Txt(TEXT("关卡操作")))
                                .OnGetMenuContent(this, &SPuzzleAuthoringPanel::MakeLevelMenu)
                                .ButtonContent()[Icon(TEXT("Icons.EllipsisVerticalNarrow"), 16.f, Look().Muted)]
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight()
                        [
                            SNew(SBox).HeightOverride_Lambda([this] { return FMath::Clamp(float(Rows.Num()) * 35.f, 42.f, 210.f); })
                            [
                                SNew(SOverlay)
                                + SOverlay::Slot()
                                [
                                    SAssignNew(List, SListView<FLevelRow>).ListViewStyle(&Look().LevelList).ListItemsSource(&Rows)
                                    .SelectionMode(ESelectionMode::Single).OnGenerateRow(this, &SPuzzleAuthoringPanel::GenerateRow)
                                    .OnSelectionChanged(this, &SPuzzleAuthoringPanel::SelectRow)
                                    .OnMouseButtonDoubleClick(this, &SPuzzleAuthoringPanel::BeginRename)
                                    .OnContextMenuOpening_Lambda([this]() -> TSharedPtr<SWidget> { return MakeLevelMenu(); })
                                ]
                                + SOverlay::Slot().Padding(8, 0).VAlign(VAlign_Center)
                                [
                                    SNew(STextBlock).Font(Look().Body).ColorAndOpacity(Look().Muted).Text(Txt(TEXT("暂无关卡")))
                                    .Visibility_Lambda([this] { return Rows.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
                                ]
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 14)
                        [SNew(SBox).HeightOverride(1.f)[SNew(SImage).Image(&Look().Divider)]]
                        + SVerticalBox::Slot().AutoHeight().Padding(3, 0, 0, 7)[Section(TEXT("放置"))]
                        + SVerticalBox::Slot().AutoHeight()
                        [SNew(SBox).IsEnabled(Editable)[Tools]]
                    ]
                ]
                + SSplitter::Slot().Value(.65f).MinSize(245.f)
                [
                    SNew(SOverlay)
                    + SOverlay::Slot()[SAssignNew(Viewport, SPuzzleViewport).Model(Model)]
                    + SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Top).Padding(13.f)
                    [
                        SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8, 5)).Visibility(EVisibility::SelfHitTestInvisible)
                        + SWrapBox::Slot()
                        [SNew(SBorder).BorderImage(&Look().Floating).Padding(3.f)[ViewControls]]
                        + SWrapBox::Slot()
                        [SNew(SBorder).BorderImage(&Look().Floating).Padding(3.f).IsEnabled(Editable)[LayerControls]]
                    ]
                    + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Font(Look().Title).ColorAndOpacity(Look().Muted).Text(Txt(TEXT("新建或选择关卡")))
                        .Visibility_Lambda([this] { return Model->Level() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
                    ]
                ]
                + SSplitter::Slot().Value(.20f).MinSize(236.f)
                [
                    SNew(SBorder).BorderImage(&Look().Panel).Padding(14, 12)
                    [
                        SNew(SVerticalBox).IsEnabled_Lambda([this] { return !Model->bPlaying; })
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 8)[Section(TEXT("属性"))]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                        [
                            SNew(SUniformGridPanel).SlotPadding(FMargin(2.f, 0))
                            + SUniformGridPanel::Slot(0, 0)
                            [
                                Toggle(Txt(TEXT("选中元素")), [this] { return !bInspectLevel; },
                                    [this](bool) { bInspectLevel = false; RefreshInspector(); })
                            ]
                            + SUniformGridPanel::Slot(1, 0)
                            [
                                Toggle(Txt(TEXT("关卡设置")), [this] { return bInspectLevel; },
                                    [this](bool) { bInspectLevel = true; RefreshInspector(); })
                            ]
                        ]
                        + SVerticalBox::Slot().FillHeight(1.f)
                        [
                            SNew(SScrollBox)
                            + SScrollBox::Slot().Padding(0, 0, 5, 0)[SAssignNew(Details, SVerticalBox)]
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SBox).HeightOverride(1.f)[SNew(SImage).Image(&Look().Divider)]]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder).BorderImage(&Look().Panel).Padding(14, 7)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                    [
                        SNew(SBox).MaxDesiredHeight(78.f)
                        [
                            SNew(SScrollBox)
                            + SScrollBox::Slot()
                            [
                                SNew(STextBlock).Font(Look().Body).AutoWrapText(true)
                                .ColorAndOpacity_Lambda([this] { return Model->bPlaying ? Look().Accent : Look().Text; })
                                .Text_Lambda([this]
                                {
                                    if (Model->bPlaying) return Txt(TEXT("PIE 试玩中"));
                                    if (!Model->Status.IsEmpty()) return FText::FromString(Model->Status);
                                    if (!Model->Asset()) return Txt(TEXT("未选择关卡"));
                                    const bool Dirty = Model->Asset()->GetOutermost()->IsDirty() ||
                                        (Model->Catalog() && Model->Catalog()->GetOutermost()->IsDirty());
                                    return Txt(Dirty ? TEXT("有未保存修改") : TEXT("已保存"));
                                })
                            ]
                        ]
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Font(Look().Small).ColorAndOpacity(Look().Muted).Text_Lambda([this]
                        {
                            const auto* Level = Model->Level();
                            return Level ? FText::FromString(FString::Printf(TEXT("%d × %d × %d   ·   箱 %d   ·   阵眼 %d"),
                                Level->GridDimensions.X, Level->GridDimensions.Y, Level->GridDimensions.Z,
                                Level->InitialBoxes.Num(), Level->Goals.Num())) : FText::GetEmpty();
                        })
                    ]
                ]
            ]
        ]
    ];
    Model->OnChanged = FSimpleDelegate::CreateSP(this, &SPuzzleAuthoringPanel::Refresh);
    Refresh();
}

TSharedRef<SWidget> SPuzzleAuthoringPanel::MakeLevelMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const auto CanEdit = FCanExecuteAction::CreateLambda([this] { return !Model->bPlaying && Model->Asset(); });
    const auto Add = [&](const TCHAR* Label, const TCHAR* Tip, const TCHAR* IconName, FExecuteAction Action, FCanExecuteAction Gate)
    {
        Menu.AddMenuEntry(Txt(Label), Txt(Tip), FSlateIcon(FAppStyle::GetAppStyleSetName(), IconName), FUIAction(MoveTemp(Action), MoveTemp(Gate)));
    };
    Add(TEXT("改名"), TEXT("编辑显示名称（F2）"), TEXT("Icons.Edit"), FExecuteAction::CreateLambda([this] { BeginRename(List->GetSelectedItems().IsEmpty() ? nullptr : List->GetSelectedItems()[0]); }), CanEdit);
    Add(TEXT("重命名棋盘资产"), TEXT("修改内容浏览器和磁盘中的棋盘资产名"), TEXT("Icons.Edit"),
        FExecuteAction::CreateLambda([this] { OpenRenameAssetDialog(); }), CanEdit);
    Add(TEXT("复制"), TEXT("在当前关卡后插入副本"), TEXT("Icons.Duplicate"), FExecuteAction::CreateLambda([this] { Model->Duplicate(); }), CanEdit);
    Add(TEXT("定位棋盘资产"), TEXT("在内容浏览器中显示此关卡的数据资产"), TEXT("Icons.Search"),
        FExecuteAction::CreateLambda([this]
        {
            FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(TArray<UObject*>{Model->Asset()});
        }), CanEdit);
    const auto Shift = [this](int32 Delta)
    {
        auto* Catalog = Model->Catalog();
        if (!Catalog) return;
        const int32 Index = Catalog->Levels.IndexOfByKey(Model->Asset());
        const int32 Next = Index + Delta;
        if (Index != INDEX_NONE && Catalog->Levels.IsValidIndex(Next)) Model->Reorder(Model->Asset(), Catalog->Levels[Next], Delta > 0);
    };
    Add(TEXT("上移"), TEXT("向前移动一位"), TEXT("Icons.ArrowUp"), FExecuteAction::CreateLambda([Shift] { Shift(-1); }), CanEdit);
    Add(TEXT("下移"), TEXT("向后移动一位"), TEXT("Icons.ArrowDown"), FExecuteAction::CreateLambda([Shift] { Shift(1); }), CanEdit);
    Add(TEXT("从目录移除"), TEXT("可用 Ctrl+Z 恢复"), TEXT("Icons.Delete"), FExecuteAction::CreateLambda([this] { Model->Remove(); }), CanEdit);
    Menu.AddMenuSeparator();
    Add(TEXT("加入所选资产"), TEXT("将内容浏览器选中的关卡资产加入目录"), TEXT("Icons.Link"),
        FExecuteAction::CreateLambda([this]
        {
            TArray<FAssetData> Selection;
            FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Selection);
            for (const auto& Item : Selection)
            {
                if (auto* Asset = Cast<UPuzzleLevelAsset>(Item.GetAsset()))
                {
                    Model->AddExisting(Asset);
                    return;
                }
            }
            Model->SetStatus(TEXT("请先在内容浏览器选择一个关卡资产"));
        }), FCanExecuteAction::CreateLambda([this] { return !Model->bPlaying; }));
    Add(TEXT("导入升律"), TEXT("按原关卡数据生成可编辑副本"), TEXT("Icons.Import"),
        FExecuteAction::CreateLambda([this] { Model->ImportShengLu(); }), FCanExecuteAction::CreateLambda([this] { return !Model->bPlaying; }));
    return Menu.MakeWidget();
}

TSharedRef<ITableRow> SPuzzleAuthoringPanel::GenerateRow(FLevelRow Item, const TSharedRef<STableViewBase>& Owner)
{
    TSharedPtr<SInlineEditableTextBlock> Name;
    auto RowWidget = SNew(STableRow<FLevelRow>, Owner).Style(&Look().LevelRow).Padding(FMargin(8, 1))
    .OnDragDetected_Lambda([this, Item](const FGeometry&, const FPointerEvent& Event)
    {
        if (Model->bPlaying || !Item->Asset.IsValid() || !Event.IsMouseButtonDown(EKeys::LeftMouseButton)) return FReply::Unhandled();
        return FReply::Handled().BeginDragDrop(FPuzzleLevelDrag::Begin(Item->Asset.Get(), Model->Catalog()));
    })
    .OnCanAcceptDrop_Lambda([this](const FDragDropEvent& Event, EItemDropZone Zone, FLevelRow Target) -> TOptional<EItemDropZone>
    {
        auto Operation = Event.GetOperationAs<FPuzzleLevelDrag>();
        if (Model->bPlaying || !Operation || Operation->Catalog.Get() != Model->Catalog() ||
            !Operation->Asset.IsValid() || Operation->Asset == Target->Asset) return {};
        return Zone == EItemDropZone::BelowItem ? Zone : EItemDropZone::AboveItem;
    })
    .OnAcceptDrop_Lambda([this](const FDragDropEvent& Event, EItemDropZone Zone, FLevelRow Target)
    {
        const auto Operation = Event.GetOperationAs<FPuzzleLevelDrag>();
        if (Operation && Model->Reorder(Operation->Asset.Get(), Target->Asset.Get(), Zone == EItemDropZone::BelowItem)) return FReply::Handled();
        return FReply::Unhandled();
    })
    [
        SNew(SBox).MinDesiredHeight(33.f).VAlign(VAlign_Center)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 5, 0)
            [
                SAssignNew(Name, SInlineEditableTextBlock).Font(Look().Body).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                .ColorAndOpacity_Lambda([this, Item] { return Model->Asset() == Item->Asset.Get() ? Look().Accent : Look().Text; })
                .Text_Lambda([Item] { return Item->Asset.IsValid() ? Item->Asset->Definition.DisplayName : FText::GetEmpty(); })
                .ToolTipText_Lambda([this, Item]
                {
                    if (!Item->Asset.IsValid()) return FText::GetEmpty();
                    const int32 Index = Model->Catalog()->Levels.IndexOfByKey(Item->Asset.Get());
                    return FText::Format(Txt(TEXT("第 {0} 关 · {1}\n棋盘资产 {2}\n双击改显示名称，拖曳调整出场顺序。")),
                        FText::AsNumber(Index + 1), Item->Asset->Definition.DisplayName, FText::FromString(Item->Asset->GetName()));
                })
                .IsReadOnly_Lambda([this] { return Model->bPlaying; })
                .IsSelected_Lambda([this, Item] { return Model->Asset() == Item->Asset.Get(); })
                .DelayedLeftClickEntersEditMode(false).DoubleSelectDelay(0.f)
                .OnVerifyTextChanged_Lambda([](const FText& Text, FText& Error)
                {
                    if (!Text.IsEmptyOrWhitespace()) return true;
                    Error = Txt(TEXT("请输入关卡显示名称"));
                    return false;
                })
                .OnTextCommitted_Lambda([this, Item](const FText& NameText, ETextCommit::Type Commit)
                {
                    if (Commit != ETextCommit::OnCleared) Model->Rename(Item->Asset.Get(), NameText);
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
            [
                SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.BulletPoint"))).ColorAndOpacity(Look().Accent)
                .Visibility_Lambda([Item]
                {
                    return Item->Asset.IsValid() && Item->Asset->GetOutermost()->IsDirty() ? EVisibility::Visible : EVisibility::Hidden;
                })
                .ToolTipText(Txt(TEXT("有未保存修改")))
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [Icon(TEXT("Icons.DragHandle"), 13.f, Look().Muted)]
        ]
    ];
    Item->InlineName = Name;
    return RowWidget;
}

void SPuzzleAuthoringPanel::SelectRow(FLevelRow Item, ESelectInfo::Type)
{
    if (bRefreshing || !Item) return;
    if (Item->Asset.Get() != Model->Asset()) Model->SetActive(Item->Asset.Get());
}

void SPuzzleAuthoringPanel::BeginRename(FLevelRow Item)
{
    if (Model->bPlaying || !Item) return;
    if (auto InlineName = Item->InlineName.Pin()) InlineName->EnterEditingMode();
}

FReply SPuzzleAuthoringPanel::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    if (Model->Crop && Event.GetKey() == EKeys::Escape)
    {
        Model->CancelCrop();
        return FReply::Handled();
    }
    if (!Model->bPlaying && Event.GetKey() == EKeys::F2 && (List->HasKeyboardFocus() || List->HasFocusedDescendants()))
    {
        if (!List->GetSelectedItems().IsEmpty()) BeginRename(List->GetSelectedItems()[0]);
        return FReply::Handled();
    }
    if (!Model->bPlaying && Event.IsControlDown())
    {
        if (Event.GetKey() == EKeys::S)
        {
            Model->Save();
            return FReply::Handled();
        }
        if (Event.GetKey() == EKeys::Y || (Event.GetKey() == EKeys::Z && Event.IsShiftDown()))
        {
            GEditor->RedoTransaction();
            return FReply::Handled();
        }
        if (Event.GetKey() == EKeys::Z)
        {
            GEditor->UndoTransaction();
            return FReply::Handled();
        }
    }
    return SCompoundWidget::OnKeyDown(Geometry, Event);
}

void SPuzzleAuthoringPanel::Refresh()
{
    TGuardValue<bool> RefreshGuard(bRefreshing, true);
    TArray<FLevelRow> Updated;
    FLevelRow Selection;
    if (auto* Catalog = Model->Catalog())
        for (UPuzzleLevelAsset* Asset : Catalog->Levels)
        {
            if (!Asset) continue;
            const FLevelRow* Existing = Rows.FindByPredicate([Asset](const FLevelRow& Row) { return Row->Asset.Get() == Asset; });
            FLevelRow Entry = Existing ? *Existing : MakeShared<FLevelListItem>();
            Entry->Asset = Asset;
            Updated.Add(Entry);
            if (Asset == Model->Asset()) Selection = Entry;
        }
    if (Updated != Rows)
    {
        Rows = MoveTemp(Updated);
        List->RequestListRefresh();
    }
    List->SetSelection(Selection, ESelectInfo::Direct);
    const bool bActiveChanged = FramedAsset.Get() != Model->Asset();
    const FIntVector Dimensions = Model->Level() ? Model->Level()->GridDimensions : FIntVector::ZeroValue;
    const bool bSizeChanged = Dimensions != FramedDimensions;
    if (bActiveChanged || bSizeChanged) GridSizeDraft = Dimensions;
    Viewport->Refresh(bActiveChanged || bSizeChanged);
    FramedAsset = Model->Asset();
    FramedDimensions = Dimensions;
    const auto Element = Model->Resolve(Model->Selection);
    if (bActiveChanged || !(InspectedElement == Element)) bInspectLevel = !Element.IsSet();
    InspectedElement = Element;
    RefreshInspector();
}

void SPuzzleAuthoringPanel::RefreshInspector()
{
    Details->ClearChildren();
    const auto Add = [&](TSharedRef<SWidget> Widget, float Bottom = 10.f)
    { Details->AddSlot().AutoHeight().Padding(0, 0, 0, Bottom)[Widget]; };
    const auto* Level = Model->Level();
    if (!Level)
    {
        Add(SNew(STextBlock).Text(Txt(TEXT("选择关卡后查看属性"))).Font(Look().Body).ColorAndOpacity(Look().Muted));
        return;
    }
    if (Model->Crop)
    {
        Add(Section(TEXT("顶视裁剪")));
        Add(SNew(STextBlock).Font(Look().Body).AutoWrapText(true).ColorAndOpacity(Look().Text)
            .Text_Lambda([this]
            {
                if (!Model->Crop) return FText::GetEmpty();
                const auto& Crop = *Model->Crop;
                return FText::FromString(FString::Printf(
                    TEXT("拖选要保留的 %d × %d 格\n当前框选 %d × %d 格\n保留 Z0 起的 %d 层，框外和更高的元素会被裁掉。"),
                    Crop.RetainedSize.X, Crop.RetainedSize.Y, Crop.Range.Width(), Crop.Range.Height(),
                    FMath::Min(Crop.Dimensions.Z, Model->Level()->GridDimensions.Z)));
            }));
        Add(Button(Txt(TEXT("应用裁剪")), [this] { Model->ApplyCrop(); }, TEXT("Icons.Check"),
            TEXT("按框选范围裁剪棋盘，保留元素的世界位置。可用 Ctrl+Z 撤销。"), true,
            TAttribute<bool>::CreateLambda([this] { return !Model->bPlaying && Model->Crop && Model->Crop->HasValidRange(); })));
        Add(Button(Txt(TEXT("取消裁剪")), [this] { Model->CancelCrop(); }, NAME_None, TEXT("Esc 退出裁剪预览")));
        return;
    }
    const auto Element = Model->Resolve(Model->Selection);
    if (!bInspectLevel)
    {
        if (!Element.IsSet())
        {
            Add(SNew(STextBlock).Text(Txt(TEXT("在视口中选择元素"))).Font(Look().Body).ColorAndOpacity(Look().Muted));
            return;
        }
        const FText Heading = Element.Id == INDEX_NONE ? PuzzleToolName(Element.Type) :
            FText::Format(Txt(TEXT("{0} #{1}")), PuzzleToolName(Element.Type), FText::AsNumber(Element.Id));
        Add(SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
            [Icon(ToolIcon(Element.Type), 17.f, Look().Accent)]
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [SNew(STextBlock).Text(Heading).Font(Look().Title).ColorAndOpacity(Look().Text)]);
        Add(Field(Txt(TEXT("网格坐标")), XYZ<FIntVector>([this] { return Model->Resolve(Model->Selection).Position; },
            [this](FIntVector P) { Model->Move(P); }, GridCoordinatesTip), GridCoordinatesTip));
        if (Element.Type == EPuzzleTool::Player)
        {
            Add(Field(FText::Format(Txt(TEXT("朝向   {0}")), UEnum::GetDisplayValueAsText(Level->PlayerStartFacing)),
                SNew(SUniformGridPanel).SlotPadding(FMargin(3, 0))
                + SUniformGridPanel::Slot(0, 0)
                [Button(Txt(TEXT("左转 90°")), [this] { Model->RotatePlayerStart(false); }, TEXT("Icons.Rotate90CounterClockwise"),
                    TEXT("从顶视图看逆时针转 90°。可用 Ctrl+Z 撤销。"))]
                + SUniformGridPanel::Slot(1, 0)
                [Button(Txt(TEXT("右转 90°")), [this] { Model->RotatePlayerStart(true); }, TEXT("Icons.Rotate90Clockwise"),
                    TEXT("从顶视图看顺时针转 90°。可用 Ctrl+Z 撤销。"))],
                TEXT("箭头显示角色开局和重试时面向的网格方向。")));
        }
        if (Element.Type == EPuzzleTool::Box)
        {
            const auto Read = [this, Id = Element.Id]
            {
                if (const auto* Def = Model->Level())
                    if (const auto* Box = Def->InitialBoxes.FindByPredicate([Id](const auto& Item) { return Item.BoxId == Id; })) return *Box;
                return FRuneBoxState{};
            };
            const TCHAR* IdTip = TEXT("本关唯一的正整数。可填入已删除箱子的编号。改号会同步引用当前箱子的阵眼。");
            Add(Field(Txt(TEXT("箱子编号")), Number<int32>([Read] { return Read().BoxId; },
                [this](int32 Id) { Model->RenumberBox(Id); }, IdTip), IdTip));
            Add(Section(TEXT("初始状态")), 8.f);
            Add(Check([Read] { return Read().bHiss; }, [this, Read](bool B) { auto Box = Read(); Box.bHiss = B; Model->ChangeBox(Box); },
                Txt(TEXT("Hiss 相态")), TEXT("箱子开局就是 Hiss，能与一个普通箱或硬障碍同格。相态决定格子占用，重力由支撑、风托和水锚决定。")), 7.f);
            Add(Check([Read] { return Read().bPhaseLocked; }, [this, Read](bool B) { auto Box = Read(); Box.bPhaseLocked = B; Model->ChangeBox(Box); },
                Txt(TEXT("PhaseLock 锁高")), TEXT("开局带水锚，免于重力下落。它仍会响应合法的电推和风抬。")), 9.f);
            const TCHAR* WindTip = TEXT("开局在当前层临时悬浮的剩余回合，范围为 0–2。成功走格或施法会推进回合。归零后，缺少支撑和水锚的箱子会下落。");
            Add(Field(Txt(TEXT("风托回合")), Number<int32>([Read] { return Read().LevitationTurns; }, [this, Read](int32 N)
            {
                if (N < 0 || N > FPuzzleSolver::LevitationDuration) { Model->SetStatus(TEXT("风托回合可填写 0–2")); return; }
                auto Box = Read(); Box.LevitationTurns = N; Model->ChangeBox(Box);
            }, WindTip), WindTip));
            if (Level->InitialBoxes.ContainsByPredicate([&](const FRuneBoxState& Box)
                { return Box.BoxId != Element.Id && Box.GridPosition == Element.Position; }))
                Add(Button(Txt(TEXT("切换同格箱")), [this] { Model->CycleBox(); }, TEXT("Icons.Refresh")));
        }
        if (Element.Type == EPuzzleTool::Goal)
        {
            const auto Read = [this, Id = Element.Id]
            {
                if (const auto* Def = Model->Level())
                    if (const auto* Goal = Def->Goals.FindByPredicate([Id](const auto& Item) { return Item.GoalId == Id; })) return *Goal;
                return FRuneGoalData{};
            };
            Add(Section(TEXT("达成条件")), 8.f);
            Add(Check([Read] { return Read().bHissPair; }, [this, Read](bool B)
            {
                auto Goal = Read();
                Goal.bHissPair = B;
                if (B && Goal.HissBoxId == INDEX_NONE) Goal.HissBoxId = FRuneGoalData::AnyBoxId;
                Model->ChangeGoal(Goal);
            }, Txt(TEXT("双箱 Hiss 叠合")), TEXT("阵眼需要一只普通箱和一只 Hiss 箱在同一格。两只箱子可分别指定编号或放宽为任意箱。")));
            const auto AddBoxSlot = [&](const TCHAR* Label, int32 FRuneGoalData::*Member, const TCHAR* Tip)
            {
                const auto Write = [this, Read, Member](int32 Id)
                {
                    auto Goal = Read();
                    Goal.*Member = Id;
                    Model->ChangeGoal(Goal);
                };
                auto Controls = SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [Check([Read, Member] { return Read().*Member == FRuneGoalData::AnyBoxId; },
                        [Write](bool bAny) { Write(bAny ? FRuneGoalData::AnyBoxId : INDEX_NONE); }, Txt(TEXT("任意箱子")), Tip)];
                if (Read().*Member != FRuneGoalData::AnyBoxId)
                    Controls->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
                    [
                        Number<int32>([Read, Member] { return Read().*Member; }, [this, Write](int32 Id)
                        {
                            if (Id > 0) Write(Id);
                            else Model->SetStatus(TEXT("请填写正整数箱号，或选择任意箱子"));
                        }, Tip)
                    ];
                Add(Field(Txt(Label), Controls, Tip));
            };
            AddBoxSlot(Read().bHissPair ? TEXT("普通箱") : TEXT("达成阵眼的箱子"), &FRuneGoalData::BoxId,
                TEXT("勾选后，符合相态和水锚条件的任意箱子都能满足这一项。取消勾选后填写指定箱号。"));
            if (Read().bHissPair)
                AddBoxSlot(TEXT("Hiss 箱"), &FRuneGoalData::HissBoxId,
                    TEXT("需要 Hiss 态的箱子。勾选后可用任意 Hiss 箱，取消勾选后填写指定箱号。"));
            else
            {
                const TCHAR* Labels[] = {TEXT("任意状态"), TEXT("普通态"), TEXT("Hiss 态")};
                const TCHAR* Tips[] = {TEXT("普通态和 Hiss 态都可达成。箱号与水锚条件照常检查。"),
                    TEXT("到达阵眼时，箱子须处于普通态。"), TEXT("到达阵眼时，箱子须处于 Hiss 态。")};
                auto States = SNew(SUniformGridPanel).SlotPadding(FMargin(2.f, 0));
                for (int32 Index = 0; Index < UE_ARRAY_COUNT(Labels); ++Index)
                {
                    const auto State = static_cast<ERuneGoalState>(Index);
                    States->AddSlot(Index, 0)
                    [Toggle(Txt(Labels[Index]), [Read, State] { return Read().RequiredState == State; }, [this, Read, State](bool B)
                    { if (B) { auto Goal = Read(); Goal.RequiredState = State; Model->ChangeGoal(Goal); } }, NAME_None, Tips[Index])];
                }
                Add(Field(Txt(TEXT("箱子状态")), States, TEXT("规定达成阵眼时箱子的相态。")));
            }
            Add(Check([Read] { return Read().bRequireWaterAnchor; }, [this, Read](bool B)
            {
                auto Goal = Read();
                Goal.bRequireWaterAnchor = B;
                Model->ChangeGoal(Goal);
            }, Txt(Read().bHissPair ? TEXT("普通箱须水锚") : TEXT("达成箱须水锚")),
                TEXT("到达阵眼时还需要有水锚。可预设箱子的锁高状态，或在关中用沦灭潭渊。双箱目标只检查普通箱的水锚。")));
        }
        Add(SNew(SBox).HeightOverride(1.f)[SNew(SImage).Image(&Look().Divider)]);
        Add(Button(Txt(TEXT("删除元素")), [this] { Model->Erase(Model->Selection); }, TEXT("Icons.Delete"),
            TEXT("删除选中元素（Delete），可用 Ctrl+Z 撤销。若删除被阵眼指定的箱子，校验会指出缺失箱号。")));
        return;
    }
    Add(Field(Txt(TEXT("网格尺寸")),
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [XYZ<FIntVector>([this] { return GridSizeDraft; }, [this](FIntVector Dims) { GridSizeDraft = Dims; }, GridDimensionsTip)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
        [Button(Txt(TEXT("应用尺寸")), [this] { ApplyGridSize(); }, NAME_None,
            TEXT("扩大会给新增的 Z0 格铺地板。缩小会进入顶视图框选保留范围。"), false,
            TAttribute<bool>::CreateLambda([this]
            { return !Model->bPlaying && Model->Level() && GridSizeDraft != Model->Level()->GridDimensions; }))],
        GridDimensionsTip));
    Add(SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 5, 0)
        [
            Field(Txt(TEXT("格宽 · cm")), Number<float>([this] { return Model->Level()->GridCellSize; }, [this](float Size)
            {
                if (!FMath::IsFinite(Size) || Size <= 0.f) { Model->SetStatus(TEXT("格宽必须为正数")); return; }
                if (Size != Model->Level()->GridCellSize) Model->UpdateLevel([&](auto& Def) { Def.GridCellSize = Size; });
            }, CellSizeTip), CellSizeTip)
        ]
        + SHorizontalBox::Slot().FillWidth(1.f)
        [
            Field(Txt(TEXT("初始 SP")), Number<int32>([this] { return Model->Level()->InitialSP; }, [this](int32 SP)
            {
                if (SP < 0) { Model->SetStatus(TEXT("初始 SP 不能为负数")); return; }
                if (SP != Model->Level()->InitialSP) Model->UpdateLevel([&](auto& Def) { Def.InitialSP = SP; });
            }, InitialSPTip), InitialSPTip)
        ]);
    Add(Field(Txt(TEXT("世界原点 · cm")), XYZ<FVector>([this] { return Model->Level()->GridOrigin; },
        [this](FVector Origin) { if (!Origin.ContainsNaN()) Model->UpdateLevel([&](auto& Def) { Def.GridOrigin = Origin; }); },
        GridOriginTip), GridOriginTip));
    Add(Section(TEXT("教程")), 8.f);
    auto Skills = SNew(SUniformGridPanel).SlotPadding(FMargin(0.f, 2.f, 8.f, 2.f));
    for (int32 Index = 0; Index < RuneSkills::Count; ++Index)
    {
        const auto& Entry = RuneSkills::Ordered[Index];
        const FText Label = FText::FromString(FString::Printf(TEXT("%d · %s"), Index + 1, Entry.Name));
        Skills->AddSlot(Index % 2, Index / 2)
        [
            Check([this, Skill = Entry.Skill]
            {
                const auto* Def = Model->Level();
                return Def && Def->AvailableSkills.Contains(Skill);
            }, [this, Skill = Entry.Skill](bool bEnabled)
            {
                Model->UpdateLevel([&](auto& Def)
                {
                    if (bEnabled) Def.AvailableSkills.AddUnique(Skill);
                    else Def.AvailableSkills.Remove(Skill);
                });
            }, Label, SkillTip(Entry.Action))
        ];
    }
    Add(Field(Txt(TEXT("可用技能")), Skills, TEXT("勾选后本关可以使用该技能。数字键各自固定，悬停某项可查看施放效果。")));
    Add(Field(Txt(TEXT("入场提示")), SNew(SMultiLineEditableTextBox).Style(&Look().Entry).Font(Look().Body)
        .Text(Level->EntryTip).HintText(Txt(TEXT("留空可跳过入场提示"))).AutoWrapText(true)
        .OnTextCommitted_Lambda([this, Id = Level->LevelId](const FText& Text, ETextCommit::Type Method)
        {
            if (Method != ETextCommit::OnCleared && Model->Level() && Model->Level()->LevelId == Id && !Text.EqualTo(Model->Level()->EntryTip))
                Model->UpdateLevel([&](auto& Def) { Def.EntryTip = Text; });
        }), TEXT("进入关卡时显示的引导文字。可留空。")));
    Add(SNew(SBox).HeightOverride(1.f)[SNew(SImage).Image(&Look().Divider)]);
    const FIntVector Start = Level->PlayerStart;
    Add(SNew(STextBlock).Font(Look().Small).ColorAndOpacity(Look().Muted)
        .Text(Level->IsValidGridCoordinate(Start) ? FText::FromString(FString::Printf(TEXT("出生点   %d, %d, %d"), Start.X, Start.Y, Start.Z)) : Txt(TEXT("出生点未设置")))
        .ToolTipText(Txt(TEXT("玩家的开局位置。用左侧出生点工具放置，格子需要有地板或障碍顶面支撑。"))));
    const TCHAR* AssetTip = TEXT("可编辑完整的棋盘资产名。按 Enter 或点「改名」提交，UE 会更新目录引用。");
    const TWeakObjectPtr<UPuzzleLevelAsset> EditedAsset = Model->Asset();
    auto AssetName = MakeShared<FText>(FText::FromString(Model->Asset()->GetName()));
    const auto RenameAsset = [this, EditedAsset, AssetName]
    {
        if (EditedAsset.IsValid() && Model->Asset() == EditedAsset.Get())
            Model->RenameAsset(AssetName->ToString());
    };
    Add(Field(Txt(TEXT("棋盘资产名")),
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SEditableTextBox).Style(&Look().Entry).Font(Look().Body).Text(*AssetName)
            .IsReadOnly_Lambda([this] { return Model->bPlaying; }).ToolTipText(Txt(AssetTip))
            .SelectAllTextWhenFocused(true)
            .OnTextChanged_Lambda([AssetName](const FText& Text) { *AssetName = Text; })
            .OnTextCommitted_Lambda([RenameAsset, AssetName](const FText& Text, ETextCommit::Type Reason)
            {
                *AssetName = Text;
                if (Reason == ETextCommit::OnEnter) RenameAsset();
            })
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
        [Button(Txt(TEXT("改名")), RenameAsset, NAME_None, AssetTip, false,
            TAttribute<bool>::CreateLambda([this, EditedAsset, AssetName]
            {
                return !Model->bPlaying && EditedAsset.IsValid() && !AssetName->IsEmptyOrWhitespace() &&
                    AssetName->ToString() != EditedAsset->GetName();
            }))], AssetTip));
}

void SPuzzleAuthoringPanel::ApplyGridSize()
{
    if (!Model->Level()) return;
    const auto Current = Model->Level()->GridDimensions;
    if (GridSizeDraft.X < Current.X || GridSizeDraft.Y < Current.Y || GridSizeDraft.Z < Current.Z)
    {
        if (!Model->BeginCrop(GridSizeDraft)) return;
        GridSizeDraft = Current;
        bTopView = true;
        Viewport->SetTopView(true);
    }
    else Model->Resize(GridSizeDraft);
}

void SPuzzleAuthoringPanel::OpenNewDialog()
{
    struct FNewLevel
    {
        FText Name = Txt(TEXT("新关卡"));
        FText AssetSuffix;
        FIntVector Dimensions{5, 5, 3};
        float Cell;
        bool Floor = true;
    };
    auto Fields = MakeShared<FNewLevel>();
    Fields->Cell = Model->DefaultCellSize();
    auto Error = MakeShared<FText>();
    auto Window = SNew(SWindow).Title(Txt(TEXT("新建网格关卡"))).SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    TSharedPtr<SEditableTextBox> NameEntry;
    Window->SetContent(SNew(SBox).MinDesiredWidth(362)
    [
        SNew(SBorder).BorderImage(&Look().Panel).Padding(20)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [
                Field(Txt(TEXT("关卡名称")), SAssignNew(NameEntry, SEditableTextBox).Text(Fields->Name)
                    .Style(&Look().Entry).Font(Look().Body).SelectAllTextWhenFocused(true)
                    .OnTextChanged_Lambda([Fields](const FText& Value) { Fields->Name = Value; }),
                    TEXT("在关卡列表和游戏中显示的名称。"))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [
                Field(Txt(TEXT("棋盘资产后缀 · DA_Puzzle_")), SNew(SEditableTextBox)
                    .Style(&Look().Entry).Font(Look().Body).HintText(Txt(TEXT("留空使用关卡名称")))
                    .OnTextChanged_Lambda([Fields](const FText& Value) { Fields->AssetSuffix = Value; }),
                    TEXT("填写内容浏览器资产名的后半部分，例如 WindTutorial。留空时根据关卡名称生成。"))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [
                Field(Txt(TEXT("网格尺寸")), XYZ<FIntVector>(
                    [Fields] { return Fields->Dimensions; }, [Fields](FIntVector Dims) { Fields->Dimensions = Dims; }, GridDimensionsTip),
                    GridDimensionsTip)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [
                Field(Txt(TEXT("格宽 · cm")), Number<float>([Fields] { return Fields->Cell; }, [Fields](float Size) { Fields->Cell = Size; },
                    CellSizeTip), CellSizeTip)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [Check([Fields] { return Fields->Floor; }, [Fields](bool B) { Fields->Floor = B; }, Txt(TEXT("生成 Z0 基础地板")),
                TEXT("创建时为最底下一层的每个格子铺地板。"))]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock).Font(Look().Body).AutoWrapText(true).Text_Lambda([Error] { return *Error; })
                .ColorAndOpacity(Look().Error).Visibility_Lambda([Error] { return Error->IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
            ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 8, 0, 0)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                [Button(Txt(TEXT("取消")), [WeakWindow] { if (auto Dialog = WeakWindow.Pin()) Dialog->RequestDestroyWindow(); })]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    Button(Txt(TEXT("创建关卡")), [this, Fields, Error, WeakWindow]
                    {
                        if (Model->NewLevel(Fields->Name, Fields->Dimensions, Fields->Cell, Fields->Floor, Fields->AssetSuffix.ToString()))
                        {
                            if (auto Dialog = WeakWindow.Pin()) Dialog->RequestDestroyWindow();
                        }
                        else *Error = FText::FromString(Model->Status);
                    }, TEXT("Icons.Plus"), TEXT(""), true)
                ]
            ]
        ]
    ]);
    Window->SetWidgetToFocusOnActivate(NameEntry);
    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().FindWidgetWindow(AsShared()));
}

void SPuzzleAuthoringPanel::OpenRenameAssetDialog()
{
    if (!Model->Asset()) return;
    auto Name = MakeShared<FText>(FText::FromString(Model->Asset()->GetName()));
    auto Error = MakeShared<FText>();
    auto Window = SNew(SWindow).Title(Txt(TEXT("重命名棋盘资产"))).SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    const auto Apply = [this, Name, Error, WeakWindow]
    {
        if (Model->RenameAsset(Name->ToString()))
        {
            if (auto Dialog = WeakWindow.Pin()) Dialog->RequestDestroyWindow();
        }
        else *Error = FText::FromString(Model->Status);
    };
    TSharedPtr<SEditableTextBox> Entry;
    Window->SetContent(SNew(SBox).MinDesiredWidth(350)
        [SNew(SBorder).BorderImage(&Look().Panel).Padding(20)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
            [
                Field(Txt(TEXT("棋盘资产名")), SAssignNew(Entry, SEditableTextBox)
                    .Style(&Look().Entry).Font(Look().Body).Text(*Name).SelectAllTextWhenFocused(true)
                    .OnTextChanged_Lambda([Name](const FText& Value) { *Name = Value; })
                    .OnTextCommitted_Lambda([Apply](const FText&, ETextCommit::Type Reason)
                    { if (Reason == ETextCommit::OnEnter) Apply(); }),
                    TEXT("修改内容浏览器里的棋盘资产名。编辑器会更新资产引用。"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Font(Look().Body).AutoWrapText(true).Text_Lambda([Error] { return *Error; }).ColorAndOpacity(Look().Error)]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 8, 0, 0)
            [Button(Txt(TEXT("重命名")), Apply, TEXT("Icons.Edit"), TEXT(""), true)]
        ]]);
    Window->SetWidgetToFocusOnActivate(Entry);
    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().FindWidgetWindow(AsShared()));
}
