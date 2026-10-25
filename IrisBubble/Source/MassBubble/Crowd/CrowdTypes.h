#pragma once

#include "CoreMinimal.h"
#include "CrowdTypes.generated.h"

/** Simulation level of detail, decided per agent from the distance to the nearest viewer. */
UENUM()
enum class ECrowdLOD : uint8
{
	High   = 0,
	Medium = 1,
	Low    = 2,
	Off    = 3,
};

constexpr int32 CrowdLODCount = 4;

/** Compact record kept per inactive region so a region can be restored exactly when it streams back in. */
struct FCrowdSavedAgent
{
	uint32 NetId = 0;
	FVector2D Location = FVector2D::ZeroVector;
	FVector2f Velocity = FVector2f::ZeroVector;
	float RetargetTimer = 0.f;
	uint32 Rng = 1;
	float Heading = 0.f;      // radians, direction of the last walking segment
	float CruiseSpeed = 0.f;  // cm/s, the agent's own preferred walking speed
};

/** Plain-old-data snapshot of the settings. Read by the simulation (possibly off the game thread). */
struct FCrowdTuning
{
	// --- Regions (aligned with the World Partition runtime cell size, 128 m by default) ---
	float RegionSizeCm = 12800.f;
	int32 ActiveRegionRadius = 2;          // 2 => 5x5 regions around every viewer
	int32 AgentsPerRegion = 400;
	float RegionDeactivateDelaySec = 10.f;
	float PersistTTLSec = 0.f;             // how long an unwanted region keeps its persisted population, 0 = for ever
	int32 MaxSpawnPerFrame = 500;          // agents created per frame (hard cap, the wall clock budget below usually hits first)
	int32 MaxDespawnPerFrame = 1000;       // agents persisted per frame (hard cap)
	int32 WorldSeed = 1337;

	// --- LOD ---
	float LODDistanceCm[3] = { 4000.f, 10000.f, 20000.f }; // upper bound of High / Medium / Low
	float LODHysteresisCm = 500.f;
	int32 LODIntervalFrames[CrowdLODCount] = { 1, 2, 6, 0 }; // simulate every N frames, 0 = frozen

	// --- Movement (see Crowd/CrowdWander.h) ---
	float MinSpeedCmPerSec = 90.f;         // range of the agents' own cruise speeds
	float MaxSpeedCmPerSec = 180.f;
	float SpeedVariation = 0.15f;          // +-15 % around the cruise speed, per walking segment
	float MinRetargetSec = 1.5f;           // length of a walking segment
	float MaxRetargetSec = 3.5f;
	float MaxTurnDeg = 60.f;               // largest heading change between two walking segments
	float WallTurnRateDeg = 60.f;          // deg/s: how fast agents bend away inside the band along the region border
	float IdleChance = 0.12f;              // chance that an average walking segment turns into a pause (per mean segment length)
	float MinIdleSec = 1.5f;
	float MaxIdleSec = 5.f;
	float AgentGroundZ = 0.f;
	float MaxStepDeltaSec = 0.5f;

	// --- Replication (see Net/CrowdRegionReplicator.h) ---
	float ReplicationHz = 10.f;            // how often the replicated state of the regions is refreshed
	float ReplicationRadiusCm = 15000.f;   // a region is replicated to a player while any part of it lies within this distance
	float NearErrorCm = 30.f;              // dead-reckoning tolerance of agents in the High tier
	float FarErrorCm = 150.f;              // ... and of every other tier
	float VelocityEpsCmPerSec = 25.f;
	float ReplicatorMarginCm = 2000.f;     // a replicator is created this far before its region comes into the replication radius
	float ReplicatorLingerSec = 5.f;       // a replicator outlives the last player's reach by this long
	int32 MaxReplicatorSpawnsPerFrame = 2; // replicators created per frame

	// --- Streaming / hitch protection (see UCrowdSettings, category "Streaming") ---
	float SpawnBudgetMs = 1.0f;            // wall clock per frame for creating agents
	float SpawnBudgetBusyMs = 0.25f;       // ... while World Partition cells are loading / being added / removed
	float DespawnBudgetMs = 1.0f;          // wall clock per frame for persisting agents
	float DespawnBudgetBusyMs = 0.25f;
	int32 StructuralBatchSize = 64;        // agents per create / persist slice, the clock is checked between slices

	// --- PCG spawn plans (see Crowd/CrowdPCGPlanner.h, UCrowdSettings category "PCG") ---
	bool bUsePCG = true;                   // ask a PCG graph where the agents of a region that was never populated start
	int32 PCGMaxConcurrent = 2;            // graph runs in flight at the same time
	float PCGTimeoutSec = 20.f;            // a graph run that takes longer than this is given up
	float PCGBoundsHalfHeightCm = 10000.f; // generation bounds of a run: the region, this far above and below AgentGroundZ
	bool bPCGUsePointYaw = false;          // initial heading from the rotation of the PCG point (default: random per agent)
	bool bPCGFallbackToRandom = true;      // a failed run populates the region randomly instead of leaving it empty

	// --- Virtual viewers (server-side load-test bots) ---
	float BotOrbitRadiusCm = 60000.f;
	float BotSpeedCmPerSec = 1500.f;
	float BotRampIntervalSec = 1.0f;       // bots appear / disappear one by one, 0 = all at once
};
