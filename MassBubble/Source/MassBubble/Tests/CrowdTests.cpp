// Unit tests for the pure-logic parts of the crowd.
// Run in the editor:  Session Frontend > Automation > filter "MassBubble.Crowd"
// or headless:        UnrealEditor-Cmd MassBubble.uproject -ExecCmds="Automation RunTests MassBubble.Crowd; Quit" -unattended -nullrhi -log
//
// No world, Mass entity manager or network connection is needed: the algorithms live in header-only helpers
// (CrowdCellGrid.h, CrowdMath.h, CrowdNetMath.h).

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Crowd/CrowdCellGrid.h"
#include "Crowd/CrowdMath.h"
#include "Net/CrowdNetMath.h"

#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

// =================================================================================================
// Spatial grid vs. brute force
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdCellGridTest, "MassBubble.Crowd.CellGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdCellGridTest::RunTest(const FString& Parameters)
{
	constexpr double Extent = 12800.0;
	const FVector2D Min(25600.0, -12800.0); // deliberately away from the origin: catches a forgotten offset

	FRandomStream Rng(1234);
	FCrowdCellGrid Grid;
	Grid.Init(Min, Extent, 8);

	Grid.BeginBuild();
	for (int32 i = 0; i < 3000; ++i)
	{
		// The last 100 agents lie slightly OUTSIDE the region. They must be clamped into border cells, not lost.
		const double Margin = (i >= 2900) ? 600.0 : 0.0;
		const FVector2D Pos(
			Min.X - Margin + Rng.FRand() * (Extent + 2.0 * Margin),
			Min.Y - Margin + Rng.FRand() * (Extent + 2.0 * Margin));
		Grid.Add(Pos, FVector2f::ZeroVector, static_cast<uint32>(i + 1));
	}
	Grid.EndBuild();
	TestEqual(TEXT("grid keeps every agent"), Grid.Num(), 3000);

	int32 Mismatches = 0;
	int32 Duplicates = 0;
	int32 BadDistances = 0;
	int32 TotalFound = 0;

	for (int32 Query = 0; Query < 300; ++Query)
	{
		// Centres also lie outside the region, circles partially cover it or not at all.
		const FVector2D Center(
			Min.X - 4000.0 + Rng.FRand() * (Extent + 8000.0),
			Min.Y - 4000.0 + Rng.FRand() * (Extent + 8000.0));
		const double Radius = 100.0 + Rng.FRand() * 7000.0;

		TSet<uint32> Expected;
		for (const FCrowdGridAgent& Agent : Grid.GetAgents())
		{
			if (FVector2D::DistSquared(Agent.Pos, Center) <= Radius * Radius)
			{
				Expected.Add(Agent.NetId);
			}
		}

		TSet<uint32> Found;
		Grid.ForEachInCircle(Center, Radius, [&](const FCrowdGridAgent& Agent, double DistSq)
		{
			if (Found.Contains(Agent.NetId))
			{
				++Duplicates;
			}
			Found.Add(Agent.NetId);

			const double Reference = FVector2D::DistSquared(Agent.Pos, Center);
			if (!FMath::IsNearlyEqual(DistSq, Reference, FMath::Max(1.0, Reference) * 1.0e-9))
			{
				++BadDistances;
			}
		});

		bool bSame = (Found.Num() == Expected.Num());
		for (const uint32 Id : Expected)
		{
			bSame = bSame && Found.Contains(Id);
		}
		Mismatches += bSame ? 0 : 1;
		TotalFound += Found.Num();
	}

	TestEqual(TEXT("queries that differ from brute force"), Mismatches, 0);
	TestEqual(TEXT("agents reported twice"), Duplicates, 0);
	TestEqual(TEXT("wrong squared distances"), BadDistances, 0);
	TestTrue(TEXT("test is not vacuous (queries found agents)"), TotalFound > 1000);

	// The grid is rebuilt every replication tick: a second build must fully replace the first.
	Grid.BeginBuild();
	for (int32 i = 0; i < 10; ++i)
	{
		Grid.Add(FVector2D(Min.X + 100.0 * i, Min.Y + 50.0 * i), FVector2f::ZeroVector, 5000u + i);
	}
	Grid.EndBuild();

	int32 AfterRebuild = 0;
	Grid.ForEachInCircle(Min + FVector2D(Extent * 0.5, Extent * 0.5), Extent * 2.0, [&](const FCrowdGridAgent&, double) { ++AfterRebuild; });
	TestEqual(TEXT("rebuild replaces the old content"), AfterRebuild, 10);

	Grid.Clear();
	int32 AfterClear = 0;
	Grid.ForEachInCircle(Min, Extent * 2.0, [&](const FCrowdGridAgent&, double) { ++AfterClear; });
	TestEqual(TEXT("cleared grid is empty"), AfterClear, 0);

	return true;
}

