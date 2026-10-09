#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdTypes.h"

/**
 * How one agent wanders. Pure functions on plain data: no UObject, no Mass, no engine state. The model is unit
 * tested (Tests/CrowdTests.cpp) and the Mass processor only has to call Advance().
 *
 * Why it looks like this (the first version walked like a drunk who bounces off invisible walls):
 *
 *   * An agent walks in SEGMENTS: straight, constant velocity, MinRetargetSec..MaxRetargetSec long. A segment is exactly
 *     what dead reckoning wants to send (position + velocity), so smooth-looking motion costs no extra bandwidth.
 *   * The next heading is the previous heading plus a SMALL random turn (triangular distribution, +-MaxTurnDeg). The
 *     first version drew a new heading uniformly from 0..360 degrees, i.e. every third segment was a reversal. The
 *     speed stays near the agent's own cruise speed instead of being re-rolled between 0.8 and 2.2 m/s.
 *   * Now and then (IdleChance) an agent pauses for a few seconds and then walks on. Pause and restart are velocity
 *     steps on the wire; the client eases them (Net/CrowdSmoothing.h). The pause chance is per unit of walking time,
 *     not per segment, so it is the same everywhere in the region.
 *
 *   * THE REGION BORDER. An agent never leaves its home region (the region is the unit of streaming and persistence).
 *     The first version reflected the velocity at the border: agents visibly bounced back, and 180 degree reversals
 *     look exactly like "walked somewhere and went back to where it started". Obvious fix: steer away before the wall.
 *     Measured result of the obvious fix (turn towards the middle when the next segment would end outside): agents
 *     pile up along the wall (3.7 x the interior density within 1-2 m). The reason is general: a rule of the form
 *     "turn only if you are heading at the wall" depends on the HEADING, it squeezes the set of headings near the wall
 *     and the density follows. A rule that depends on the POSITION only keeps the uniform density uniform (Liouville:
 *     a flow whose turn rate does not depend on the heading preserves the uniform measure on (position, heading)).
 *
 *     So the border is a "magnetic" layer: inside a band of LayerDepthCm along the region border every agent turns at
 *     the same rate WallTurnRateDeg, to the left or to the right (a fixed side per agent, derived from its id), no
 *     matter where it looks. An agent that walks into the band therefore bends away on a circle of radius speed/rate
 *     and walks out again; its deepest point is 2 * radius, and the band is wider than that for the fastest agent. It
 *     can never touch the wall, there is no reflection and no cut, and the density stays uniform (see the
 *     "MassBubble.Crowd.Wander.Density" test for the measured profile).
 *     The arc is walked as chords (LayerChordSec each): still constant velocity per segment, so still dead reckoning.
 *
 *   * Stepping is exact at segment boundaries: a step that spans one is split. Time slicing (stepping an agent every
 *     N-th frame with the delta it accumulated) therefore walks the same path as stepping it every frame.
 *   * Agents must START outside the band (SpawnBox()). Inside the band an agent always continues the arc it is on, an
 *     arc that began at the inner edge of the band; a fresh agent dropped into the band at a random heading could be
 *     facing the wall with less room than the arc needs.
 *
 * TMotion is duck typed (FCrowdMotionFragment, FCrowdSavedAgent, or a plain struct in the tests). It needs the members
 *   FVector2f Velocity;  float RetargetTimer;  uint32 Rng;  float Heading;  float CruiseSpeed;
 */
namespace CrowdWander
{
	/** Home region of an agent, in world space. */
	struct FBox2
	{
		FVector2D Min;
		FVector2D Max;
	};

	/** The planner keeps agents this far inside their region border (cm). Only the safety clamp ever uses it up. */
	constexpr double WallMarginCm = 50.0;

	/** Extra width of the band on top of what the arc geometry needs: chord approximation, the straight run before the first chord, float noise. */
	constexpr float LayerSlackCm = 40.f;

	/** Length of one chord of the turning arc. 0.5 s at 60 deg/s = 30 degrees per chord. */
	constexpr float LayerChordSec = 0.5f;

	/** The shortest segment the planner produces when it cuts one at a band edge. */
	constexpr float MinSegmentSec = 0.1f;

	/** A cut at a band edge ends this long AFTER the edge, so the next planning step sees the new side however the floats round. */
	constexpr float BoundaryOvershootSec = 0.03f;

	constexpr double Never = 1.0e12;

	/** Radians wrapped to [-pi, pi). */
	FORCEINLINE float WrapRadians(float Radians)
	{
		constexpr float TwoPi = 2.f * UE_PI;
		float Wrapped = FMath::Fmod(Radians + UE_PI, TwoPi);
		if (Wrapped < 0.f)
		{
			Wrapped += TwoPi;
		}
		return Wrapped - UE_PI;
	}

