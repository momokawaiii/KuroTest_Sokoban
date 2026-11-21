#include "ShengLuHUD.h"
#include "ShengLuGame.h"

#include "BatchedElements.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "GlobalRenderResources.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr double ResultExitSeconds = .24;
    const FLinearColor Gold(.95f, .69f, .37f);
    const FLinearColor Violet(.53f, .31f, .83f);
    const FLinearColor Mint(.37f, .88f, .76f);
    const FLinearColor Paper(.96f, .91f, .84f);
    const FLinearColor Muted(.69f, .65f, .71f);
    const FLinearColor Dark(.12f, .061f, .036f);
    const FLinearColor Warning(.99f, .52f, .37f);
    const FLinearColor SkillColors[] = {Gold, Mint, {.34f, .72f, 1.f}, {.72f, .44f, 1.f}};
    static_assert(UE_ARRAY_COUNT(SkillColors) == RuneSkills::Count);

    UTexture2D* LoadRuneCursor(FIntPoint& Hotspot)
    {
        constexpr int32 HeaderSize = 6;
        constexpr int32 EntrySize = 16;
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectContentDir() / TEXT("Slate/Cursors/PRTS_Default.cur"))) ||
            Bytes.Num() < HeaderSize || FPlatformMemory::ReadUnaligned<uint16>(Bytes.GetData()) != 0 ||
            FPlatformMemory::ReadUnaligned<uint16>(Bytes.GetData() + 2) != 2)
            return nullptr;

        const int32 EntryCount = FPlatformMemory::ReadUnaligned<uint16>(Bytes.GetData() + 4);
        const int32 DirectoryEnd = HeaderSize + EntryCount * EntrySize;
        if (!EntryCount || DirectoryEnd > Bytes.Num()) return nullptr;

        uint8* Selected = nullptr;
        FIntPoint SelectedSize = FIntPoint::ZeroValue;
        for (int32 Index = 0; Index < EntryCount; ++Index)
        {
            uint8* Entry = &Bytes[HeaderSize + Index * EntrySize];
            const FIntPoint Size(Entry[0] ? Entry[0] : 256, Entry[1] ? Entry[1] : 256);
            const FIntPoint Anchor(FPlatformMemory::ReadUnaligned<uint16>(Entry + 4),
                FPlatformMemory::ReadUnaligned<uint16>(Entry + 6));
            const uint32 Length = FPlatformMemory::ReadUnaligned<uint32>(Entry + 8);
            const uint32 Offset = FPlatformMemory::ReadUnaligned<uint32>(Entry + 12);
            // CUR stores a hotspot where ICO stores planes and bit depth.
            Entry[6] = Entry[7] = 0;
            if (Anchor.X >= Size.X || Anchor.Y >= Size.Y || Offset < uint32(DirectoryEnd) ||
                uint64(Offset) + Length > uint64(Bytes.Num()) || Size.X * Size.Y <= SelectedSize.X * SelectedSize.Y)
                continue;

            static constexpr uint8 PngSignature[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
            const uint8* Image = Bytes.GetData() + Offset;
            const bool bPng = Length >= sizeof(PngSignature) && FMemory::Memcmp(Image, PngSignature, sizeof(PngSignature)) == 0;
            const bool bColorBitmap = Length >= 40 && FPlatformMemory::ReadUnaligned<uint16>(Image + 14) == 32;
            if (!bPng && !bColorBitmap) continue;

            Selected = Entry;
            SelectedSize = Size;
            Hotspot = Anchor;
        }
        if (!Selected) return nullptr;

        // Decode just the selected colour entry with UE's ICO reader, keeping the source file intact.
        Bytes[2] = 1;
        Selected[4] = 1;
        Selected[5] = 0;
        Selected[6] = 32;
        UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(Bytes);
        return Texture && Texture->GetSizeX() == SelectedSize.X && Texture->GetSizeY() == SelectedSize.Y ? Texture : nullptr;
    }

    enum class EPart : uint8
    {
        Veil, EdgeGlass, FloatingGlass, Level, Resource, Operation, OperationTab, OperationIcon, Context,
        Skill1, Skill2, Skill3, Skill4, Undo, Retry, DepthView, SkillTooltip, TooltipIcon, CellTooltip,
        ResultVeil, ResultOrbit, ResultGlass, ResultDial, Seal, Heading, Score, Accept, ResultRetry, ResultOther, Count
    };

    enum class EMode : uint8 { Panel, Skill, Dial, Glass, Veil, Heading, Icon, Frame };
    enum class EPanel : uint8 { Info, Resource = 2, Utility = 4, Secondary, Primary, Tooltip };
    enum class EFrame : uint8 { Level, Operation, OperationTab, Guide };

    struct FPose
    {
        float Opacity = 1.f;
        float Reveal = 1.f;
        float Focus = 0.f;
        float Pulse = 1.f;
        float Variant = 0.f;
        float Lock = 0.f;
    };

    float Progress(double Age, double Duration)
    {
        return float(FMath::Clamp(Age / Duration, 0., 1.));
    }

    float Ease(double Age, double Duration)
    {
        return FMath::SmoothStep(0.f, 1.f, Progress(Age, Duration));
    }

    FLinearColor Fade(FLinearColor Color, float Opacity)
    {
        Color.A *= Opacity;
        return Color;
    }

    bool HitSlant(FVector2D Point, FVector2D At, FVector2D Size, float Skew)
    {
        const FVector2D P = Point - At;
        if (P.Y < 0. || P.Y > Size.Y) return false;
        const double Left = Skew * (1. - P.Y / Size.Y);
        return P.X >= Left && P.X <= Left + Size.X - Skew;
    }

    struct FMeasuredText : FCanvasTextStringViewItem
    {
        using FCanvasTextStringViewItem::FCanvasTextStringViewItem;
        using FCanvasTextStringViewItem::GetTextSize;
    };

    struct FHUDPainter
    {
        UCanvas& Canvas;
        const TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials;
        const UFont& Font;
        float Scale;
        float Clock;

        void Line(FVector2D A, FVector2D B, FLinearColor Color, float Width = 1.f) const
        {
            if (Color.A <= 0.f || A.Equals(B)) return;
            const FVector2D Direction = (B - A).GetSafeNormal();
            const FVector2D Side = FVector2D(-Direction.Y, Direction.X) * (.5 * Width);
            FCanvas& DrawCanvas = *Canvas.Canvas;
            auto* Batch = DrawCanvas.GetBatchedElements(FCanvas::ET_Triangle, nullptr, GWhiteTexture, SE_BLEND_Translucent);
            const auto Vertex = [&](FVector2D At)
            {
                return Batch->AddVertexf(FVector4f(float(At.X * Scale), float(At.Y * Scale), 0.f, 1.f),
                    FVector2f::ZeroVector, Color, DrawCanvas.GetHitProxyId());
            };
            const int32 A0 = Vertex(A - Side), A1 = Vertex(A + Side);
            const int32 B0 = Vertex(B - Side), B1 = Vertex(B + Side);
            Batch->AddTriangle(A0, A1, B0, GWhiteTexture, SE_BLEND_Translucent);
            Batch->AddTriangle(B0, A1, B1, GWhiteTexture, SE_BLEND_Translucent);
        }

        void Rect(FVector2D At, FVector2D Size, FLinearColor Color) const
        {
            FCanvasTileItem Item(At * Scale, Size * Scale, Color);
            Item.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Item);
        }

        void Triangle(FVector2D A, FVector2D B, FVector2D C, FLinearColor Color) const
        {
            FCanvasTriangleItem Item(A * Scale, B * Scale, C * Scale, GWhiteTexture);
            Item.SetColor(Color);
            Item.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Item);
        }

        FSlateFontInfo FontInfo(float Points, bool bBold) const
        {
            FSlateFontInfo Info = Font.GetLegacySlateFontInfo();
            Info.Size = FMath::Max(1, FMath::RoundToInt(Points * Scale * .82f));
            Info.TypefaceFontName = bBold ? TEXT("Bold") : TEXT("Regular");
            return Info;
        }

        void Text(FStringView Label, FVector2D At, FLinearColor Color, float Points,
            bool bCenter = false, float Width = 0.f, bool bBold = false) const
        {
            if (Label.IsEmpty() || Color.A <= 0.f) return;
            FMeasuredText Item(At * Scale, Label, FontInfo(Points, bBold), Color);
            if (Width > 0.f)
            {
                const float Measured = Item.GetTextSize(Canvas.GetDPIScale()).X;
                if (Measured > Width * Scale) Item.Scale.X = Width * Scale / Measured;
            }
            Item.bCentreX = Item.bCentreY = bCenter;
            Canvas.DrawItem(Item);
        }

        void Diamond(FVector2D Center, float Radius, FLinearColor Color) const
        {
            const FVector2D Points[] = {{0, -Radius}, {Radius, 0}, {0, Radius}, {-Radius, 0}};
            for (int32 Index = 0; Index < 4; ++Index)
                Line(Center + Points[Index], Center + Points[(Index + 1) % 4], Color);
        }

        void Key(FStringView Label, FVector2D Center, float Width, FLinearColor Ink, FLinearColor Edge) const
        {
            const FVector2D Half(Width * .5f, 10);
            const FVector2D A = Center - Half, B = Center + Half;
            Line(A, {B.X, A.Y}, Edge);
            Line({B.X, A.Y}, B, Edge);
            Line(B, {A.X, B.Y}, Edge);
            Line({A.X, B.Y}, A, Edge);
            Text(Label, Center, Ink, 11.f, true);
        }

        UMaterialInstanceDynamic* SetPaint(EPart Part, EMode Mode, FVector2D Size, float Skew,
            FLinearColor Accent, FPose Pose) const
        {
            if (Materials.IsEmpty()) return nullptr;
            auto* Material = Materials[int32(Part)].Get();
            static const FName Geometry(TEXT("Geometry")), Motion(TEXT("Motion")), State(TEXT("State")), Color(TEXT("Accent"));
            Material->SetVectorParameterValue(Geometry, {float(Size.X), float(Size.Y), Skew, float(Mode)});
            Material->SetVectorParameterValue(Motion, {Clock, Pose.Opacity, Pose.Focus, Pose.Pulse});
            Material->SetVectorParameterValue(State, {Pose.Reveal, Pose.Variant, Pose.Lock, 0.f});
            Material->SetVectorParameterValue(Color, Accent);
            return Material;
        }

        void Quad(EPart Part, EMode Mode, FVector2D At, FVector2D Size, float Skew,
            FLinearColor Accent, FPose Pose) const
        {
            if (Pose.Opacity <= 0.f) return;
            if (auto* Material = SetPaint(Part, Mode, Size, Skew, Accent, Pose))
            {
                FCanvasTileItem Item(At * Scale, Material->GetRenderProxy(), Size * Scale);
                Canvas.DrawItem(Item);
            }
            else if (Mode == EMode::Panel || Mode == EMode::Skill || Mode == EMode::Veil || Mode == EMode::Frame)
            {
                Rect(At, Size, Fade({.026f, .021f, .042f, .87f}, Pose.Opacity));
            }
        }

        void Panel(EPart Part, EPanel Style, FVector2D At, FVector2D Size, float Skew,
            FLinearColor Accent, FPose Pose) const
        {
            Pose.Variant = float(Style);
            Quad(Part, EMode::Panel, At, Size, Skew, Accent, Pose);
        }

        void Frame(EPart Part, EFrame Style, FVector2D At, FVector2D Size, float Skew,
            FLinearColor Accent, FPose Pose) const
        {
            Pose.Variant = float(Style);
            Quad(Part, EMode::Frame, At, Size, Skew, Accent, Pose);
        }
    };

    struct FShard
    {
        FVector2D A, B, C;
        float Artwork, Opacity;
    };

    void DrawGlass(const FHUDPainter& P, EPart Part, TConstArrayView<FShard> Shards, FVector2D Origin,
        float Opacity, float Reveal, float Age, float VioletTint)
    {
        const bool bFloating = Part == EPart::FloatingGlass;
        const bool bAssembling = Part == EPart::ResultGlass;
        auto* Material = P.SetPaint(Part, EMode::Glass, {1, 1}, 0, Gold,
            {Opacity, Reveal, 0.f, 1.f, bFloating ? 1.f : 0.f, VioletTint});
        if (!Material || Opacity <= 0.f) return;
        FCanvasTriangleItem Items(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, nullptr);
        Items.TriangleList.Reset(Shards.Num());
        Items.MaterialRenderProxy = Material->GetRenderProxy();
        for (int32 Index = 0; Index < Shards.Num(); ++Index)
        {
            const FShard& Shard = Shards[Index];
            const FVector2D Center = (Shard.A + Shard.B + Shard.C) / 3.;
            const float Side = Center.X < 0 ? -1.f : 1.f;
            const float Offset = bAssembling ? 1.f - Ease(Age - (Index % 5) * .085, .86) : 0.f;
            FVector2D Drift(Side * Offset * (75 + (Index % 3) * 31), Offset * ((Index % 4) * 64 - 80));
            double Tilt = Side * Offset * (39 - (Index % 4) * 16);
            if (bFloating)
            {
                const double Phase = Index * 1.7;
                const double Bob = P.Clock * UE_DOUBLE_TWO_PI / (6. + (Index % 3) * 1.5) + Phase;
                const double Sway = P.Clock * UE_DOUBLE_TWO_PI / (9. + (Index % 2) * 3.) + Phase;
                Drift += FVector2D(11. * FMath::Sin(Sway), 18. * FMath::Sin(Bob));
                Tilt += 5. * FMath::Sin(Sway + 1.);
            }
            const auto Vertex = [&](FVector2D Pos)
            {
                const FVector2D Displaced = (Pos - Center).GetRotated(Tilt);
                return (Origin + Center + Drift + Displaced * (1. - .23 * Offset)) * P.Scale;
            };
            const auto Sample = [&](FVector2D Pos)
            {
                // Canvas encodes vertex RGB as sRGB. Predecode our UV and art weight before packing.
                const auto Channel = [](double Value) { return FMath::RoundToInt(FMath::Clamp(Value, 0., 1.) * 255.); };
                return FLinearColor::FromSRGBColor(FColor(Channel(.5 + Pos.X / 1060.), Channel((Pos.Y + 360.) / 521.),
                    Channel(Shard.Artwork), Channel(Shard.Opacity)));
            };
            FCanvasUVTri& Tri = Items.TriangleList.AddDefaulted_GetRef();
            Tri.V0_Pos = Vertex(Shard.A); Tri.V1_Pos = Vertex(Shard.B); Tri.V2_Pos = Vertex(Shard.C);
            Tri.V0_UV = {0., 0.}; Tri.V1_UV = {1., 0.}; Tri.V2_UV = {0., 1.};
            Tri.V0_Color = Sample(Shard.A); Tri.V1_Color = Sample(Shard.B); Tri.V2_Color = Sample(Shard.C);
        }
        P.Canvas.DrawItem(Items);
    }

    void DrawPeriphery(const FHUDPainter& P, FVector2D Size, float Opacity, float Entry, bool bFirstPerson)
    {
        const double X = Size.X * .5, Y = Size.Y * .5;
        const FShard UpperShards[] = {
            {{-X, -Y + 161}, {-X + 85, -Y}, {-X + 121, -Y}, .48f, .78f},
            {{-X + 41, -Y + 114}, {-X + 106, -Y + 22}, {-X + 231, -Y - 7}, .21f, .78f},
            {{-X + 314, -Y + 113}, {-X + 394, -Y}, {-X + 482, -Y - 12}, .11f, .48f},
        };
        const FShard LowerShards[] = {
            {{-X - 8, Y - 165}, {-X + 111, Y - 99}, {-X + 170, Y - 131}, .06f, .43f},
            {{-X + 28, Y - 3}, {-X + 135, Y - 27}, {-X + 207, Y - 149}, .28f, .79f},
            {{X + 13, Y - 354}, {X - 226, Y - 132}, {X - 9, Y - 232}, .22f, .65f},
            {{X + 89, Y - 196}, {X - 84, Y - 121}, {X - 350, Y - 47}, .09f, .77f},
            {{X + 36, Y - 56}, {X - 125, Y - 47}, {X - 246, Y + 20}, .34f, .73f},
        };
        P.Quad(EPart::Veil, EMode::Veil, {}, Size, 0, Violet, {Opacity * (bFirstPerson ? .55f : .92f)});
        DrawGlass(P, EPart::EdgeGlass, UpperShards, Size * .5, Opacity * .82f, Entry, 0.f, .98f);
        DrawGlass(P, EPart::FloatingGlass, LowerShards, Size * .5, Opacity * .82f, Entry, 0.f, .98f);
        P.Line({18, Size.Y - 31}, {Size.X - 18, Size.Y - 31}, Fade(Gold, Opacity * Entry * .18f));
    }

    bool DrawLevelPanel(const FHUDPainter& P, const AShengLuBoard& Game, FStringView LevelName,
        FVector2D Mouse, FPose Pose)
    {
        const auto& Level = Game.Level();
        const auto& Runtime = Game.Runtime();
        const FVector2D At(32. - 22. * (1.f - Pose.Reveal), 34);
        const FVector2D Extent(326, 166);
        constexpr float Skew = 27.f;
        const float Alpha = Pose.Opacity * Pose.Reveal;
        const FLinearColor Ink = Fade(Paper, Alpha);
        const FLinearColor GoldInk = Fade(Gold, Alpha);
        P.Frame(EPart::Level, EFrame::Level, At, Extent, Skew, Gold, Pose);
        P.Text(LevelName, At + FVector2D(37, 14), Ink, 22.5f, false, 260.f, true);
        P.Line(At + FVector2D(38, 45), At + FVector2D(289, 45), Fade(GoldInk, .42f));
        P.Diamond(At + FVector2D(289, 45), 2.2f, Fade(GoldInk, .78f));
        const FString Summary = Game.UsesLegacyPresentation() ? FString(TEXT("将符文 A 落座在金色台顶")) :
            FString::Printf(TEXT("%d 个阵眼 · %d 个符文箱"), Level.Goals.Num(), Runtime.Boxes.Num());
        P.Text(Summary, At + FVector2D(37, 53), Fade(Muted, Alpha), 14.f, false, 244.f);

        const FVector2D Resource = At + FVector2D(21, 78);
        P.Panel(EPart::Resource, EPanel::Resource, Resource, {284, 41}, 11, Gold, Pose);
        const FVector2D Bolt = Resource + FVector2D(20, 18);
        P.Diamond(Bolt, 7.5f, Fade(GoldInk, .82f));
        P.Line(Bolt + FVector2D(1.5, -4), Bolt + FVector2D(-1.5, 0), GoldInk, 1.25f);
        P.Line(Bolt + FVector2D(-1.5, 0), Bolt + FVector2D(1.5, 0), GoldInk, 1.25f);
        P.Line(Bolt + FVector2D(1.5, 0), Bolt + FVector2D(-1.5, 4), GoldInk, 1.25f);
        P.Text(FString::Printf(TEXT("SP  %d / %d"), Runtime.SP, Level.InitialSP),
            Resource + FVector2D(34, 7), Ink, 16.f, false, 108.f, true);
        const FVector2D Meter = Resource + FVector2D(34, 32);
        const float Reserve = FMath::Clamp(float(Runtime.SP) / FMath::Max(1, Level.InitialSP), 0.f, 1.f);
        const FVector2D FillEnd = Meter + FVector2D(99.f * Reserve, 0);
        P.Line(Meter, Meter + FVector2D(99, 0), Fade(Violet, Alpha * .48f), 1.6f);
        P.Line(Meter, FillEnd, Fade(GoldInk, .80f), 1.6f);
        if (Reserve > 0.f) P.Diamond(FillEnd, 2.5f, GoldInk);
        P.Line(Resource + FVector2D(147, 8), Resource + FVector2D(147, 33), Fade(GoldInk, .23f));
        const FVector2D Hourglass = Resource + FVector2D(165, 19);
        P.Line(Hourglass + FVector2D(-4, -6), Hourglass + FVector2D(4, -6), GoldInk);
        P.Line(Hourglass + FVector2D(-4, 6), Hourglass + FVector2D(4, 6), GoldInk);
        P.Line(Hourglass + FVector2D(-3, -5), Hourglass + FVector2D(3, 5), Fade(GoldInk, .8f));
        P.Line(Hourglass + FVector2D(3, -5), Hourglass + FVector2D(-3, 5), Fade(GoldInk, .8f));
        P.Text(FString::Printf(TEXT("回合  %02d"), Runtime.Turn), Resource + FVector2D(177, 8), Ink, 15.f, false, 85.f);

        const int32 WindTurns = Runtime.PlayerWindTurns;
        const FString Position = WindTurns > 0 ? FString::Printf(TEXT("乘风  Z%d · 剩 %d 回合"), Runtime.PlayerPosition.Z, WindTurns) :
            FString::Printf(TEXT("角色  Z%d"), Runtime.PlayerPosition.Z);
        const FLinearColor PositionInk = Fade(WindTurns > 0 ? Mint : Muted, Alpha);
        P.Diamond(At + FVector2D(31, 140), 5.f, Fade(WindTurns > 0 ? Mint : Violet, Alpha));
        P.Text(Position, At + FVector2D(44, 130), PositionInk, 14.f, false, 234.f);
        return HitSlant(Mouse, At, Extent, Skew);
    }

    bool DrawGuidePanel(const FHUDPainter& P, FVector2D Size, FVector2D Mouse, FStringView Title,
        TConstArrayView<FString> Lines, FLinearColor StatusColor, double Age, FPose Pose)
    {
        const bool bExpanded = !Lines.IsEmpty();
        const float LineSpacing = FMath::Min(22.f, 166.f / FMath::Max(1, Lines.Num()));
        const FVector2D Extent(bExpanded ? 400 : 390, bExpanded ? 57.f + Lines.Num() * LineSpacing : 49.f);
        const FVector2D At(Size.X - Extent.X - 24. + (1.f - Pose.Reveal) * 22., 54);
        constexpr float Skew = 36.f;
        const float Alpha = Pose.Opacity * Pose.Reveal;
        const FLinearColor Accent = StatusColor == Warning ? Warning : Gold;
        P.Frame(EPart::Context, EFrame::Guide, At, Extent, Skew, Accent, Pose);
        const FVector2D RailTop = At + FVector2D(48, 10);
        const FVector2D RailBottom = RailTop + FVector2D(-18, 31);
        P.Line(RailTop, RailBottom, Fade(Accent, Alpha * .18f), 8.f);
        P.Line(RailTop, RailBottom, Fade(Accent, Alpha * .87f), 2.f);
        const float TitleAlpha = Alpha * (.70f + .30f * Ease(Age, .20));
        P.Text(Title, At + FVector2D(61, bExpanded ? 12 : 14), Fade(bExpanded ? Paper : StatusColor, TitleAlpha),
            bExpanded ? 19.f : 16.f, false, float(Extent.X) - 89.f, bExpanded);
        if (bExpanded)
        {
            P.Line(At + FVector2D(61, 40), At + FVector2D(Extent.X - 30, 40), Fade(Accent, Alpha * .43f));
            for (int32 Index = 0; Index < Lines.Num(); ++Index)
            {
                const float Y = 49.f + LineSpacing * Index;
                const float RowAlpha = Alpha * Ease(Age - .07 - .039 * Index, .22);
                P.Diamond(At + FVector2D(60, Y + 8), 2.2f, Fade(Accent, RowAlpha * .68f));
                P.Text(Lines[Index], At + FVector2D(72, Y), Fade(Paper, RowAlpha * .83f),
                    FMath::Min(14.f, LineSpacing * .78f), false, float(Extent.X) - 100.f);
            }
        }
        return HitSlant(Mouse, At, Extent, Skew);
    }

    void DrawEmblem(const FHUDPainter& P, FVector2D Center, float Opacity, float Order)
    {
        const FVector2D Cube[] = {{0, -70}, {55, -40}, {55, 23}, {0, 55}, {-55, 23}, {-55, -40}, {0, -8}};
        const int32 Edges[][2] = {{0,1}, {1,2}, {2,3}, {3,4}, {4,5}, {5,0}, {5,6}, {1,6}, {3,6}};
        for (const auto& Edge : Edges)
        {
            const FVector2D Scatter = Cube[Edge[0]] * (1.f - Order) * .69;
            const FVector2D A = Center + Cube[Edge[0]] + Scatter;
            const FVector2D B = Center + Cube[Edge[1]] + Scatter;
            P.Line(A, B, Fade(Gold, .045f * Opacity), 13.f);
            P.Line(A, B, Fade(Gold, .11f * Opacity), 6.f);
            P.Line(A, B, Fade(Gold, .32f * Opacity), 3.f);
            P.Line(A, B, Fade(Paper, .94f * Opacity), 1.2f);
        }
        P.Line(Center + FVector2D(0, -134), Center + FVector2D(0, 87), Fade(Gold, .57f * Opacity), 1.25f);
        P.Diamond(Center + FVector2D(0, -8), 9.f, Fade(Gold, Opacity));
        P.Diamond(Center + FVector2D(0, 82), 5.f, Fade(Paper, Opacity));
    }

    void DrawLaurels(const FHUDPainter& P, FVector2D Center, float Radius, float Alpha)
    {
        for (float Side : {-1.f, 1.f})
        {
            FVector2D Previous = Center + FVector2D(Side * (Radius - 23), 26);
            for (int32 Index = 0; Index < 7; ++Index)
            {
                const float V = Index / 6.f;
                const FVector2D At = Center + FVector2D(Side * (Radius + 10 * FMath::Sin(V * PI) - 19 * FMath::Square(1.f - V)), 23 - 62 * V);
                P.Line(Previous, At, Fade(Gold, Alpha * .69f), 1.1f);
                const FVector2D Leaf(Side * (Index % 2 ? 10. : -8.), -13.);
                P.Line(At, At + Leaf * .8, Fade(Gold, Alpha * .84f), 3.4f);
                P.Line(At + Leaf * .5, At + Leaf, Fade(Paper, Alpha * .77f));
                Previous = At;
            }
        }
    }

    const TCHAR* BoxOperation(ERuneAction Skill)
    {
        switch (Skill)
        {
        case ERuneAction::Pulse: return TEXT("向前一格");
        case ERuneAction::Levitate: return TEXT("向上一格");
        case ERuneAction::WaterAnchor: return TEXT("锚定高度");
        case ERuneAction::Hiss: return TEXT("允许重叠");
        default: return TEXT("暂无操作");
        }
    }

    TPair<FString, FString> SkillEffect(ERuneAction Skill)
    {
        switch (Skill)
        {
        case ERuneAction::Pulse:
            return {TEXT("长按左键持续命中，沿视线将箱子推一格。"),
                TEXT("可从斜侧推。竖推需要风托或水锚。")};
        case ERuneAction::Levitate:
            return {FString::Printf(TEXT("左击抬升附近箱子一层，风托 %d 回合。"), FPuzzleSolver::LevitationDuration),
                FString::Printf(TEXT("对脚下施法可免费升一层，乘风 %d 回合。"), FPuzzleSolver::PlayerWindDuration)};
        case ERuneAction::WaterAnchor:
            return {TEXT("左击投出水球，命中后落锚并锁定箱高。"), TEXT("可向空地试投，不耗 SP。")};
        case ERuneAction::Hiss:
            return {TEXT("左击将箱子或障碍物转为 Hiss，允许叠合。"), TEXT("Tab 切换同格目标。")};
        default: return {};
        }
    }

    void DrawSkillTooltip(const FHUDPainter& P, const FPuzzleLevelDefinition& Level, int32 Index,
        FVector2D CardTop, FVector2D View, float Opacity, bool bHasIcon)
    {
        const auto& Card = RuneSkills::Ordered[Index];
        const FLinearColor Accent = SkillColors[Index];
        const FVector2D Size(365, 144);
        const FVector2D At(FMath::Clamp(CardTop.X - 115., 18., View.X - Size.X - 18.),
            CardTop.Y - Size.Y - 20. + (1.f - Opacity) * 8.);
        P.Panel(EPart::SkillTooltip, EPanel::Tooltip, At, Size, 10, Accent, {Opacity, .62f + .38f * Opacity, .54f});

        const FVector2D Joint(CardTop.X, At.Y + Size.Y - 1.);
        const FVector2D Left = Joint - FVector2D(10, 0), Right = Joint + FVector2D(11, 0);
        P.Triangle(Left, Right, CardTop, Fade({.034f, .031f, .077f, .96f}, Opacity));
        P.Line(Left, CardTop, Fade(Accent, Opacity * .74f));
        P.Line(CardTop, Right, Fade(Accent, Opacity * .74f));
        if (bHasIcon)
            P.Quad(EPart::TooltipIcon, EMode::Icon, At + FVector2D(16, 3), {72, 72}, 0, Accent,
                {Opacity, 1.f, .6f, 1.f, float(Index)});
        const bool bLocked = !RuneSkills::IsUnlocked(Level, Card.Action);
        P.Text(Card.Name, At + FVector2D(91, 15), Fade(Paper, Opacity), 20.f, false, 241.f, true);
        P.Text(FString::Printf(TEXT("%d SP%s"), FPuzzleSolver::Cost(Card.Action), bLocked ? TEXT(" · 本关未开放") : TEXT("")),
            At + FVector2D(92, 43), Fade(bLocked ? Muted : Accent, Opacity), 14.f);
        P.Line(At + FVector2D(19, 74), At + FVector2D(343, 74), Fade(Accent, Opacity * .45f));
        const auto Effect = SkillEffect(Card.Action);
        P.Text(Effect.Key, At + FVector2D(21, 85), Fade(Paper, Opacity), 16.f, false, 323.f);
        P.Text(Effect.Value, At + FVector2D(21, 111), Fade(Muted, Opacity), 15.f, false, 323.f);
    }

    void DrawCellTooltip(const FHUDPainter& P, const FPuzzleLevelDefinition& Level, FIntVector Cell,
        FVector2D Mouse, FVector2D View, float Opacity)
    {
        const auto* Goal = Level.Goals.FindByPredicate([&](const FRuneGoalData& Entry) { return Entry.GridPosition == Cell; });
        const auto* Terrain = Level.CellAt(Cell);
        const FString Label = Goal ? FString::Printf(TEXT("阵眼 #%d%s%s"), Goal->GoalId,
            Goal->bHissPair ? TEXT(" · Hiss 叠合") : TEXT(""), Goal->bRequireWaterAnchor ? TEXT(" · 水锚") : TEXT("")) :
            FString(Terrain && Terrain->bBlocked ? TEXT("障碍物") : TEXT("高台"));
        const FVector2D Size(178, 60);
        const FVector2D At(FMath::Clamp(Mouse.X + 14., 12., View.X - Size.X - 12.),
            FMath::Clamp(Mouse.Y - Size.Y - 12., 12., View.Y - Size.Y - 12.));
        P.Panel(EPart::CellTooltip, EPanel::Tooltip, At, Size, 8, Gold, {Opacity});
        P.Text(Label, At + FVector2D(18, 8), Fade(Gold, Opacity), 15.f, false, 143.f, true);
        P.Text(FString::Printf(TEXT("X %d · Y %d · Z %d"), Cell.X, Cell.Y, Cell.Z),
            At + FVector2D(18, 32), Fade(Paper, Opacity), 14.f, false, 143.f);
    }
}