// =================================================================================================
// Quantization and origin handling
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdQuantizationTest, "MassBubble.Crowd.Quantization",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdQuantizationTest::RunTest(const FString& Parameters)
{
	// ---- position: error is at most half a quantization step ----
	double MaxPosError = 0.0;
	for (double Cm = -30000.0; Cm <= 30000.0; Cm += 0.37)
	{
		const double Back = CrowdNet::DequantizeOffset(CrowdNet::QuantizeOffset(Cm));
		MaxPosError = FMath::Max(MaxPosError, FMath::Abs(Back - Cm));
	}
	TestTrue(TEXT("position round trip error <= half a step"), MaxPosError <= 0.5 * CrowdNet::PosUnitCm + 1.0e-9);

	TestEqual(TEXT("position clamps high"), static_cast<int32>(CrowdNet::QuantizeOffset(1.0e9)), 32767);
	TestEqual(TEXT("position clamps low"), static_cast<int32>(CrowdNet::QuantizeOffset(-1.0e9)), -32768);

	// ---- velocity ----
	float MaxVelError = 0.f;
	for (float V = -600.f; V <= 600.f; V += 0.77f)
	{
		const float Back = CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(V));
		MaxVelError = FMath::Max(MaxVelError, FMath::Abs(Back - V));
	}
	TestTrue(TEXT("velocity round trip error <= half a step"), MaxVelError <= 0.5f * CrowdNet::VelUnitCmPerSec + 1.0e-3f);
	TestEqual(TEXT("velocity clamps high"), static_cast<int32>(CrowdNet::QuantizeVelocity(5000.f)), 127);
	TestEqual(TEXT("velocity clamps low"), static_cast<int32>(CrowdNet::QuantizeVelocity(-5000.f)), -127);

	// ---- the invariant the wire format relies on: with the origin policy, no offset ever hits the int16 limit ----
	// Walk a viewer along a long diagonal. The origin is moved exactly like ACrowdBubble does it.
	{
		bool bInitialized = false;
		FIntPoint Lattice = FIntPoint::ZeroValue;
		int32 Rebases = 0;
		bool bAllFit = true;
		bool bAllNear = true;

		for (double D = -80000.0; D <= 80000.0; D += 123.0)
		{
			const FVector2D Viewer(D, D * 0.5);
			if (!bInitialized || CrowdNet::NeedsRebase(Viewer, CrowdNet::LatticeToWorld(Lattice)))
			{
				Lattice = CrowdNet::WorldToLattice(Viewer);
				bInitialized = true;
				++Rebases;
			}

			const FVector2D Origin = CrowdNet::LatticeToWorld(Lattice);
			bAllNear = bAllNear && !CrowdNet::NeedsRebase(Viewer, Origin);

			// Worst case agents: the four corners of the square that contains the bubble circle.
			const double R = CrowdNet::MaxBubbleRadiusCm;
			for (const FVector2D& Corner : { FVector2D(R, R), FVector2D(-R, R), FVector2D(R, -R), FVector2D(-R, -R) })
			{
				const FVector2D Offset = (Viewer + Corner) - Origin;
				const bool bFits = FMath::Abs(Offset.X / CrowdNet::PosUnitCm) < 32767.0 && FMath::Abs(Offset.Y / CrowdNet::PosUnitCm) < 32767.0;
				bAllFit = bAllFit && bFits;
			}
		}

		TestTrue(TEXT("viewer is always inside the safe zone of its origin"), bAllNear);
		TestTrue(TEXT("every agent offset fits into int16"), bAllFit);
		// 1.6 km in X and 0.8 km in Y with a 200 m lattice => roughly 10 origin moves in ~1300 steps.
		AddInfo(FString::Printf(TEXT("origin moves along the path: %d"), Rebases));
		TestTrue(TEXT("rebases are rare (about one per 200 m of travel)"), Rebases >= 8 && Rebases <= 20);
	}

	// ---- hysteresis: a viewer jittering on a lattice border must not flip the origin ----
	{
		FIntPoint Lattice = CrowdNet::WorldToLattice(FVector2D(9900.0, 0.0)); // just left of the 100 m border => lattice 0
		TestEqual(TEXT("start lattice"), Lattice.X, 0);

		int32 HysteresisRebases = 0;
		int32 NaiveFlips = 0;
		FIntPoint NaiveLattice = Lattice;

		for (int32 Step = 0; Step < 200; ++Step)
		{
			const double X = (Step % 2 == 0) ? 9700.0 : 10300.0; // +-3 m around the border at 100 m
			const FVector2D Viewer(X, 0.0);

			if (CrowdNet::NeedsRebase(Viewer, CrowdNet::LatticeToWorld(Lattice)))
			{
				Lattice = CrowdNet::WorldToLattice(Viewer);
				++HysteresisRebases;
			}

			const FIntPoint Rounded = CrowdNet::WorldToLattice(Viewer);
			if (Rounded != NaiveLattice)
			{
				NaiveLattice = Rounded;
				++NaiveFlips;
			}
		}

		TestEqual(TEXT("hysteresis: no rebase while jittering on the border"), HysteresisRebases, 0);
		TestTrue(TEXT("(for contrast) naive rounding would flip on almost every step"), NaiveFlips > 150);
	}

	return true;
}

