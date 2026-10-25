#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdLodTable.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"
#include "Net/CrowdNetMath.h"

/**
 * One simulated agent. A region keeps its agents in one contiguous array and an agent never leaves it, so the index in
 * that array identifies the agent in the region's replicated state as well.
 *
 * The fields every pass reads (id, tier, position) come first. The Sent* members are the dequantized state the region's
 * replicator holds, i.e. what the clients know of the agent (dead reckoning, see Net/CrowdNetMath.h).
 */
struct FCrowdAgentSim
{
	uint32 NetId = 0;
	ECrowdLOD Tier = ECrowdLOD::Off;
	FVector2D Location = FVector2D::ZeroVector;
	FVector2f Velocity = FVector2f::ZeroVector;
	float RetargetTimer = 0.f;
	uint32 Rng = 1;
	float Heading = 0.f;
	float CruiseSpeed = 140.f;

	/** Delta time accumulated while the time slicer skipped this agent. */
	float PendingDelta = 0.f;

	FVector2D SentPos = FVector2D::ZeroVector;
	FVector2f SentVel = FVector2f::ZeroVector;
	double SentTime = 0.0;
};

/**
 * The simulation of the agents of one region: LOD tier, movement and the decision what to publish. Pure functions on
 * plain data: no UObject, no engine state, no world. The subsystem calls them once per region and frame; regions do not
 * share data, so different regions can run on different threads.
 */
namespace CrowdSim
{
	FORCEINLINE FCrowdAgentSim FromSaved(const FCrowdSavedAgent& Saved)
	{
		FCrowdAgentSim Agent;
		Agent.NetId = Saved.NetId;
		Agent.Location = Saved.Location;
		Agent.Velocity = Saved.Velocity;
		Agent.RetargetTimer = Saved.RetargetTimer;
		Agent.Rng = CrowdMath::NonZeroSeed(Saved.Rng);
		Agent.Heading = Saved.Heading;
		Agent.CruiseSpeed = Saved.CruiseSpeed;
		return Agent;
	}

	FORCEINLINE FCrowdSavedAgent ToSaved(const FCrowdAgentSim& Agent)
	{
		FCrowdSavedAgent Saved;
		Saved.NetId = Agent.NetId;
		Saved.Location = Agent.Location;
		Saved.Velocity = Agent.Velocity;
		Saved.RetargetTimer = Agent.RetargetTimer;
		Saved.Rng = Agent.Rng;
		Saved.Heading = Agent.Heading;
		Saved.CruiseSpeed = Agent.CruiseSpeed;
		return Saved;
	}

	// ---------------------------------------------------------------------------------------------
	// LOD
	// ---------------------------------------------------------------------------------------------

	struct FClassifyParams
	{
		const FCrowdTuning* Tuning = nullptr;

		/** Per-region bounds (Crowd/CrowdLodTable.h). Null: every agent measures the distance to every viewer. */
		const CrowdLod::FTable* Table = nullptr;
		TConstArrayView<FVector2D> Viewers;

		uint32 Frame = 0;

		/** False: every agent is High. */
		bool bUseLOD = true;
	};

