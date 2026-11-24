using System.IO;
using UnrealBuildTool;

public class GaussianPointSplattingRuntime : ModuleRules
{
	public GaussianPointSplattingRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Renderer",
			"RenderCore",
			"RHI"
		});
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("AssetRegistry");
		}

		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"),
			Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private")
		});
	}
}
