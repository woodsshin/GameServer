#include "Crowd/CrowdSettings.h"

#include "Crowd/CrowdWander.h"

#include "HAL/IConsoleManager.h"

namespace CrowdCVars
{
	int32 Enable = 1;
	int32 TimeSlicing = 1;
	int32 LOD = 1;
	int32 ParallelMove = 1;
	int32 Replicate = 1;
	int32 DeadReckoning = 1;
	float ReplicationHz = 0.f;

	int32 BudgetedPump = 1;
	int32 SnapshotScope = 1;
	float BotRampIntervalSec = -1.f;
	int32 BotActivateCells = 1;
	int32 PCG = 1;
	int32 LODPrepass = 1;
	int32 TrimRegions = 1;

	static FAutoConsoleVariableRef CVarEnable(
		TEXT("opt.crowd.Enable"), Enable,
		TEXT("Master switch for the crowd simulation (server/standalone)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarTimeSlicing(
		TEXT("opt.crowd.TimeSlicing"), TimeSlicing,
		TEXT("1: simulate agents every N-th frame according to their LOD. 0: simulate every agent every frame."), ECVF_Default);

	static FAutoConsoleVariableRef CVarLOD(
		TEXT("opt.crowd.LOD"), LOD,
		TEXT("1: distance based LOD. 0: force every agent to High LOD."), ECVF_Default);

	static FAutoConsoleVariableRef CVarParallelMove(
		TEXT("opt.crowd.ParallelMove"), ParallelMove,
		TEXT("1: LOD and movement run one task per region on worker threads. 0: single threaded."), ECVF_Default);

	static FAutoConsoleVariableRef CVarReplicate(
		TEXT("opt.crowd.Replicate"), Replicate,
		TEXT("1: the region replicators publish the state of their agents. 0: they stop updating (isolates simulation cost)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarDeadReckoning(
		TEXT("opt.crowd.DeadReckoning"), DeadReckoning,
		TEXT("1: only resend an agent when the client's extrapolation drifts beyond tolerance. 0: resend every tick."), ECVF_Default);

	static FAutoConsoleVariableRef CVarReplicationHz(
		TEXT("opt.crowd.ReplicationHz"), ReplicationHz,
		TEXT("If > 0 overrides the Replication Hz from the project settings."), ECVF_Default);

	static FAutoConsoleVariableRef CVarBudgetedPump(
		TEXT("opt.crowd.BudgetedPump"), BudgetedPump,
		TEXT("1: region spawn / despawn is limited by a wall clock budget per frame (and shrinks while World Partition streams). 0: old fixed per-frame counts (reproduces the hitch)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarSnapshotScope(
		TEXT("opt.crowd.SnapshotScope"), SnapshotScope,
		TEXT("1: only regions that a real player can reach get a replicator. 0: every active region gets one."), ECVF_Default);

	static FAutoConsoleVariableRef CVarBotRampSec(
		TEXT("opt.crowd.BotRampSec"), BotRampIntervalSec,
		TEXT("Seconds between two load-test bots appearing / disappearing. 0 = all at once (the burst). < 0 = use the project setting."), ECVF_Default);

	static FAutoConsoleVariableRef CVarBotActivateCells(
		TEXT("opt.crowd.BotActivateCells"), BotActivateCells,
		TEXT("Bots created afterwards: 1 = their World Partition source ACTIVATES cells (BeginPlay, tick, registration: like a player). 0 = only LOADS them (cheap, enough for the crowd)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarPCG(
		TEXT("opt.crowd.PCG"), PCG,
		TEXT("1: a region that was never populated asks the PCG spawn graph where its agents start (needs a graph in the settings). 0: plain random placement. Regions that already have a population keep it."), ECVF_Default);

	static FAutoConsoleVariableRef CVarLODPrepass(
		TEXT("opt.crowd.LODPrepass"), LODPrepass,
		TEXT("1: LOD tiers are measured against the viewers that can be nearest to a region (per-region bounds, same tiers). 0: every agent measures the distance to every viewer."), ECVF_Default);

	static FAutoConsoleVariableRef CVarTrimRegions(
		TEXT("opt.crowd.TrimRegions"), TrimRegions,
		TEXT("1: a region frees what it does not use (saved agents while active, simulated agents while inactive) and reserves what it will fill. 0: it keeps the capacity."), ECVF_Default);
}

UCrowdSettings::UCrowdSettings()
{
	CategoryName = TEXT("Game");
}

namespace
{
	FCrowdTuning GCrowdTuning;
	bool GCrowdTuningReady = false;

	/**
	 * The band along a region border (CrowdWander::LayerDepthCm) must stay well inside the region: a slow turn rate with
	 * a small region would leave no open ground. Raises the turn rate until the band plus margin is at most 30 % of the
	 * region on each side. Called again after the geometry of the previous snapshot has been restored.
	 */
	void ClampWallTurnToRegion(FCrowdTuning& Out)
	{
		const float RoomCm = 0.3f * Out.RegionSizeCm - static_cast<float>(CrowdWander::WallMarginCm) - CrowdWander::LayerSlackCm;
		if (RoomCm > 1.f)
		{
			const float MinRateDeg = FMath::RadiansToDegrees(2.f * Out.MaxSpeedCmPerSec / RoomCm);
			Out.WallTurnRateDeg = FMath::Max(Out.WallTurnRateDeg, MinRateDeg);
		}
	}

	void BuildTuning(const UCrowdSettings& S, FCrowdTuning& Out)
	{
		Out.RegionSizeCm = FMath::Max(S.RegionSizeCm, 1600.f);
		Out.ActiveRegionRadius = FMath::Clamp(S.ActiveRegionRadius, 1, 8);
		Out.AgentsPerRegion = FMath::Clamp(S.AgentsPerRegion, 1, 5000);
		Out.RegionDeactivateDelaySec = FMath::Max(S.RegionDeactivateDelaySec, 0.f);
		Out.PersistTTLSec = FMath::Max(S.PersistTTLSec, 0.f);
		Out.MaxSpawnPerFrame = FMath::Max(S.MaxSpawnPerFrame, 1);
		Out.MaxDespawnPerFrame = FMath::Max(S.MaxDespawnPerFrame, 1);
		Out.WorldSeed = S.WorldSeed;

		// Keep the three LOD bounds strictly increasing.
		Out.LODDistanceCm[0] = FMath::Max(S.HighLODDistanceCm, 100.f);
		Out.LODDistanceCm[1] = FMath::Max(S.MediumLODDistanceCm, Out.LODDistanceCm[0] + 100.f);
		Out.LODDistanceCm[2] = FMath::Max(S.LowLODDistanceCm, Out.LODDistanceCm[1] + 100.f);
		Out.LODHysteresisCm = FMath::Max(S.LODHysteresisCm, 0.f);
		Out.LODIntervalFrames[0] = FMath::Max(S.HighIntervalFrames, 1);
		Out.LODIntervalFrames[1] = FMath::Max(S.MediumIntervalFrames, 1);
		Out.LODIntervalFrames[2] = FMath::Max(S.LowIntervalFrames, 1);
		Out.LODIntervalFrames[3] = FMath::Max(S.OffIntervalFrames, 0);

		Out.MinSpeedCmPerSec = FMath::Max(S.MinSpeedCmPerSec, 1.f);
		Out.MaxSpeedCmPerSec = FMath::Max(S.MaxSpeedCmPerSec, Out.MinSpeedCmPerSec);
		Out.SpeedVariation = FMath::Clamp(S.SpeedVariation, 0.f, 0.5f);
		Out.MinRetargetSec = FMath::Max(S.MinRetargetSec, 0.1f);
		Out.MaxRetargetSec = FMath::Max(S.MaxRetargetSec, Out.MinRetargetSec);
		Out.MaxTurnDeg = FMath::Clamp(S.MaxTurnDeg, 0.f, 180.f);
		Out.WallTurnRateDeg = FMath::Clamp(S.WallTurnRateDeg, 10.f, 360.f);
		Out.IdleChance = FMath::Clamp(S.IdleChance, 0.f, 0.95f);
		Out.MinIdleSec = FMath::Max(S.MinIdleSec, CrowdWander::MinSegmentSec); // the planner's shortest segment, see Advance()
		Out.MaxIdleSec = FMath::Max(S.MaxIdleSec, Out.MinIdleSec);
		Out.AgentGroundZ = S.AgentGroundZ;
		Out.MaxStepDeltaSec = FMath::Max(S.MaxStepDeltaSec, 0.05f);

		Out.ReplicationHz = FMath::Clamp(S.ReplicationHz, 1.f, 60.f);
		Out.ReplicationRadiusCm = FMath::Max(S.ReplicationRadiusCm, 1000.f);
		Out.NearErrorCm = FMath::Max(S.NearErrorCm, 10.f);
		Out.FarErrorCm = FMath::Max(S.FarErrorCm, Out.NearErrorCm);
		Out.VelocityEpsCmPerSec = FMath::Max(S.VelocityEpsCmPerSec, 1.f);
		Out.ReplicatorMarginCm = FMath::Max(S.ReplicatorMarginCm, 0.f);
		Out.ReplicatorLingerSec = FMath::Max(S.ReplicatorLingerSec, 0.f);
		Out.MaxReplicatorSpawnsPerFrame = FMath::Clamp(S.MaxReplicatorSpawnsPerFrame, 1, 16);

		ClampWallTurnToRegion(Out);

		// Streaming / hitch protection. The "busy" budgets may be 0: one batch per frame is always created / persisted.
		Out.SpawnBudgetMs = FMath::Max(S.SpawnBudgetMs, 0.05f);
		Out.SpawnBudgetBusyMs = FMath::Clamp(S.SpawnBudgetBusyMs, 0.f, Out.SpawnBudgetMs);
		Out.DespawnBudgetMs = FMath::Max(S.DespawnBudgetMs, 0.05f);
		Out.DespawnBudgetBusyMs = FMath::Clamp(S.DespawnBudgetBusyMs, 0.f, Out.DespawnBudgetMs);
		Out.StructuralBatchSize = FMath::Clamp(S.StructuralBatchSize, 8, 2048);

		Out.bUsePCG = S.bUsePCG;
		Out.PCGMaxConcurrent = FMath::Clamp(S.PCGMaxConcurrent, 1, 16);
		Out.PCGTimeoutSec = FMath::Max(S.PCGTimeoutSec, 1.f);
		Out.PCGBoundsHalfHeightCm = FMath::Max(S.PCGBoundsHalfHeightCm, 100.f);
		Out.bPCGUsePointYaw = S.bPCGUsePointYaw;
		Out.bPCGFallbackToRandom = S.bPCGFallbackToRandom;

		Out.BotOrbitRadiusCm = FMath::Max(S.BotOrbitRadiusCm, 1000.f);
		Out.BotSpeedCmPerSec = FMath::Max(S.BotSpeedCmPerSec, 0.f);
		Out.BotRampIntervalSec = FMath::Max(S.BotRampIntervalSec, 0.f);
	}
}

void ReloadCrowdTuning(bool bKeepGeometry)
{
	check(IsInGameThread());

	const FCrowdTuning Previous = GCrowdTuning;
	BuildTuning(*GetDefault<UCrowdSettings>(), GCrowdTuning);

	if (bKeepGeometry && GCrowdTuningReady)
	{
		GCrowdTuning.RegionSizeCm = Previous.RegionSizeCm;
		ClampWallTurnToRegion(GCrowdTuning);
	}
	GCrowdTuningReady = true;
}

const FCrowdTuning& GetCrowdTuning()
{
	if (!GCrowdTuningReady)
	{
		ReloadCrowdTuning();
	}
	return GCrowdTuning;
}

float GetBotRampIntervalSec()
{
	return CrowdCVars::BotRampIntervalSec >= 0.f ? CrowdCVars::BotRampIntervalSec : GetCrowdTuning().BotRampIntervalSec;
}
