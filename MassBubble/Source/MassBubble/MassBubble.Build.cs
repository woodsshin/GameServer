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
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MassCommon",
			"MassSimulation",
		});

		SetupIrisSupport(Target);
	}
}
