#pragma once

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"

struct FPuzzleEditorStyle
{
    static FLinearColor Color(const TCHAR* Hex) { return FLinearColor::FromSRGBColor(FColor::FromHex(Hex)); }
    static const FPuzzleEditorStyle& Get()
    {
        static const FPuzzleEditorStyle Style;
        return Style;
    }

    const FLinearColor Text = Color(TEXT("EDF1F8"));
    const FLinearColor Muted = Color(TEXT("AAB5C8"));
    const FLinearColor Accent = Color(TEXT("C5BAFF"));
    const FLinearColor Error = Color(TEXT("FFB1AD"));
    const FLinearColor MainFill = Color(TEXT("AD9DFA"));
    const FLinearColor MainText = Color(TEXT("171129"));
    const FLinearColor Surface = Color(TEXT("181D27"));
    const FLinearColor Field = Color(TEXT("111620"));
    const FLinearColor Line = Color(TEXT("2B3342"));
    const FLinearColor Hover = Color(TEXT("252D3B"));
    const FLinearColor Selected = Color(TEXT("30284A"));

    const FSlateColorBrush Panel{Surface};
    const FSlateColorBrush Base{Color(TEXT("11151E"))};
    const FSlateColorBrush Divider{Line};
    const FSlateNoResource EmptyBrush;
    const FSlateRoundedBoxBrush Floating{Color(TEXT("1B2230")), 7.f, Line, 1.f};
    const FSlateRoundedBoxBrush GlyphTile{Selected, 8.f};
    const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 15);
    const FSlateFontInfo Section = FCoreStyle::GetDefaultFontStyle("Bold", 11);
    const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 10);
    const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 9);
    FButtonStyle Quiet;
    FButtonStyle Primary;
    FCheckBoxStyle Toggle;
    FEditableTextBoxStyle Entry;
    FTableViewStyle LevelList;
    FTableRowStyle LevelRow;
    FSplitterStyle Splitter;

private:
    FPuzzleEditorStyle()
    {
        Quiet = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
        Quiet.SetNormal(EmptyBrush)
            .SetHovered(FSlateRoundedBoxBrush(Hover, 5.f))
            .SetPressed(FSlateRoundedBoxBrush(Selected, 5.f))
            .SetDisabled(EmptyBrush)
            .SetNormalForeground(Text).SetHoveredForeground(Text).SetPressedForeground(Accent)
            .SetDisabledForeground(Muted)
            .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));

        Primary = Quiet;
        Primary.SetNormal(FSlateRoundedBoxBrush(MainFill, 6.f))
            .SetHovered(FSlateRoundedBoxBrush(Accent, 6.f))
            .SetPressed(FSlateRoundedBoxBrush(Color(TEXT("9787E3")), 6.f))
            .SetDisabled(FSlateRoundedBoxBrush(Hover, 6.f))
            .SetNormalForeground(MainText).SetHoveredForeground(MainText).SetPressedForeground(MainText);

        Toggle = FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox");
        Toggle.SetUncheckedImage(EmptyBrush)
            .SetUncheckedHoveredImage(FSlateRoundedBoxBrush(Hover, 5.f))
            .SetUncheckedPressedImage(FSlateRoundedBoxBrush(Hover, 5.f))
            .SetCheckedImage(FSlateRoundedBoxBrush(Selected, 5.f, Color(TEXT("69588C")), 1.f))
            .SetCheckedHoveredImage(FSlateRoundedBoxBrush(Selected, 5.f, Accent, 1.f))
            .SetCheckedPressedImage(FSlateRoundedBoxBrush(Selected, 5.f, Accent, 1.f))
            .SetForegroundColor(Muted).SetHoveredForegroundColor(Text).SetPressedForegroundColor(Text)
            .SetCheckedForegroundColor(Accent).SetCheckedHoveredForegroundColor(Accent).SetCheckedPressedForegroundColor(Accent)
            .SetPadding(FMargin(0));

        Entry = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
        Entry.SetBackgroundImageNormal(FSlateRoundedBoxBrush(Field, 5.f, Line, 1.f))
            .SetBackgroundImageHovered(FSlateRoundedBoxBrush(Field, 5.f, Muted, 1.f))
            .SetBackgroundImageFocused(FSlateRoundedBoxBrush(Field, 5.f, Accent, 1.f))
            .SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(Field, 5.f, Line, 1.f))
            .SetForegroundColor(Text)
            .SetPadding(FMargin(8.f, 5.f));

        LevelList = FAppStyle::Get().GetWidgetStyle<FTableViewStyle>("ListView");
        LevelList.SetBackgroundBrush(EmptyBrush);
        LevelRow = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
        LevelRow.SetEvenRowBackgroundBrush(EmptyBrush).SetOddRowBackgroundBrush(EmptyBrush)
            .SetEvenRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(Hover, 5.f))
            .SetOddRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(Hover, 5.f))
            .SetActiveBrush(FSlateRoundedBoxBrush(Selected, 5.f))
            .SetInactiveBrush(FSlateRoundedBoxBrush(Selected, 5.f))
            .SetActiveHoveredBrush(FSlateRoundedBoxBrush(Selected, 5.f))
            .SetInactiveHoveredBrush(FSlateRoundedBoxBrush(Selected, 5.f))
            .SetTextColor(Text).SetSelectedTextColor(Accent);

        Splitter = FCoreStyle::Get().GetWidgetStyle<FSplitterStyle>("Splitter");
        Splitter.SetHandleNormalBrush(Divider).SetHandleHighlightBrush(FSlateColorBrush(Accent));
    }
};
