using UnrealBuildTool;

// Client-only build for connecting to a dedicated server (requires a source-built engine).
public class MassBubbleClientTarget : TargetRules
{
	public MassBubbleClientTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Client;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "MassBubble", "MassBubbleRender" });
	}
}