AShengLuHUD::AShengLuHUD()
{
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Style(TEXT("/Game/UI/ResonantRunes/M_RuneHUD"));
    HUDMaterial = Style.Object;
    static ConstructorHelpers::FObjectFinder<UTexture2D> Icons(TEXT("/Game/UI/ResonantRunes/T_SkillIcons"));
    SkillAtlas = Icons.Object;
}

void AShengLuHUD::BeginPlay()
{
    Super::BeginPlay();
    auto* PC = GetOwningPlayerController();
    if (!PC || !PC->IsLocalController()) return;

    CursorTexture = LoadRuneCursor(CursorHotspot);
    if (!CursorTexture)
    {
        UE_LOG(LogTemp, Warning, TEXT("Could not load PRTS_Default.cur as a software cursor. Using the system cursor."));
        return;
    }
    PreviousMouseCursor = PC->CurrentMouseCursor.GetValue();
    PC->CurrentMouseCursor = EMouseCursor::None;
}

void AShengLuHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (auto* PC = GetOwningPlayerController(); PC && PreviousMouseCursor && PC->CurrentMouseCursor == EMouseCursor::None)
        PC->CurrentMouseCursor = *PreviousMouseCursor;
    Super::EndPlay(EndPlayReason);
}

void AShengLuHUD::PostRender()
{
    Super::PostRender();
    const auto* PC = GetOwningPlayerController();
    FVector2D Mouse;
    if (!CursorTexture || !CursorTexture->GetResource() || !Canvas || !PC || !PC->ShouldShowMouseCursor() ||
        !PC->GetMousePosition(Mouse.X, Mouse.Y) || Mouse.X < 0 || Mouse.Y < 0 ||
        Mouse.X >= Canvas->SizeX || Mouse.Y >= Canvas->SizeY)
        return;

    auto* Client = GetWorld()->GetGameViewport();
    auto* Viewport = Client ? Client->Viewport : nullptr;
    // Render-only viewports have no platform cursor to hand back to when unfocused.
    static const bool bOffscreen = FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen"));
    if (!Viewport || (!bOffscreen && Client->GetCursor(Viewport, int32(Mouse.X), int32(Mouse.Y)) != EMouseCursor::None))
        return;

    FCanvasTileItem Cursor(Mouse - FVector2D(CursorHotspot), CursorTexture->GetResource(), FLinearColor::White);
    Cursor.BlendMode = SE_BLEND_Translucent;
    Canvas->DrawItem(Cursor);
}

