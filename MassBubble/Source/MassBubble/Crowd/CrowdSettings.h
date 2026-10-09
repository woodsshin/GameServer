#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Crowd/CrowdTypes.h"
#include "CrowdSettings.generated.h"

/**
 * Designer-facing tunables (Project Settings > Game > MassBubble Crowd, stored in DefaultGame.ini).
 * Processors never touch this UObject directly: they read the POD FCrowdTuning copy (GetCrowdTuning()).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "MassBubble Crowd"))
class MASSBUBBLE_API UCrowdSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCrowdSettings();

	// ---- Regions ----
	/** Population region edge length. Keep equal to the World Partition runtime grid cell size. */
	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "1600"))
	float RegionSizeCm = 12800.f;

	/** Regions kept active around every viewer (Chebyshev radius). 2 => 5x5. */
	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "1", ClampMax = "8"))
	int32 ActiveRegionRadius = 2;

	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "1", ClampMax = "5000"))
	int32 AgentsPerRegion = 400;

	/** A region must be unwanted for this long before it is despawned (hysteresis against boundary flicker). */
	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "0"))
	float RegionDeactivateDelaySec = 10.f;

	/**
	 * Hard cap of agents created per frame.
	 * The wall clock budget (category "Streaming") normally stops earlier.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "1"))
	int32 MaxSpawnPerFrame = 500;

	/** Hard cap of agents destroyed per frame. The wall clock budget (category "Streaming") normally stops earlier. */
	UPROPERTY(Config, EditAnywhere, Category = "Regions", meta = (ClampMin = "1"))
	int32 MaxDespawnPerFrame = 1000;

	UPROPERTY(Config, EditAnywhere, Category = "Regions")
	int32 WorldSeed = 1337;

	// ---- LOD ----
	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "100"))
	float HighLODDistanceCm = 4000.f;

	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "100"))
	float MediumLODDistanceCm = 10000.f;

	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "100"))
	float LowLODDistanceCm = 20000.f;

	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "0"))
	float LODHysteresisCm = 500.f;

	/** Simulate every N-th frame (time slicing). 1 = every frame. */
	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "1"))
	int32 HighIntervalFrames = 1;

	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "1"))
	int32 MediumIntervalFrames = 2;

	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "1"))
	int32 LowIntervalFrames = 6;

	/** 0 = frozen while no viewer is within LowLODDistance. */
	UPROPERTY(Config, EditAnywhere, Category = "LOD", meta = (ClampMin = "0"))
	int32 OffIntervalFrames = 0;

	// ---- Movement (how an agent wanders: Crowd/CrowdWander.h) ----
	/** Agents walk at their own cruise speed, drawn from this range once per agent. */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "1"))
	float MinSpeedCmPerSec = 90.f;

	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "1"))
	float MaxSpeedCmPerSec = 180.f;

	/** A walking segment may be this much faster or slower than the agent's cruise speed (0.15 = +-15 %). */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0", ClampMax = "0.5"))
	float SpeedVariation = 0.15f;

	/** Length of one straight walking segment. Longer = fewer corners and fewer network updates. */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float MinRetargetSec = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float MaxRetargetSec = 3.5f;

	/** Largest heading change between two walking segments. 60 = gentle meandering, 180 = the old "pick any direction". */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0", ClampMax = "180"))
	float MaxTurnDeg = 60.f;

	/**
	 * Turn rate (deg/s) of an agent inside the band along its region border. The agent bends away on a circle of radius
	 * speed / rate. Lower = wider, smoother turns but a wider strip along the border where the crowd is thinner.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "10", ClampMax = "360"))
	float WallTurnRateDeg = 60.f;

	/** Chance that an average walking segment turns into a pause (0.12 = about one in eight). */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0", ClampMax = "0.95"))
	float IdleChance = 0.12f;

	/** Shortest and longest pause in seconds. Never below 0.1 s: that is the shortest segment the planner works with (CrowdWander.h). */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float MinIdleSec = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float MaxIdleSec = 5.f;

	/** Flat test world: all agents walk on this Z. */
	UPROPERTY(Config, EditAnywhere, Category = "Movement")
	float AgentGroundZ = 0.f;

	/** Upper bound for the accumulated delta of a time-sliced step (prevents jumps after long gaps). */
	UPROPERTY(Config, EditAnywhere, Category = "Movement", meta = (ClampMin = "0.05"))
	float MaxStepDeltaSec = 0.5f;

	// ---- Replication ----
	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "1", ClampMax = "60"))
	float ReplicationHz = 10.f;

	/** Per-player interest radius. Must fit the int16 offset range (see CrowdNetMath.h): <= 21000. */
	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "1000", ClampMax = "21000"))
	float BubbleRadiusCm = 15000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "1", ClampMax = "2000"))
	int32 MaxAgentsPerBubble = 400;

	/** Dead-reckoning tolerance for agents near the player. Smaller = smoother, more bandwidth. */
	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "10"))
	float NearErrorCm = 30.f;

	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "10"))
	float FarErrorCm = 150.f;

	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "100"))
	float NearDistanceCm = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "1"))
	float VelocityEpsCmPerSec = 25.f;

	/** Spatial grid cell edge inside a region. */
	UPROPERTY(Config, EditAnywhere, Category = "Replication", meta = (ClampMin = "400"))
	float GridCellSizeCm = 1600.f;

	// ---- Streaming / hitch protection ----
	/**
	 * Wall clock budget (ms per frame) for creating the agents of newly wanted regions. The clock is checked after
	 * every StructuralBatchSize agents; at least one batch is always created per frame.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0.05"))
	float SpawnBudgetMs = 1.0f;

	/** Same budget while World Partition cells are loading / being added to / removed from the world. */
	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0"))
	float SpawnBudgetBusyMs = 0.25f;

	/** Wall clock budget (ms per frame) for persisting and destroying the agents of unwanted regions. */
	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0.05"))
	float DespawnBudgetMs = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0"))
	float DespawnBudgetBusyMs = 0.25f;

	/** Agents per Mass create / destroy call. Smaller = finer time slicing, slightly more per-call overhead. */
	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "8", ClampMax = "2048"))
	int32 StructuralBatchSize = 64;

	/**
	 * Only regions that can reach into the interest circle of a REAL player (bubble radius + this margin) are
	 * snapshotted for replication. Regions that only bots look at cost nothing here.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0"))
	float SnapshotMarginCm = 2000.f;

	// ---- Load-test bots ----
	UPROPERTY(Config, EditAnywhere, Category = "Bots")
	float BotOrbitRadiusCm = 60000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Bots")
	float BotSpeedCmPerSec = 1500.f;

	/**
	 * Seconds between two bots appearing (and later disappearing). 0 = all at once, which makes World Partition
	 * load (or unload + GC) dozens of cells in a single frame.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Bots", meta = (ClampMin = "0"))
	float BotRampIntervalSec = 1.0f;
};

/** Console-variable switches. Each optimization can be turned off at runtime to measure its effect. */
namespace CrowdCVars
{
	extern MASSBUBBLE_API int32 Enable;        // opt.crowd.Enable        master switch for the simulation
	extern MASSBUBBLE_API int32 TimeSlicing;   // opt.crowd.TimeSlicing   0 = every agent every frame
	extern MASSBUBBLE_API int32 LOD;           // opt.crowd.LOD           0 = everybody is High LOD
	extern MASSBUBBLE_API int32 ParallelMove;  // opt.crowd.ParallelMove  1 = ParallelForEachEntityChunk
	extern MASSBUBBLE_API int32 Replicate;     // opt.crowd.Replicate     0 = bubbles stop updating
	extern MASSBUBBLE_API int32 DeadReckoning; // opt.crowd.DeadReckoning 0 = resend every moving agent each tick
	extern MASSBUBBLE_API float ReplicationHz; // opt.crowd.ReplicationHz > 0 overrides the setting

