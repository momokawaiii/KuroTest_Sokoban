#include "Editor.h"
#include "EditorModeRegistry.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "PuzzleViewportMode.h"
#include "ShengLuGame.h"
#include "SPuzzleAuthoringPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "ResonantRunesEditor"

namespace
{
    const FName AuthoringTab(TEXT("ResonantRunesPuzzleAuthoring"));
}

class FResonantRunesEditorModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        FEditorModeRegistry::Get().RegisterMode<FPuzzleViewportMode>(FPuzzleViewportMode::ModeId, LOCTEXT("ViewportMode", "符文关卡"));
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FResonantRunesEditorModule::RegisterMenu));
        FEditorDelegates::PreBeginPIE.AddRaw(this, &FResonantRunesEditorModule::BeginPIE);
        FEditorDelegates::EndPIE.AddRaw(this, &FResonantRunesEditorModule::EndPIE);
        FEditorDelegates::CancelPIE.AddRaw(this, &FResonantRunesEditorModule::CancelPIE);
        OpenCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("Puzzle.OpenEditor"), TEXT("Open the puzzle editor tab."),
            FConsoleCommandDelegate::CreateRaw(this, &FResonantRunesEditorModule::OpenEditor));
        ExamplesCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("Puzzle.CreateExamples"),
            TEXT("Import ShengLu and author the Z1 example in the puzzle catalog."),
            FConsoleCommandDelegate::CreateLambda([this]
            {
                const auto Author = Model ? Model : MakeShared<FPuzzleAuthoring>();
                if (Author->ImportShengLu() && Author->CreateDemo() && Author->Save())
                {
                    UE_LOG(LogTemp, Display, TEXT("Puzzle examples saved: %s"), *Author->Asset()->GetPathName());
                }
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("Puzzle examples could not be saved: %s"), *Author->Status);
                }
            }));
    }

    void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FEditorDelegates::PreBeginPIE.RemoveAll(this);
        FEditorDelegates::EndPIE.RemoveAll(this);
        FEditorDelegates::CancelPIE.RemoveAll(this);
        if (OpenCommand) IConsoleManager::Get().UnregisterConsoleObject(OpenCommand);
        if (ExamplesCommand) IConsoleManager::Get().UnregisterConsoleObject(ExamplesCommand);
        if (const auto Tab = EditorTab.Pin())
        {
            Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback());
            Tab->RequestCloseTab();
        }
        Model.Reset();
        FEditorModeRegistry::Get().UnregisterMode(FPuzzleViewportMode::ModeId);
        AShengLuGameMode::SetPIELevel(nullptr);
    }

private:
    void RegisterMenu()
    {
        FToolMenuOwnerScoped Owner(this);
        auto* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
        auto& Section = Menu->AddSection(TEXT("PuzzleAuthoring"), FText(), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
        Section.AddMenuEntry(TEXT("ResonantRunesPuzzleAuthoring"), LOCTEXT("Menu", "符文关卡编辑器"),
            LOCTEXT("MenuHint", "在独立三维视口中编辑解谜关卡"), FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FResonantRunesEditorModule::OpenEditor)));
    }

    void OpenEditor()
    {
        if (const auto Tab = EditorTab.Pin())
        {
            Tab->ActivateInParent(ETabActivationCause::SetDirectly);
            if (auto Window = Tab->GetParentWindow()) Window->BringToFront();
            return;
        }
        Model = MakeShared<FPuzzleAuthoring>();
        Model->bPlaying = GEditor->PlayWorld != nullptr;
        auto Tab = SNew(SDockTab).TabRole(ETabRole::MajorTab).Label(LOCTEXT("Tab", "符文关卡编辑器"))
        [SNew(SPuzzleAuthoringPanel).Model(Model)];
        Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &FResonantRunesEditorModule::TabClosed));
        EditorTab = Tab;
        FGlobalTabmanager::Get()->InsertNewDocumentTab(TEXT("DockedToolkit"), AuthoringTab,
            FTabManager::FLiveTabSearch(TEXT("LevelEditor")), Tab);
    }

    void TabClosed(TSharedRef<SDockTab>)
    {
        EditorTab.Reset();
        Model.Reset();
    }

    void BeginPIE(bool)
    {
        if (Model) { Model->bPlaying = true; Model->Changed(); }
    }

    void EndPIE(bool) { CancelPIE(); }

    void CancelPIE()
    {
        AShengLuGameMode::SetPIELevel(nullptr);
        if (!Model) return;
        Model->bPlaying = false;
        Model->Changed();
    }

    TWeakPtr<SDockTab> EditorTab;
    TSharedPtr<FPuzzleAuthoring> Model;
    IConsoleObject* ExamplesCommand = nullptr;
    IConsoleObject* OpenCommand = nullptr;
};

IMPLEMENT_MODULE(FResonantRunesEditorModule, ResonantRunesEditor)

#undef LOCTEXT_NAMESPACE