bool AShengLuHUD::FButton::Contains(FVector2D Point) const
{
    return HitSlant(Point, Position, Size, Skew);
}

void AShengLuHUD::FMotion::Update(AShengLuBoard& NewBoard, double Now, FStringView NewOperation, bool bNewCanOperate)
{
    if (Board.Get() != &NewBoard)
    {
        *this = FMotion();
        Board = &NewBoard;
        EnteredAt = Now;
        SP = NewBoard.Runtime().SP;
        Operation = NewOperation;
        bCanOperate = bNewCanOperate;
    }
    if (Selection != NewBoard.Selection())
    {
        Selection = NewBoard.Selection();
        EquippedAt = Now;
    }
    if (FStringView(Operation) != NewOperation || bCanOperate != bNewCanOperate)
    {
        Operation = NewOperation;
        bCanOperate = bNewCanOperate;
        OperationAt = Now;
    }
    if (NewBoard.Runtime().SP < SP) SpentAt = Now;
    else if (NewBoard.Runtime().SP > SP) SpentAt = -10.;
    SP = NewBoard.Runtime().SP;
    if (NewBoard.IsShowingResult())
    {
        if (ResultAt < 0.) ResultAt = Now;
    }
    else
    {
        ResultAt = ExitAt = -1.;
        PendingAction.Reset();
    }
}

