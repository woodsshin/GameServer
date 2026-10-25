// Unit tests for the spawn plan helpers (Crowd/CrowdPlan.h): what becomes an agent when a PCG graph delivers points.
// Run in the editor:  Session Frontend > Automation > filter "MassBubble.Crowd.Plan"
// or headless:        UnrealEditor-Cmd MassBubble.uproject -ExecCmds="Automation RunTests MassBubble.Crowd.Plan; Quit" -unattended -nullrhi -log
//
// Like the rest of the crowd logic these functions need no world, no Mass entity manager and no PCG: the planner hands over
// plain FCrowdPlanPoint records, so everything that decides which of them become agents can be tested like this. The part
// that talks to PCG (CrowdPCGHost.cpp / CrowdPCGPlanner.cpp) is only a thin shell around a graph run; check it in the
// editor with opt.crowd.PCG.Stats.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdPlan.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"

#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace CrowdPlanTestSupport
{
	constexpr double RegionCm = 12800.0;
	constexpr uint32 RegionSeed = 777u;

	inline CrowdWander::FBox2 Region()
	{
		return CrowdWander::FBox2{ FVector2D(0.0, 0.0), FVector2D(RegionCm, RegionCm) };
	}

	/** The facing a test point gets: a pure function of its position, so a test can tell which point an agent came from. */
	inline float YawOf(const FVector2D& Location)
	{
		return static_cast<float>((Location.X / RegionCm * 2.0 - 1.0) * UE_DOUBLE_PI);
	}

	/**
	 * Points like a graph delivers them: uniform over the WHOLE region (so a share of them lies in the band along the
	 * border and is rejected), each with its own seed, as PCG would compute it from the position.
	 */
	inline TArray<FCrowdPlanPoint> UniformPoints(int32 Num, int32 RandomSeed)
	{
		FRandomStream Rng(RandomSeed);
		TArray<FCrowdPlanPoint> Points;
		Points.SetNum(Num);
		for (int32 i = 0; i < Num; ++i)
		{
			Points[i].Location = FVector2D(Rng.FRand() * RegionCm, Rng.FRand() * RegionCm);
			Points[i].YawRad = YawOf(Points[i].Location);
			Points[i].Seed = CrowdMath::Hash32(static_cast<uint32>(i) + 1u);
			Points[i].Density = 1.f;
		}
		return Points;
	}

	inline bool SameAgent(const FCrowdSavedAgent& A, const FCrowdSavedAgent& B)
	{
		return A.NetId == B.NetId && A.Location.X == B.Location.X && A.Location.Y == B.Location.Y
			&& A.Rng == B.Rng && A.Heading == B.Heading && A.CruiseSpeed == B.CruiseSpeed && A.RetargetTimer == B.RetargetTimer;
	}
}

