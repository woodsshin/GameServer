// Console commands for load tests and measurements.
// The A/B switches are console variables declared in CrowdSettings.cpp:
//   opt.crowd.Enable / TimeSlicing / LOD / ParallelMove / Replicate / DeadReckoning / ReplicationHz
// Run the commands on the machine that owns the simulation: dedicated server console / -ExecCmds,
// or the editor console when playing as listen server / standalone.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdSubsystem.h"
#include "MassBubble.h"
#include "World/MassBubbleStreamingAnchor.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/** A command can arrive without a world (e.g. -ExecCmds right after startup): fall back to the first game world. */
	UWorld* ResolveWorld(UWorld* InWorld)
	{
		if (InWorld != nullptr)
		{
			return InWorld;
		}
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
				{
					return Context.World();
				}
			}
		}
		return nullptr;
	}

	void CmdStats(const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = ResolveWorld(InWorld);
		UCrowdSubsystem* Crowd = (World != nullptr) ? World->GetSubsystem<UCrowdSubsystem>() : nullptr;
		if (Crowd == nullptr)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("opt.crowd.Stats: no crowd subsystem in this world (pure client, or not a game world)."));
			return;
		}

		Crowd->DumpStats();

		const FCrowdTuning& T = GetCrowdTuning();
		UE_LOG(LogMassBubble, Log, TEXT("[Crowd] switches: Enable=%d TimeSlicing=%d LOD=%d ParallelMove=%d Replicate=%d DeadReckoning=%d ReplicationHz=%.1f (0 = from settings: %.1f)"),
			CrowdCVars::Enable, CrowdCVars::TimeSlicing, CrowdCVars::LOD, CrowdCVars::ParallelMove,
			CrowdCVars::Replicate, CrowdCVars::DeadReckoning, CrowdCVars::ReplicationHz, T.ReplicationHz);
		UE_LOG(LogMassBubble, Log, TEXT("[Crowd] geometry: region=%.0f cm, active radius=%d, agents/region=%d, replication radius=%.0f cm"),
			T.RegionSizeCm, T.ActiveRegionRadius, T.AgentsPerRegion, T.ReplicationRadiusCm);
	}

	void CmdSpawnBots(const TArray<FString>& Args, UWorld* InWorld)
	{
		const int32 Count = Args.Num() > 0 ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 64) : 4;
		AMassBubbleStreamingAnchor::SpawnBots(ResolveWorld(InWorld), Count);
	}

	void CmdClearBots(const TArray<FString>& Args, UWorld* InWorld)
	{
		AMassBubbleStreamingAnchor::ClearBots(ResolveWorld(InWorld));
	}

	void CmdReload(const TArray<FString>& Args)
	{
		ReloadCrowdTuning(/*bKeepGeometry=*/true);

		const FCrowdTuning& T = GetCrowdTuning();
		const UCrowdSettings* Settings = GetDefault<UCrowdSettings>();
		if (!FMath::IsNearlyEqual(Settings->RegionSizeCm, T.RegionSizeCm))
		{
			UE_LOG(LogMassBubble, Warning, TEXT("opt.crowd.Reload: RegionSizeCm changed in the settings. Live regions were built with the old value, so it is kept (%.0f). Restart the world to apply."),
				T.RegionSizeCm);
		}
		UE_LOG(LogMassBubble, Log, TEXT("opt.crowd.Reload: tuning reloaded."));
	}

	FAutoConsoleCommandWithWorldAndArgs GCmdStats(
		TEXT("opt.crowd.Stats"),
		TEXT("Log region / LOD / viewer counters and the current A/B switches of the crowd subsystem."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CmdStats));

	FAutoConsoleCommandWithWorldAndArgs GCmdSpawnBots(
		TEXT("opt.crowd.SpawnBots"),
		TEXT("opt.crowd.SpawnBots [N=4]  Spawn N server-side streaming anchors that circle the map (World Partition + crowd load test)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CmdSpawnBots));

	FAutoConsoleCommandWithWorldAndArgs GCmdClearBots(
		TEXT("opt.crowd.ClearBots"),
		TEXT("Remove all anchors created by opt.crowd.SpawnBots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CmdClearBots));

	FAutoConsoleCommand GCmdReload(
		TEXT("opt.crowd.Reload"),
		TEXT("Re-read Project Settings > MassBubble Crowd. The region size is kept until the world restarts."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdReload));
}

#endif // !UE_BUILD_SHIPPING
