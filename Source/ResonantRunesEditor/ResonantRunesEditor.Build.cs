using UnrealBuildTool;

public class ResonantRunesEditor : ModuleRules
{
    public ResonantRunesEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore",
            "UnrealEd", "EditorFramework", "LevelEditor", "ToolMenus", "AssetRegistry", "AssetTools",
            "ContentBrowser", "GameplayAbilities", "GameplayTags", "ResonantRunes"
        });
    }
}