// =================================================================================================
// What is rejected
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdPlanFilterTest, "MassBubble.Crowd.Plan.Filter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdPlanFilterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdPlanTestSupport;

	FCrowdTuning Tuning;
	const CrowdWander::FBox2 Start = CrowdWander::SpawnBox(Region(), Tuning);
	const double Nan = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();

	TArray<FCrowdPlanPoint> Points;
	auto Add = [&Points](double X, double Y, float Density = 1.f)
	{
		FCrowdPlanPoint Point;
		Point.Location = FVector2D(X, Y);
		Point.Density = Density;
		Point.Seed = 100u + static_cast<uint32>(Points.Num());
		Points.Add(Point);
	};

	Add(6400.0, 6400.0);                                   // kept: middle of the open ground
	Add(Start.Min.X + 10.0, Start.Min.Y + 10.0);           // kept: just inside the spawn box
	Add(Start.Min.X - 10.0, 6400.0);                       // off ground: just inside the band along the border
	Add(100.0, 6400.0);                                    // off ground: deep in the band
	Add(-500.0, 6400.0);                                   // off ground: outside the region
	Add(RegionCm + 500.0, RegionCm + 500.0);               // off ground: outside the region
	Add(3000.0, 3000.0, 0.f);                              // no density
	Add(3100.0, 3000.0, -1.f);                             // no density
	Add(3200.0, 3000.0, static_cast<float>(Nan));          // no density (NaN)
	Add(Nan, 3000.0);                                      // not finite
	Add(Inf, 3000.0);                                      // not finite

	uint32 NextNetId = 100u;
	TArray<FCrowdSavedAgent> Agents;
	const CrowdPlan::FBuildStats Stats = CrowdPlan::BuildAgents(Points, Region(), RegionSeed, 400, false, Tuning, NextNetId, Agents);

	AddInfo(FString::Printf(TEXT("%d points: created %d, off open ground %d, no density %d, not finite %d, thinned %d"),
		Stats.Received, Stats.Created, Stats.OffGround, Stats.NoDensity, Stats.NotFinite, Stats.Thinned));

	TestEqual(TEXT("points received"), Stats.Received, 11);
	TestEqual(TEXT("agents created"), Stats.Created, 2);
	TestEqual(TEXT("... the array holds as many"), Agents.Num(), 2);
	TestEqual(TEXT("points off the open ground (band along the border, outside the region)"), Stats.OffGround, 4);
	TestEqual(TEXT("points without density (0, negative, NaN)"), Stats.NoDensity, 3);
	TestEqual(TEXT("points that are not finite"), Stats.NotFinite, 2);
	TestEqual(TEXT("nothing thinned below the cap"), Stats.Thinned, 0);
	TestEqual(TEXT("every point is accounted for"), Stats.Created + Stats.OffGround + Stats.NoDensity + Stats.NotFinite + Stats.Thinned, Stats.Received);

	bool bAllOnOpenGround = true;
	for (const FCrowdSavedAgent& Agent : Agents)
	{
		bAllOnOpenGround = bAllOnOpenGround && CrowdWander::IsStrictlyInside(Start, Agent.Location);
	}
	TestTrue(TEXT("every agent stands on open ground (CrowdWander::SpawnBox)"), bAllOnOpenGround);

	TestEqual(TEXT("net ids continue where the subsystem stopped"), static_cast<int32>(NextNetId), 102);
	TestTrue(TEXT("... in order"), Agents.Num() == 2 && Agents[0].NetId == 100u && Agents[1].NetId == 101u);

	// ---- nothing to do ----
	uint32 Next = 1u;
	const CrowdPlan::FBuildStats Empty = CrowdPlan::BuildAgents(TArray<FCrowdPlanPoint>(), Region(), RegionSeed, 400, false, Tuning, Next, Agents);
	TestTrue(TEXT("no points => no agents (an empty region stays empty)"), Empty.Created == 0 && Agents.Num() == 0 && Next == 1u);

	const TArray<FCrowdPlanPoint> Many = UniformPoints(500, 1);
	CrowdPlan::BuildAgents(Many, Region(), RegionSeed, 0, false, Tuning, Next, Agents);
	TestTrue(TEXT("a cap of 0 => no agents"), Agents.Num() == 0 && Next == 1u);
	CrowdPlan::BuildAgents(Many, Region(), RegionSeed, -5, false, Tuning, Next, Agents);
	TestTrue(TEXT("a negative cap is the same as 0"), Agents.Num() == 0 && Next == 1u);

	return true;
}

