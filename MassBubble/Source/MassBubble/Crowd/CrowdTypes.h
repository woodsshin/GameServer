#pragma once

#include "CoreMinimal.h"
#include "CrowdTypes.generated.h"

/**
 * Simulation level of detail, decided per agent from the distance to the nearest viewer.
 * Stored as a VALUE in a fragment (not as a tag): changing a value is a cheap write, whereas changing a
 * tag moves the entity to another archetype (structural change).
 */
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
};

/** Plain-old-data snapshot of the settings. Read by processors (possibly off the game thread). */
struct FCrowdTuning
{
	// --- Regions (aligned with the World Partition runtime cell size, 128 m by default) ---
	float RegionSizeCm = 12800.f;
	int32 ActiveRegionRadius = 2;          // 2 => 5x5 regions around every viewer
	int32 AgentsPerRegion = 400;
	float RegionDeactivateDelaySec = 10.f;
	int32 MaxSpawnPerFrame = 500;          // spawn time-slicing budget (hard cap, the wall clock budget below usually hits first)
	int32 MaxDespawnPerFrame = 1000;       // despawn time-slicing budget (hard cap)
	int32 WorldSeed = 1337;

	// --- LOD ---
	float LODDistanceCm[3] = { 4000.f, 10000.f, 20000.f }; // upper bound of High / Medium / Low
	float LODHysteresisCm = 500.f;
	int32 LODIntervalFrames[CrowdLODCount] = { 1, 2, 6, 0 }; // simulate every N frames, 0 = frozen

	// --- Movement ---
	float MinSpeedCmPerSec = 80.f;
	float MaxSpeedCmPerSec = 220.f;
	float MinRetargetSec = 2.f;
	float MaxRetargetSec = 6.f;
	float AgentGroundZ = 0.f;
	float MaxStepDeltaSec = 0.5f;

	// --- Replication ---
	float ReplicationHz = 10.f;
	float BubbleRadiusCm = 15000.f;
	int32 MaxAgentsPerBubble = 400;
	float NearErrorCm = 30.f;              // dead-reckoning tolerance close to the player
	float FarErrorCm = 150.f;              // ... and far from the player
	float NearDistanceCm = 3000.f;
	float VelocityEpsCmPerSec = 25.f;
	float GridCellSizeCm = 1600.f;

	// --- Streaming / hitch protection (see UCrowdSettings, category "Streaming") ---
	float SpawnBudgetMs = 1.0f;            // wall clock per frame for creating agents
	float SpawnBudgetBusyMs = 0.25f;       // ... while World Partition cells are loading / being added / removed
	float DespawnBudgetMs = 1.0f;          // wall clock per frame for destroying agents
	float DespawnBudgetBusyMs = 0.25f;
	int32 StructuralBatchSize = 64;        // agents per create / destroy call, the clock is checked between calls
	float SnapshotMarginCm = 2000.f;       // regions this far outside a player's bubble are still snapshotted

	// --- Derived ---
	int32 CellsPerSide = 8;

	// --- Virtual viewers (server-side load-test bots) ---
	float BotOrbitRadiusCm = 60000.f;
	float BotSpeedCmPerSec = 1500.f;
	float BotRampIntervalSec = 1.0f;       // bots appear / disappear one by one, 0 = all at once
};