float AShengLuHUD::FMotion::ResultVisibility(double Now) const
{
    const float In = Ease(Now - ResultAt, .46);
    return In * (ExitAt < 0. ? 1.f : 1.f - Ease(Now - ExitAt, ResultExitSeconds));
}

bool AShengLuHUD::RespondToResult(ERuneHUDAction Action)
{
    const auto* PC = GetOwningPlayerController();
    auto* Game = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
    if (!Game || !Game->IsShowingResult()) return false;
    if (Motion.PendingAction || (Action == ERuneHUDAction::Inspect && !Game->ShouldShowCampaignVictory())) return true;
    const double Now = GetWorld()->GetRealTimeSeconds();
    Motion.Update(*Game, Now, Motion.Board.Get() == Game ? FStringView(Motion.Operation) : FStringView(), Motion.bCanOperate);
    Motion.PendingAction = Action;
    Motion.ExitAt = Now;
    return true;
}

void AShengLuHUD::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Motion.PendingAction || GetWorld()->GetRealTimeSeconds() - Motion.ExitAt < ResultExitSeconds) return;
    auto* Board = Motion.Board.Get();
    const auto Action = *Motion.PendingAction;
    Motion.PendingAction.Reset();
    Motion.ExitAt = -1.;
    const auto* PC = GetOwningPlayerController();
    if (!Board || !PC || Board != PC->GetPawn() || !Board->IsShowingResult()) return;
    switch (Action)
    {
    case ERuneHUDAction::Accept:
        if (Board->ShouldShowCampaignVictory()) Board->FinishCampaign();
        else Board->AdvanceLevel();
        break;
    case ERuneHUDAction::Retry: Board->ResetPuzzle(); break;
    case ERuneHUDAction::Undo: Board->Undo(); break;
    case ERuneHUDAction::Inspect: Board->DismissCampaignVictory(); break;
    default: break;
    }
}

