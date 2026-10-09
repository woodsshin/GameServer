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
 *
 * THE EXTRAPOLATION CONTRACT (client <-> server). Read this before changing either side.
 *   The client draws a replicated agent at   SentPos + SentVel * min(elapsed, MaxExtrapolationSec).
 *   The server decides what to resend by predicting EXACTLY that (Extrapolate / ShouldResend), and it re-sends a walking
 *   agent every HeartbeatSec at the latest, i.e. before the client's extrapolation runs out. Both sides use the
 *   constants below. There is no second, private cap on the client that the server does not know about.
 *   (The first version had one: the server assumed a straight walker could be extrapolated for ever, the client stopped
 *   after 1.5 s. Agents froze in the middle of a walk and jumped to the right place at the next update.)
 */
namespace CrowdNet
{
	constexpr double PosUnitCm = 1.0;
	constexpr double OriginLatticeCm = 20000.0;
	constexpr double RebaseDistanceCm = 11000.0;
	constexpr float VelUnitCmPerSec = 5.f;

	/** Largest bubble radius that is guaranteed to fit the int16 range: 110 m (rebase distance) + 210 m = 320 m < 327.67 m. */
	constexpr float MaxBubbleRadiusCm = 21000.f;

	/**
	 * A client never extrapolates an agent for longer than this after the last update it received for it
	 * (a stalled connection must not fling agents across the map). The server knows this: see ShouldResend().
	 */
	constexpr double MaxExtrapolationSec = 3.0;

	/** A walking agent that has been silent this long is re-sent, so a healthy connection never reaches the cap above. */
	constexpr double HeartbeatSec = 2.0;

	/** Interest set hysteresis: an agent that is already replicated to a player counts as 10 % closer than it is ... */
	constexpr double InterestKeepDistanceFactor = 0.9;

	/** ... and stays in range up to 10 % beyond the radius a newcomer needs. */
	constexpr double InterestExitRadiusFactor = 1.1;

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
	 * Where the client draws an agent (before its cosmetic smoothing): the last received state plus velocity * elapsed,
	 * with elapsed capped at MaxExtrapolationSec. The server calls the same function to know what the client sees.
	 */
	FORCEINLINE FVector2D Extrapolate(const FVector2D& SentPos, const FVector2f& SentVel, double SecondsSinceSent)
	{
		const double Elapsed = FMath::Clamp(SecondsSinceSent, 0.0, MaxExtrapolationSec);
		return SentPos + FVector2D(SentVel.X, SentVel.Y) * Elapsed;
	}

	/**
	 * The velocity a client walks a drawn agent with (CrowdSmoothing's velocity feed-forward): the replicated one while
	 * extrapolation is allowed, ZERO once the cap has run out. Past the cap Extrapolate() no longer advances the position,
	 * so the velocity must not keep pushing the drawn agent either: a stalled connection ends in a gentle stop.
	 */
	FORCEINLINE FVector2f DrivingVelocity(const FVector2f& SentVel, double SecondsSinceSent)
	{
		return SecondsSinceSent > MaxExtrapolationSec ? FVector2f::ZeroVector : SentVel;
	}

	/**
	 * Dead reckoning: the server only resends an agent when
	 *   * what the client draws (Extrapolate) drifts further than ToleranceCm from the truth, or
	 *   * the velocity changed noticeably (a new walking segment, a pause, a restart), or
	 *   * a walking agent has been silent for HeartbeatSec (the client's extrapolation must never run dry).
	 * SentPos / SentVel must be the DEQUANTIZED values, i.e. exactly what the client believes.
	 */
	inline bool ShouldResend(
		const FVector2D& TruePos, const FVector2f& TrueVel,
		const FVector2D& SentPos, const FVector2f& SentVel,
		double SecondsSinceSent, float ToleranceCm, float VelocityEpsCmPerSec)
	{
		const FVector2D Predicted = Extrapolate(SentPos, SentVel, SecondsSinceSent);
		if (FVector2D::DistSquared(TruePos, Predicted) > static_cast<double>(ToleranceCm) * ToleranceCm)
		{
			return true;
		}

		const FVector2f DeltaV = TrueVel - SentVel;
		if (DeltaV.SizeSquared() > VelocityEpsCmPerSec * VelocityEpsCmPerSec)
		{
			return true;
		}

		return SecondsSinceSent >= HeartbeatSec && !SentVel.IsNearlyZero();
	}

	/**
	 * Sort key of an interest set candidate (smaller = more important). Agents that are already replicated get a head
	 * start, so an agent hovering around the cut-off rank does not enter and leave the set again and again.
	 */
	FORCEINLINE double InterestRank(double DistSq, bool bAlreadyReplicated)
	{
		return bAlreadyReplicated ? DistSq * (InterestKeepDistanceFactor * InterestKeepDistanceFactor) : DistSq;
	}

	/** Radius inside which an already replicated agent may stay. Never above MaxBubbleRadiusCm (int16 range). */
	FORCEINLINE double InterestExitRadius(double EnterRadiusCm)
	{
		return FMath::Min(EnterRadiusCm * InterestExitRadiusFactor, static_cast<double>(MaxBubbleRadiusCm));
	}

	/**
	 * Is an agent at DistSq (squared distance to the viewer) a candidate for the interest set, and with which sort key?
	 * A newcomer has to be inside EnterRadiusCm, a member may stay up to InterestExitRadius(EnterRadiusCm). The caller
	 * sorts the candidates by OutRank and keeps the first MaxAgentsPerBubble. ACrowdBubble and the tests use this very function.
	 */
	FORCEINLINE bool InterestCandidate(double DistSq, bool bAlreadyReplicated, double EnterRadiusCm, double& OutRank)
	{
		const double Radius = bAlreadyReplicated ? InterestExitRadius(EnterRadiusCm) : EnterRadiusCm;
		if (DistSq > Radius * Radius)
		{
			return false;
		}
		OutRank = InterestRank(DistSq, bAlreadyReplicated);
		return true;
	}
}
