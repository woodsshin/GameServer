using UnrealBuildTool;

public class MassBubble : ModuleRules
{
	public MassBubble(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Lets us write #include "Crowd/CrowdTypes.h" from anywhere in the module.
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"DeveloperSettings",

			// Push Model / FastArraySerializer / DOREPLIFETIME_WITH_PARAMS_FAST live here.
			"NetCore",

			// IPluginManager: which plugin owns a loaded module (opt.boot.Modules 1, opt.diet.*, Core/MassBubbleDiet.cpp).
			"Projects",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Spawn plans: a PCG graph decides where the agents of a region start (Crowd/CrowdPCGPlanner.cpp).
			// The PCG plugin has to be enabled in the .uproject.
			"PCG",
		});

		// Iris replication: UE_WITH_IRIS and the IrisCore dependency.
		SetupIrisSupport(Target);
	}
}