	extern MASSBUBBLE_API int32 BudgetedPump;        // opt.crowd.BudgetedPump      0 = fixed per-frame counts, no wall clock budget
	extern MASSBUBBLE_API int32 SnapshotScope;       // opt.crowd.SnapshotScope     0 = snapshot every live region
	extern MASSBUBBLE_API float BotRampIntervalSec;  // opt.crowd.BotRampSec        >= 0 overrides the setting, 0 = all bots at once
	extern MASSBUBBLE_API int32 BotActivateCells;    // opt.crowd.BotActivateCells  1 = bots stream cells as "Activated" (default: "Loaded")
}

/** Immutable-per-frame tuning snapshot. Safe to read from any thread; rebuilt only via ReloadCrowdTuning(). */
MASSBUBBLE_API const FCrowdTuning& GetCrowdTuning();

/** Seconds between two ramped bot operations: opt.crowd.BotRampSec if >= 0, otherwise the project setting. */
MASSBUBBLE_API float GetBotRampIntervalSec();

/**
 * Game-thread only, never while Mass is processing. Rebuilds the snapshot from UCrowdSettings (called at subsystem
 * init and by opt.crowd.Reload).
 * bKeepGeometry: keep RegionSizeCm / GridCellSizeCm / CellsPerSide of the previous snapshot. Live regions and their
 * grids were built with those values, so they can only change with a restart of the world (opt.crowd.Reload uses true).
 */
MASSBUBBLE_API void ReloadCrowdTuning(bool bKeepGeometry = false);
