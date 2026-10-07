#pragma once

#include "CoreMinimal.h"

/**
 * Pure functions behind the crowd replication format (no UObject, no engine state => unit testable).
 *
 * Wire format per agent (see FCrowdAgentItem):   NetId 4 B + X,Y int16 + VX,VY int8  = 10 B
 * versus a naive FVector (3 x double = 24 B) + FVector velocity (24 B) + int32 id ~= 52 B.
 *
 *   * Positions are sent relative to a per-bubble ORIGIN snapped to a 200 m lattice, in 1 cm units, as int16:
 *     range +-327 m. The player is never further than RebaseDistanceCm (110 m) from the origin and the bubble
 *     radius is at most MaxBubbleRadiusCm (210 m), so every offset is <= 320 m and always fits.
 *   * Velocity is int8 in 5 cm/s units (+-635 cm/s).
 *   * The origin only changes when the player is MORE than RebaseDistanceCm away from it. The lattice half cell is
 *     100 m, so there is 10 m of hysteresis: a player jittering on a lattice border does not trigger a full resend
 *     (a rebase re-sends every agent) on each flip.
 */
namespace CrowdNet
{
	constexpr double PosUnitCm = 1.0;
	constexpr double OriginLatticeCm = 20000.0;
	constexpr double RebaseDistanceCm = 11000.0;
	constexpr float VelUnitCmPerSec = 5.f;

	/** Largest bubble radius that is guaranteed to fit the int16 range: 110 m (rebase distance) + 210 m = 320 m < 327.67 m. */
	constexpr float MaxBubbleRadiusCm = 21000.f;

	FORCEINLINE int16 QuantizeOffset(double RelativeCm)
	{
		return static_cast<int16>(FMath::Clamp(FMath::RoundToInt(RelativeCm / PosUnitCm), -32768, 32767));
	}

	FORCEINLINE double DequantizeOffset(int16 Quantized)
	{
		return static_cast<double>(Quantized) * PosUnitCm;
	}

	FORCEINLINE int8 QuantizeVelocity(float CmPerSec)
	{
		return static_cast<int8>(FMath::Clamp(FMath::RoundToInt(CmPerSec / VelUnitCmPerSec), -127, 127));
	}

	FORCEINLINE float DequantizeVelocity(int8 Quantized)
	{
		return static_cast<float>(Quantized) * VelUnitCmPerSec;
	}

	FORCEINLINE FIntPoint WorldToLattice(const FVector2D& P)
	{
		return FIntPoint(FMath::RoundToInt(P.X / OriginLatticeCm), FMath::RoundToInt(P.Y / OriginLatticeCm));
	}

	FORCEINLINE FVector2D LatticeToWorld(const FIntPoint& Lattice)
	{
		return FVector2D(static_cast<double>(Lattice.X) * OriginLatticeCm, static_cast<double>(Lattice.Y) * OriginLatticeCm);
	}

	/** True when the viewer has left the safe zone around the current origin and the origin has to move (full resend). */
	FORCEINLINE bool NeedsRebase(const FVector2D& Viewer, const FVector2D& OriginWorld)
	{
		return FMath::Max(FMath::Abs(Viewer.X - OriginWorld.X), FMath::Abs(Viewer.Y - OriginWorld.Y)) > RebaseDistanceCm;
	}

	/**
	 * Dead reckoning: the client extrapolates SentPos + SentVel * dt. The server only resends an agent when
	 * that prediction drifts further than ToleranceCm from the truth, or the velocity changed noticeably.
	 * SentPos / SentVel must be the DEQUANTIZED values, i.e. exactly what the client believes.
	 */
	inline bool ShouldResend(
		const FVector2D& TruePos, const FVector2f& TrueVel,
		const FVector2D& SentPos, const FVector2f& SentVel,
		double SecondsSinceSent, float ToleranceCm, float VelocityEpsCmPerSec)
	{
		const FVector2D Predicted = SentPos + FVector2D(SentVel.X, SentVel.Y) * SecondsSinceSent;
		if (FVector2D::DistSquared(TruePos, Predicted) > static_cast<double>(ToleranceCm) * ToleranceCm)
		{
			return true;
		}

		const FVector2f DeltaV = TrueVel - SentVel;
		return DeltaV.SizeSquared() > VelocityEpsCmPerSec * VelocityEpsCmPerSec;
	}
}