// =================================================================================================
// Dead reckoning
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdDeadReckoningTest, "MassBubble.Crowd.DeadReckoning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdDeadReckoningTest::RunTest(const FString& Parameters)
{
	const float Tolerance = 30.f;
	const float VelEps = 25.f;

	// ---- thresholds ----
	{
		const FVector2D SentPos(0.0, 0.0);
		const FVector2f SentVel(100.f, 0.f);

		// A mover that keeps its velocity is never resent, however long it walks.
		bool bEverResent = false;
		for (double T = 0.0; T <= 20.0; T += 0.1)
		{
			const FVector2D TruePos(SentVel.X * T, 0.0);
			bEverResent = bEverResent || CrowdNet::ShouldResend(TruePos, SentVel, SentPos, SentVel, T, Tolerance, VelEps);
		}
		TestFalse(TEXT("straight walker is never resent"), bEverResent);

		// One second later the prediction says (100, 0).
		TestTrue(TEXT("31 cm off the prediction => resend"),
			CrowdNet::ShouldResend(FVector2D(100.0, 31.0), SentVel, SentPos, SentVel, 1.0, Tolerance, VelEps));
		TestFalse(TEXT("29 cm off the prediction => keep"),
			CrowdNet::ShouldResend(FVector2D(100.0, 29.0), SentVel, SentPos, SentVel, 1.0, Tolerance, VelEps));

		TestTrue(TEXT("velocity changed by 26 cm/s => resend"),
			CrowdNet::ShouldResend(FVector2D(100.0, 0.0), FVector2f(100.f, 26.f), SentPos, SentVel, 1.0, Tolerance, VelEps));
		TestFalse(TEXT("velocity changed by 24 cm/s => keep"),
			CrowdNet::ShouldResend(FVector2D(100.0, 0.0), FVector2f(100.f, 24.f), SentPos, SentVel, 1.0, Tolerance, VelEps));
	}

	// ---- bandwidth: simulate a crowd that behaves like the Mass movement processor and count messages ----
	{
		struct FSimAgent
		{
			FVector2D Pos = FVector2D::ZeroVector;
			FVector2f Vel = FVector2f::ZeroVector;
			double NextTurn = 0.0;
			FVector2D SentPos = FVector2D::ZeroVector;
			FVector2f SentVel = FVector2f::ZeroVector;
			double SentTime = 0.0;
		};

		FRandomStream Rng(42);
		auto PickVelocity = [&Rng]() -> FVector2f
		{
			if (Rng.FRand() < 0.25f)
			{
				return FVector2f::ZeroVector; // idle
			}
			const float Angle = Rng.FRand() * 2.f * UE_PI;
			const float Speed = 80.f + Rng.FRand() * 140.f;
			return FVector2f(FMath::Cos(Angle) * Speed, FMath::Sin(Angle) * Speed);
		};

		constexpr int32 NumAgents = 400;
		constexpr double Dt = 0.1;      // 10 Hz replication
		constexpr double Duration = 60.0;

		TArray<FSimAgent> Agents;
		Agents.SetNum(NumAgents);
		for (FSimAgent& Agent : Agents)
		{
			Agent.Pos = FVector2D(Rng.FRand() * 10000.0, Rng.FRand() * 10000.0);
			Agent.Vel = PickVelocity();
			Agent.NextTurn = 2.0 + Rng.FRand() * 4.0;
			Agent.SentPos = Agent.Pos;
			Agent.SentVel = FVector2f(CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Agent.Vel.X)), CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Agent.Vel.Y)));
		}

		int64 NaiveMessages = 0;
		int64 DeadReckoningMessages = 0;
		double MaxPredictionError = 0.0;

		for (double Now = Dt; Now <= Duration; Now += Dt)
		{
			for (FSimAgent& Agent : Agents)
			{
				if (Now >= Agent.NextTurn)
				{
					Agent.Vel = PickVelocity();
					Agent.NextTurn = Now + 2.0 + Rng.FRand() * 4.0;
				}
				Agent.Pos += FVector2D(Agent.Vel.X, Agent.Vel.Y) * Dt;

				// Baseline: resend everything that moves (or has just stopped).
				if (!Agent.Vel.IsNearlyZero() || !Agent.SentVel.IsNearlyZero())
				{
					++NaiveMessages;
				}

				// What the client would draw versus the truth, measured BEFORE this tick's correction.
				const FVector2D Predicted = Agent.SentPos + FVector2D(Agent.SentVel.X, Agent.SentVel.Y) * (Now - Agent.SentTime);
				MaxPredictionError = FMath::Max(MaxPredictionError, FVector2D::Distance(Predicted, Agent.Pos));

				if (CrowdNet::ShouldResend(Agent.Pos, Agent.Vel, Agent.SentPos, Agent.SentVel, Now - Agent.SentTime, Tolerance, VelEps))
				{
					++DeadReckoningMessages;
					Agent.SentPos = FVector2D(
						CrowdNet::DequantizeOffset(CrowdNet::QuantizeOffset(Agent.Pos.X)),
						CrowdNet::DequantizeOffset(CrowdNet::QuantizeOffset(Agent.Pos.Y)));
					Agent.SentVel = FVector2f(
						CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Agent.Vel.X)),
						CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Agent.Vel.Y)));
					Agent.SentTime = Now;
				}
			}
		}

		AddInfo(FString::Printf(TEXT("naive messages: %lld, dead reckoning messages: %lld (%.1f%%), max prediction error: %.1f cm"),
			NaiveMessages, DeadReckoningMessages,
			NaiveMessages > 0 ? 100.0 * static_cast<double>(DeadReckoningMessages) / static_cast<double>(NaiveMessages) : 0.0,
			MaxPredictionError));

		TestTrue(TEXT("naive baseline produced traffic"), NaiveMessages > 100000);
		TestTrue(TEXT("dead reckoning sends under 15% of the naive messages"), DeadReckoningMessages * 100 < NaiveMessages * 15);
		// Before a correction the error was <= tolerance at the previous tick, then it grows by |v - vSent| * dt
		// (a 180 degree turn at 220 cm/s is 440 cm/s => 44 cm in one 0.1 s tick).
		TestTrue(TEXT("client prediction error stays bounded"), MaxPredictionError < Tolerance + 2.0 * 220.0 * Dt + 5.0);
	}

	return true;
}

