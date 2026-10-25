#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdTypes.h"

/** Small, allocation-free helpers shared by the subsystem and the Mass processors. */
namespace CrowdMath
{
	/** xorshift32. State must be non-zero. */
	FORCEINLINE uint32 NextRandom(uint32& State)
	{
		State ^= State << 13;
		State ^= State >> 17;
		State ^= State << 5;
		return State;
	}

	/** Uniform float in [0, 1). */
	FORCEINLINE float Random01(uint32& State)
	{
		return static_cast<float>(NextRandom(State) >> 8) * (1.0f / 16777216.0f);
	}

	/** Avalanche hash, used to derive per-region / per-agent seeds. */
	FORCEINLINE uint32 Hash32(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352dU;
		X ^= X >> 15;
		X *= 0x846ca68bU;
		X ^= X >> 16;
		return X;
	}

	FORCEINLINE uint32 NonZeroSeed(uint32 Seed)
	{
		return Seed != 0 ? Seed : 0x9E3779B9u;
	}

	/**
	 * Distance based LOD with hysteresis (no flicker at tier boundaries).
	 * Moving to a coarser tier needs Dist > boundary + hysteresis, moving to a finer tier needs Dist < boundary - hysteresis.
	 */
	inline ECrowdLOD ComputeTier(float Dist, ECrowdLOD Current, const FCrowdTuning& T)
	{
		const ECrowdLOD Wanted =
			Dist < T.LODDistanceCm[0] ? ECrowdLOD::High :
			Dist < T.LODDistanceCm[1] ? ECrowdLOD::Medium :
			Dist < T.LODDistanceCm[2] ? ECrowdLOD::Low : ECrowdLOD::Off;

		if (Wanted == Current)
		{
			return Current;
		}

		if (static_cast<uint8>(Wanted) > static_cast<uint8>(Current))
		{
			// Going coarser: the boundary we cross is the one right behind the current tier.
			return Dist > T.LODDistanceCm[static_cast<int32>(Current)] + T.LODHysteresisCm ? Wanted : Current;
		}

		// Going finer: the boundary we cross is the one right behind the wanted tier.
		return Dist < T.LODDistanceCm[static_cast<int32>(Wanted)] - T.LODHysteresisCm ? Wanted : Current;
	}

	FORCEINLINE FIntPoint WorldToRegion(const FVector2D& P, float RegionSizeCm)
	{
		return FIntPoint(FMath::FloorToInt(P.X / RegionSizeCm), FMath::FloorToInt(P.Y / RegionSizeCm));
	}

	FORCEINLINE FVector2D RegionMin(const FIntPoint& Coord, float RegionSizeCm)
	{
		return FVector2D(static_cast<double>(Coord.X) * RegionSizeCm, static_cast<double>(Coord.Y) * RegionSizeCm);
	}
}