	/**
	 * Time-sliced LOD: each agent is re-classified every 4th frame (bucketed by NetId), with hysteresis. Adds the tier of
	 * every agent to Counts. Slot is the region's slot in the table.
	 */
	inline void ClassifyRegion(TArrayView<FCrowdAgentSim> Agents, int32 Slot, const FClassifyParams& Params, int32 (&Counts)[CrowdLODCount])
	{
		const FCrowdTuning& Tuning = *Params.Tuning;

		for (FCrowdAgentSim& Agent : Agents)
		{
			if (!Params.bUseLOD)
			{
				Agent.Tier = ECrowdLOD::High;
			}
			else if (((Params.Frame + Agent.NetId) & 3u) == 0u)
			{
				Agent.Tier = (Params.Table != nullptr)
					? Params.Table->Classify(Slot, Agent.Location, Agent.Tier, Tuning)
					: CrowdLod::ClassifyAgainst(Params.Viewers.GetData(), Params.Viewers.Num(), Agent.Location, Agent.Tier, Tuning);
			}

			++Counts[static_cast<int32>(Agent.Tier)];
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Movement
	// ---------------------------------------------------------------------------------------------

	struct FMoveParams
	{
		const FCrowdTuning* Tuning = nullptr;
		uint32 Frame = 0;
		float DeltaSeconds = 0.f;

		/** False: every agent is stepped every frame. */
		bool bTimeSlice = true;
	};

	struct FMoveStats
	{
		int32 Simulated = 0;

		/** Agents the safety clamp of the wander model had to pull back into the region (the planner keeps them inside, so 0). */
		int32 Clamped = 0;
	};

	/**
	 * Wander simulation with time slicing: an agent is stepped every N-th frame (N from its LOD tier), using the delta time
	 * it accumulated while it was skipped (Crowd/CrowdWander.h). Touches nothing but the agents it is given.
	 */
	inline FMoveStats MoveRegion(TArrayView<FCrowdAgentSim> Agents, const FIntPoint& Coord, const FMoveParams& Params)
	{
		const FCrowdTuning& Tuning = *Params.Tuning;
		const FVector2D Min = CrowdMath::RegionMin(Coord, Tuning.RegionSizeCm);
		const CrowdWander::FBox2 Home{ Min, Min + FVector2D(Tuning.RegionSizeCm, Tuning.RegionSizeCm) };

		FMoveStats Stats;
		for (FCrowdAgentSim& Agent : Agents)
		{
			// 0 = frozen, 1 = every frame, N = every N-th frame (agents are spread over frames by NetId).
			const int32 Interval = Params.bTimeSlice ? Tuning.LODIntervalFrames[static_cast<int32>(Agent.Tier)] : 1;
			if (Interval == 0)
			{
				continue;
			}
			if (Interval > 1 && ((Params.Frame + Agent.NetId) % static_cast<uint32>(Interval)) != 0u)
			{
				Agent.PendingDelta += Params.DeltaSeconds; // remember how much time this agent still owes
				continue;
			}

			const float Step = FMath::Min(Agent.PendingDelta + Params.DeltaSeconds, Tuning.MaxStepDeltaSec);
			Agent.PendingDelta = 0.f;

			if (CrowdWander::Advance(Agent.Location, Agent, Step, Home, Tuning, CrowdWander::TurnSignForId(Agent.NetId)))
			{
				++Stats.Clamped;
			}
			++Stats.Simulated;
		}
		return Stats;
	}

	// ---------------------------------------------------------------------------------------------
	// Publishing
	// ---------------------------------------------------------------------------------------------

	/** A frozen agent must not look like it is walking: clients extrapolate with the published velocity. */
	FORCEINLINE FVector2f PublishedVelocity(const FCrowdAgentSim& Agent)
	{
		return Agent.Tier == ECrowdLOD::Off ? FVector2f::ZeroVector : Agent.Velocity;
	}

	/**
	 * True when the replicated state of the agent has to be written again. The tolerance follows the tier: agents the
	 * nearest viewer can see closely (High) are held to the near error, everything else to the far error. Without dead
	 * reckoning everything that moves (or has just stopped) is published every time.
	 */
	inline bool NeedsPublish(const FCrowdAgentSim& Agent, const FVector2f& Velocity, double Now, const FCrowdTuning& Tuning, bool bDeadReckoning)
	{
		if (!bDeadReckoning)
		{
			return !Velocity.IsNearlyZero() || !Agent.SentVel.IsNearlyZero();
		}

		const float ToleranceCm = (Agent.Tier == ECrowdLOD::High) ? Tuning.NearErrorCm : Tuning.FarErrorCm;
		return CrowdNet::ShouldResend(Agent.Location, Velocity, Agent.SentPos, Agent.SentVel, Now - Agent.SentTime, ToleranceCm, Tuning.VelocityEpsCmPerSec);
	}
}
