using UnrealBuildTool;

// Editor build. Both modules are loaded so PIE can run "dedicated server + N clients" in one process.
public class MassBubbleEditorTarget : TargetRules
{
	public MassBubbleEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "MassBubble", "MassBubbleRender" });
	}
}
