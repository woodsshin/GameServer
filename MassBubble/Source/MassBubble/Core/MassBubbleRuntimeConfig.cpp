#include "Core/MassBubbleRuntimeConfig.h"

#include "MassBubble.h"

#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace MassBubbleStreamCVars
{
	int32 ApplyProfile = 1;
	int32 QuietGC = 1;
	float GCQuietSec = 0.75f;
	float GCMaxDeferSec = 20.f;
	float GCMinSpacingSec = 5.f;
	float HitchLogMs = 0.f;

	static FAutoConsoleVariableRef CVarApplyProfile(
		TEXT("opt.stream.ApplyProfile"), ApplyProfile,
		TEXT("1: apply the World Partition / level streaming / GC CVar profile when the first world starts (see opt.stream.Dump). 0: leave the engine defaults alone."), ECVF_Default);

	static FAutoConsoleVariableRef CVarQuietGC(
		TEXT("opt.stream.QuietGC"), QuietGC,
		TEXT("1: after cells were unloaded, run the garbage collection when World Partition is quiet instead of right away (pair with s.ForceGCAfterLevelStreamedOut=0)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarGCQuietSec(
		TEXT("opt.stream.GCQuietSec"), GCQuietSec,
		TEXT("Seconds without any cell loading / being added / being removed that count as 'quiet'."), ECVF_Default);

	static FAutoConsoleVariableRef CVarGCMaxDeferSec(
		TEXT("opt.stream.GCMaxDeferSec"), GCMaxDeferSec,
		TEXT("Run the post-unload GC anyway after waiting this long for a quiet moment."), ECVF_Default);

	static FAutoConsoleVariableRef CVarGCMinSpacingSec(
		TEXT("opt.stream.GCMinSpacingSec"), GCMinSpacingSec,
		TEXT("Minimum time between two post-unload GCs."), ECVF_Default);

	static FAutoConsoleVariableRef CVarHitchLogMs(
		TEXT("opt.hitch.LogMs"), HitchLogMs,
		TEXT("> 0: log every frame that took longer than this many ms, with GC / World Partition / crowd state. 0 = off."), ECVF_Default);
}

namespace
{
	enum class EProfileScope : uint8
	{
		Both,
		ServerOnly,   // dedicated server process
		ClientOnly,
	};

	struct FProfileEntry
	{
		const TCHAR* Name;
		const TCHAR* Value;
		EProfileScope Scope;
		const TCHAR* Why;
	};

	/**
	 * Engine CVars that spread out World Partition / level streaming / GC work.
	 * Entries that do not exist in the running engine version are reported at startup and skipped.
	 */
	const FProfileEntry GStreamingProfile[] =
	{
		{ TEXT("s.ForceGCAfterLevelStreamedOut"), TEXT("0"), EProfileScope::Both,
			TEXT("the engine forces a full GC right after cells were unloaded - UMassBubbleStreamingMonitor runs it when World Partition is quiet") },

		{ TEXT("s.LevelStreamingActorsUpdateTimeLimit"), TEXT("3.0"), EProfileScope::Both,
			TEXT("ms per frame for adding a cell's actors to the world (AddToWorld)") },

		{ TEXT("s.PriorityLevelStreamingActorsUpdateExtraTime"), TEXT("2.0"), EProfileScope::Both,
			TEXT("extra ms per frame for cells that are marked as priority") },

		{ TEXT("s.LevelStreamingComponentsRegistrationGranularity"), TEXT("4"), EProfileScope::Both,
			TEXT("components registered between two clock checks: smaller = the time limit is overshot less") },

		{ TEXT("s.UnregisterComponentsTimeLimit"), TEXT("1.0"), EProfileScope::Both,
			TEXT("ms per frame for removing a cell's components (RemoveFromWorld)") },

		{ TEXT("s.LevelStreamingComponentsUnregistrationGranularity"), TEXT("2"), EProfileScope::Both,
			TEXT("components unregistered between two clock checks") },

		{ TEXT("wp.Runtime.BlockOnSlowStreaming"), TEXT("0"), EProfileScope::ServerOnly,
			TEXT("never freeze the server tick (= every player) while a cell is still loading") },

		{ TEXT("wp.Runtime.MaxLoadingLevelStreamingCells"), TEXT("2"), EProfileScope::ServerOnly,
			TEXT("cells loading at the same time: smaller bursts of PostLoad / registration work") },
	};

	bool IsInScope(EProfileScope Scope)
	{
		const bool bDedicatedServer = IsRunningDedicatedServer();
		return Scope == EProfileScope::Both
			|| (Scope == EProfileScope::ServerOnly && bDedicatedServer)
			|| (Scope == EProfileScope::ClientOnly && !bDedicatedServer);
	}

	bool SameNumber(const FString& Current, const TCHAR* Wanted)
	{
		return FMath::IsNearlyEqual(FCString::Atof(*Current), FCString::Atof(Wanted), 1.0e-4f);
	}

	const TCHAR* ScopeName(EProfileScope Scope)
	{
		switch (Scope)
		{
		case EProfileScope::ServerOnly: return TEXT("server");
		case EProfileScope::ClientOnly: return TEXT("client");
		default:                        return TEXT("both");
		}
	}
}

int32 MassBubbleConfig::ApplyStreamingProfile(bool bLog)
{
	int32 InEffect = 0;

	for (const FProfileEntry& Entry : GStreamingProfile)
	{
		if (!IsInScope(Entry.Scope))
		{
			continue;
		}

		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Entry.Name);
		if (Var == nullptr)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("[Stream] console variable '%s' does not exist in this engine version - entry skipped (renamed? run 'opt.stream.Dump')."), Entry.Name);
			continue;
		}

		if ((static_cast<uint32>(Var->GetFlags()) & static_cast<uint32>(ECVF_ReadOnly)) != 0u)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("[Stream] '%s' is read-only - put it into DefaultEngine.ini [SystemSettings] instead."), Entry.Name);
			continue;
		}

		const FString Before = Var->GetString();
		Var->Set(Entry.Value, ECVF_SetByProjectSetting);
		const FString After = Var->GetString();

		if (SameNumber(After, Entry.Value))
		{
			++InEffect;
			if (bLog)
			{
				UE_LOG(LogMassBubble, Log, TEXT("[Stream] %s = %s (was %s) - %s"), Entry.Name, *After, *Before, Entry.Why);
			}
		}
		else
		{
			UE_LOG(LogMassBubble, Warning, TEXT("[Stream] %s stays %s: a higher priority source (ini / command line / console) owns it. The profile wants %s."), Entry.Name, *After, Entry.Value);
		}
	}

	return InEffect;
}