// =================================================================================================
// Thinning to the cap: deterministic, order independent, uniform
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdPlanThinningTest, "MassBubble.Crowd.Plan.Thinning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdPlanThinningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdPlanTestSupport;

	FCrowdTuning Tuning;
	constexpr int32 Cap = 400;
	const TArray<FCrowdPlanPoint> Points = UniformPoints(3000, 1234);

	uint32 NextA = 1u;
	TArray<FCrowdSavedAgent> A;
	const CrowdPlan::FBuildStats StatsA = CrowdPlan::BuildAgents(Points, Region(), RegionSeed, Cap, false, Tuning, NextA, A);

	AddInfo(FString::Printf(TEXT("%d points: %d off open ground, %d thinned, %d agents"), StatsA.Received, StatsA.OffGround, StatsA.Thinned, StatsA.Created));
	TestTrue(TEXT("test is not vacuous: more candidates than the cap"), StatsA.Thinned > 1000);
	TestEqual(TEXT("exactly the cap is created"), A.Num(), Cap);
	TestEqual(TEXT("net ids are consecutive"), static_cast<int32>(NextA), 1 + Cap);

	bool bConsecutive = true;
	for (int32 i = 0; i < A.Num(); ++i)
	{
		bConsecutive = bConsecutive && A[i].NetId == static_cast<uint32>(i + 1);
	}
	TestTrue(TEXT("agent i has net id i + 1"), bConsecutive);

	// ---- the same points in another order give the very same crowd ----
	TArray<FCrowdPlanPoint> Shuffled = Points;
	FRandomStream Rng(99);
	for (int32 i = Shuffled.Num() - 1; i > 0; --i)
	{
		Shuffled.Swap(i, Rng.RandRange(0, i));
	}
	uint32 NextB = 1u;
	TArray<FCrowdSavedAgent> B;
	CrowdPlan::BuildAgents(Shuffled, Region(), RegionSeed, Cap, false, Tuning, NextB, B);

	bool bSame = A.Num() == B.Num();
	for (int32 i = 0; bSame && i < A.Num(); ++i)
	{
		bSame = SameAgent(A[i], B[i]);
	}
	TestTrue(TEXT("the order the graph delivers the points in does not matter"), bSame);

	// ---- another region (seed) keeps another subset of the same points ----
	uint32 NextC = 1u;
	TArray<FCrowdSavedAgent> C;
	CrowdPlan::BuildAgents(Points, Region(), RegionSeed + 1u, Cap, false, Tuning, NextC, C);
	int32 AlsoInC = 0;
	for (const FCrowdSavedAgent& AgentA : A)
	{
		for (const FCrowdSavedAgent& AgentC : C)
		{
			if (AgentA.Location.X == AgentC.Location.X && AgentA.Location.Y == AgentC.Location.Y)
			{
				++AlsoInC;
				break;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("subsets for two region seeds share %d of %d agents"), AlsoInC, Cap));
	TestTrue(TEXT("a different seed keeps a (mostly) different subset"), AlsoInC < Cap / 2);

	// ---- below the cap nothing is dropped ----
	uint32 NextD = 1u;
	TArray<FCrowdSavedAgent> D;
	const CrowdPlan::FBuildStats StatsD = CrowdPlan::BuildAgents(Points, Region(), RegionSeed, 100000, false, Tuning, NextD, D);
	TestEqual(TEXT("without a binding cap every point on open ground becomes an agent"), StatsD.Created, StatsD.Received - StatsD.OffGround);
	TestEqual(TEXT("... and nothing is thinned"), StatsD.Thinned, 0);

	// ---- uniform: thinning keeps the density the graph painted ----
	// 12000 uniform points, a cap of 400: every quarter of the open ground should keep about a hundred.
	const TArray<FCrowdPlanPoint> Dense = UniformPoints(12000, 4321);
	uint32 NextE = 1u;
	TArray<FCrowdSavedAgent> E;
	CrowdPlan::BuildAgents(Dense, Region(), RegionSeed, Cap, false, Tuning, NextE, E);
	int32 Quarter[4] = { 0, 0, 0, 0 };
	for (const FCrowdSavedAgent& Agent : E)
	{
		Quarter[(Agent.Location.X > 0.5 * RegionCm ? 1 : 0) + (Agent.Location.Y > 0.5 * RegionCm ? 2 : 0)]++;
	}
	AddInfo(FString::Printf(TEXT("quarters of the region keep %d / %d / %d / %d of %d agents"), Quarter[0], Quarter[1], Quarter[2], Quarter[3], E.Num()));
	bool bBalanced = E.Num() == Cap;
	for (const int32 Count : Quarter)
	{
		bBalanced = bBalanced && Count > 100 - 35 && Count < 100 + 35;
	}
	TestTrue(TEXT("thinning is uniform over the region (about a quarter of the agents in each quarter)"), bBalanced);

	// ---- a graph that does not compute seeds (every point carries 0) still gives distinct, deterministic agents ----
	TArray<FCrowdPlanPoint> NoSeeds = UniformPoints(1500, 55);
	for (FCrowdPlanPoint& Point : NoSeeds)
	{
		Point.Seed = 0u;
	}
	uint32 NextF = 1u;
	uint32 NextG = 1u;
	TArray<FCrowdSavedAgent> F;
	TArray<FCrowdSavedAgent> G;
	CrowdPlan::BuildAgents(NoSeeds, Region(), RegionSeed, Cap, false, Tuning, NextF, F);
	TArray<FCrowdPlanPoint> NoSeedsShuffled = NoSeeds;
	for (int32 i = NoSeedsShuffled.Num() - 1; i > 0; --i)
	{
		NoSeedsShuffled.Swap(i, Rng.RandRange(0, i));
	}
	CrowdPlan::BuildAgents(NoSeedsShuffled, Region(), RegionSeed, Cap, false, Tuning, NextG, G);

	bool bSameWithoutSeeds = F.Num() == Cap && F.Num() == G.Num();
	for (int32 i = 0; bSameWithoutSeeds && i < F.Num(); ++i)
	{
		bSameWithoutSeeds = SameAgent(F[i], G[i]);
	}
	TArray<uint32> RngStates;
	for (const FCrowdSavedAgent& Agent : F)
	{
		RngStates.Add(Agent.Rng);
	}
	RngStates.Sort();
	int32 SharedStreams = 0;
	for (int32 i = 1; i < RngStates.Num(); ++i)
	{
		SharedStreams += (RngStates[i] == RngStates[i - 1]) ? 1 : 0;
	}
	TestTrue(TEXT("points without seeds: still the same crowd in any input order (ties are broken by position)"), bSameWithoutSeeds);
	TestEqual(TEXT("... and no two agents share a random stream (they do not walk in lockstep)"), SharedStreams, 0);

	return true;
}