void AShengLuHUD::DrawHUD()
{
    Super::DrawHUD();
    Buttons.Reset();
    auto* PC = GetOwningPlayerController();
    auto* Game = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
    const UFont* Font = GEngine->GetMediumFont();
    if (!Game || !Canvas || !Font || Canvas->SizeX <= 0 || Canvas->SizeY <= 0) return;
    if (HUDMaterial && Materials.IsEmpty())
    {
        static const FName ArtParameter(TEXT("Art"));
        for (int32 Index = 0; Index < int32(EPart::Count); ++Index)
        {
            auto* Material = UMaterialInstanceDynamic::Create(HUDMaterial, this);
            const EPart Part = EPart(Index);
            if (SkillAtlas && ((Part >= EPart::Skill1 && Part <= EPart::Skill4) ||
                Part == EPart::TooltipIcon || Part == EPart::OperationIcon))
                Material->SetTextureParameterValue(ArtParameter, SkillAtlas.Get());
            Materials.Add(Material);
        }
    }

    const auto& Level = Game->Level();
    const auto& Runtime = Game->Runtime();
    const bool Legacy = Game->UsesLegacyPresentation();
    const bool FirstPerson = Game->IsFirstPerson();
    const bool Restored = Game->IsRestored();
    const bool Won = Restored && !Game->IsAnimating();
    const bool FullVictory = Game->ShouldShowCampaignVictory();
    const bool ShowingResult = FullVictory || Game->CanAdvanceLevel();
    bResultButtons = ShowingResult;
    const bool SelfWind = Game->AimedAtSelf();
    const bool Obstacle = Game->AimedAtObstacle();
    const FRuneBoxState* Target = Runtime.FindBox(Game->AimBoxId());
    const ERuneAction Skill = Game->Selection().Get(ERuneAction::Pulse);
    const int32 Slot = RuneSkills::Slot(Skill);
    TOptional<FPuzzleActionResult> Preview;
    if (FirstPerson && Game->Selection() && Game->Phase() != ECastPhase::End && !Restored)
        Preview = Game->Preview();
    const bool bOperating = FirstPerson && Preview && !Game->IsAnimating() && (Target || SelfWind || Obstacle ||
        !Runtime.GetBoxesAt(Game->AimCell()).IsEmpty());
    const TCHAR* Operation = bOperating ? (SelfWind ? TEXT("乘风上升") : BoxOperation(Skill)) : TEXT("暂无操作");
    const bool ValidOperation = bOperating && Preview->IsValid();
    const double Now = GetWorld()->GetRealTimeSeconds();
    Motion.Update(*Game, Now, Operation, ValidOperation);

    const float Scale = FMath::Min(Canvas->SizeX / 1280.f, Canvas->SizeY / 720.f);
    const FVector2D Size = FVector2D(Canvas->SizeX, Canvas->SizeY) / Scale;
    const FVector2D Center = Size * .5;
    const FHUDPainter P{*Canvas, Materials, *Font, Scale, float(FMath::Fmod(Now, 3600.))};
    const float Entry = Ease(Now - Motion.EnteredAt, .52);
    const float ResultOpacity = ShowingResult ? Motion.ResultVisibility(Now) : 0.f;
    const float HUDOpacity = 1.f - ResultOpacity;
    FVector2D MousePixels;
    const bool bPointer = PC->bShowMouseCursor && PC->GetMousePosition(MousePixels.X, MousePixels.Y) &&
        MousePixels.X >= 0 && MousePixels.X < Canvas->SizeX && MousePixels.Y >= 0 && MousePixels.Y < Canvas->SizeY;
    const FVector2D Mouse = bPointer ? MousePixels / Scale : FVector2D(-1, -1);

    const auto Button = [&](TOptional<ERuneHUDAction> Action, EPart Part, EPanel Style, FStringView Label, FStringView Key,
        FVector2D At, FVector2D Extent, float Skew, FLinearColor Accent, float Opacity, float Reveal,
        bool bInteractive, bool bEnabled = true, bool bActive = false)
    {
        const bool bClickable = bInteractive && bEnabled && Action.IsSet();
        if (bClickable) Buttons.Add({Action.GetValue(), At * Scale, Extent * Scale, Skew * Scale});
        const bool bFocused = bActive || (bClickable && HitSlant(Mouse, At, Extent, Skew));
        const bool Primary = Style == EPanel::Primary && HUDMaterial;
        const FLinearColor Ink = Primary ? Dark : bEnabled ? Paper : Muted;
        P.Panel(Part, Style, At, Extent, Skew, Accent, {Opacity * (bEnabled ? 1.f : .59f), Reveal, bFocused ? 1.f : 0.f});
        const FVector2D Mid = At + Extent * .5;
        const float TextOpacity = Opacity * Ease(Reveal - .36f, .54f);
        if (Style == EPanel::Utility)
        {
            P.Text(Key, Mid + FVector2D(-22, 0), Fade(Gold, TextOpacity * .81f), 12.f, true);
            P.Text(Label, Mid + FVector2D(7, 0), Fade(Ink, TextOpacity), 14.f, true);
        }
        else
        {
            const float KeyWidth = Key.Len() > 1 ? 42.f : 19.f;
            P.Diamond({At.X + Skew * .5 + 23, Mid.Y}, 5.f, Fade(Primary ? Dark : Gold, TextOpacity * .72f));
            P.Text(Label, {Mid.X - KeyWidth * .25 - 4., Mid.Y}, Fade(Ink, TextOpacity),
                Primary ? 25.f : 17.f, true, Extent.X - KeyWidth - 70, Primary);
            P.Key(Key, {At.X + Extent.X - Skew * .5 - 26 - KeyWidth * .5, Mid.Y}, KeyWidth,
                Fade(Ink, TextOpacity), Fade(Primary ? Dark : Accent, TextOpacity * .7f));
        }
    };

    if (HUDOpacity > .025f)
    {
        DrawPeriphery(P, Size, HUDOpacity, Entry, FirstPerson);

        const FString LevelName = Legacy ? FString(TEXT("02   /   升律")) : Level.DisplayName.ToString();
        bool bPointerOverPanel = DrawLevelPanel(P, *Game, LevelName, Mouse,
            {HUDOpacity, Entry, 0.f, Progress(Now - Motion.SpentAt, .66)});

        const float OpMotion = Ease(Now - Motion.OperationAt, .30);
        const float OpEntry = Ease(Now - Motion.EnteredAt - .10, .46);
        const FLinearColor OpColor = bOperating ? (ValidOperation ? SkillColors[Slot - 1] : Warning) : Violet;
        const float OpAlpha = HUDOpacity * OpEntry;
        const float OpPulse = Progress(Now - Motion.OperationAt, .48);
        const FVector2D Op(38 - (1.f - OpEntry) * 20, Size.Y * .326);
        const FVector2D OpBody = Op + FVector2D((1.f - OpMotion) * 9, 30);
        const FVector2D OpTab = Op - FVector2D((1.f - OpMotion) * 8, 0);
        const FVector2D OpBodySize(251, 105), OpTabSize(213, 35);
        bPointerOverPanel |= HitSlant(Mouse, OpBody, OpBodySize, 27) || HitSlant(Mouse, OpTab, OpTabSize, 20);
        P.Frame(EPart::Operation, EFrame::Operation, OpBody, OpBodySize, 27, OpColor,
            {HUDOpacity, OpEntry, bOperating ? .78f : .18f, OpPulse});
        P.Frame(EPart::OperationTab, EFrame::OperationTab, OpTab, OpTabSize, 20, Gold,
            {HUDOpacity, OpEntry, .28f, OpPulse});
        P.Line(OpTab + FVector2D(27, 23), OpTab + FVector2D(34, 11), Fade(Gold, OpAlpha), 1.5f);
        P.Text(TEXT("当前操作"), OpTab + FVector2D(45, 8), Fade(Paper, OpAlpha), 16.f, false, 112.f, true);
        const FVector2D Sigil = OpBody + FVector2D(46, 48);
        const FLinearColor SigilInk = Fade(OpColor, OpAlpha * (bOperating ? .76f : .59f));
        P.Diamond(Sigil, 19.f, SigilInk);
        if (bOperating && SkillAtlas)
        {
            P.Quad(EPart::OperationIcon, EMode::Icon, Sigil - FVector2D(18, 18), {36, 36}, 0.f, OpColor,
                {OpAlpha, 1.f, .64f, OpPulse, float(Slot - 1), ValidOperation ? 0.f : .55f});
        }
        else
        {
            P.Diamond(Sigil, 9.f, Fade(SigilInk, .67f));
            P.Diamond(Sigil, 2.5f, Fade(Paper, OpAlpha * .56f));
        }
        P.Line(OpBody + FVector2D(77, 25), OpBody + FVector2D(77, 76), Fade(SigilInk, .55f));
        P.Text(Operation, OpBody + FVector2D(90 + (1.f - OpMotion) * 6, 31),
            Fade(bOperating ? OpColor : Muted, OpAlpha * (.68f + .32f * OpMotion)),
            bOperating ? 21.f : 17.f, false, 136.f, bOperating);
        const bool Failed = !Game->IsAnimating() && !Won && !FPuzzleSolver::CanContinue(Level, Runtime);
        if (FirstPerson)
        {
            Motion.ContextTitle.Reset();
            FString Feedback;
            FLinearColor FeedbackColor = Muted;
            const bool Beam = Skill == ERuneAction::Pulse && Game->IsFiring() && !Won;
            if (!Game->Message().IsEmpty()) { Feedback = Game->Message(); FeedbackColor = Warning; }
            else if (Failed) { Feedback = TEXT("行动耗尽 · Z 撤销 · R 重试"); FeedbackColor = Warning; }
            else if (Won && Game->CanFinishCampaign()) Feedback = TEXT("Enter 返回结算");
            else if (Won) Feedback = TEXT("阵列已复原");
            else if (Game->IsAnimating()) Feedback = TEXT("行动结算中");
            else if (bOperating && !ValidOperation) { Feedback = Preview->Reason; FeedbackColor = Warning; }
            else if (Skill == ERuneAction::WaterAnchor && !Target && Preview && Game->AimCell().X >= 0)
                Feedback = TEXT("空地试投 · 不耗 SP");
            if (Beam)
            {
                const float Charge = FMath::Clamp(Game->BeamProgress(), 0.f, 1.f);
                if (Feedback.IsEmpty()) Feedback = FString::Printf(TEXT("持续命中  %.1f / %.1f s"),
                    AShengLuBoard::PulseHitSeconds * Charge, AShengLuBoard::PulseHitSeconds);
                const FVector2D Meter = OpBody + FVector2D(36, 93);
                P.Line(Meter, Meter + FVector2D(186, 0), Fade(Violet, OpAlpha * .58f), 1.4f);
                P.Line(Meter, Meter + FVector2D(186 * Charge, 0), Fade(Gold, OpAlpha), 2.5f);
            }
            P.Text(Feedback, OpBody + FVector2D(36, 73), Fade(FeedbackColor, OpAlpha), 13.5f, false, 189.f);
        }
        else
        {
            FLinearColor StatusColor = Muted;
            FString ContextTitle;
            TArray<FString> ContextLines;
            if (!Game->Message().IsEmpty()) { ContextTitle = Game->Message(); StatusColor = Warning; }
            else if (Won)
            {
                ContextTitle = FString::Printf(TEXT("阵列已复原 · 消耗 %d SP%s"), Level.InitialSP - Runtime.SP, Legacy ? TEXT(" / 参考 6 SP") : TEXT(""));
                StatusColor = Gold;
                if (Game->CanFinishCampaign() && !ShowingResult) ContextLines.Add(TEXT("Enter 返回结算"));
            }
            else if (Failed) { ContextTitle = TEXT("行动耗尽 · 可撤销一步或重试"); StatusColor = Warning; }
            else if (Game->IsAnimating()) ContextTitle = TEXT("行动结算中");
            else if (Runtime.Turn == 0 && !Level.EntryTip.IsEmptyOrWhitespace() && Game->GetGameTimeSinceCreation() < 12.f)
            {
                ContextTitle = TEXT("进入关卡 · ") + LevelName;
                Level.EntryTip.ToString().ParseIntoArrayLines(ContextLines);
            }
            else if (Game->IsPlayerLevitating()) ContextTitle = Game->IsPawnSelected() ?
                TEXT("点击亮起的同层格落脚 · G 解除浮空") : TEXT("乘风中 · 点击角色或按空格选落脚点");
            else ContextTitle = Game->IsPawnSelected() ?
                TEXT("点击亮起的相邻格移动 · WASD 转向") : TEXT("点击角色所在格或按空格进入移动模式");

            if (Motion.ContextTitle != ContextTitle)
            {
                Motion.ContextTitle = ContextTitle;
                Motion.ContextAt = Now;
            }
            const double Age = Now - Motion.ContextAt;
            const float ContextEntry = Ease(Now - Motion.EnteredAt - .15, .38);
            bPointerOverPanel |= DrawGuidePanel(P, Size, Mouse, ContextTitle, ContextLines, StatusColor, Age,
                {HUDOpacity, ContextEntry, 0.f, Progress(Age, .52)});
        }

        constexpr int32 SkillCount = RuneSkills::Count;
        constexpr double SkillStep = 198.;
        constexpr double UtilityInset = 8.;
        constexpr float UtilitySkew = 12.f;
        const FVector2D UtilitySize(80, 38);
        const FVector2D UtilityStep(UtilitySize.X + 3, 0);
        const double UtilityWidth = UtilityInset + UtilityStep.X * 2 + UtilitySize.X;
        const double Start = Center.X - (SkillCount * SkillStep + UtilityWidth) * .5;
        const double Bottom = Size.Y - 112;
        int32 HoveredSkill = INDEX_NONE;
        FVector2D CardTop{};
        for (int32 Index = 0; Index < SkillCount; ++Index)
        {
            const auto& Card = RuneSkills::Ordered[Index];
            const bool Locked = !RuneSkills::IsUnlocked(Level, Card.Action);
            const bool Selected = Game->Selection() == Card.Action;
            const float Reveal = Ease(Now - Motion.EnteredAt - Index * .067, .44);
            const float MotionPulse = Selected ? Progress(Now - Motion.EquippedAt, .46) : 1.f;
            const float CastPulse = Game->ConfirmedSkillSlot() == Index + 1 ? Progress(Now - Game->SkillConfirmedTime(), .65) : 1.f;
            const FVector2D At(Start + Index * SkillStep - (1.f - Reveal) * 15, Bottom + (1.f - Reveal) * 22);
            const FVector2D Extent(199, 66);
            const bool Interactive = !ShowingResult && !FirstPerson && Reveal > .45f;
            const auto Action = ERuneHUDAction(int32(ERuneHUDAction::Skill1) + Index);
            const bool Hover = Interactive && HitSlant(Mouse, At, Extent, 28);
            if (Hover)
            {
                HoveredSkill = Index;
                CardTop = At + FVector2D(Extent.X * .5, 2);
            }
            if (Interactive) Buttons.Add({Action, At * Scale, Extent * Scale, 28 * Scale});
            const float Opacity = HUDOpacity * (Locked ? .83f : FirstPerson && !Selected ? .86f : 1.f);
            P.Quad(EPart(int32(EPart::Skill1) + Index), SkillAtlas ? EMode::Skill : EMode::Panel, At, Extent, 28, SkillColors[Index],
                {Opacity, Reveal, Selected ? 1.f : Hover ? .83f : .12f, FMath::Min(MotionPulse, CastPulse),
                    SkillAtlas ? float(Index) : 0.f, Locked ? .68f : 0.f});
            const float LabelOpacity = Opacity * Ease(Reveal - .24f, .76f);
            P.Key(FString::FromInt(Index + 1), At + FVector2D(82, 30), 19, Fade(Locked ? Muted : Paper, LabelOpacity),
                Fade(SkillColors[Index], LabelOpacity * (Locked ? .45f : .79f)));
            P.Text(Card.Name, At + FVector2D(99, 17), Fade(Locked ? Muted : Paper, LabelOpacity), 17.f, false, 86.f, Selected);
            P.Text(Locked ? FString(TEXT("未开放")) : FString::Printf(TEXT("%d SP"), FPuzzleSolver::Cost(Card.Action)),
                At + FVector2D(100, 41), Fade(Locked ? Muted : SkillColors[Index], LabelOpacity), 11.f);
        }
        const float UtilityEntry = Ease(Now - Motion.EnteredAt - .18, .42);
        const bool Utilities = !ShowingResult && !FirstPerson && UtilityEntry > .45f;
        const FVector2D UtilityStart(Start + SkillCount * SkillStep + UtilityInset, Bottom + 14);
        Button(ERuneHUDAction::Undo, EPart::Undo, EPanel::Utility, TEXT("撤销"), TEXT("Z"),
            UtilityStart, UtilitySize, UtilitySkew, Violet, HUDOpacity, UtilityEntry, Utilities, Game->HasHistory());
        Button(ERuneHUDAction::Retry, EPart::Retry, EPanel::Utility, TEXT("重试"), TEXT("R"),
            UtilityStart + UtilityStep, UtilitySize, UtilitySkew, Gold, HUDOpacity, UtilityEntry, Utilities);
        if (!ShowingResult)
        {
            const FVector2D DepthViewAt = UtilityStart + UtilityStep * 2;
            const bool bDepthViewActive = Game->IsDepthViewActive();
            Button({}, EPart::DepthView, EPanel::Utility, TEXT("透视"), TEXT("E"), DepthViewAt, UtilitySize,
                UtilitySkew, bDepthViewActive ? Gold : Violet, HUDOpacity, UtilityEntry, false, true, bDepthViewActive);
            bPointerOverPanel |= Utilities && HitSlant(Mouse, DepthViewAt, UtilitySize, UtilitySkew);
        }
        if (FirstPerson && !ShowingResult && !Game->ActiveContact())
            P.Diamond(Center, 3.f, Fade(Game->AimedAtBox() ? SkillColors[Slot - 1] : Muted, HUDOpacity * .85f));
        if (Motion.HoveredSkill != HoveredSkill)
        {
            Motion.HoveredSkill = HoveredSkill;
            Motion.HoveredAt = Now;
        }
        if (HoveredSkill != INDEX_NONE)
        {
            const float Alpha = Ease(Now - Motion.HoveredAt - .08, .16);
            if (Alpha > 0.f) DrawSkillTooltip(P, Level, HoveredSkill, CardTop, Size, Alpha, SkillAtlas != nullptr);
        }
        else if (bPointer && !FirstPerson && !ShowingResult && !bPointerOverPanel &&
            !Buttons.ContainsByPredicate([&](const FButton& Item) { return Item.Contains(MousePixels); }))
        {
            if (const auto Cell = Game->InspectCellAt(MousePixels)) DrawCellTooltip(P, Level, *Cell, Mouse, Size, Entry);
        }
    }

    // Settlement is drawn over the active camera and consumes the pointer until it exits.
    if (!ShowingResult || ResultOpacity <= 0.f) return;
    const float Age = float(Now - Motion.ResultAt);
    const float Out = Motion.ExitAt < 0. ? 0.f : Progress(Now - Motion.ExitAt, ResultExitSeconds);
    const float Order = Ease(Age, 1.04) * (1.f - Out * .54f);
    const FLinearColor ResultColor = FullVictory ? Gold : Violet;
    P.Quad(EPart::ResultVeil, EMode::Veil, {}, Size, 0, Gold, {ResultOpacity, 1.f, 0.f, 1.f, 1.f});
    P.Quad(EPart::ResultOrbit, EMode::Dial, Center - FVector2D(584, 329), {1168, 658}, 0, ResultColor,
        {ResultOpacity * Order * .68f, 1.f, 0.f, 1.f, 3.f});

    const FShard Left[] = {
        {{-492, -338}, {-123, -223}, {-306, -130}, .94f, .95f},
        {{-551, -279}, {-414, -266}, {-466, -197}, .67f, .86f},
        {{-444, -205}, {-284, -123}, {-404, -141}, .34f, .64f},
        {{-483, -92}, {-281, -128}, {-420, 23}, .82f, .84f},
        {{-521, 84}, {-354, -38}, {-449, 63}, .55f, .80f},
        {{-522, 158}, {-360, 126}, {-467, 203}, .23f, .76f},
        {{-353, 208}, {-184, 156}, {-305, 220}, .16f, .63f}
    };
    TArray<FShard, TInlineAllocator<14>> Wings;
    for (const auto& Shard : Left)
    {
        const float Art = Shard.Artwork * (FullVictory ? 1.f : .64f);
        Wings.Add({Shard.A, Shard.B, Shard.C, Art, Shard.Opacity});
        const FVector2D Mirror(-1, 1);
        Wings.Add({Shard.A * Mirror, Shard.B * Mirror, Shard.C * Mirror, Art, Shard.Opacity});
    }
    DrawGlass(P, EPart::ResultGlass, Wings, Center, ResultOpacity, Ease(Age, .7) * (1.f - Out * .67f),
        Age - Out * .55f, FullVictory ? .18f : .88f);

    const FVector2D Emblem = Center - FVector2D(0, 212 + (1.f - Order) * 30);
    P.Quad(EPart::ResultDial, EMode::Dial, Emblem - FVector2D(158, 158), {316, 316}, 0, ResultColor,
        {ResultOpacity * .76f, 1.f, 0.f, 1.f, 1.f});
    DrawEmblem(P, Emblem, ResultOpacity * Ease(Age - .12, .37), Order);
    const float HeadingReveal = Ease(Age - .24, .76) * (1.f - Out * .8f);
    const FVector2D Heading = Center - FVector2D(0, 87 + (1.f - Order) * 21);
    const FVector2D HeadingSize(FullVictory ? 419 : 462, FullVictory ? 108 : 119);
    P.Quad(EPart::Heading, EMode::Heading, Heading - HeadingSize * .5, HeadingSize, 0, Gold,
        {ResultOpacity, HeadingReveal, 0.f, 1.f, FullVictory ? 1.f : 0.f});
    if (!HUDMaterial)
        P.Text(FullVictory ? TEXT("完全胜利") : TEXT("阵列已复原"), Heading, Fade(Gold, ResultOpacity * HeadingReveal), 54.f, true, 440, true);
    DrawLaurels(P, Heading + FVector2D(0, 8), FullVictory ? 226.f : 240.f, ResultOpacity * HeadingReveal);

    const float ScoreIn = Ease(Age - .47, .35);
    const float DetailOpacity = ResultOpacity * ScoreIn;
    const FVector2D Divider = Center - FVector2D(0, 27);
    P.Line(Divider - FVector2D(328, 0), Divider - FVector2D(13, 0), Fade(Gold, DetailOpacity * .52f));
    P.Line(Divider + FVector2D(13, 0), Divider + FVector2D(328, 0), Fade(Gold, DetailOpacity * .52f));
    P.Diamond(Divider, 7.f, Fade(Paper, DetailOpacity));
    const FVector2D Score = Center + FVector2D(-293, -9 + (1.f - ScoreIn) * 30);
    P.Panel(EPart::Score, EPanel::Info, Score, {586, FullVictory ? 101. : 98.}, 29, ResultColor, {DetailOpacity, ScoreIn});
    if (FullVictory)
    {
        const FString Values[] = {FString::Printf(TEXT("%02d"), Runtime.Turn),
            FString::Printf(TEXT("%d / %d"), Level.InitialSP - Runtime.SP, Level.InitialSP),
            FString::Printf(TEXT("%d / %d"), Level.Goals.Num(), Level.Goals.Num())};
        const TCHAR* Labels[] = {TEXT("回合"), TEXT("SP 消耗"), TEXT("完成目标")};
        for (int32 Index = 0; Index < 3; ++Index)
        {
            const FVector2D At = Score + FVector2D(112 + Index * 181, 28);
            P.Text(Labels[Index], At, Fade(Muted, DetailOpacity), 15.f, true);
            P.Text(Values[Index], At + FVector2D(0, 36), Fade(Paper, DetailOpacity), 28.f, true, 153.f);
            if (Index < 2) P.Line(At + FVector2D(91, -8), At + FVector2D(91, 53), Fade(Gold, DetailOpacity * .19f));
        }
    }
    else if (const auto* Next = Game->NextPuzzleLevel())
    {
        TArray<FString, TInlineAllocator<RuneSkills::Count>> Unlocks;
        for (const auto& Ability : RuneSkills::Ordered)
            if (!RuneSkills::IsUnlocked(Level, Ability.Action) && RuneSkills::IsUnlocked(Next->Definition, Ability.Action))
                Unlocks.Add(Ability.Name);
        P.Text(TEXT("下一关 · ") + Next->Definition.DisplayName.ToString(), Score + FVector2D(293, 30),
            Fade(Paper, DetailOpacity), 22.f, true, 484.f);
        const FString Summary = Unlocks.IsEmpty() ? FString::Printf(TEXT("消耗 %d SP"), Level.InitialSP - Runtime.SP) :
            FString::Printf(TEXT("消耗 %d SP   ·   解锁 %s"), Level.InitialSP - Runtime.SP, *FString::Join(Unlocks, TEXT(" / ")));
        P.Text(Game->Message().IsEmpty() ? Summary : Game->Message(), Score + FVector2D(293, 65),
            Fade(Game->Message().IsEmpty() ? Muted : Warning, DetailOpacity), 15.f, true, 476.f);
    }

    const FVector2D Seal = Center + FVector2D(0, 142);
    P.Quad(EPart::Seal, EMode::Dial, Seal - FVector2D(203, 34), {406, 68}, 0, ResultColor,
        {DetailOpacity * .81f, 1.f, 0.f, 1.f, 2.f});
    P.Line(Seal - FVector2D(0, 49), Seal + FVector2D(0, 21), Fade(Gold, DetailOpacity * .18f), 8.f);
    P.Line(Seal - FVector2D(0, 49), Seal + FVector2D(0, 21), Fade(Paper, DetailOpacity * .79f), 1.35f);
    const float ButtonsIn = Ease(Age - .55, .40);
    const float ButtonsAlpha = ResultOpacity * ButtonsIn;
    const FVector2D Bar = Center + FVector2D(0, 197 + (1.f - ButtonsIn) * 28);
    const bool Clickable = !Motion.PendingAction && ButtonsIn > .45f;
    Button(ERuneHUDAction::Retry, EPart::ResultRetry, EPanel::Secondary, TEXT("重新挑战"), TEXT("R"),
        Bar + FVector2D(-360 - (1.f - ButtonsIn) * 42, 6), {224, 49}, 32, Gold, ButtonsAlpha, ButtonsIn, Clickable);
    Button(ERuneHUDAction::Accept, EPart::Accept, EPanel::Primary, FullVictory ? TEXT("结束游戏") : TEXT("下一关"), TEXT("Enter"),
        Bar - FVector2D(149, 0), {298, 61}, 40, Gold, ButtonsAlpha, ButtonsIn, Clickable);
    Button(FullVictory ? ERuneHUDAction::Inspect : ERuneHUDAction::Undo, EPart::ResultOther, EPanel::Secondary,
        FullVictory ? TEXT("查看棋盘") : TEXT("撤销一步"), FullVictory ? TEXT("Esc") : TEXT("Z"),
        Bar + FVector2D(136 + (1.f - ButtonsIn) * 42, 6), {224, 49}, 32, Violet, ButtonsAlpha, ButtonsIn, Clickable,
        FullVictory || Game->HasHistory());
    P.Line(Bar + FVector2D(0, 61), Bar + FVector2D(0, 88), Fade(Gold, ButtonsAlpha * .66f));
    P.Diamond(Bar + FVector2D(0, 92), 4.f, Fade(Paper, ButtonsAlpha));
}

bool AShengLuHUD::TryClick(FVector2D Mouse)
{
    const auto* PC = GetOwningPlayerController();
    auto* Game = PC ? Cast<AShengLuBoard>(PC->GetPawn()) : nullptr;
    if (!Game) return false;
    const bool bResult = Game->IsShowingResult();
    if (bResult != bResultButtons) return bResult;
    for (const FButton& Button : Buttons)
    {
        if (!Button.Contains(Mouse)) continue;
        if (bResult) return RespondToResult(Button.Action);
        const int32 SkillIndex = int32(Button.Action) - int32(ERuneHUDAction::Skill1);
        if (SkillIndex >= 0 && SkillIndex < RuneSkills::Count) Game->SelectSkill(RuneSkills::Ordered[SkillIndex].Action);
        else if (Button.Action == ERuneHUDAction::Undo) Game->Undo();
        else if (Button.Action == ERuneHUDAction::Retry) Game->ResetPuzzle();
        return true;
    }
    return bResult;
}