// =================================================================================================
// LOD hysteresis and deterministic helpers
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdMathTest, "MassBubble.Crowd.Math",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdMathTest::RunTest(const FString& Parameters)
{
	FCrowdTuning Tuning; // defaults: High < 40 m < Medium < 100 m < Low < 200 m < Off, hysteresis 5 m

	// ---- tier selection ----
	TestEqual(TEXT("close => High"), static_cast<int32>(CrowdMath::ComputeTier(1000.f, ECrowdLOD::Off, Tuning)), static_cast<int32>(ECrowdLOD::High));
	TestEqual(TEXT("far => Off"), static_cast<int32>(CrowdMath::ComputeTier(30000.f, ECrowdLOD::High, Tuning)), static_cast<int32>(ECrowdLOD::Off));

	// ---- hysteresis: oscillating around the High/Medium border at 40 m must not change the tier ----
	ECrowdLOD Tier = ECrowdLOD::High;
	int32 Changes = 0;
	for (int32 Step = 0; Step < 100; ++Step)
	{
		const float Dist = (Step % 2 == 0) ? 3800.f : 4200.f;
		const ECrowdLOD Next = CrowdMath::ComputeTier(Dist, Tier, Tuning);
		Changes += (Next != Tier) ? 1 : 0;
		Tier = Next;
	}
	TestEqual(TEXT("no tier flicker at the border"), Changes, 0);
	TestEqual(TEXT("still High"), static_cast<int32>(Tier), static_cast<int32>(ECrowdLOD::High));

	// ... but a real departure (> border + hysteresis) does change it, and returning needs border - hysteresis.
	Tier = CrowdMath::ComputeTier(4600.f, Tier, Tuning);
	TestEqual(TEXT("4600 cm leaves High"), static_cast<int32>(Tier), static_cast<int32>(ECrowdLOD::Medium));
	Tier = CrowdMath::ComputeTier(3900.f, Tier, Tuning);
	TestEqual(TEXT("3900 cm does not return yet"), static_cast<int32>(Tier), static_cast<int32>(ECrowdLOD::Medium));
	Tier = CrowdMath::ComputeTier(3400.f, Tier, Tuning);
	TestEqual(TEXT("3400 cm returns to High"), static_cast<int32>(Tier), static_cast<int32>(ECrowdLOD::High));

	// ---- random numbers: deterministic, in range ----
	uint32 A = 12345u;
	uint32 B = 12345u;
	bool bSameSequence = true;
	for (int32 i = 0; i < 1000; ++i)
	{
		bSameSequence = bSameSequence && (CrowdMath::NextRandom(A) == CrowdMath::NextRandom(B));
	}
	TestTrue(TEXT("same seed => same sequence (server restores the same crowd)"), bSameSequence);

	uint32 State = 777u;
	float MinValue = 1.f;
	float MaxValue = 0.f;
	for (int32 i = 0; i < 100000; ++i)
	{
		const float R = CrowdMath::Random01(State);
		MinValue = FMath::Min(MinValue, R);
		MaxValue = FMath::Max(MaxValue, R);
	}
	TestTrue(TEXT("Random01 stays in [0, 1)"), MinValue >= 0.f && MaxValue < 1.f);
	TestTrue(TEXT("Random01 covers the range"), MinValue < 0.01f && MaxValue > 0.99f);

	// ---- hash: used for per-region seeds, must not collide for neighbouring regions ----
	TSet<uint32> Hashes;
	for (uint32 i = 0; i < 10000u; ++i)
	{
		Hashes.Add(CrowdMath::Hash32(i));
	}
	TestEqual(TEXT("Hash32 is collision free on 0..9999"), Hashes.Num(), 10000);
	TestTrue(TEXT("NonZeroSeed never returns 0"), CrowdMath::NonZeroSeed(0u) != 0u);

	// ---- region math ----
	TestTrue(TEXT("region of a negative position"), CrowdMath::WorldToRegion(FVector2D(-1.0, -12800.0), 12800.f) == FIntPoint(-1, -1));
	TestTrue(TEXT("region border belongs to the upper region"), CrowdMath::WorldToRegion(FVector2D(12800.0, 0.0), 12800.f) == FIntPoint(1, 0));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