void MassBubbleConfig::ApplyStreamingProfileOnce()
{
	static bool bDone = false;
	if (bDone || MassBubbleStreamCVars::ApplyProfile == 0)
	{
		return;
	}
	bDone = true;

	const int32 InEffect = ApplyStreamingProfile(/*bLog=*/true);
	UE_LOG(LogMassBubble, Log, TEXT("[Stream] streaming profile applied (%d entries in effect). Details: opt.stream.Dump"), InEffect);
}

void MassBubbleConfig::DumpStreamingProfile()
{
	UE_LOG(LogMassBubble, Log, TEXT("[Stream] profile (dedicated server = %s, opt.stream.ApplyProfile=%d):"),
		IsRunningDedicatedServer() ? TEXT("yes") : TEXT("no"), MassBubbleStreamCVars::ApplyProfile);

	for (const FProfileEntry& Entry : GStreamingProfile)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Entry.Name);
		if (Var == nullptr)
		{
			UE_LOG(LogMassBubble, Log, TEXT("[Stream]   %s | MISSING in this engine version | scope: %s"), Entry.Name, ScopeName(Entry.Scope));
			continue;
		}

		const FString Current = Var->GetString();
		const TCHAR* State = !IsInScope(Entry.Scope) ? TEXT("not used by this process")
			: (SameNumber(Current, Entry.Value) ? TEXT("profile value active") : TEXT("DIFFERENT from profile"));

		UE_LOG(LogMassBubble, Log, TEXT("[Stream]   %s | now=%s | profile=%s | scope=%s | %s"),
			Entry.Name, *Current, Entry.Value, ScopeName(Entry.Scope), State);
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Stream] quiet GC: opt.stream.QuietGC=%d (quiet %.2f s, max wait %.1f s, min spacing %.1f s) | hitch log: opt.hitch.LogMs=%.1f"),
		MassBubbleStreamCVars::QuietGC, MassBubbleStreamCVars::GCQuietSec, MassBubbleStreamCVars::GCMaxDeferSec,
		MassBubbleStreamCVars::GCMinSpacingSec, MassBubbleStreamCVars::HitchLogMs);
}

FString MassBubbleConfig::GetReplicationModeString()
{
	int32 Requested = 0;
	bool bFromCommandLine = false;

	if (FParse::Value(FCommandLine::Get(), TEXT("UseIrisReplication="), Requested))
	{
		bFromCommandLine = true;
	}
	else if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("net.Iris.UseIrisReplication")))
	{
		Requested = Var->GetInt();
	}

	return FString::Printf(TEXT("%s (%s)"),
		Requested != 0 ? TEXT("Iris") : TEXT("legacy"),
		bFromCommandLine ? TEXT("command line") : TEXT("net.Iris.UseIrisReplication"));
}

void MassBubbleConfig::DumpNetConfig()
{
	const auto CVarValue = [](const TCHAR* Name) -> FString
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var != nullptr ? Var->GetString() : FString(TEXT("<missing>"));
	};

	UE_LOG(LogMassBubble, Log, TEXT("[Net] replication system this process asked for: %s"), *GetReplicationModeString());
	UE_LOG(LogMassBubble, Log, TEXT("[Net] net.Iris.UseIrisReplication=%s"), *CVarValue(TEXT("net.Iris.UseIrisReplication")));
	UE_LOG(LogMassBubble, Log, TEXT("[Net] net.IsPushModelEnabled=%s (the crowd bubble is push based: needs 1)"), *CVarValue(TEXT("net.IsPushModelEnabled")));
	UE_LOG(LogMassBubble, Log, TEXT("[Net] net.SubObjects.DefaultUseSubObjectReplicationList=%s (Iris needs 1)"), *CVarValue(TEXT("net.SubObjects.DefaultUseSubObjectReplicationList")));
	UE_LOG(LogMassBubble, Log, TEXT("[Net] To confirm that Iris really replicates the bubble: console 'Net.Iris.PrintPushBasedStatuses' (CrowdBubble must show PushBased: 1) and the LogIris lines at startup."));
}

namespace
{
	FAutoConsoleCommand GCmdStreamApply(
		TEXT("opt.stream.Apply"),
		TEXT("Apply the World Partition / level streaming / GC CVar profile now (values owned by ini / command line / console are kept)."),
		FConsoleCommandDelegate::CreateLambda([]() { MassBubbleConfig::ApplyStreamingProfile(/*bLog=*/true); }));

	FAutoConsoleCommand GCmdStreamDump(
		TEXT("opt.stream.Dump"),
		TEXT("Log every entry of the streaming CVar profile next to the value that is currently active."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleConfig::DumpStreamingProfile));

	FAutoConsoleCommand GCmdNetInfo(
		TEXT("opt.net.Info"),
		TEXT("Log which replication system this process uses (Iris / legacy) and the CVars it depends on."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleConfig::DumpNetConfig));
}
