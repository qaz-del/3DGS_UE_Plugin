using UnrealBuildTool;

public class GaussianPointSplattingEditor : ModuleRules
{
	public GaussianPointSplattingEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GaussianPointSplattingRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetRegistry",
			"Slate",
			"SlateCore"
		});
	}
}
