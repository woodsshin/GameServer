#pragma once

#include "CoreMinimal.h"

/**
 * Pure functions behind the crowd replication format (no UObject, no engine state => unit testable).
 *
 * Wire format per agent (see FCrowdAgentItem):   X,Y uint16 + VX,VY int8 + Stamp uint8  = 7 B.
 *
 *   * Positions are offsets from the lower corner of the agent's region, 0..RegionSizeCm in 65535 steps. An agent never
 *     leaves its region, so the range is fixed: 0.2 cm per step in a 128 m region, whatever the distance to the world origin.
 *   * Velocity is int8 in 5 cm/s units (+-635 cm/s).
 *   * Every state carries a time stamp: StampHz ticks per second of server time, 8 bit, wrapping after 8 s. The replicated
 *     state of a region is shared by every connection, so the age of a state cannot be derived from the moment it arrived.
 *     The age is the difference between the stamp of the item and the newest stamp of its region.
 *
 * THE EXTRAPOLATION CONTRACT (client <-> server). Read this before changing either side.
 *   The client draws a replicated agent at   Pos + Vel * min(age, MaxExtrapolationSec),
 *   age = StampAgeSec(newest stamp of the region, stamp of the item) + seconds since the last update of the region arrived.
 *   The server decides what to publish by predicting EXACTLY that (Extrapolate / ShouldResend), and it publishes a walking
 *   agent every HeartbeatSec at the latest, i.e. before the client's extrapolation runs out. Both sides use the constants
 *   below. A stamp survives HeartbeatSec + MaxExtrapolationSec before it wraps.
 */
namespace CrowdNet
{
	/** Number of steps an offset inside a region is divided into. */
	constexpr uint32 PosSteps = 65535u;

	constexpr float VelUnitCmPerSec = 5.f;

	/** Ticks of the replicated time stamp per second of server time. 8 bit => wraps after 255 / StampHz = 8 s. */
	constexpr double StampHz = 32.0;

	/**
	 * A client never extrapolates an agent for longer than this after the state it holds was valid
	 * (a stalled connection must not fling agents across the map). The server knows this: see ShouldResend().
	 */
	constexpr double MaxExtrapolationSec = 3.0;

	/** A walking agent whose state is this old is published again, so a healthy connection never reaches the cap above. */
	constexpr double HeartbeatSec = 2.0;

	/** Offset from the lower corner of the region, clamped to the region. */
	FORCEINLINE uint16 QuantizeRegionOffset(double OffsetCm, double RegionSizeCm)
	{
		const double Unit = FMath::Clamp(OffsetCm / FMath::Max(RegionSizeCm, 1.0), 0.0, 1.0);
		return static_cast<uint16>(FMath::RoundToInt(Unit * static_cast<double>(PosSteps)));
	}

	FORCEINLINE double DequantizeRegionOffset(uint16 Quantized, double RegionSizeCm)
	{
		return static_cast<double>(Quantized) * (RegionSizeCm / static_cast<double>(PosSteps));
	}

	/** Size of one position step in centimetres. */
	FORCEINLINE double RegionOffsetStepCm(double RegionSizeCm)
	{
		return RegionSizeCm / static_cast<double>(PosSteps);
	}

	FORCEINLINE int8 QuantizeVelocity(float CmPerSec)
	{
		return static_cast<int8>(FMath::Clamp(FMath::RoundToInt(CmPerSec / VelUnitCmPerSec), -127, 127));
	}

	FORCEINLINE float DequantizeVelocity(int8 Quantized)
	{
		return static_cast<float>(Quantized) * VelUnitCmPerSec;
	}

	/** The time stamp of a moment in server time (seconds). */
	FORCEINLINE uint8 StampOf(double ServerSeconds)
	{
		const int64 Tick = static_cast<int64>(FMath::FloorToDouble(FMath::Max(ServerSeconds, 0.0) * StampHz));
		return static_cast<uint8>(Tick & 0xFF);
	}

	/** Seconds from the stamp of an item to the newest stamp of its region (accurate to one tick, wraps with the stamp). */
	FORCEINLINE double StampAgeSec(uint8 NewestStamp, uint8 ItemStamp)
	{
		return static_cast<double>(static_cast<uint8>(NewestStamp - ItemStamp)) / StampHz;
	}

	/**
	 * Where the client draws an agent (before its cosmetic smoothing): the state it holds plus velocity * age,
	 * with the age capped at MaxExtrapolationSec. The server calls the same function to know what the client sees.
	 */
	FORCEINLINE FVector2D Extrapolate(const FVector2D& SentPos, const FVector2f& SentVel, double AgeSec)
	{
		const double Elapsed = FMath::Clamp(AgeSec, 0.0, MaxExtrapolationSec);
		return SentPos + FVector2D(SentVel.X, SentVel.Y) * Elapsed;
	}

	/**
	 * The velocity a client walks a drawn agent with (CrowdSmoothing's velocity feed-forward): the replicated one while
	 * extrapolation is allowed, ZERO once the cap has run out. Past the cap Extrapolate() no longer advances the position,
	 * so the velocity must not keep pushing the drawn agent either: a stalled connection ends in a gentle stop.
	 */
	FORCEINLINE FVector2f DrivingVelocity(const FVector2f& SentVel, double AgeSec)
	{
		return AgeSec > MaxExtrapolationSec ? FVector2f::ZeroVector : SentVel;
	}

	/**
	 * Dead reckoning: the server only publishes an agent again when
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
}
