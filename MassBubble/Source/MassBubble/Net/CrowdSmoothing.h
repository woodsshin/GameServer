#pragma once

#include "CoreMinimal.h"

/**
 * Client-side presentation of ONE replicated agent. Pure functions on plain data (no UObject, no engine state), so
 * they are unit tested without Unreal (Tests/CrowdTests.cpp) and UCrowdRenderSubsystem only has to call Step().
 *
 * The replicated state is cheap and therefore coarse: a position and a CONSTANT velocity per walking segment. Drawn
 * as it is, every new segment is a corner (instant turn), every pause an instant stop and every dead-reckoning
 * correction a small jump. The presentation hides that without lying about where the agent is:
 *
 *   1. the velocity we draw with EASES towards the replicated one (a turn becomes a short curve, a stop a short slide),
 *   2. the position is dead reckoned with that eased velocity ("velocity feed-forward") ...
 *   3. ... and pulled onto the extrapolated server position, so the error can never build up,
 *   4. the yaw follows the drawn velocity and keeps its last value while the agent stands.
 *
 * The first version only low-pass filtered the position. That lags behind a walking agent, smears every correction
 * and cannot ease a turn.
 */
namespace CrowdSmoothing
{
	struct FParams
	{
		/** 1/s. How fast the drawn velocity follows the replicated one. 7 => ~0.14 s time constant. */
		float VelocityRate = 7.f;

		/** 1/s. How fast the drawn position is pulled onto the extrapolated one. 4 => ~0.25 s time constant. */
		float CorrectionRate = 4.f;

		/** 1/s. How fast the yaw turns towards the walking direction. */
		float YawRate = 10.f;

		/** Farther than this from the target (cm): do not glide, teleport (new data after a long gap, first sighting). */
		double SnapDistanceCm = 500.0;

		/** Below this drawn speed (cm/s) the yaw is left alone: a standing agent keeps facing where it walked. */
		float MinYawSpeedCmPerSec = 15.f;
	};

	struct FState
	{
		FVector2D Pos = FVector2D::ZeroVector;
		FVector2f Vel = FVector2f::ZeroVector;
		float YawDeg = 0.f;
	};

	/** Blend factors of one frame (1 - exp(-rate * dt)). Computed once per frame, not once per agent. */
	struct FFrame
	{
		float Velocity = 1.f;
		float Correction = 1.f;
		float Yaw = 1.f;
	};

	inline FFrame MakeFrame(const FParams& Params, double DeltaSeconds)
	{
		auto Blend = [DeltaSeconds](float Rate)
		{
			return Rate > 0.f ? static_cast<float>(1.0 - FMath::Exp(-static_cast<double>(Rate) * DeltaSeconds)) : 1.f;
		};

		FFrame Frame;
		Frame.Velocity = Blend(Params.VelocityRate);
		Frame.Correction = Blend(Params.CorrectionRate);
		Frame.Yaw = Blend(Params.YawRate);
		return Frame;
	}

	/** Angle in degrees wrapped to [-180, 180). */
	FORCEINLINE float WrapDegrees(float Degrees)
	{
		float Wrapped = FMath::Fmod(Degrees + 180.f, 360.f);
		if (Wrapped < 0.f)
		{
			Wrapped += 360.f;
		}
		return Wrapped - 180.f;
	}

	/** First time this agent is drawn: exactly where the server says, with the replicated velocity. */
	inline FState Begin(const FVector2D& TargetPos, const FVector2f& TargetVel, float FallbackYawDeg)
	{
		FState State;
		State.Pos = TargetPos;
		State.Vel = TargetVel;
		State.YawDeg = (TargetVel.SizeSquared() > 1.f)
			? FMath::RadiansToDegrees(FMath::Atan2(TargetVel.Y, TargetVel.X))
			: FallbackYawDeg;
		return State;
	}

	/**
	 * One frame. TargetPos is the extrapolated server position (CrowdNet::Extrapolate). TargetVel is the velocity to
	 * feed forward: the replicated one while extrapolation is allowed, ZERO once the cap has run out, so that a stalled
	 * connection makes agents come to a gentle stop instead of walking on.
	 */
	inline void Step(FState& State, const FVector2D& TargetPos, const FVector2f& TargetVel,
		double DeltaSeconds, const FFrame& Frame, const FParams& Params)
	{
		State.Vel += (TargetVel - State.Vel) * Frame.Velocity;

		State.Pos += FVector2D(State.Vel.X, State.Vel.Y) * DeltaSeconds;
		State.Pos += (TargetPos - State.Pos) * static_cast<double>(Frame.Correction);

		if (FVector2D::DistSquared(State.Pos, TargetPos) > Params.SnapDistanceCm * Params.SnapDistanceCm)
		{
			State.Pos = TargetPos;
		}

		if (State.Vel.SizeSquared() > Params.MinYawSpeedCmPerSec * Params.MinYawSpeedCmPerSec)
		{
			const float WalkYaw = FMath::RadiansToDegrees(FMath::Atan2(State.Vel.Y, State.Vel.X));
			State.YawDeg = WrapDegrees(State.YawDeg + WrapDegrees(WalkYaw - State.YawDeg) * Frame.Yaw);
		}
	}
}
