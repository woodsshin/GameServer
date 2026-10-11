#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"

/**
 * Spawn plans: WHERE the agents of a region start.
 *
 * A region that was never populated asks a planner (today: a PCG graph, see CrowdPCGPlanner.h) for candidate
 * positions. The planner answers with plain points. Everything that decides what becomes an agent lives in this file, as
 * pure functions without UObject or engine state (unit tested in Tests/CrowdPlanTests.cpp):
 *
 *   * A point is only used if it lies on open ground (CrowdWander::SpawnBox). An agent that starts inside the band along
 *     the region border would break the assumptions of the wander model (see CrowdWander.h).
 *   * More points than AgentsPerRegion are thinned out by a hash of the point's own seed. The same graph output always
 *     keeps the same points, whatever order the graph delivered them in, and the thinning is uniform, so the density the
 *     designer painted survives. AgentsPerRegion is therefore the CAP of a planned region, not its exact size.
 *   * Everything else about an agent (cruise speed, first pause, own random stream, heading) comes from the same
 *     CrowdWander::InitNewAgent that the plain random crowd uses. A planned agent differs from a random one only in
 *     where it stands (and optionally which way it faces).
 */

/** One candidate start position as delivered by a planner. World space, centimetres. */
struct FCrowdPlanPoint
{
	FVector2D Location = FVector2D::ZeroVector;

	/** Facing of the point in radians. Only used when the project asks for it (bPCGUsePointYaw). */
	float YawRad = 0.f;

	/** The point's own seed (PCG: point seed). Drives the thinning and the agent's random stream. */
	uint32 Seed = 0;

	/** PCG density. A point with a density of 0 or less is "not there". */
	float Density = 1.f;
};

/** What the subsystem asks a planner for: the population of ONE region. */
struct FCrowdPlanRequest
{
	FIntPoint Coord = FIntPoint::ZeroValue;

	/** World position of the lower corner of the region. */
	FVector2D Min = FVector2D::ZeroVector;
	double SizeCm = 12800.0;

	/** The region seed: the same region always asks with the same seed. */
	uint32 Seed = 1;

	int32 MaxAgents = 400;
};

enum class ECrowdPlanStatus : uint8
{
	Ok,
	Failed,
	TimedOut,
};

struct FCrowdPlanResult
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	ECrowdPlanStatus Status = ECrowdPlanStatus::Failed;
	TArray<FCrowdPlanPoint> Points;

	/** From the request to the answer, including the time spent waiting for a free graph slot. */
	double LatencySec = 0.0;
};

namespace CrowdPlan
{
	/** What happened to the points of one plan. Received = NotFinite + NoDensity + OffGround + Thinned + Created. */
	struct FBuildStats
	{
		int32 Received = 0;
		int32 NotFinite = 0; // NaN or infinite position
		int32 NoDensity = 0; // density of 0 or less
		int32 OffGround = 0; // outside the spawn box: in the band along the region border, or outside the region
		int32 Thinned = 0;   // dropped to respect MaxAgents
		int32 Created = 0;
	};

	/** Order independent selection key of a point: the points with the smallest keys are kept first. */
	FORCEINLINE uint32 PointKey(const FCrowdPlanPoint& Point, uint32 RegionSeed)
	{
		return CrowdMath::Hash32(Point.Seed ^ CrowdMath::Hash32(RegionSeed));
	}

	/**
	 * Turns the points of a plan into persisted agent records (FCrowdSavedAgent is the same record the region keeps
	 * for a despawned crowd, so a planned population is spawned by exactly the code that restores a saved one).
	 *
	 * Deterministic: for the same set of points the result is the same, in the same order, whatever order the points
	 * come in. NextNetId is advanced by the number of agents created.
	 */
	inline FBuildStats BuildAgents(
		TConstArrayView<FCrowdPlanPoint> Points,
		const CrowdWander::FBox2& Home,
		uint32 RegionSeed,
		int32 MaxAgents,
		bool bUsePointYaw,
		const FCrowdTuning& Tuning,
		uint32& NextNetId,
		TArray<FCrowdSavedAgent>& OutAgents)
	{
		FBuildStats Stats;
		Stats.Received = Points.Num();
		OutAgents.Reset();

		const CrowdWander::FBox2 Start = CrowdWander::SpawnBox(Home, Tuning);

		struct FKeyed
		{
			uint32 Key = 0;
			int32 Index = 0;
		};

		TArray<FKeyed> Keep;
		Keep.Reserve(Points.Num());
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const FCrowdPlanPoint& Point = Points[i];

			if (!FMath::IsFinite(Point.Location.X) || !FMath::IsFinite(Point.Location.Y))
			{
				++Stats.NotFinite;
				continue;
			}
			if (!(Point.Density > 0.f)) // written this way so that a NaN density is rejected as well
			{
				++Stats.NoDensity;
				continue;
			}
			if (!CrowdWander::IsStrictlyInside(Start, Point.Location))
			{
				++Stats.OffGround;
				continue;
			}

			FKeyed Keyed;
			Keyed.Key = PointKey(Point, RegionSeed);
			Keyed.Index = i;
			Keep.Add(Keyed);
		}

		// Total order (key, then position): the result does not depend on the order the graph delivered the points in.
		Keep.Sort([&Points](const FKeyed& A, const FKeyed& B)
		{
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			const FVector2D& LocA = Points[A.Index].Location;
			const FVector2D& LocB = Points[B.Index].Location;
			if (LocA.X != LocB.X)
			{
				return LocA.X < LocB.X;
			}
			return LocA.Y < LocB.Y;
		});

		const int32 Count = FMath::Min(Keep.Num(), FMath::Max(MaxAgents, 0));
		Stats.Thinned = Keep.Num() - Count;
		Stats.Created = Count;

		OutAgents.Reserve(Count);
		for (int32 n = 0; n < Count; ++n)
		{
			const FCrowdPlanPoint& Point = Points[Keep[n].Index];

			FCrowdSavedAgent Agent;
			Agent.NetId = NextNetId++;

			// The agent's own random stream depends on the region, its point and its place in the (deterministic) order, so
			// two points that happen to carry the same seed do not end up walking in lockstep.
			const uint32 AgentSeed = CrowdMath::Hash32(RegionSeed ^ Point.Seed) + static_cast<uint32>(n) * 2654435761u;
			CrowdWander::InitNewAgent(Agent, AgentSeed, Home, Tuning);

			Agent.Location = Point.Location;
			if (bUsePointYaw)
			{
				Agent.Heading = CrowdWander::WrapRadians(Point.YawRad);
			}

			OutAgents.Add(Agent);
		}

		return Stats;
	}
}
