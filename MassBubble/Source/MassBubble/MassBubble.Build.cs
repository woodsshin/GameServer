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

			// Mass. Public because our fragments / subsystem headers expose Mass types.
			"MassCore",   // UE 5.8+ only. Remove this line when building against 5.7 or older.
			"MassEntity",
		
			// IPluginManager: which plugin owns a loaded module (opt.boot.Modules 1, opt.diet.*, Core/MassBubbleDiet.cpp).
			"Projects",
        });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MassCommon",
			"MassSimulation",

			// Spawn plans: a PCG graph decides where the agents of a region start (Crowd/CrowdPCGPlanner.cpp).
			// The PCG plugin has to be enabled in the .uproject.
			"PCG",
		});

		SetupIrisSupport(Target);
	}
}