	/** The box shrunk by Cm on every side. */
	FORCEINLINE FBox2 Shrink(const FBox2& Box, double Cm)
	{
		return FBox2{ Box.Min + FVector2D(Cm, Cm), Box.Max - FVector2D(Cm, Cm) };
	}

	FORCEINLINE bool IsStrictlyInside(const FBox2& Box, const FVector2D& P)
	{
		return P.X > Box.Min.X && P.X < Box.Max.X && P.Y > Box.Min.Y && P.Y < Box.Max.Y;
	}

	/** Turn rate inside the band (radians per second, always > 0). */
	FORCEINLINE float WallTurnRate(const FCrowdTuning& Tuning)
	{
		return FMath::Max(FMath::DegreesToRadians(Tuning.WallTurnRateDeg), 0.05f);
	}

	/**
	 * Width of the band. An agent that enters it at speed v and turns at rate w follows a circle of radius v / w. Its
	 * displacement towards the wall is (v / w) * (sin(a + w t) - sin(a)) <= 2 v / w, whatever the entry angle a is.
	 */
	FORCEINLINE float LayerDepthCm(const FCrowdTuning& Tuning)
	{
		return 2.f * Tuning.MaxSpeedCmPerSec / WallTurnRate(Tuning) + LayerSlackCm;
	}

	/** Region minus the margin and the band: where agents walk straight. */
	FORCEINLINE FBox2 InnerBox(const FBox2& Home, const FCrowdTuning& Tuning)
	{
		return Shrink(Home, WallMarginCm + static_cast<double>(LayerDepthCm(Tuning)));
	}

	/** Where a new agent may be dropped (strictly inside InnerBox). */
	FORCEINLINE FBox2 SpawnBox(const FBox2& Home, const FCrowdTuning& Tuning)
	{
		return Shrink(Home, WallMarginCm + static_cast<double>(LayerDepthCm(Tuning)) + 1.0);
	}

	/** Which way this agent turns in the band: +1 (counter clockwise) or -1. A fixed property of the agent, derived from its id. */
	FORCEINLINE float TurnSignForId(uint32 NetId)
	{
		uint32 H = NetId * 2654435761u;
		H ^= H >> 15;
		H *= 2246822519u;
		H ^= H >> 13;
		return (H & 1u) ? 1.f : -1.f;
	}

