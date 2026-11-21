#include "SPuzzleViewport.h"

#include "AssetEditorModeManager.h"
#include "Components/DirectionalLightComponent.h"
#include "EditorViewportClient.h"
#include "PreviewScene.h"
#include "PuzzleViewportMode.h"
#include "UnrealWidget.h"

namespace
{
    class FPuzzlePreviewClient final : public FEditorViewportClient
    {
    public:
        FPuzzlePreviewClient(const TSharedRef<SEditorViewport>& Widget, const TSharedRef<FPuzzleAuthoring>& Model,
            const TSharedRef<FPreviewScene>& InScene, const TSharedRef<FAssetEditorModeManager>& Manager)
            : FEditorViewportClient(&Manager.Get(), &InScene.Get(), Widget), Scene(InScene)
        {
            Manager->SetPreviewScene(&Scene.Get());
            Manager->SetDefaultMode(FPuzzleViewportMode::ModeId);
            Manager->ActivateMode(FPuzzleViewportMode::ModeId);
            Manager->SetCoordSystem(COORD_World);
            Manager->SetWidgetMode(UE::Widget::WM_Translate);
            PuzzleMode().Attach(Model, Scene.Get());
            ShowWidget(false);
            SetViewMode(VMI_Lit);
            EngineShowFlags.SetBillboardSprites(true);
            EngineShowFlags.SetGrid(false);
            EngineShowFlags.SetSnap(false);
            EngineShowFlags.SetFog(false);
            EngineShowFlags.SetAmbientOcclusion(false);
            ExposureSettings.bFixed = true;
            ExposureSettings.FixedEV100 = 0;
            SetRealtime(true);
        }

        ~FPuzzlePreviewClient() override
        {
            GetModeTools()->DestroyMode(FPuzzleViewportMode::ModeId);
            static_cast<FAssetEditorModeManager*>(GetModeTools())->SetPreviewScene(nullptr);
        }

        void Refresh(bool bFrame)
        {
            PuzzleMode().RefreshScene();
            ShowWidget(PuzzleMode().UsesTransformWidget());
            bNeedsFrame |= bFrame;
            Invalidate();
        }

        void SetTopView(bool bNewTopView)
        {
            bTopView = bNewTopView;
            Refresh(true);
        }

        void Tick(float DeltaSeconds) override
        {
            FEditorViewportClient::Tick(DeltaSeconds);
            if (bNeedsFrame && Viewport && Viewport->GetSizeXY().GetMin() > 0)
            {
                bNeedsFrame = false;
                PuzzleMode().FrameBoard(*this, bTopView);
            }
        }

        FLinearColor GetBackgroundColor() const override { return FLinearColor(.021f, .027f, .049f); }

        void TrackingStarted(const FInputEventState&, bool bIsDraggingWidget, bool) override
        {
            if (bIsDraggingWidget && Viewport) GetModeTools()->StartTracking(this, Viewport);
        }

        void TrackingStopped() override
        {
            if (Viewport && GetModeTools()->IsTracking()) GetModeTools()->EndTracking(this, Viewport);
        }

    private:
        FPuzzleViewportMode& PuzzleMode() const
        {
            return *GetModeTools()->GetActiveModeTyped<FPuzzleViewportMode>(FPuzzleViewportMode::ModeId);
        }

        TSharedRef<FPreviewScene> Scene;
        bool bNeedsFrame = true;
        bool bTopView = false;
    };
}

void SPuzzleViewport::Construct(const FArguments& Args)
{
    Model = Args._Model;
    SEditorViewport::Construct(SEditorViewport::FArguments());
}

TSharedRef<FEditorViewportClient> SPuzzleViewport::MakeEditorViewportClient()
{
    auto Scene = MakeShared<FPreviewScene>(FPreviewScene::ConstructionValues()
        .SetCreatePhysicsScene(false).SetTransactional(false).SetForceMipsResident(false)
        .SetLightRotation(FRotator(-46.f, -50.f, 0.f)).SetLightBrightness(2.35f).SetSkyBrightness(.8f));
    Scene->DirectionalLight->SetLightSourceAngle(7.f);
    Scene->DirectionalLight->SetForwardShadingPriority(1);
    auto* Fill = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    Fill->SetIntensity(1.15f);
    Fill->SetLightColor(FLinearColor(.76f, .83f, 1.f));
    Fill->SetCastShadows(false);
    Scene->AddComponent(Fill, FTransform(FRotator(-32.f, 65.f, 0.f)));
    auto ModeManager = MakeShared<FAssetEditorModeManager>();
    return MakeShared<FPuzzlePreviewClient>(SharedThis(this), Model.ToSharedRef(), Scene, ModeManager);
}

void SPuzzleViewport::Refresh(bool bFrame)
{
    static_cast<FPuzzlePreviewClient&>(*GetViewportClient()).Refresh(bFrame);
}

void SPuzzleViewport::SetTopView(bool bTopView)
{
    static_cast<FPuzzlePreviewClient&>(*GetViewportClient()).SetTopView(bTopView);
}

void SPuzzleViewport::OnFocusViewportToSelection()
{
    Refresh(true);
}
