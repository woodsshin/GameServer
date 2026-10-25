using UnrealBuildTool;

// Standalone / listen-server game build (server + client logic in one process).
public class MassBubbleTarget : TargetRules
{
	public MassBubbleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "MassBubble", "MassBubbleRender" });
	}
}