	/** The part of the line P + V * t that is inside the (open) box, as the interval [Enter, Exit]. False if there is none. */
	FORCEINLINE bool Slab(const FBox2& Box, const FVector2D& P, const FVector2f& V, double& Enter, double& Exit)
	{
		Enter = -Never;
		Exit = Never;

		const double Lo[2] = { Box.Min.X - P.X, Box.Min.Y - P.Y };
		const double Hi[2] = { Box.Max.X - P.X, Box.Max.Y - P.Y };
		const double Vel[2] = { static_cast<double>(V.X), static_cast<double>(V.Y) };

		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (Vel[Axis] == 0.0)
			{
				if (Lo[Axis] >= 0.0 || Hi[Axis] <= 0.0)
				{
					return false; // standing outside this slab, and not moving
				}
				continue;
			}

			double T0 = Lo[Axis] / Vel[Axis];
			double T1 = Hi[Axis] / Vel[Axis];
			if (T0 > T1)
			{
				const double Tmp = T0;
				T0 = T1;
				T1 = Tmp;
			}
			Enter = FMath::Max(Enter, T0);
			Exit = FMath::Min(Exit, T1);
		}
		return Enter < Exit;
	}

	/** Seconds until a straight path from P (strictly inside the box) reaches the box border. */
	FORCEINLINE float TimeToLeave(const FBox2& Box, const FVector2D& P, const FVector2f& V)
	{
		double Enter = 0.0;
		double Exit = 0.0;
		if (!IsStrictlyInside(Box, P) || !Slab(Box, P, V, Enter, Exit))
		{
			return 0.f;
		}
		return static_cast<float>(Exit);
	}

	/** Seconds until a straight path from P (not strictly inside the box) is inside the box. "Never" if it does not get there. */
	FORCEINLINE float TimeToEnter(const FBox2& Box, const FVector2D& P, const FVector2f& V)
	{
		double Enter = 0.0;
		double Exit = 0.0;
		if (!Slab(Box, P, V, Enter, Exit) || Exit <= 0.0)
		{
			return static_cast<float>(Never);
		}
		return static_cast<float>(FMath::Max(Enter, 0.0));
	}

	/** A circle has to dip this far (cm) into open ground, measured on the polygon of chords, before we call it a way out. */
	constexpr double ReachMarginCm = 3.0;

	/**
	 * Does the arc that is tangent to Heading at P, has the given radius and turns to the left (Sign +1) or right (-1),
	 * come back to open ground (the inner box)? An arc that does not is a TRAP: the agent would run on it for ever.
	 * The arc is walked as a polygon of chords, whose edges bulge inwards by up to Radius * (1 - cos(HalfChordAngle)),
	 * so that is what the circle has to clear.
	 *
	 * An agent that enters the band exactly at the inner edge is never on such an arc. One that is a few centimetres
	 * past the edge (the planner cuts a segment at the edge plus a small overshoot) can be: a nearly tangential entry
	 * that turns towards the wall. The planner then turns the other way for that arc.
	 */
	FORCEINLINE bool ArcReachesOpenGround(const FBox2& Inner, const FVector2D& P, float Heading, float Radius, float Sign, float HalfChordAngle)
	{
		float SinH = 0.f;
		float CosH = 1.f;
		FMath::SinCos(&SinH, &CosH, Heading);
		const FVector2D Center = P + FVector2D(-SinH, CosH) * static_cast<double>(Radius * Sign);
		const double NearX = FMath::Clamp(Center.X, Inner.Min.X, Inner.Max.X);
		const double NearY = FMath::Clamp(Center.Y, Inner.Min.Y, Inner.Max.Y);
		const double Distance = FMath::Sqrt((Center.X - NearX) * (Center.X - NearX) + (Center.Y - NearY) * (Center.Y - NearY));
		return Distance < static_cast<double>(Radius) * static_cast<double>(FMath::Cos(HalfChordAngle)) - ReachMarginCm;
	}

	/** Chance that a walk of this length is replaced by a pause: IdleChance per MEAN segment, so it is the same per second of walking everywhere. */
	FORCEINLINE float PauseChance(const FCrowdTuning& Tuning, float WalkSeconds)
	{
		const float MeanSegment = FMath::Max(0.5f * (Tuning.MinRetargetSec + Tuning.MaxRetargetSec), 0.1f);
		const float Keep = FMath::Clamp(1.f - Tuning.IdleChance, 0.f, 1.f);
		return 1.f - FMath::Pow(Keep, WalkSeconds / MeanSegment);
	}

	/** Share of the time an agent stands still in the long run: pause hazard per second of walking times the mean pause. */
	FORCEINLINE float SteadyStateIdleShare(const FCrowdTuning& Tuning)
	{
		const float MeanSegment = FMath::Max(0.5f * (Tuning.MinRetargetSec + Tuning.MaxRetargetSec), 0.1f);
		const float Keep = FMath::Clamp(1.f - Tuning.IdleChance, 1.0e-4f, 1.f);
		const float Hazard = -FMath::Loge(Keep) / MeanSegment;
		const float PausePerWalkSecond = Hazard * 0.5f * (Tuning.MinIdleSec + Tuning.MaxIdleSec);
		return PausePerWalkSecond / (1.f + PausePerWalkSecond);
	}

	/**
	 * Picks the next segment (a walk, or a pause) for an agent that stands at Pos. Consumes the agent's own random stream.
	 * TurnSign is TurnSignForId(NetId).
	 */
	template <typename TMotion>
	inline void BeginSegment(const FVector2D& Pos, TMotion& Motion, const FBox2& Home, const FCrowdTuning& Tuning, float TurnSign)
	{
		const FBox2 Inner = InnerBox(Home, Tuning);
		const bool bWasWalking = Motion.Velocity.SizeSquared() > 1.f;

		float Heading = Motion.Heading;
		FVector2f Velocity = FVector2f::ZeroVector;
		float Duration = 0.f;

		if (IsStrictlyInside(Inner, Pos))
		{
			// Open ground. Heading: the previous one plus a small turn (triangular distribution: small turns are the most likely).
			const float MaxTurn = FMath::DegreesToRadians(Tuning.MaxTurnDeg);
			const float TurnA = CrowdMath::Random01(Motion.Rng);
			const float TurnB = CrowdMath::Random01(Motion.Rng);
			Heading = WrapRadians(Heading + (TurnA + TurnB - 1.f) * MaxTurn);

			// Speed: the agent's own cruise speed with a little variation. Length: random.
			const float Variation = 1.f + Tuning.SpeedVariation * (2.f * CrowdMath::Random01(Motion.Rng) - 1.f);
			const float Speed = FMath::Clamp(Motion.CruiseSpeed * Variation, Tuning.MinSpeedCmPerSec, Tuning.MaxSpeedCmPerSec);
			Duration = FMath::Lerp(Tuning.MinRetargetSec, Tuning.MaxRetargetSec, CrowdMath::Random01(Motion.Rng));

			float SinH = 0.f;
			float CosH = 1.f;
			FMath::SinCos(&SinH, &CosH, Heading);
			Velocity = FVector2f(CosH * Speed, SinH * Speed);

			// Cut the segment where it would run into the band, so that the turn starts exactly at the band edge.
			const float Leave = TimeToLeave(Inner, Pos, Velocity);
			if (Leave < Duration)
			{
				Duration = FMath::Max(Leave + BoundaryOvershootSec, MinSegmentSec);
			}
		}
		else
		{
			// In the band: one chord of the circle the agent is on. No random numbers: the arc is fully determined by the
			// entry, which is what makes it safe (see the file comment).
			const float Rate = WallTurnRate(Tuning);
			const float Speed = FMath::Clamp(Motion.CruiseSpeed, Tuning.MinSpeedCmPerSec, Tuning.MaxSpeedCmPerSec);
			const float Radius = Speed / Rate;
			const float HalfChord = 0.5f * Rate * LayerChordSec;

			// Which way to turn: the agent's own side, unless only the other side's circle gets back to open ground.
			float Omega = Rate * TurnSign; // rad/s, signed
			if (!ArcReachesOpenGround(Inner, Pos, Motion.Heading, Radius, TurnSign, HalfChord))
			{
				if (ArcReachesOpenGround(Inner, Pos, Motion.Heading, Radius, -TurnSign, HalfChord))
				{
					Omega = -Omega;
				}
				else
				{
					// Neither circle does: the agent is deeper in the band than an arc can bring it back from (after a
					// safety clamp or a tuning change at runtime). Never happens in normal operation. Steer for the
					// middle of open ground at the normal turn rate and walk straight once the agent faces it.
					const FVector2D Middle = (Inner.Min + Inner.Max) * 0.5;
					const float Inward = FMath::Atan2(static_cast<float>(Middle.Y - Pos.Y), static_cast<float>(Middle.X - Pos.X));
					Omega = FMath::Clamp(WrapRadians(Inward - Motion.Heading) / LayerChordSec, -Rate, Rate);
				}
			}

			// Velocity of the chord of an arc that turns Omega * D from the current heading: it points along the mean
			// heading and is shorter than the arc by sin(x) / x (the chord of a circle).
			auto ChordVelocity = [&](float D)
			{
				const float Half = 0.5f * Omega * D;
				const float AbsHalf = FMath::Abs(Half);
				const float Chord = Speed * (AbsHalf > 1.0e-4f ? FMath::Sin(AbsHalf) / AbsHalf : 1.f);
				float SinM = 0.f;
				float CosM = 1.f;
				FMath::SinCos(&SinM, &CosM, Motion.Heading + Half);
				return FVector2f(CosM * Chord, SinM * Chord);
			};

			Duration = LayerChordSec;
			Velocity = ChordVelocity(Duration);

			// Cut the chord where it comes back to open ground, so that the straight walk starts exactly at the band edge.
			const float Enter = TimeToEnter(Inner, Pos, Velocity);
			if (Enter < Duration)
			{
				Duration = FMath::Clamp(Enter + BoundaryOvershootSec, MinSegmentSec, LayerChordSec);
				Velocity = ChordVelocity(Duration);
			}
			Heading = WrapRadians(Motion.Heading + Omega * Duration);
		}

		// A pause now and then. Never two in a row.
		if (bWasWalking && CrowdMath::Random01(Motion.Rng) < PauseChance(Tuning, Duration))
		{
			Motion.Velocity = FVector2f::ZeroVector; // Motion.Heading stays: the agent walks on from where it faced
			Motion.RetargetTimer = FMath::Lerp(Tuning.MinIdleSec, Tuning.MaxIdleSec, CrowdMath::Random01(Motion.Rng));
			return;
		}

		Motion.Heading = Heading;
		Motion.Velocity = Velocity;
		Motion.RetargetTimer = Duration;
	}

	/**
	 * The initial state of a brand new agent (UCrowdSubsystem::MakeAgent). TAgent is FCrowdSavedAgent or a test double:
	 * Location, Velocity, RetargetTimer, Rng, Heading, CruiseSpeed. Seed decides everything, so the same region always
	 * spawns the same crowd.
	 *   * The agent starts on open ground (SpawnBox), never inside the band along the region border.
	 *   * Heading and cruise speed are its own; a share of agents (the long-run share) starts paused, so a freshly
	 *     spawned region does not look like a crowd that all set off at the same moment.
	 */
	template <typename TAgent>
	inline void InitNewAgent(TAgent& Agent, uint32 Seed, const FBox2& Home, const FCrowdTuning& Tuning)
	{
		uint32 State = CrowdMath::NonZeroSeed(CrowdMath::Hash32(Seed));

		const FBox2 Start = SpawnBox(Home, Tuning);
		const double RX = CrowdMath::Random01(State);
		const double RY = CrowdMath::Random01(State);
		Agent.Location = FVector2D(Start.Min.X + RX * (Start.Max.X - Start.Min.X), Start.Min.Y + RY * (Start.Max.Y - Start.Min.Y));

		Agent.Heading = (2.f * CrowdMath::Random01(State) - 1.f) * UE_PI;
		Agent.CruiseSpeed = FMath::Lerp(Tuning.MinSpeedCmPerSec, Tuning.MaxSpeedCmPerSec, CrowdMath::Random01(State));

		const bool bStartsIdle = CrowdMath::Random01(State) < SteadyStateIdleShare(Tuning);
		const float IdleRoll = CrowdMath::Random01(State);
		Agent.Velocity = FVector2f::ZeroVector;
		Agent.RetargetTimer = bStartsIdle ? IdleRoll * Tuning.MaxIdleSec : 0.f; // 0 = plans its first walking segment on the first step
		Agent.Rng = CrowdMath::NonZeroSeed(State);
	}

	/**
	 * Advances one agent by DeltaSeconds (one frame, or the time it accumulated while the time slicer skipped it).
	 * Splits the step at segment boundaries, so the result does not depend on how the time is cut into steps.
	 * Returns true if the safety clamp at the very end had to act (it should never: the tests assert that).
	 */
	template <typename TMotion>
	inline bool Advance(FVector2D& Pos, TMotion& Motion, float DeltaSeconds, const FBox2& Home, const FCrowdTuning& Tuning, float TurnSign)
	{
		// Every segment except the first and the last of a step is at least MinSegmentSec long (walks and pauses have a
		// floor in the settings, cut walks and chords one in the planner), so this many iterations get through the whole
		// step: 7 for the largest step the processor makes (MaxStepDeltaSec = 0.5 s). A fixed 4 was not enough for an agent
		// that skims along the edge of the band, where cut segments follow each other every 0.1 s.
		const int32 MaxIterations = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / MinSegmentSec) + 2, 4, 64);

		float Remaining = DeltaSeconds;
		for (int32 Iteration = 0; Iteration < MaxIterations && Remaining > 0.f; ++Iteration)
		{
			if (Motion.RetargetTimer <= 0.f)
			{
				BeginSegment(Pos, Motion, Home, Tuning, TurnSign);
			}

			const float Slice = FMath::Min(Remaining, Motion.RetargetTimer);
			Pos += FVector2D(Motion.Velocity.X, Motion.Velocity.Y) * static_cast<double>(Slice);
			Motion.RetargetTimer -= Slice;
			Remaining -= Slice;
		}

		if (Remaining > 0.f)
		{
			// Only possible for a step far beyond MaxStepDeltaSec. Never drop time silently.
			Pos += FVector2D(Motion.Velocity.X, Motion.Velocity.Y) * static_cast<double>(Remaining);
			Motion.RetargetTimer -= Remaining;
		}

		if (Motion.RetargetTimer <= 0.f)
		{
			BeginSegment(Pos, Motion, Home, Tuning, TurnSign); // so the next snapshot already carries the new velocity
		}

		// Safety net: the planner keeps agents inside, this only catches what it cannot foresee (a tuning change at runtime, say).
		bool bClamped = false;
		if (Pos.X < Home.Min.X)      { Pos.X = Home.Min.X; Motion.Velocity.X = FMath::Max(Motion.Velocity.X, 0.f); bClamped = true; }
		else if (Pos.X > Home.Max.X) { Pos.X = Home.Max.X; Motion.Velocity.X = FMath::Min(Motion.Velocity.X, 0.f); bClamped = true; }
		if (Pos.Y < Home.Min.Y)      { Pos.Y = Home.Min.Y; Motion.Velocity.Y = FMath::Max(Motion.Velocity.Y, 0.f); bClamped = true; }
		else if (Pos.Y > Home.Max.Y) { Pos.Y = Home.Max.Y; Motion.Velocity.Y = FMath::Min(Motion.Velocity.Y, 0.f); bClamped = true; }

		if (bClamped)
		{
			Motion.RetargetTimer = 0.f; // plan again at the next step
		}
		return bClamped;
	}
}
