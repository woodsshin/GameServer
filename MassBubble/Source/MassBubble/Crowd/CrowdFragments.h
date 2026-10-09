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
 *   FCrowdMotionFragment   24 B  velocity + wander state (see Crowd/CrowdWander.h)
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

	/** Velocity of the current walking segment (cm/s). Zero while the agent pauses. */
	UPROPERTY()
	FVector2f Velocity = FVector2f::ZeroVector;

	/** Seconds until the current segment (walk or pause) ends and the agent plans the next one. */
	UPROPERTY()
	float RetargetTimer = 0.f;

	/** xorshift32 state: allocation free, deterministic, per agent. */
	UPROPERTY()
	uint32 Rng = 1;

	/** Direction of the last walking segment in radians. Kept while the agent pauses, so it walks on from where it faced. */
	UPROPERTY()
	float Heading = 0.f;

	/** This agent's own preferred walking speed (cm/s). Segments vary around it, they do not re-roll it. */
	UPROPERTY()
	float CruiseSpeed = 140.f;
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
