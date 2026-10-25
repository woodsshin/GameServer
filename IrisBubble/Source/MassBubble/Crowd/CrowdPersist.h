#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdTypes.h"

/**
 * Persisted state of one agent of a region nobody looks at: 36 bytes where FCrowdSavedAgent takes 48.
 * The location is stored relative to the region origin as float (error below 0.001 cm inside a 128 m region);
 * every other field is copied bit for bit.
 */
struct FCrowdPackedAgent
{
	uint32 NetId = 0;
	FVector2f Offset = FVector2f::ZeroVector;
	FVector2f Velocity = FVector2f::ZeroVector;
	float RetargetTimer = 0.f;
	uint32 Rng = 1;
	float Heading = 0.f;
	float CruiseSpeed = 0.f;
};

static_assert(sizeof(FCrowdPackedAgent) == 36, "FCrowdPackedAgent is expected to be 36 bytes");

namespace CrowdPersist
{
	FORCEINLINE FCrowdPackedAgent Pack(const FCrowdSavedAgent& Agent, const FVector2D& RegionMin)
	{
		FCrowdPackedAgent Packed;
		Packed.NetId = Agent.NetId;
		Packed.Offset = FVector2f(static_cast<float>(Agent.Location.X - RegionMin.X), static_cast<float>(Agent.Location.Y - RegionMin.Y));
		Packed.Velocity = Agent.Velocity;
		Packed.RetargetTimer = Agent.RetargetTimer;
		Packed.Rng = Agent.Rng;
		Packed.Heading = Agent.Heading;
		Packed.CruiseSpeed = Agent.CruiseSpeed;
		return Packed;
	}

	FORCEINLINE FCrowdSavedAgent Unpack(const FCrowdPackedAgent& Packed, const FVector2D& RegionMin)
	{
		FCrowdSavedAgent Agent;
		Agent.NetId = Packed.NetId;
		Agent.Location = FVector2D(RegionMin.X + static_cast<double>(Packed.Offset.X), RegionMin.Y + static_cast<double>(Packed.Offset.Y));
		Agent.Velocity = Packed.Velocity;
		Agent.RetargetTimer = Packed.RetargetTimer;
		Agent.Rng = Packed.Rng;
		Agent.Heading = Packed.Heading;
		Agent.CruiseSpeed = Packed.CruiseSpeed;
		return Agent;
	}
}
