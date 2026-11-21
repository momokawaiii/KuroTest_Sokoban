#pragma once

#include "EdMode.h"
#include "PuzzleAuthoring.h"

class FPreviewScene;
class UPrimitiveComponent;

class FPuzzleViewportMode : public FEdMode
{
public:
    static const FEditorModeID ModeId;
    static TOptional<FIntVector> OnLayer(const FPuzzleLevelDefinition& Level, int32 Layer, FVector Origin, FVector Direction);
    void Attach(TSharedRef<FPuzzleAuthoring> Authoring, FPreviewScene& Scene);
    void RefreshScene();
    void FrameBoard(FEditorViewportClient& Client, bool bTopView);
    void Exit() override;

    bool HandleClick(FEditorViewportClient* Client, HHitProxy* Hit, const FViewportClick& Click) override;
    bool MouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y) override;
    bool CapturedMouseMove(FEditorViewportClient* Client, FViewport* Viewport, int32 X, int32 Y) override;
    bool MouseLeave(FEditorViewportClient* Client, FViewport* Viewport) override;
    bool InputKey(FEditorViewportClient* Client, FViewport* Viewport, FKey Key, EInputEvent Event) override;
    bool InputAxis(FEditorViewportClient* Client, FViewport* Viewport, int32 ControllerId, FKey Key, float Delta, float DeltaTime) override;
    bool StartTracking(FEditorViewportClient* Client, FViewport* Viewport) override;
    bool InputDelta(FEditorViewportClient* Client, FViewport* Viewport, FVector& Drag, FRotator& Rotation, FVector& Scale) override;
    bool EndTracking(FEditorViewportClient* Client, FViewport* Viewport) override;
    bool UsesTransformWidget() const override;
    bool UsesTransformWidget(UE::Widget::EWidgetMode WidgetMode) const override;
    bool ShouldDrawWidget() const override { return UsesTransformWidget(); }
    bool ShowModeWidgets() const override { return true; }
    EAxisList::Type GetWidgetAxisToDraw(UE::Widget::EWidgetMode WidgetMode) const override;
    FVector GetWidgetLocation() const override;
    bool HandleDragDuplicate() override { return true; }

    void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
    void DrawHUD(FEditorViewportClient* Client, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;

private:
    struct FPreviewPiece
    {
        FPuzzleElement Element;
        FBox Bounds;
    };

    bool IsEditing() const;
    void UpdateCropRange(FEditorViewportClient& Client);
    void ClearScene();
    FPuzzleElement Pick(FVector Origin, FVector Direction) const;
    TSharedPtr<FPuzzleAuthoring> Model;
    FPreviewScene* Preview = nullptr;
    TArray<UPrimitiveComponent*> PreviewComponents;
    TArray<FPreviewPiece> Pieces;
    uint64 SceneRevision = MAX_uint64;
    int32 IsolatedLayer = INDEX_NONE;
    TOptional<FIntVector> Hover;
    TOptional<FIntPoint> CropAnchor;
    TOptional<FVector> DragWorld;
    FViewport* HoverViewport = nullptr;
    bool bCancelDrag = false;
};
