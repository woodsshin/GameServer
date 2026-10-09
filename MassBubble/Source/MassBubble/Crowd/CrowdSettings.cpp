#include "Crowd/CrowdSettings.h"

#include "Crowd/CrowdWander.h"
#include "Net/CrowdNetMath.h"

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

	static FAutoConsoleVariableRef CVarEnable(
		TEXT("opt.crowd.Enable"), Enable,
		TEXT("Master switch for the Mass crowd simulation (server/standalone)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarTimeSlicing(
		TEXT("opt.crowd.TimeSlicing"), TimeSlicing,
		TEXT("1: simulate agents every N-th frame according to their LOD. 0: simulate every agent every frame."), ECVF_Default);

	static FAutoConsoleVariableRef CVarLOD(
		TEXT("opt.crowd.LOD"), LOD,
		TEXT("1: distance based LOD. 0: force every agent to High LOD."), ECVF_Default);

	static FAutoConsoleVariableRef CVarParallelMove(
		TEXT("opt.crowd.ParallelMove"), ParallelMove,
		TEXT("1: movement processor uses ParallelForEachEntityChunk. 0: single threaded ForEachEntityChunk."), ECVF_Default);

	static FAutoConsoleVariableRef CVarReplicate(
		TEXT("opt.crowd.Replicate"), Replicate,
		TEXT("1: per-player crowd bubbles replicate. 0: bubbles stop updating (isolates simulation cost)."), ECVF_Default);

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
		TEXT("1: only regions that a real player can see are snapshotted for replication. 0: every live region (old behaviour)."), ECVF_Default);

	static FAutoConsoleVariableRef CVarBotRampSec(
		TEXT("opt.crowd.BotRampSec"), BotRampIntervalSec,
		TEXT("Seconds between two load-test bots appearing / disappearing. 0 = all at once (the burst). < 0 = use the project setting."), ECVF_Default);

	static FAutoConsoleVariableRef CVarBotActivateCells(
		TEXT("opt.crowd.BotActivateCells"), BotActivateCells,
		TEXT("Bots created afterwards: 1 = their World Partition source ACTIVATES cells (BeginPlay, tick, registration: like a player). 0 = only LOADS them (cheap, enough for the crowd)."), ECVF_Default);
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
		// int16 offsets in 1 cm units cover +-327 m. The viewer may be up to CrowdNet::RebaseDistanceCm (110 m) away
		// from the bubble origin, which leaves 217 m for the radius; CrowdNet::MaxBubbleRadiusCm (210 m) keeps a margin.
		Out.BubbleRadiusCm = FMath::Clamp(S.BubbleRadiusCm, 1000.f, CrowdNet::MaxBubbleRadiusCm);
		Out.MaxAgentsPerBubble = FMath::Clamp(S.MaxAgentsPerBubble, 1, 2000);
		Out.NearErrorCm = FMath::Max(S.NearErrorCm, 10.f);
		Out.FarErrorCm = FMath::Max(S.FarErrorCm, Out.NearErrorCm);
		Out.NearDistanceCm = FMath::Max(S.NearDistanceCm, 100.f);
		Out.VelocityEpsCmPerSec = FMath::Max(S.VelocityEpsCmPerSec, 1.f);
		Out.GridCellSizeCm = FMath::Max(S.GridCellSizeCm, 400.f);

		Out.CellsPerSide = FMath::Max(1, FMath::CeilToInt(Out.RegionSizeCm / Out.GridCellSizeCm));
		ClampWallTurnToRegion(Out);

		// Streaming / hitch protection. The "busy" budgets may be 0: one batch per frame is always created / destroyed.
		Out.SpawnBudgetMs = FMath::Max(S.SpawnBudgetMs, 0.05f);
		Out.SpawnBudgetBusyMs = FMath::Clamp(S.SpawnBudgetBusyMs, 0.f, Out.SpawnBudgetMs);
		Out.DespawnBudgetMs = FMath::Max(S.DespawnBudgetMs, 0.05f);
		Out.DespawnBudgetBusyMs = FMath::Clamp(S.DespawnBudgetBusyMs, 0.f, Out.DespawnBudgetMs);
		Out.StructuralBatchSize = FMath::Clamp(S.StructuralBatchSize, 8, 2048);
		Out.SnapshotMarginCm = FMath::Max(S.SnapshotMarginCm, 0.f);

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
		GCrowdTuning.GridCellSizeCm = Previous.GridCellSizeCm;
		GCrowdTuning.CellsPerSide = Previous.CellsPerSide;
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
