#pragma once

#include "CoreMinimal.h"
#include "MassEntityTypes.h"
#include "Crowd/CrowdTypes.h"
#include "CrowdFragments.generated.h"

/**
 * Data layout: one small fragment per access pattern, so a chunk that is only being read for LOD
 * does not drag movement data through the cache and vice versa.
 *
 *   FCrowdIdFragment       16 B  identity + home region (read-mostly)
 *   FCrowdLocationFragment 16 B  position, double precision (LWC safe for huge worlds)
 *   FCrowdMotionFragment   16 B  velocity + wander state
 *   FCrowdLODFragment       8 B  current tier + accumulated delta for time slicing
 */

/** Marks every crowd agent. Used by queries and by tooling (mass.PrintEntityFragments etc.). */
USTRUCT()
struct MASSBUBBLE_API FCrowdAgentTag : public FMassTag
{
	GENERATED_BODY()
};

USTRUCT()
struct MASSBUBBLE_API FCrowdIdFragment : public FMassFragment
{
	GENERATED_BODY()

	/** Stable network id. Survives despawn / respawn of the owning region. */
	UPROPERTY()
	uint32 NetId = 0;

	/** Home region the agent is leashed to. The region (not the agent) is the unit of streaming. */
	UPROPERTY()
	int32 HomeX = 0;

	UPROPERTY()
	int32 HomeY = 0;

	/** Index into UCrowdSubsystem's region table. Avoids a hash lookup per agent in the snapshot pass. */
	UPROPERTY()
	int32 RegionSlot = INDEX_NONE;
};

USTRUCT()
struct MASSBUBBLE_API FCrowdLocationFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector2D Location = FVector2D::ZeroVector;
};

USTRUCT()
struct MASSBUBBLE_API FCrowdMotionFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector2f Velocity = FVector2f::ZeroVector;

	/** Seconds until the agent picks a new wander direction. */
	UPROPERTY()
	float RetargetTimer = 0.f;

	/** xorshift32 state: allocation free, deterministic, per agent. */
	UPROPERTY()
	uint32 Rng = 1;
};

USTRUCT()
struct MASSBUBBLE_API FCrowdLODFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	ECrowdLOD Tier = ECrowdLOD::Off;

	/** Delta time accumulated while this agent was skipped by the time slicer. */
	UPROPERTY()
	float PendingDelta = 0.f;
};