// =================================================================================================
// What an agent made from a point looks like
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdPlanAgentTest, "MassBubble.Crowd.Plan.Agent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdPlanAgentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdPlanTestSupport;

	FCrowdTuning Tuning;
	const TArray<FCrowdPlanPoint> Points = UniformPoints(2000, 2468);

	uint32 NextOff = 1u;
	uint32 NextOn = 1u;
	TArray<FCrowdSavedAgent> YawOff;
	TArray<FCrowdSavedAgent> YawOn;
	CrowdPlan::BuildAgents(Points, Region(), RegionSeed, 400, false, Tuning, NextOff, YawOff);
	CrowdPlan::BuildAgents(Points, Region(), RegionSeed, 400, true, Tuning, NextOn, YawOn);
	TestTrue(TEXT("test is not vacuous"), YawOff.Num() == 400 && YawOn.Num() == 400);

	// ---- the state of a new agent is the one InitNewAgent gives every agent ----
	bool bSpeedInRange = true;
	bool bRngValid = true;
	bool bStanding = true;
	bool bTimerValid = true;
	bool bHeadingValid = true;
	for (const FCrowdSavedAgent& Agent : YawOff)
	{
		bSpeedInRange = bSpeedInRange && Agent.CruiseSpeed >= Tuning.MinSpeedCmPerSec && Agent.CruiseSpeed <= Tuning.MaxSpeedCmPerSec;
		bRngValid = bRngValid && Agent.Rng != 0u;
		bStanding = bStanding && Agent.Velocity.IsNearlyZero();
		bTimerValid = bTimerValid && Agent.RetargetTimer >= 0.f && Agent.RetargetTimer <= Tuning.MaxIdleSec;
		bHeadingValid = bHeadingValid && Agent.Heading >= -UE_PI - 1.0e-4f && Agent.Heading <= UE_PI + 1.0e-4f;
	}
	TestTrue(TEXT("cruise speed lies in the configured range"), bSpeedInRange);
	TestTrue(TEXT("the random stream is never 0 (xorshift would stay at 0 for ever)"), bRngValid);
	TestTrue(TEXT("a new agent stands (its first segment is planned on the first step)"), bStanding);
	TestTrue(TEXT("the first timer is 0 or a pause that is not longer than the longest pause"), bTimerValid);
	TestTrue(TEXT("the heading is a valid angle"), bHeadingValid);

	// ---- heading: random by default, the rotation of the point if the project asks for it ----
	int32 FacingItsPointWhenOff = 0;
	bool bFollowsPoint = true;
	for (int32 i = 0; i < YawOn.Num(); ++i)
	{
		const float Wanted = CrowdWander::WrapRadians(YawOf(YawOn[i].Location));
		bFollowsPoint = bFollowsPoint && FMath::Abs(YawOn[i].Heading - Wanted) < 1.0e-4f;

		const float WantedOff = CrowdWander::WrapRadians(YawOf(YawOff[i].Location));
		FacingItsPointWhenOff += (FMath::Abs(YawOff[i].Heading - WantedOff) < 1.0e-3f) ? 1 : 0;
	}
	TestTrue(TEXT("bPCGUsePointYaw: every agent faces the way its point is rotated"), bFollowsPoint);
	TestTrue(TEXT("default: the heading is random, it does not follow the point"), FacingItsPointWhenOff < 10);

	// The position is the point's in both cases: the yaw option changes nothing else.
	bool bSamePlaces = YawOff.Num() == YawOn.Num();
	for (int32 i = 0; bSamePlaces && i < YawOff.Num(); ++i)
	{
		bSamePlaces = YawOff[i].Location.X == YawOn[i].Location.X && YawOff[i].Location.Y == YawOn[i].Location.Y
			&& YawOff[i].CruiseSpeed == YawOn[i].CruiseSpeed && YawOff[i].Rng == YawOn[i].Rng;
	}
	TestTrue(TEXT("the yaw option only changes the heading"), bSamePlaces);

	return true;
}

