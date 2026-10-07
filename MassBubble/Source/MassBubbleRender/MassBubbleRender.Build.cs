using UnrealBuildTool;

// ClientOnly module: never compiled into / loaded by the dedicated server.
public class MassBubbleRender : ModuleRules
{
	public MassBubbleRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"MassBubble",
		});

        SetupIrisSupport(Target);
    }
}
