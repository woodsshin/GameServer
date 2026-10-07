using UnrealBuildTool;

// Dedicated server build (requires a source-built engine).
// "MassBubbleRender" is intentionally not listed: it is a client-only module,
// so the server binary contains no rendering code.
public class MassBubbleServerTarget : TargetRules
{
	public MassBubbleServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("MassBubble");

		// Keep logs in Shipping servers: operators need them for cause analysis in live ops.
		bUseLoggingInShipping = true;
	}
}