// =================================================================================================
// The wander model accepts what the plan produces
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdPlanWanderTest, "MassBubble.Crowd.Plan.Wander",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdPlanWanderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdPlanTestSupport;

	// A planned crowd is walked by the same CrowdWander::Advance as a random one. The plan only has to keep its promise:
	// agents start on open ground. Then the planner keeps them inside the region WITHOUT the safety clamp, however the graph
	// distributed them (here: uniformly, and in a tight cluster right next to the border band, the worst case for the arcs).
	FCrowdTuning Tuning;
	const CrowdWander::FBox2 Start = CrowdWander::SpawnBox(Region(), Tuning);

	TArray<FCrowdPlanPoint> Points = UniformPoints(1000, 8642);
	FRandomStream Rng(3);
	for (int32 i = 0; i < 400; ++i)
	{
		FCrowdPlanPoint Point;
		Point.Location = FVector2D(Start.Min.X + 20.0 + Rng.FRand() * 150.0, Start.Min.Y + 20.0 + Rng.FRand() * 150.0);
		Point.Seed = 50000u + static_cast<uint32>(i);
		Points.Add(Point);
	}

	uint32 NextNetId = 1u;
	TArray<FCrowdSavedAgent> Agents;
	CrowdPlan::BuildAgents(Points, Region(), RegionSeed, 400, false, Tuning, NextNetId, Agents);
	TestTrue(TEXT("test is not vacuous"), Agents.Num() == 400);

	int32 ClampHits = 0;
	int32 Outside = 0;
	double ClosestCm = 1.0e9;
	constexpr float Dt = 1.f / 20.f;
	for (FCrowdSavedAgent& Agent : Agents)
	{
		const float TurnSign = CrowdWander::TurnSignForId(Agent.NetId);
		for (int32 Step = 0; Step < 20 * 60; ++Step) // one minute
		{
			ClampHits += CrowdWander::Advance(Agent.Location, Agent, Dt, Region(), Tuning, TurnSign) ? 1 : 0;

			const double ToBorder = FMath::Min(FMath::Min(Agent.Location.X, RegionCm - Agent.Location.X), FMath::Min(Agent.Location.Y, RegionCm - Agent.Location.Y));
			ClosestCm = FMath::Min(ClosestCm, ToBorder);
			Outside += (ToBorder < 0.0) ? 1 : 0;
		}
	}

	AddInfo(FString::Printf(TEXT("%d planned agents walked for a minute: safety clamp hits %d, steps outside the region %d, closest to the border %.0f cm"),
		Agents.Num(), ClampHits, Outside, ClosestCm));
	TestEqual(TEXT("planned agents the safety clamp had to rescue"), ClampHits, 0);
	TestEqual(TEXT("planned agents that left their region"), Outside, 0);
	TestTrue(TEXT("nobody comes closer to the border than the margin"), ClosestCm >= CrowdWander::WallMarginCm);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
