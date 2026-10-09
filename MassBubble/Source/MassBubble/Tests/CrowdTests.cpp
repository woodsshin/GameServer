// Unit tests for the pure-logic parts of the crowd.
// Run in the editor:  Session Frontend > Automation > filter "MassBubble.Crowd"
// or headless:        UnrealEditor-Cmd MassBubble.uproject -ExecCmds="Automation RunTests MassBubble.Crowd; Quit" -unattended -nullrhi -log
//
// Nothing here needs a world, a Mass entity manager or a network connection: the algorithms live in header-only
// helpers (CrowdCellGrid.h, CrowdMath.h, CrowdWander.h, CrowdNetMath.h, CrowdSmoothing.h) exactly so that they can be
// tested like this. MassBubble.Crowd.Presentation runs the whole path (server walk -> replication decision -> client
// extrapolation and smoothing) with those real functions and compares it with the first version of the crowd.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Crowd/CrowdCellGrid.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"
#include "Net/CrowdNetMath.h"
#include "Net/CrowdSmoothing.h"

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
// Helpers shared by the movement / replication / presentation tests
// =================================================================================================

namespace CrowdTestSupport
{
	constexpr double RegionCm = 12800.0;

	inline CrowdWander::FBox2 Region()
	{
		return CrowdWander::FBox2{ FVector2D(0.0, 0.0), FVector2D(RegionCm, RegionCm) };
	}

	/** An agent exactly as UCrowdSubsystem::MakeAgent creates it (FCrowdSavedAgent is the real persistence record). */
	inline FCrowdSavedAgent NewAgent(uint32 Index, const FCrowdTuning& Tuning)
	{
		FCrowdSavedAgent Agent;
		Agent.NetId = Index + 1u;
		CrowdWander::InitNewAgent(Agent, 777u + Index * 2654435761u, Region(), Tuning);
		return Agent;
	}

	/** One simulation step exactly as UCrowdMovementProcessor does it. True if the safety clamp had to act. */
	inline bool Step(FCrowdSavedAgent& Agent, float DeltaSeconds, const FCrowdTuning& Tuning)
	{
		return CrowdWander::Advance(Agent.Location, Agent, DeltaSeconds, Region(), Tuning, CrowdWander::TurnSignForId(Agent.NetId));
	}

	/** Distance to the nearest region border (cm). */
	inline double DistanceToBorder(const FVector2D& P)
	{
		return FMath::Min(FMath::Min(P.X, RegionCm - P.X), FMath::Min(P.Y, RegionCm - P.Y));
	}

	/** Angle between two directions in degrees, 0..180. */
	inline double AngleBetweenDeg(const FVector2D& A, const FVector2D& B)
	{
		double Delta = FMath::Atan2(B.Y, B.X) - FMath::Atan2(A.Y, A.X);
		Delta = FMath::Fmod(Delta + 3.0 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI) - UE_DOUBLE_PI; // [-pi, pi)
		return FMath::Abs(Delta) * 180.0 / UE_DOUBLE_PI;
	}

	inline double AngleBetweenDeg(const FVector2f& A, const FVector2f& B)
	{
		return AngleBetweenDeg(FVector2D(A.X, A.Y), FVector2D(B.X, B.Y));
	}

	// ---------------------------------------------------------------------------------------------
	// The wire: what the server sends and what the client believes afterwards
	// ---------------------------------------------------------------------------------------------

	struct FSentState
	{
		FVector2D Pos = FVector2D::ZeroVector;
		FVector2f Vel = FVector2f::ZeroVector;
		double Time = 0.0;
	};

	/** What FillItem puts on the wire and the client reads back: positions in whole cm, velocities in 5 cm/s steps. */
	inline FSentState Send(const FVector2D& Pos, const FVector2f& Vel, double Now)
	{
		FSentState Sent;
		Sent.Pos = FVector2D(CrowdNet::DequantizeOffset(CrowdNet::QuantizeOffset(Pos.X)), CrowdNet::DequantizeOffset(CrowdNet::QuantizeOffset(Pos.Y)));
		Sent.Vel = FVector2f(CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Vel.X)), CrowdNet::DequantizeVelocity(CrowdNet::QuantizeVelocity(Vel.Y)));
		Sent.Time = Now;
		return Sent;
	}

	// ---------------------------------------------------------------------------------------------
	// The first version of the crowd, kept as the "before" of the presentation test
	// ---------------------------------------------------------------------------------------------

	/**
	 * The first version's walk: a new, uniformly random heading every 2-6 s (a third of them reversals), the speed
	 * re-rolled in 80-220 cm/s, a quarter of the segments are pauses, hard reflection at the region border.
	 */
	inline void LegacyStep(FCrowdSavedAgent& Agent, float DeltaSeconds)
	{
		Agent.RetargetTimer -= DeltaSeconds;
		if (Agent.RetargetTimer <= 0.f)
		{
			const float Angle = CrowdMath::Random01(Agent.Rng) * 2.f * UE_PI;
			const float Speed = FMath::Lerp(80.f, 220.f, CrowdMath::Random01(Agent.Rng));
			const bool bIdle = CrowdMath::Random01(Agent.Rng) < 0.25f;
			Agent.Velocity = bIdle ? FVector2f::ZeroVector : FVector2f(FMath::Cos(Angle) * Speed, FMath::Sin(Angle) * Speed);
			Agent.RetargetTimer = FMath::Lerp(2.f, 6.f, CrowdMath::Random01(Agent.Rng));
		}

		Agent.Location += FVector2D(Agent.Velocity.X, Agent.Velocity.Y) * DeltaSeconds;
		if (Agent.Location.X < 0.0)             { Agent.Location.X = 0.0;      Agent.Velocity.X = FMath::Abs(Agent.Velocity.X); }
		else if (Agent.Location.X > RegionCm)   { Agent.Location.X = RegionCm; Agent.Velocity.X = -FMath::Abs(Agent.Velocity.X); }
		if (Agent.Location.Y < 0.0)             { Agent.Location.Y = 0.0;      Agent.Velocity.Y = FMath::Abs(Agent.Velocity.Y); }
		else if (Agent.Location.Y > RegionCm)   { Agent.Location.Y = RegionCm; Agent.Velocity.Y = -FMath::Abs(Agent.Velocity.Y); }
	}

	/**
	 * The first version's replication test: it predicted the client's position with NO cap (a straight walker was never
	 * re-sent) while the client stopped extrapolating after 1.5 s. See the extrapolation contract in Net/CrowdNetMath.h.
	 */
	inline bool LegacyShouldResend(const FVector2D& TruePos, const FVector2f& TrueVel, const FSentState& Sent, double Now, float ToleranceCm, float VelocityEpsCmPerSec)
	{
		const FVector2D Predicted = Sent.Pos + FVector2D(Sent.Vel.X, Sent.Vel.Y) * (Now - Sent.Time);
		return FVector2D::DistSquared(TruePos, Predicted) > static_cast<double>(ToleranceCm) * ToleranceCm
			|| (TrueVel - Sent.Vel).SizeSquared() > VelocityEpsCmPerSec * VelocityEpsCmPerSec;
	}

	// ---------------------------------------------------------------------------------------------
	// What a viewer of the crowd sees
	// ---------------------------------------------------------------------------------------------

	/** Drawn-vs-true errors in 1 cm bins: percentiles without keeping a million samples. */
	struct FErrorHistogram
	{
		TArray<int64> Bins;
		int64 Total = 0;
		double Max = 0.0;

		FErrorHistogram()
		{
			Bins.Init(0, 2001); // 0..1999 cm; the last bin takes everything above
		}

		void Add(double Cm)
		{
			++Bins[FMath::Min(FMath::FloorToInt(Cm), 2000)];
			++Total;
			Max = FMath::Max(Max, Cm);
		}

		/** The error (cm, upper edge of its bin) that a fraction P of all samples stays below. */
		double Percentile(double P) const
		{
			const int64 Wanted = static_cast<int64>(P * static_cast<double>(Total));
			int64 Running = 0;
			for (int32 i = 0; i < Bins.Num(); ++i)
			{
				Running += Bins[i];
				if (Running > Wanted)
				{
					return static_cast<double>(i + 1);
				}
			}
			return static_cast<double>(Bins.Num());
		}
	};

	struct FViewMetrics
	{
		int64 AgentFrames = 0;
		int64 WalkingFrames = 0;   // the truth walks (> 50 cm/s) ...
		int64 FrozenFrames = 0;    // ... but the drawn agent hardly moves (< 10 cm/s): "stands although it should walk"
		int64 BackwardFrames = 0;  // ... or jumps against the way it walks (> 20 cm in one frame): "goes back to where it was"
		int64 BigStepFrames = 0;   // a drawn step of more than 60 cm in one frame (3.6 m/s at 60 fps)
		double MaxStepCm = 0.0;    // the largest drawn step in one frame
		FErrorHistogram Error;
		int64 Messages = 0;
		int64 NaiveMessages = 0;   // what "resend everything that moves, every tick" would have cost
		int64 ClampHits = 0;       // how often the wander safety clamp had to act
		double AgentSeconds = 0.0;

		void Observe(const FVector2D& TruePos, const FVector2f& TrueVel, const FVector2D& Drawn, const FVector2D& PreviousDrawn, double Dt)
		{
			++AgentFrames;
			const FVector2D Moved = Drawn - PreviousDrawn;
			const double StepCm = Moved.Size();
			const double TrueSpeed = TrueVel.Size();
			MaxStepCm = FMath::Max(MaxStepCm, StepCm);
			BigStepFrames += (StepCm > 60.0) ? 1 : 0;
			if (TrueSpeed > 50.0)
			{
				++WalkingFrames;
				FrozenFrames += (StepCm / Dt < 10.0) ? 1 : 0;
				const double Along = (Moved.X * TrueVel.X + Moved.Y * TrueVel.Y) / TrueSpeed; // cm moved the way the truth walks
				BackwardFrames += (Along < -20.0) ? 1 : 0;
			}
			Error.Add(FVector2D::Distance(Drawn, TruePos));
		}

		double FrozenShare() const { return WalkingFrames > 0 ? static_cast<double>(FrozenFrames) / static_cast<double>(WalkingFrames) : 0.0; }
		double BackwardPerAgentMinute() const { return static_cast<double>(BackwardFrames) / (AgentSeconds / 60.0); }
		double MessageShare() const { return NaiveMessages > 0 ? static_cast<double>(Messages) / static_cast<double>(NaiveMessages) : 0.0; }

		FString Describe() const
		{
			return FString::Printf(TEXT("frozen while walking %.2f%%, backward jumps %.2f per agent-minute, steps over 60 cm: %lld, largest step %.0f cm, error p50/p99/max %.0f/%.0f/%.0f cm, messages %.2f per agent-second (%.1f%% of a resend-everything-that-moves baseline)"),
				100.0 * FrozenShare(), BackwardPerAgentMinute(), BigStepFrames, MaxStepCm,
				Error.Percentile(0.5), Error.Percentile(0.99), Error.Max, static_cast<double>(Messages) / AgentSeconds, 100.0 * MessageShare());
		}
	};

	struct FPipelineOptions
	{
		bool bLegacy = false;       // the first version end to end (walk, replication test, client)
		double LatencySec = 0.0;    // one way, server -> client
		double JitterSec = 0.0;     // plus up to this much, random per message
		int32 NumAgents = 150;
		double Seconds = 100.0;
		double WarmupSec = 10.0;
	};

	/**
	 * The whole path of the crowd, frame by frame, with the REAL pieces: CrowdWander (server walk), CrowdNet (what the
	 * server sends, what the client extrapolates) and CrowdSmoothing (what the client draws). 60 fps, replication at 10 Hz.
	 * bLegacy swaps in the first version of all of them, to show that the metrics do see the reported behaviour.
	 */
	inline FViewMetrics RunPipeline(const FPipelineOptions& Options)
	{
		constexpr double Dt = 1.0 / 60.0;
		constexpr int32 FramesPerTick = 6;

		FCrowdTuning Tuning;
		CrowdSmoothing::FParams SmoothingParams;
		const CrowdSmoothing::FFrame Frame = CrowdSmoothing::MakeFrame(SmoothingParams, Dt);
		const double LegacyBlend = 1.0 - FMath::Exp(-15.0 * Dt); // the first client: low pass on the position alone, rate 15

		struct FPacket
		{
			double Arrive = 0.0;
			FSentState State;
		};
		struct FViewed
		{
			FCrowdSavedAgent Server;           // the truth (what the Mass entity holds)
			FSentState Sent;                   // what the server knows the client has
			FSentState Received;               // what the client has (Time = when it arrived)
			TArray<FPacket> InFlight;
			CrowdSmoothing::FState Shown;      // what is drawn
			FVector2D PreviousShown = FVector2D::ZeroVector;
		};

		TArray<FViewed> Agents;
		Agents.SetNum(Options.NumAgents);
		for (int32 i = 0; i < Options.NumAgents; ++i)
		{
			FViewed& A = Agents[i];
			if (Options.bLegacy)
			{
				A.Server.NetId = static_cast<uint32>(i) + 1u;
				uint32 State = CrowdMath::NonZeroSeed(CrowdMath::Hash32(12345u + static_cast<uint32>(i) * 2654435761u));
				A.Server.Location = FVector2D(CrowdMath::Random01(State) * RegionCm, CrowdMath::Random01(State) * RegionCm);
				A.Server.Rng = CrowdMath::NonZeroSeed(State);
				A.Server.RetargetTimer = 0.f;
			}
			else
			{
				A.Server = NewAgent(static_cast<uint32>(i), Tuning);
			}
			A.Sent = Send(A.Server.Location, A.Server.Velocity, 0.0);
			A.Received = A.Sent;
			A.Shown = CrowdSmoothing::Begin(A.Received.Pos, A.Received.Vel, 0.f);
			A.PreviousShown = A.Shown.Pos;
		}

		FViewMetrics Metrics;
		Metrics.AgentSeconds = static_cast<double>(Options.NumAgents) * (Options.Seconds - Options.WarmupSec);
		uint32 JitterRng = 99u;

		const int32 TotalFrames = static_cast<int32>(Options.Seconds / Dt);
		const int32 WarmupFrames = static_cast<int32>(Options.WarmupSec / Dt);
		for (int32 FrameIndex = 0; FrameIndex < TotalFrames; ++FrameIndex)
		{
			const double Now = static_cast<double>(FrameIndex) * Dt;
			const bool bTick = (FrameIndex % FramesPerTick) == 0;
			const bool bMeasure = FrameIndex >= WarmupFrames;

			for (FViewed& A : Agents)
			{
				// ---- server: the agent walks ----
				if (Options.bLegacy)
				{
					LegacyStep(A.Server, static_cast<float>(Dt));
				}
				else
				{
					Metrics.ClampHits += Step(A.Server, static_cast<float>(Dt), Tuning) ? 1 : 0;
				}

				// ---- server: replication tick ----
				if (bTick)
				{
					if (bMeasure && (!A.Server.Velocity.IsNearlyZero() || !A.Sent.Vel.IsNearlyZero()))
					{
						++Metrics.NaiveMessages;
					}

					const double Elapsed = Now - A.Sent.Time;
					const bool bResend = Options.bLegacy
						? LegacyShouldResend(A.Server.Location, A.Server.Velocity, A.Sent, Now, Tuning.NearErrorCm, Tuning.VelocityEpsCmPerSec)
						: CrowdNet::ShouldResend(A.Server.Location, A.Server.Velocity, A.Sent.Pos, A.Sent.Vel, Elapsed, Tuning.NearErrorCm, Tuning.VelocityEpsCmPerSec);
					if (bResend)
					{
						A.Sent = Send(A.Server.Location, A.Server.Velocity, Now);
						Metrics.Messages += bMeasure ? 1 : 0;

						FPacket Packet;
						Packet.Arrive = Now + Options.LatencySec + Options.JitterSec * CrowdMath::Random01(JitterRng);
						Packet.State = A.Sent;
						A.InFlight.Add(Packet);
					}
				}

				// ---- client: messages that have arrived (in order, like a reliable channel) ----
				while (A.InFlight.Num() > 0 && A.InFlight[0].Arrive <= Now)
				{
					A.Received = A.InFlight[0].State;
					A.Received.Time = Now; // FCrowdAgentItem::RecvTime
					A.InFlight.RemoveAt(0);
				}

				// ---- client: what is drawn this frame ----
				const double SinceReceived = Now - A.Received.Time;
				A.PreviousShown = A.Shown.Pos;
				if (Options.bLegacy)
				{
					const double Capped = FMath::Clamp(SinceReceived, 0.0, 1.5); // the client's private cap
					const FVector2D Target = A.Received.Pos + FVector2D(A.Received.Vel.X, A.Received.Vel.Y) * Capped;
					A.Shown.Pos += (Target - A.Shown.Pos) * LegacyBlend;
				}
				else
				{
					const FVector2D Target = CrowdNet::Extrapolate(A.Received.Pos, A.Received.Vel, SinceReceived);
					const FVector2f Drive = CrowdNet::DrivingVelocity(A.Received.Vel, SinceReceived);
					CrowdSmoothing::Step(A.Shown, Target, Drive, Dt, Frame, SmoothingParams);
				}

				if (bMeasure)
				{
					Metrics.Observe(A.Server.Location, A.Server.Velocity, A.Shown.Pos, A.PreviousShown, Dt);
				}
			}
		}
		return Metrics;
	}
}

// =================================================================================================
// Dead reckoning and the extrapolation contract
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdDeadReckoningTest, "MassBubble.Crowd.DeadReckoning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdDeadReckoningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	const float Tolerance = 30.f;
	const float VelEps = 25.f;
	const FVector2D SentPos(0.0, 0.0);
	const FVector2f SentVel(100.f, 0.f);

	// ---- the contract between server and client: ONE extrapolation function with ONE cap ----
	// (The first version had a second, private cap on the client. The server assumed a straight walker could be
	//  extrapolated for ever, the client stopped after 1.5 s: agents froze in the middle of a walk and jumped at the
	//  next update. That is the "walks, then seems to return to where it was" the first version showed.)
	{
		const FVector2D Before = CrowdNet::Extrapolate(SentPos, SentVel, -1.0);
		TestTrue(TEXT("no extrapolation before the data arrived"), Before.X == 0.0 && Before.Y == 0.0);

		const FVector2D AtCap = CrowdNet::Extrapolate(SentPos, SentVel, CrowdNet::MaxExtrapolationSec);
		const FVector2D PastCap = CrowdNet::Extrapolate(SentPos, SentVel, 10.0 * CrowdNet::MaxExtrapolationSec);
		TestTrue(TEXT("extrapolation stops at the shared cap"), FVector2D::Distance(AtCap, PastCap) < 1.0e-9);
		TestTrue(TEXT("... and has advanced until then"), FMath::Abs(AtCap.X - SentVel.X * CrowdNet::MaxExtrapolationSec) < 1.0e-6);

		TestTrue(TEXT("the heartbeat comes before the cap"), CrowdNet::HeartbeatSec < CrowdNet::MaxExtrapolationSec);
		TestTrue(TEXT("... with room for a replication tick and network jitter"), CrowdNet::MaxExtrapolationSec - CrowdNet::HeartbeatSec >= 0.5);

		// The velocity the client feeds forward stops with the position.
		TestTrue(TEXT("walks on the replicated velocity while extrapolation is allowed"), CrowdNet::DrivingVelocity(SentVel, 1.0) == SentVel);
		TestTrue(TEXT("... still at the cap"), CrowdNet::DrivingVelocity(SentVel, CrowdNet::MaxExtrapolationSec) == SentVel);
		TestTrue(TEXT("... and not a moment longer"), CrowdNet::DrivingVelocity(SentVel, CrowdNet::MaxExtrapolationSec + 0.01).IsNearlyZero());
	}

	// ---- thresholds ----
	{
		// A straight walker is quiet between heartbeats, however well the prediction matches ...
		bool bResentEarly = false;
		for (double T = 0.0; T < CrowdNet::HeartbeatSec - 1.0e-6; T += 0.1)
		{
			const FVector2D TruePos(SentVel.X * T, 0.0);
			bResentEarly = bResentEarly || CrowdNet::ShouldResend(TruePos, SentVel, SentPos, SentVel, T, Tolerance, VelEps);
		}
		TestFalse(TEXT("straight walker is quiet between heartbeats"), bResentEarly);

		// ... and is re-sent when the heartbeat is due, before the client's extrapolation can run out.
		TestTrue(TEXT("straight walker is re-sent when the heartbeat is due"),
			CrowdNet::ShouldResend(FVector2D(SentVel.X * CrowdNet::HeartbeatSec, 0.0), SentVel, SentPos, SentVel, CrowdNet::HeartbeatSec, Tolerance, VelEps));

		// A standing agent needs no heartbeat: the client does not extrapolate it.
		TestFalse(TEXT("standing agent is never re-sent for being quiet"),
			CrowdNet::ShouldResend(SentPos, FVector2f::ZeroVector, SentPos, FVector2f::ZeroVector, 100.0, Tolerance, VelEps));

		// One second later the prediction says (100, 0).
		TestTrue(TEXT("31 cm off the prediction => resend"),
			CrowdNet::ShouldResend(FVector2D(100.0, 31.0), SentVel, SentPos, SentVel, 1.0, Tolerance, VelEps));
		TestFalse(TEXT("29 cm off the prediction => keep"),
			CrowdNet::ShouldResend(FVector2D(100.0, 29.0), SentVel, SentPos, SentVel, 1.0, Tolerance, VelEps));

		TestTrue(TEXT("velocity changed by 26 cm/s => resend"),
			CrowdNet::ShouldResend(FVector2D(100.0, 0.0), FVector2f(100.f, 26.f), SentPos, SentVel, 1.0, Tolerance, VelEps));
		TestFalse(TEXT("velocity changed by 24 cm/s => keep"),
			CrowdNet::ShouldResend(FVector2D(100.0, 0.0), FVector2f(100.f, 24.f), SentPos, SentVel, 1.0, Tolerance, VelEps));

		// A walker whose data is as old as the client's cap is always due (the heartbeat fires long before).
		TestTrue(TEXT("a walker silent for the whole cap is re-sent"),
			CrowdNet::ShouldResend(FVector2D(SentVel.X * CrowdNet::MaxExtrapolationSec, 0.0), SentVel, SentPos, SentVel, CrowdNet::MaxExtrapolationSec, 1000.f, 1000.f));
	}

	// ---- bandwidth: a crowd that walks exactly like the Mass movement processor, replicated at 10 Hz ----
	{
		FCrowdTuning Tuning;

		struct FSim
		{
			FCrowdSavedAgent Agent;
			FSentState Sent;
		};

		constexpr int32 NumAgents = 400;
		constexpr double Dt = 0.1;
		constexpr double Duration = 120.0;

		TArray<FSim> Sims;
		Sims.SetNum(NumAgents);
		for (int32 i = 0; i < NumAgents; ++i)
		{
			FSim& Sim = Sims[i];
			Sim.Agent = NewAgent(static_cast<uint32>(i), Tuning);
			Step(Sim.Agent, 0.f, Tuning);                       // plans the first segment
			Step(Sim.Agent, static_cast<float>(Dt), Tuning);
			Sim.Sent = Send(Sim.Agent.Location, Sim.Agent.Velocity, 0.0); // what a new member of the interest set gets
		}

		int64 NaiveMessages = 0;
		int64 DeadReckoningMessages = 0;
		double MaxPredictionError = 0.0;
		double MaxSilenceWhileWalking = 0.0;

		for (double Now = 2.0 * Dt; Now <= Duration; Now += Dt)
		{
			for (FSim& Sim : Sims)
			{
				Step(Sim.Agent, static_cast<float>(Dt), Tuning);
				const FVector2D& Pos = Sim.Agent.Location;
				const FVector2f& Vel = Sim.Agent.Velocity;

				// Baseline: resend everything that moves (or has just stopped).
				if (!Vel.IsNearlyZero() || !Sim.Sent.Vel.IsNearlyZero())
				{
					++NaiveMessages;
				}

				// What the client draws versus the truth, measured BEFORE this tick's correction.
				const double Elapsed = Now - Sim.Sent.Time;
				const FVector2D Predicted = CrowdNet::Extrapolate(Sim.Sent.Pos, Sim.Sent.Vel, Elapsed);
				MaxPredictionError = FMath::Max(MaxPredictionError, FVector2D::Distance(Predicted, Pos));
				if (!Sim.Sent.Vel.IsNearlyZero())
				{
					MaxSilenceWhileWalking = FMath::Max(MaxSilenceWhileWalking, Elapsed);
				}

				if (CrowdNet::ShouldResend(Pos, Vel, Sim.Sent.Pos, Sim.Sent.Vel, Elapsed, Tolerance, VelEps))
				{
					++DeadReckoningMessages;
					Sim.Sent = Send(Pos, Vel, Now);
				}
			}
		}

		AddInfo(FString::Printf(TEXT("naive messages: %lld, dead reckoning messages: %lld (%.1f%%), max prediction error: %.1f cm, longest silence of a walker: %.2f s"),
			NaiveMessages, DeadReckoningMessages,
			NaiveMessages > 0 ? 100.0 * static_cast<double>(DeadReckoningMessages) / static_cast<double>(NaiveMessages) : 0.0,
			MaxPredictionError, MaxSilenceWhileWalking));

		TestTrue(TEXT("naive baseline produced traffic"), NaiveMessages > 100000);
		TestTrue(TEXT("dead reckoning sends under 15% of the naive messages"), DeadReckoningMessages * 100 < NaiveMessages * 15);
		// Before a correction the error was <= tolerance at the previous tick, then it grows by |v - vSent| * dt
		// (a 90 degree turn at the top speed is 1.4 times the speed, a reversal 2 times).
		TestTrue(TEXT("client prediction error stays bounded"), MaxPredictionError < Tolerance + 2.0 * Tuning.MaxSpeedCmPerSec * Dt + 5.0);
		// The point of the heartbeat: a walking agent is never silent for longer than the client can extrapolate.
		TestTrue(TEXT("a walker is never silent for longer than the heartbeat plus one tick"), MaxSilenceWhileWalking <= CrowdNet::HeartbeatSec + 1.5 * Dt);
	}

	return true;
}

// =================================================================================================
// Client smoothing (Net/CrowdSmoothing.h)
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdSmoothingTest, "MassBubble.Crowd.Smoothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdSmoothingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	CrowdSmoothing::FParams Params; // what the project ships
	constexpr double Dt = 1.0 / 60.0;
	const CrowdSmoothing::FFrame Frame = CrowdSmoothing::MakeFrame(Params, Dt);
	const FVector2f East(150.f, 0.f);
	const FVector2f North(0.f, 150.f);
	const FVector2f Still = FVector2f::ZeroVector;

	// ---- blend factors ----
	{
		CrowdSmoothing::FParams Off;
		Off.VelocityRate = 0.f;
		Off.CorrectionRate = 0.f;
		Off.YawRate = 0.f;
		const CrowdSmoothing::FFrame OffFrame = CrowdSmoothing::MakeFrame(Off, Dt);
		TestTrue(TEXT("rate 0 switches a filter off (blend factor 1)"), OffFrame.Velocity == 1.f && OffFrame.Correction == 1.f && OffFrame.Yaw == 1.f);

		const CrowdSmoothing::FFrame Hitch = CrowdSmoothing::MakeFrame(Params, 5.0);
		TestTrue(TEXT("a 5 s hitch cannot overshoot (blend factors stay <= 1)"), Hitch.Velocity <= 1.f && Hitch.Correction <= 1.f && Hitch.Yaw <= 1.f && Hitch.Correction > 0.99f);

		// Frame rate independence: two half frames blend exactly like one frame.
		const CrowdSmoothing::FFrame Half = CrowdSmoothing::MakeFrame(Params, 0.5 * Dt);
		const float TwoHalves = 1.f - (1.f - Half.Correction) * (1.f - Half.Correction);
		TestTrue(TEXT("two half frames blend like one frame"), FMath::Abs(TwoHalves - Frame.Correction) < 1.0e-5f);
	}

	// ---- a walker is drawn where the server predicts it: no lag ----
	{
		FVector2D Target = FVector2D::ZeroVector;
		CrowdSmoothing::FState State = CrowdSmoothing::Begin(Target, East, 0.f);
		double OldShown = 0.0; // the first version: a low pass on the position alone, rate 15
		const double OldBlend = 1.0 - FMath::Exp(-15.0 * Dt);
		double WorstLag = 0.0;
		for (int32 i = 0; i < 300; ++i) // 5 s
		{
			Target += FVector2D(East.X, East.Y) * Dt;
			CrowdSmoothing::Step(State, Target, East, Dt, Frame, Params);
			OldShown += (Target.X - OldShown) * OldBlend;
			WorstLag = FMath::Max(WorstLag, FVector2D::Distance(State.Pos, Target));
		}
		AddInfo(FString::Printf(TEXT("walking at 150 cm/s: drawn %.2f cm off the prediction at worst (the first version's low pass: %.1f cm behind)"), WorstLag, Target.X - OldShown));
		TestTrue(TEXT("a walking agent is drawn on the predicted position (no lag)"), WorstLag < 0.5);
		TestTrue(TEXT("(for contrast) the first version's low pass trailed a walker by several cm"), Target.X - OldShown > 6.0);
	}

	// ---- a 90 degree turn is a curve, not a corner ----
	{
		FVector2D Target = FVector2D::ZeroVector;
		FVector2f TargetVel = East;
		CrowdSmoothing::FState State = CrowdSmoothing::Begin(Target, TargetVel, 0.f);

		double MaxDrawnTurn = 0.0;
		double MaxRawTurn = 0.0;
		double MaxFromPrediction = 0.0;
		FVector2D PreviousDrawnStep = FVector2D::ZeroVector;
		FVector2D PreviousRawStep = FVector2D::ZeroVector;
		for (int32 i = 0; i < 240; ++i) // 4 s, the turn comes after 1 s
		{
			if (i == 60)
			{
				TargetVel = North; // a new replicated state: same place, new velocity
			}
			const FVector2D RawStep = FVector2D(TargetVel.X, TargetVel.Y) * Dt;
			Target += RawStep;

			const FVector2D Before = State.Pos;
			CrowdSmoothing::Step(State, Target, TargetVel, Dt, Frame, Params);
			const FVector2D DrawnStep = State.Pos - Before;

			if (i > 0)
			{
				MaxRawTurn = FMath::Max(MaxRawTurn, AngleBetweenDeg(PreviousRawStep, RawStep));
				if (DrawnStep.Size() > 0.1 && PreviousDrawnStep.Size() > 0.1)
				{
					MaxDrawnTurn = FMath::Max(MaxDrawnTurn, AngleBetweenDeg(PreviousDrawnStep, DrawnStep));
				}
			}
			PreviousDrawnStep = DrawnStep;
			PreviousRawStep = RawStep;
			MaxFromPrediction = FMath::Max(MaxFromPrediction, FVector2D::Distance(State.Pos, Target));
		}

		AddInfo(FString::Printf(TEXT("90 degree turn: raw extrapolation turns %.0f deg in one frame, drawn at most %.1f deg per frame, at most %.1f cm from the prediction"), MaxRawTurn, MaxDrawnTurn, MaxFromPrediction));
		TestTrue(TEXT("(for contrast) the raw extrapolation turns 90 degrees within one frame"), MaxRawTurn > 89.0);
		TestTrue(TEXT("the drawn agent never turns more than 25 degrees between two frames"), MaxDrawnTurn < 25.0);
		TestTrue(TEXT("... and cuts the corner by less than 25 cm"), MaxFromPrediction < 25.0);
		TestTrue(TEXT("... and is back on the prediction two seconds later"), FVector2D::Distance(State.Pos, Target) < 1.0);
		TestTrue(TEXT("... facing the new direction"), FMath::Abs(CrowdSmoothing::WrapDegrees(State.YawDeg - 90.f)) < 3.f);
	}

	// ---- a pause: the agent glides to a stop and settles; it neither stops dead nor runs on ----
	{
		FVector2D Target = FVector2D::ZeroVector;
		CrowdSmoothing::FState State = CrowdSmoothing::Begin(Target, East, 0.f);
		for (int32 i = 0; i < 180; ++i)
		{
			Target += FVector2D(East.X, East.Y) * Dt;
			CrowdSmoothing::Step(State, Target, East, Dt, Frame, Params);
		}
		const FVector2D StopPoint = Target; // the server stopped (and said so) here: the prediction stays here from now on

		double FirstFrameStep = 0.0;
		double MaxSlide = 0.0;
		double MaxSettleSpeed = 0.0;
		for (int32 i = 0; i < 300; ++i) // 5 s
		{
			const double Before = State.Pos.X;
			CrowdSmoothing::Step(State, StopPoint, Still, Dt, Frame, Params);
			FirstFrameStep = (i == 0) ? State.Pos.X - Before : FirstFrameStep;
			MaxSlide = FMath::Max(MaxSlide, State.Pos.X - StopPoint.X);
			MaxSettleSpeed = FMath::Max(MaxSettleSpeed, (Before - State.Pos.X) / Dt);
		}

		AddInfo(FString::Printf(TEXT("pause from 150 cm/s: slides %.1f cm past the stop, settles back at no more than %.1f cm/s"), MaxSlide, MaxSettleSpeed));
		TestTrue(TEXT("does not stop dead (still moving in the first frame after the stop)"), FirstFrameStep > 1.0);
		TestTrue(TEXT("slides on past the server's stop by less than 15 cm"), MaxSlide < 15.0);
		TestTrue(TEXT("settles back slowly (under 25 cm/s)"), MaxSettleSpeed < 25.0);
		TestTrue(TEXT("rests on the server's position"), FVector2D::Distance(State.Pos, StopPoint) < 0.5 && State.Vel.Size() < 1.f);
		TestTrue(TEXT("keeps facing where it walked"), FMath::Abs(CrowdSmoothing::WrapDegrees(State.YawDeg)) < 1.f);
	}

	// ---- a stalled connection: after the cap the agent comes to rest, it does not walk on ----
	{
		const FSentState Last = Send(FVector2D(1000.0, 500.0), East, 0.0);
		const FVector2D CapPoint = CrowdNet::Extrapolate(Last.Pos, Last.Vel, CrowdNet::MaxExtrapolationSec);

		CrowdSmoothing::FState Good = CrowdSmoothing::Begin(Last.Pos, Last.Vel, 0.f);
		CrowdSmoothing::FState NeverStops = Good; // (for contrast) a feed-forward that is not cut at the cap
		double MaxBeyondCap = 0.0;
		for (int32 i = 1; i <= 600; ++i) // 10 s without a single message
		{
			const double Since = static_cast<double>(i) * Dt;
			const FVector2D Target = CrowdNet::Extrapolate(Last.Pos, Last.Vel, Since);
			CrowdSmoothing::Step(Good, Target, CrowdNet::DrivingVelocity(Last.Vel, Since), Dt, Frame, Params);
			CrowdSmoothing::Step(NeverStops, Target, Last.Vel, Dt, Frame, Params);
			MaxBeyondCap = FMath::Max(MaxBeyondCap, Good.Pos.X - CapPoint.X);
		}

		AddInfo(FString::Printf(TEXT("10 s without data: drawn %.1f cm beyond the capped position at most; without the zero feed-forward it would stand %.1f cm beyond it"), MaxBeyondCap, NeverStops.Pos.X - CapPoint.X));
		TestTrue(TEXT("never beyond the capped position by more than 15 cm"), MaxBeyondCap < 15.0);
		TestTrue(TEXT("rests on the capped position"), FVector2D::Distance(Good.Pos, CapPoint) < 0.5 && Good.Vel.Size() < 1.f);
		TestTrue(TEXT("(for contrast) a feed-forward that never stops leaves the agent far beyond it"), NeverStops.Pos.X - CapPoint.X > 25.0);
	}

	// ---- a correction glides: the server says the agent is 25 cm to the side; nobody sees a jump ----
	{
		FVector2D Target = FVector2D::ZeroVector;
		CrowdSmoothing::FState State = CrowdSmoothing::Begin(Target, East, 0.f);
		for (int32 i = 0; i < 120; ++i)
		{
			Target += FVector2D(East.X, East.Y) * Dt;
			CrowdSmoothing::Step(State, Target, East, Dt, Frame, Params);
		}

		Target += FVector2D(0.0, 25.0); // the correction
		double MaxSidewaysPerFrame = 0.0;
		for (int32 i = 0; i < 120; ++i) // 2 s
		{
			Target += FVector2D(East.X, East.Y) * Dt;
			const double Before = State.Pos.Y;
			CrowdSmoothing::Step(State, Target, East, Dt, Frame, Params);
			MaxSidewaysPerFrame = FMath::Max(MaxSidewaysPerFrame, FMath::Abs(State.Pos.Y - Before));
		}

		AddInfo(FString::Printf(TEXT("25 cm correction: at most %.2f cm sideways per frame, %.2f cm left after 2 s"), MaxSidewaysPerFrame, FVector2D::Distance(State.Pos, Target)));
		TestTrue(TEXT("a 25 cm correction is spread over frames (under 3 cm sideways per frame)"), MaxSidewaysPerFrame < 3.0);
		TestTrue(TEXT("... and applied after two seconds"), FVector2D::Distance(State.Pos, Target) < 0.5);
	}

	// ---- a long gap in the data: appear, do not fly in ----
	{
		CrowdSmoothing::FState Far = CrowdSmoothing::Begin(FVector2D::ZeroVector, Still, 0.f);
		const FVector2D FarTarget(600.0, 0.0);
		CrowdSmoothing::Step(Far, FarTarget, Still, Dt, Frame, Params);
		TestTrue(TEXT("beyond the snap distance: teleports to the target"), FVector2D::Distance(Far.Pos, FarTarget) < 1.0e-6);

		CrowdSmoothing::FState Near = CrowdSmoothing::Begin(FVector2D::ZeroVector, Still, 0.f);
		CrowdSmoothing::Step(Near, FVector2D(400.0, 0.0), Still, Dt, Frame, Params);
		TestTrue(TEXT("within the snap distance: glides"), Near.Pos.X > 0.0 && Near.Pos.X < 40.0);
	}

	// ---- yaw ----
	{
		// Facing +170 degrees, now walking towards -170: the short way over 180, not back through 0.
		CrowdSmoothing::FState State;
		State.YawDeg = 170.f;
		State.Vel = FVector2f(150.f * FMath::Cos(FMath::DegreesToRadians(-170.f)), 150.f * FMath::Sin(FMath::DegreesToRadians(-170.f)));
		float LowestExcursion = 0.f;
		float HighestExcursion = 0.f;
		for (int32 i = 0; i < 120; ++i)
		{
			CrowdSmoothing::Step(State, State.Pos, State.Vel, Dt, Frame, Params);
			const float Excursion = CrowdSmoothing::WrapDegrees(State.YawDeg - 170.f); // 0..20 = the short way
			LowestExcursion = FMath::Min(LowestExcursion, Excursion);
			HighestExcursion = FMath::Max(HighestExcursion, Excursion);
		}
		TestTrue(TEXT("yaw takes the short way round (never leaves the 20 degree arc)"), LowestExcursion >= -0.01f && HighestExcursion <= 20.01f);
		TestTrue(TEXT("... and arrives"), FMath::Abs(CrowdSmoothing::WrapDegrees(State.YawDeg + 170.f)) < 1.f);

		// A standing agent keeps its facing.
		CrowdSmoothing::FState Standing;
		Standing.YawDeg = 33.f;
		for (int32 i = 0; i < 60; ++i)
		{
			CrowdSmoothing::Step(Standing, Standing.Pos, Still, Dt, Frame, Params);
		}
		TestTrue(TEXT("a standing agent keeps facing where it faced"), Standing.YawDeg == 33.f);

		// A new agent that stands has no direction to face: it takes the one it is given.
		TestTrue(TEXT("first sighting of a standing agent uses the fallback facing"), CrowdSmoothing::Begin(FVector2D::ZeroVector, Still, 77.f).YawDeg == 77.f);
		TestTrue(TEXT("first sighting of a walking agent faces the way it walks"), FMath::Abs(CrowdSmoothing::Begin(FVector2D::ZeroVector, North, 77.f).YawDeg - 90.f) < 1.0e-3f);
	}

	// ---- the drawn path does not depend on the frame rate ----
	{
		// Compared in the middle of the curve after a turn (0.25 s later), where the drawn agent is still ~10 cm off the
		// prediction; two seconds later every frame rate has long converged and the test would see nothing.
		auto Run = [&](double FrameDt) -> FVector2D
		{
			const CrowdSmoothing::FFrame F = CrowdSmoothing::MakeFrame(Params, FrameDt);
			FVector2D Target = FVector2D::ZeroVector;
			FVector2f TargetVel = East;
			CrowdSmoothing::FState State = CrowdSmoothing::Begin(Target, TargetVel, 0.f);
			const int32 Frames = FMath::RoundToInt(1.25 / FrameDt);
			const int32 TurnFrame = FMath::RoundToInt(1.0 / FrameDt);
			for (int32 i = 0; i < Frames; ++i)
			{
				TargetVel = (i < TurnFrame) ? East : North;
				Target += FVector2D(TargetVel.X, TargetVel.Y) * FrameDt;
				CrowdSmoothing::Step(State, Target, TargetVel, FrameDt, F, Params);
			}
			return State.Pos;
		};
		const FVector2D At30 = Run(1.0 / 30.0);
		const FVector2D At60 = Run(1.0 / 60.0);
		const FVector2D At144 = Run(1.0 / 144.0);
		const double Off30 = FVector2D::Distance(At30, At144);
		const double Off60 = FVector2D::Distance(At60, At144);
		AddInfo(FString::Printf(TEXT("0.25 s into the curve after a turn: 30 / 60 fps are %.2f / %.2f cm away from where 144 fps draws the agent (the curve itself deviates up to 13 cm from the prediction)"), Off30, Off60));
		// What is left is the integration error of one explicit step per frame, O(dt). A per-frame constant blend (or
		// rate * dt instead of 1 - exp(-rate * dt)) would show up as tens of cm here.
		TestTrue(TEXT("60 fps draws the same curve as 144 fps within 2 cm"), Off60 < 2.0);
		TestTrue(TEXT("30 fps draws the same curve as 144 fps within 6 cm"), Off30 < 6.0);
	}

	return true;
}

// =================================================================================================
// Wander: the movement model of one agent (Crowd/CrowdWander.h)
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderGeometryTest, "MassBubble.Crowd.Wander.Geometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderGeometryTest::RunTest(const FString& Parameters)
{
	const CrowdWander::FBox2 Box{ FVector2D(0.0, 0.0), FVector2D(100.0, 100.0) };
	auto Near = [](float A, float B) { return FMath::Abs(A - B) < 1.0e-3f; };

	// ---- leaving a box ----
	TestTrue(TEXT("leave to the right"), Near(CrowdWander::TimeToLeave(Box, FVector2D(50.0, 50.0), FVector2f(10.f, 0.f)), 5.f));
	TestTrue(TEXT("leave to the left"), Near(CrowdWander::TimeToLeave(Box, FVector2D(20.0, 50.0), FVector2f(-10.f, 0.f)), 2.f));
	TestTrue(TEXT("leave diagonally: the nearer wall counts"), Near(CrowdWander::TimeToLeave(Box, FVector2D(50.0, 90.0), FVector2f(10.f, 10.f)), 1.f));
	TestTrue(TEXT("standing still never leaves"), CrowdWander::TimeToLeave(Box, FVector2D(50.0, 50.0), FVector2f(0.f, 0.f)) > 1.0e8f);
	TestTrue(TEXT("already outside: 0"), Near(CrowdWander::TimeToLeave(Box, FVector2D(150.0, 50.0), FVector2f(10.f, 0.f)), 0.f));

	// ---- entering a box ----
	TestTrue(TEXT("enter from the left"), Near(CrowdWander::TimeToEnter(Box, FVector2D(-50.0, 50.0), FVector2f(10.f, 0.f)), 5.f));
	TestTrue(TEXT("walking away never enters"), CrowdWander::TimeToEnter(Box, FVector2D(-50.0, 50.0), FVector2f(-10.f, 0.f)) > 1.0e8f);
	TestTrue(TEXT("passing by never enters"), CrowdWander::TimeToEnter(Box, FVector2D(-50.0, 150.0), FVector2f(10.f, 0.f)) > 1.0e8f);
	TestTrue(TEXT("a diagonal that clips the corner enters"), CrowdWander::TimeToEnter(Box, FVector2D(-10.0, 50.0), FVector2f(10.f, 5.f)) < 1.5f);
	TestTrue(TEXT("standing outside never enters"), CrowdWander::TimeToEnter(Box, FVector2D(-50.0, 50.0), FVector2f(0.f, 0.f)) > 1.0e8f);

	// ---- the band: wide enough for the fastest agent's arc (2 * v / w), never wider than a third of the region ----
	FCrowdTuning Tuning;
	const float Band = CrowdWander::LayerDepthCm(Tuning);
	const float Radius = Tuning.MaxSpeedCmPerSec / CrowdWander::WallTurnRate(Tuning);
	TestTrue(TEXT("band is wider than the deepest arc"), Band > 2.f * Radius);
	TestTrue(TEXT("band leaves open ground"), 2.0 * (CrowdWander::WallMarginCm + Band) < 0.5 * CrowdTestSupport::RegionCm);
	const CrowdWander::FBox2 Inner = CrowdWander::InnerBox(CrowdTestSupport::Region(), Tuning);
	const CrowdWander::FBox2 Spawn = CrowdWander::SpawnBox(CrowdTestSupport::Region(), Tuning);
	TestTrue(TEXT("spawn box lies strictly inside the open ground"), Spawn.Min.X > Inner.Min.X && Spawn.Max.Y < Inner.Max.Y);

	// ---- the arc check: an arc that turns away from the open ground is a trap, one that turns towards it is a way out ----
	{
		// Open ground is x > 0 (a big box). The agent stands in the band, 10 cm past the edge, walking along the edge (+Y).
		const CrowdWander::FBox2 Open{ FVector2D(0.0, -100000.0), FVector2D(100000.0, 100000.0) };
		const FVector2D P(-10.0, 0.0);
		const float R = 130.f;
		const float Half = 0.26f;
		// Heading +Y (90 degrees): turning left (counter clockwise) curves towards -X, deeper into the band ...
		TestFalse(TEXT("turning away from the open ground is a trap"), CrowdWander::ArcReachesOpenGround(Open, P, 0.5f * UE_PI, R, +1.f, Half));
		// ... turning right curves towards +X, back to open ground.
		TestTrue(TEXT("turning towards the open ground is a way out"), CrowdWander::ArcReachesOpenGround(Open, P, 0.5f * UE_PI, R, -1.f, Half));
	}

	// ---- handedness: both sides occur, per agent fixed ----
	int32 Left = 0;
	for (uint32 Id = 1; Id <= 10000; ++Id)
	{
		Left += (CrowdWander::TurnSignForId(Id) > 0.f) ? 1 : 0;
		if (CrowdWander::TurnSignForId(Id) != CrowdWander::TurnSignForId(Id))
		{
			TestTrue(TEXT("handedness is a pure function of the id"), false);
		}
	}
	TestTrue(TEXT("about half of the agents turn left"), Left > 4500 && Left < 5500);

	return true;
}

// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderTimeSlicingTest, "MassBubble.Crowd.Wander.TimeSlicing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderTimeSlicingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// The LOD time slicer steps far agents every N-th frame with the time they accumulated (UCrowdMovementProcessor).
	// That must not change where they walk: a step is split at segment boundaries, however many there are in it.
	//
	// Compared over windows of 30 s that START FROM THE SAME STATE. Over minutes the two ways of stepping drift apart by
	// float rounding alone (the segment timers are floats: about 1e-6 of the time), and a walk is a chaotic system: sooner
	// or later a pause roll or a band edge decision falls on the other side of its threshold. What matters is that
	// slicing introduces no difference of its own, and that is what a short window from an identical state shows.
	FCrowdTuning Tuning;
	int32 Differing = 0;
	int32 Windows = 0;
	int32 BandWindows = 0;
	double WorstCm = 0.0;

	for (uint32 Index = 0; Index < 24; ++Index)
	{
		FCrowdSavedAgent Reference = NewAgent(Index, Tuning); // stepped in 1/60 s frames; the walk continues from it
		uint32 Chunker = CrowdMath::NonZeroSeed(900u + Index);

		for (int32 Window = 0; Window < 20; ++Window) // 20 x 30 s = 10 minutes of walking, pauses, the band, ...
		{
			FCrowdSavedAgent Sliced = Reference;
			bool bTouchedBand = false;

			for (int32 Frame = 0; Frame < 1800; ++Frame)
			{
				Step(Reference, 1.f / 60.f, Tuning);
				bTouchedBand = bTouchedBand || DistanceToBorder(Reference.Location) < CrowdWander::LayerDepthCm(Tuning) + CrowdWander::WallMarginCm;
			}

			// The same 30 s in irregular steps: 1, 2 or 6 frames (LOD tiers), now and then a 0.5 s hitch (MaxStepDeltaSec).
			double Left = 30.0;
			while (Left > 1.0e-9)
			{
				const float Roll = CrowdMath::Random01(Chunker);
				const double Want = (Roll < 0.4f) ? 1.0 / 60.0 : (Roll < 0.7f) ? 2.0 / 60.0 : (Roll < 0.95f) ? 6.0 / 60.0 : 0.5;
				const double Chunk = FMath::Min(Want, Left);
				Step(Sliced, static_cast<float>(Chunk), Tuning);
				Left -= Chunk;
			}

			const double Distance = FVector2D::Distance(Reference.Location, Sliced.Location);
			WorstCm = FMath::Max(WorstCm, Distance);
			Differing += (Distance > 0.5) ? 1 : 0;
			BandWindows += bTouchedBand ? 1 : 0;
			++Windows;
		}
	}

	AddInfo(FString::Printf(TEXT("%d windows of 30 s (%d of them along the region border), largest difference between sliced and every-frame stepping: %.4f cm"), Windows, BandWindows, WorstCm));
	TestTrue(TEXT("test is not vacuous: many windows lead through the band along the border (many short segments)"), BandWindows > Windows / 4);
	TestEqual(TEXT("windows where time slicing changed the path"), Differing, 0);

	// Same seed, same crowd; different agents differ.
	const FCrowdSavedAgent A = NewAgent(7, Tuning);
	const FCrowdSavedAgent B = NewAgent(7, Tuning);
	const FCrowdSavedAgent C = NewAgent(8, Tuning);
	TestTrue(TEXT("same index => same agent"), A.Location.X == B.Location.X && A.Location.Y == B.Location.Y && A.Rng == B.Rng && A.Heading == B.Heading);
	TestTrue(TEXT("different index => different agent"), A.Location.X != C.Location.X || A.Location.Y != C.Location.Y);

	return true;
}

// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderContainmentTest, "MassBubble.Crowd.Wander.Containment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderContainmentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// Agents stay in their region (the unit of streaming and persistence) WITHOUT the safety clamp: the planner turns
	// them away in the band along the border. Slowest and fastest cruise speeds are covered on purpose, both turn
	// directions occur (ids), and the step sizes vary like they do under time slicing.
	FCrowdTuning Tuning;
	const CrowdWander::FBox2 Inner = CrowdWander::InnerBox(Region(), Tuning);

	constexpr int32 NumAgents = 300;
	constexpr double Seconds = 300.0;
	const float StepOptions[4] = { 1.f / 60.f, 1.f / 30.f, 0.1f, 0.25f };

	int32 ClampHits = 0;
	int64 BandVisits = 0;
	int64 TotalSteps = 0;
	int64 BandSteps = 0;
	int32 Lefties = 0;
	double ClosestCm = 1.0e9;
	double LongestStaySec = 0.0;

	for (int32 i = 0; i < NumAgents; ++i)
	{
		FCrowdSavedAgent Agent = NewAgent(static_cast<uint32>(i), Tuning);
		if (i % 3 == 0)
		{
			Agent.CruiseSpeed = Tuning.MinSpeedCmPerSec;
		}
		else if (i % 3 == 1)
		{
			Agent.CruiseSpeed = Tuning.MaxSpeedCmPerSec;
		}
		Lefties += (CrowdWander::TurnSignForId(Agent.NetId) > 0.f) ? 1 : 0;

		const float Dt = StepOptions[i % 4];
		bool bInBand = false;
		double StayStart = 0.0;
		for (double Now = 0.0; Now < Seconds; Now += Dt)
		{
			ClampHits += Step(Agent, Dt, Tuning) ? 1 : 0;
			ClosestCm = FMath::Min(ClosestCm, DistanceToBorder(Agent.Location));

			const bool bNowInBand = !CrowdWander::IsStrictlyInside(Inner, Agent.Location);
			if (bNowInBand && !bInBand)
			{
				++BandVisits;
				StayStart = Now;
			}
			if (bNowInBand)
			{
				LongestStaySec = FMath::Max(LongestStaySec, Now - StayStart);
			}
			bInBand = bNowInBand;

			++TotalSteps;
			BandSteps += bNowInBand ? 1 : 0;
		}
	}

	AddInfo(FString::Printf(TEXT("%d agents x %.0f s: band visits %lld, time in the band %.1f%%, longest stay %.1f s, closest to the border %.0f cm, safety clamp hits %d"),
		NumAgents, Seconds, BandVisits, 100.0 * static_cast<double>(BandSteps) / static_cast<double>(TotalSteps), LongestStaySec, ClosestCm, ClampHits));

	TestTrue(TEXT("test is not vacuous: the agents did use the band"), BandVisits > 300);
	TestTrue(TEXT("test is not vacuous: both turn directions occur"), Lefties > NumAgents / 4 && Lefties < NumAgents * 3 / 4);
	TestEqual(TEXT("agents the safety clamp had to rescue"), ClampHits, 0);
	TestTrue(TEXT("nobody ever comes closer to the border than the margin"), ClosestCm >= CrowdWander::WallMarginCm);
	// One arc takes at most a full circle (2 pi / w = 6 s) plus a pause or two. A trap would show as a stay of minutes.
	TestTrue(TEXT("nobody is stuck in the band"), LongestStaySec < 40.0);
	return true;
}

// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderRecoveryTest, "MassBubble.Crowd.Wander.Recovery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// States the planner may find itself in without having walked into the band from open ground: a tuning change at
	// runtime, a restored save, a safety clamp. Every agent must find its way out again.
	FCrowdTuning Tuning;
	const CrowdWander::FBox2 Inner = CrowdWander::InnerBox(Region(), Tuning);
	const float Band = CrowdWander::LayerDepthCm(Tuning);

	// ---- A) dropped into the band, up to half its depth, at any heading: out within 20 s, no clamp, no touching the border ----
	{
		int32 NotOut = 0;
		int32 ClampHits = 0;
		double ClosestCm = 1.0e9;
		constexpr int32 NumAgents = 400;
		uint32 Rng = 4242u;

		for (int32 i = 0; i < NumAgents; ++i)
		{
			FCrowdSavedAgent Agent = NewAgent(static_cast<uint32>(i), Tuning);
			const double Depth = CrowdMath::Random01(Rng) * 0.5 * Band;            // from the inner edge, towards the wall
			const double Along = FMath::Lerp(Inner.Min.X, Inner.Max.X, static_cast<double>(CrowdMath::Random01(Rng)));
			switch (i % 4)
			{
			case 0: Agent.Location = FVector2D(Inner.Min.X - Depth, Along); break;
			case 1: Agent.Location = FVector2D(Inner.Max.X + Depth, Along); break;
			case 2: Agent.Location = FVector2D(Along, Inner.Min.Y - Depth); break;
			default: Agent.Location = FVector2D(Along, Inner.Max.Y + Depth); break;
			}
			Agent.Heading = (2.f * CrowdMath::Random01(Rng) - 1.f) * UE_PI;
			Agent.CruiseSpeed = FMath::Lerp(Tuning.MinSpeedCmPerSec, Tuning.MaxSpeedCmPerSec, CrowdMath::Random01(Rng));
			Agent.Velocity = FVector2f::ZeroVector;
			Agent.RetargetTimer = 0.f;

			bool bLeft = false;
			for (int32 Frame = 0; Frame < 60 * 20; ++Frame)
			{
				ClampHits += Step(Agent, 1.f / 60.f, Tuning) ? 1 : 0;
				ClosestCm = FMath::Min(ClosestCm, DistanceToBorder(Agent.Location));
				bLeft = bLeft || CrowdWander::IsStrictlyInside(Inner, Agent.Location);
			}
			NotOut += bLeft ? 0 : 1;
		}

		AddInfo(FString::Printf(TEXT("dropped into the band: %d of %d did not get out within 20 s, clamp hits %d, closest to the border %.0f cm"), NotOut, NumAgents, ClampHits, ClosestCm));
		TestEqual(TEXT("agents dropped into the band that did not find the way out"), NotOut, 0);
		TestEqual(TEXT("... that needed the safety clamp"), ClampHits, 0);
		TestTrue(TEXT("... that came closer to the border than the margin"), ClosestCm >= CrowdWander::WallMarginCm);
	}

	// ---- B) dropped ANYWHERE, even against the wall facing it: they stay in the region and get out of the band within a minute ----
	{
		int32 NotOut = 0;
		int32 Outside = 0;
		constexpr int32 NumAgents = 400;
		uint32 Rng = 777u;

		for (int32 i = 0; i < NumAgents; ++i)
		{
			FCrowdSavedAgent Agent = NewAgent(static_cast<uint32>(i), Tuning);
			Agent.Location = FVector2D(CrowdMath::Random01(Rng) * RegionCm, CrowdMath::Random01(Rng) * RegionCm);
			if (i % 2 == 0)
			{
				Agent.Location.X = (i % 4 == 0) ? 20.0 : RegionCm - 20.0; // right at the wall
			}
			Agent.Heading = (2.f * CrowdMath::Random01(Rng) - 1.f) * UE_PI;
			Agent.CruiseSpeed = FMath::Lerp(Tuning.MinSpeedCmPerSec, Tuning.MaxSpeedCmPerSec, CrowdMath::Random01(Rng));
			Agent.Velocity = FVector2f::ZeroVector;
			Agent.RetargetTimer = 0.f;

			bool bLeft = false;
			for (int32 Frame = 0; Frame < 60 * 60; ++Frame)
			{
				Step(Agent, 1.f / 60.f, Tuning);
				bLeft = bLeft || CrowdWander::IsStrictlyInside(Inner, Agent.Location);
				Outside += (DistanceToBorder(Agent.Location) < 0.0) ? 1 : 0;
			}
			NotOut += bLeft ? 0 : 1;
		}

		AddInfo(FString::Printf(TEXT("dropped anywhere: %d of %d did not reach open ground within 60 s"), NotOut, NumAgents));
		TestEqual(TEXT("agents that never left the band"), NotOut, 0);
		TestEqual(TEXT("agents that left the region"), Outside, 0);
	}

	return true;
}

// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderSmoothnessTest, "MassBubble.Crowd.Wander.Smoothness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderSmoothnessTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// What the old model got wrong: every new segment was a uniformly random heading (a third of them reversals), the
	// speed was re-rolled each time and a quarter of the crowd stood still. Here: small turns, steady speed.
	FCrowdTuning Tuning;
	constexpr int32 NumAgents = 300;
	constexpr double Seconds = 300.0;
	constexpr float Dt = 1.f / 30.f;

	int64 Turns = 0;
	int64 Over90 = 0;
	double SumTurnDeg = 0.0;
	double MaxTurnDeg = 0.0;
	double SlowestWalk = 1.0e9;
	double FastestWalk = 0.0;
	int64 PausedSteps = 0;
	int64 TotalSteps = 0;
	int64 DoublePauses = 0;
	int64 Pauses = 0;

	for (int32 i = 0; i < NumAgents; ++i)
	{
		FCrowdSavedAgent Agent = NewAgent(static_cast<uint32>(i), Tuning);
		FVector2f LastWalking = FVector2f::ZeroVector;
		bool bLastWasPause = false;

		for (double Now = 0.0; Now < Seconds; Now += Dt)
		{
			const FVector2f Before = Agent.Velocity;
			Step(Agent, Dt, Tuning);
			const FVector2f After = Agent.Velocity;

			const bool bPaused = After.SizeSquared() < 1.f;
			++TotalSteps;
			PausedSteps += bPaused ? 1 : 0;

			if (After.X != Before.X || After.Y != Before.Y) // a new segment began during this step
			{
				if (bPaused)
				{
					++Pauses;
					DoublePauses += bLastWasPause ? 1 : 0;
				}
				else
				{
					const double Speed = After.Size();
					SlowestWalk = FMath::Min(SlowestWalk, Speed);
					FastestWalk = FMath::Max(FastestWalk, Speed);
					if (LastWalking.SizeSquared() > 1.f)
					{
						const double Angle = AngleBetweenDeg(LastWalking, After);
						SumTurnDeg += Angle;
						MaxTurnDeg = FMath::Max(MaxTurnDeg, Angle);
						Over90 += (Angle > 90.0) ? 1 : 0;
						++Turns;
					}
					LastWalking = After;
				}
				bLastWasPause = bPaused;
			}
		}
	}

	const double MeanTurn = SumTurnDeg / static_cast<double>(FMath::Max<int64>(Turns, 1));
	const double PausedShare = static_cast<double>(PausedSteps) / static_cast<double>(TotalSteps);
	const double ExpectedShare = CrowdWander::SteadyStateIdleShare(Tuning);
	AddInfo(FString::Printf(TEXT("%lld turns: mean %.1f deg, max %.1f deg, over 90 deg: %lld; walking speed %.0f..%.0f cm/s; paused %.1f%% (model %.1f%%), %lld pauses"),
		Turns, MeanTurn, MaxTurnDeg, Over90, SlowestWalk, FastestWalk, 100.0 * PausedShare, 100.0 * ExpectedShare, Pauses));

	// A turn between two walking segments is the random turn (<= MaxTurnDeg) plus at most half a chord of the arc in the band.
	const double HalfChordDeg = 0.5 * Tuning.WallTurnRateDeg * CrowdWander::LayerChordSec;
	TestTrue(TEXT("test is not vacuous"), Turns > 20000 && Pauses > 500);
	TestTrue(TEXT("no turn is larger than the model allows"), MaxTurnDeg <= Tuning.MaxTurnDeg + HalfChordDeg + 1.0);
	TestEqual(TEXT("reversals (turns over 90 degrees)"), static_cast<int32>(Over90), 0);
	TestTrue(TEXT("mean turn is gentle"), MeanTurn < 30.0);
	TestTrue(TEXT("walking speed stays in the configured range"), SlowestWalk >= 0.98 * Tuning.MinSpeedCmPerSec && FastestWalk <= 1.001 * Tuning.MaxSpeedCmPerSec);
	TestTrue(TEXT("share of standing agents matches the model"), FMath::Abs(PausedShare - ExpectedShare) < 0.025);
	TestEqual(TEXT("pauses that follow a pause directly"), static_cast<int32>(DoublePauses), 0);
	return true;
}

// -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdWanderDensityTest, "MassBubble.Crowd.Wander.Density",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdWanderDensityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// Where do the agents spend their time? The first border handling (steer away when heading at the wall) piled agents
	// up along the border: 3.7 times the interior density within 1-2 m. A reflecting wall keeps the density uniform
	// but turns agents around on the spot. The magnetic band keeps the OPEN GROUND uniform, bends agents away smoothly
	// and thins out the last few metres in front of the border (an arc of radius v/w needs room to turn).
	FCrowdTuning Tuning;
	constexpr int32 NumAgents = 1000;
	constexpr double Seconds = 400.0;
	constexpr double Warmup = 60.0;
	constexpr float Dt = 1.f / 20.f;
	constexpr int32 Bins = 64; // metres to the nearest border, 1 m each (the region is 128 m wide)

	TArray<double> Counts;
	Counts.Init(0.0, Bins);

	TArray<FCrowdSavedAgent> Agents;
	Agents.SetNum(NumAgents);
	for (int32 i = 0; i < NumAgents; ++i)
	{
		Agents[i] = NewAgent(static_cast<uint32>(i), Tuning);
	}

	int32 Samples = 0;
	for (double Now = 0.0; Now < Seconds; Now += Dt)
	{
		for (FCrowdSavedAgent& Agent : Agents)
		{
			Step(Agent, Dt, Tuning);
		}
		if (Now > Warmup && FMath::Fmod(Now, 1.0) < Dt)
		{
			for (const FCrowdSavedAgent& Agent : Agents)
			{
				const int32 Bin = FMath::FloorToInt(DistanceToBorder(Agent.Location) / 100.0);
				if (Bin >= 0 && Bin < Bins)
				{
					Counts[Bin] += 1.0;
				}
			}
			++Samples;
		}
	}

	// Density per area (the ring of bin b has area (L - 2b)^2 - (L - 2b - 2)^2) relative to the open ground.
	const double L = RegionCm / 100.0;
	TArray<double> Density;
	Density.Init(0.0, Bins);
	for (int32 b = 0; b < Bins; ++b)
	{
		const double Area = (L - 2.0 * b) * (L - 2.0 * b) - (L - 2.0 * b - 2.0) * (L - 2.0 * b - 2.0);
		Density[b] = Area > 0.0 ? Counts[b] / Area : 0.0;
	}
	double Reference = 0.0;
	for (int32 b = 15; b < 60; ++b)
	{
		Reference += Density[b];
	}
	Reference /= 45.0;

	double WorstOpenGround = 0.0;     // furthest deviation from 1 on open ground, away from the band (>= 8 m from the border)
	double HighestAnywhere = 0.0;     // highest density anywhere (a pile-up would show here)
	for (int32 b = 0; b < Bins - 4; ++b)
	{
		const double Relative = Density[b] / Reference;
		HighestAnywhere = FMath::Max(HighestAnywhere, Relative);
		if (b >= 8)
		{
			WorstOpenGround = FMath::Max(WorstOpenGround, FMath::Abs(Relative - 1.0));
		}
	}
	const double Strip = (Density[0] + Density[1]) * 0.5 / Reference; // within 2 m of the border

	FString Profile;
	for (int32 b = 0; b < 9; ++b)
	{
		Profile += FString::Printf(TEXT("%d-%dm %.2f  "), b, b + 1, Density[b] / Reference);
	}
	AddInfo(FString::Printf(TEXT("density relative to open ground by distance to the border: %s| worst open-ground deviation %.0f%%, highest %.2f"),
		*Profile, 100.0 * WorstOpenGround, HighestAnywhere));

	TestTrue(TEXT("open ground (8 m and more from the border) is uniform within 12%"), WorstOpenGround < 0.12);
	TestTrue(TEXT("no pile-up anywhere (the steering version had 3.7)"), HighestAnywhere < 1.25);
	TestTrue(TEXT("the strip in front of the border is thin"), Strip < 0.25);
	return true;
}

// =================================================================================================
// What the player sees: server walk -> replication -> client drawing, end to end
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdPresentationTest, "MassBubble.Crowd.Presentation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdPresentationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdTestSupport;

	// The report that started this: "the cylinders walk, then go back to where they started and walk again".
	// What that is, measurably: a drawn agent that STANDS although it walks (frozen), and then JUMPS (backward or
	// forward). The same numbers are taken for the first version (so we know the detector sees the problem) and for
	// the current pipeline, with the real CrowdWander / CrowdNet / CrowdSmoothing code in both.
	FPipelineOptions LegacyOptions;
	LegacyOptions.bLegacy = true;

	FPipelineOptions CurrentOptions;

	FPipelineOptions LaggyOptions; // a real connection: 80 ms one way plus up to 40 ms jitter
	LaggyOptions.LatencySec = 0.08;
	LaggyOptions.JitterSec = 0.04;

	const FViewMetrics Before = RunPipeline(LegacyOptions);
	const FViewMetrics After = RunPipeline(CurrentOptions);
	const FViewMetrics AfterLaggy = RunPipeline(LaggyOptions);

	AddInfo(FString::Printf(TEXT("first version:   %s"), *Before.Describe()));
	AddInfo(FString::Printf(TEXT("current:         %s"), *After.Describe()));
	AddInfo(FString::Printf(TEXT("current, laggy:  %s"), *AfterLaggy.Describe()));

	// ---- the detector sees the reported behaviour in the first version ----
	TestTrue(TEXT("(for contrast) the first version stood still for a large part of the time it should walk"), Before.FrozenShare() > 0.20);
	TestTrue(TEXT("(for contrast) ... and sent agents back (backward jumps every few seconds)"), Before.BackwardPerAgentMinute() > 3.0);
	TestTrue(TEXT("(for contrast) ... in steps of metres"), Before.MaxStepCm > 150.0);
	TestTrue(TEXT("(for contrast) ... with errors of several metres"), Before.Error.Percentile(0.99) > 300.0);

	// ---- the current pipeline: no standing, no jumps, bounded error ----
	TestEqual(TEXT("agents the safety clamp had to rescue"), static_cast<int32>(After.ClampHits), 0);
	TestTrue(TEXT("test is not vacuous: agents walked"), After.WalkingFrames > After.AgentFrames / 2);
	TestTrue(TEXT("a walking agent is drawn standing less than 1% of the time"), After.FrozenShare() < 0.01);
	TestEqual(TEXT("backward jumps"), static_cast<int32>(After.BackwardFrames), 0);
	TestEqual(TEXT("steps over 60 cm in one frame"), static_cast<int32>(After.BigStepFrames), 0);
	TestTrue(TEXT("no drawn step is larger than 12 cm in one frame (7 m/s at 60 fps; a 180 cm/s walker makes 3 cm)"), After.MaxStepCm <= 12.0);
	TestTrue(TEXT("99% of the time the drawn agent is within 40 cm of the truth"), After.Error.Percentile(0.99) <= 40.0);
	TestTrue(TEXT("... and never further than 90 cm"), After.Error.Max <= 90.0);
	TestTrue(TEXT("replication costs less than 15% of resending everything that moves"), After.MessageShare() < 0.15);

	// ---- the same with a real connection ----
	TestTrue(TEXT("with latency and jitter: standing less than 2% of the time it walks"), AfterLaggy.FrozenShare() < 0.02);
	TestEqual(TEXT("with latency and jitter: backward jumps"), static_cast<int32>(AfterLaggy.BackwardFrames), 0);
	TestTrue(TEXT("with latency and jitter: no step over 12 cm"), AfterLaggy.MaxStepCm <= 12.0);
	TestTrue(TEXT("with latency and jitter: 99% within 60 cm"), AfterLaggy.Error.Percentile(0.99) <= 60.0);
	return true;
}

// =================================================================================================
// Interest set hysteresis
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdInterestTest, "MassBubble.Crowd.Interest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdInterestTest::RunTest(const FString& Parameters)
{
	// ---- the numbers ----
	TestTrue(TEXT("a member counts as closer than a newcomer at the same distance"), CrowdNet::InterestRank(10000.0, true) < CrowdNet::InterestRank(10000.0, false));
	TestTrue(TEXT("a member may stay 10% beyond the radius a newcomer needs"), FMath::IsNearlyEqual(CrowdNet::InterestExitRadius(15000.0), 16500.0, 1.0e-6));
	TestTrue(TEXT("... but never beyond what the int16 wire format can carry"), FMath::IsNearlyEqual(CrowdNet::InterestExitRadius(21000.0), 21000.0, 1.0e-6));
	{
		bool bAlwaysFits = true;
		for (double Radius = 100.0; Radius <= 1.0e6; Radius *= 1.7)
		{
			bAlwaysFits = bAlwaysFits && CrowdNet::InterestExitRadius(Radius) <= static_cast<double>(CrowdNet::MaxBubbleRadiusCm);
		}
		TestTrue(TEXT("the exit radius never exceeds MaxBubbleRadiusCm"), bAlwaysFits);
	}
	{
		double Rank = 0.0;
		const double Enter = 10000.0;
		TestFalse(TEXT("a newcomer outside the radius is no candidate"), CrowdNet::InterestCandidate(10500.0 * 10500.0, false, Enter, Rank));
		TestTrue(TEXT("a newcomer inside is"), CrowdNet::InterestCandidate(9500.0 * 9500.0, false, Enter, Rank));
		TestTrue(TEXT("a member 5% outside the radius stays"), CrowdNet::InterestCandidate(10500.0 * 10500.0, true, Enter, Rank));
		TestFalse(TEXT("a member 15% outside the radius leaves"), CrowdNet::InterestCandidate(11500.0 * 11500.0, true, Enter, Rank));
	}

	// ---- the effect: a crowd that walks around the edge of the interest set ----
	// Four regions with 300 agents each around a viewer who stands at their common corner, a bubble that holds 300.
	// The edge of the set is then a rank cut at about 80 m, and agents wander across it all the time.
	FCrowdTuning Tuning;
	constexpr int32 PerRegion = 300;
	constexpr int32 NumAgents = 4 * PerRegion;
	constexpr int32 MaxAgents = 300;
	constexpr double EnterRadius = 15000.0;
	constexpr double Dt = 0.1; // replication tick
	constexpr double Seconds = 120.0;
	const FVector2D Viewer(CrowdTestSupport::RegionCm, CrowdTestSupport::RegionCm);

	struct FWalker
	{
		FCrowdSavedAgent Agent;
		CrowdWander::FBox2 Home;
	};
	TArray<FWalker> Walkers;
	Walkers.SetNum(NumAgents);
	for (int32 i = 0; i < NumAgents; ++i)
	{
		const int32 Region = i / PerRegion;
		const FVector2D Min(CrowdTestSupport::RegionCm * static_cast<double>(Region % 2), CrowdTestSupport::RegionCm * static_cast<double>(Region / 2));
		Walkers[i].Home = CrowdWander::FBox2{ Min, Min + FVector2D(CrowdTestSupport::RegionCm, CrowdTestSupport::RegionCm) };
		Walkers[i].Agent.NetId = static_cast<uint32>(i) + 1u;
		CrowdWander::InitNewAgent(Walkers[i].Agent, 4242u + static_cast<uint32>(i) * 2654435761u, Walkers[i].Home, Tuning);
	}

	// One interest set update as ACrowdBubble::ServerRebuild step 2 does it. The candidate rule and the rank are the
	// real CrowdNet functions; without hysteresis nobody counts as a member (= the first version).
	struct FCandidate
	{
		int32 Index = 0;
		double Rank = 0.0;
	};
	auto Update = [&](bool bHysteresis, TArray<uint8>& InSet, int32& OutEntered, int32& OutLeft, int32& OutSize, bool& bOutNewcomerOutside, bool& bOutMemberTooFar)
	{
		TArray<FCandidate> Candidates;
		for (int32 i = 0; i < NumAgents; ++i)
		{
			const double DistSq = FVector2D::DistSquared(Walkers[i].Agent.Location, Viewer);
			FCandidate Candidate;
			Candidate.Index = i;
			if (CrowdNet::InterestCandidate(DistSq, bHysteresis && InSet[i] != 0, EnterRadius, Candidate.Rank))
			{
				Candidates.Add(Candidate);
			}
		}
		if (Candidates.Num() > MaxAgents)
		{
			Candidates.Sort([](const FCandidate& L, const FCandidate& R) { return L.Rank < R.Rank; });
			Candidates.SetNum(MaxAgents, EAllowShrinking::No);
		}

		TArray<uint8> Next;
		Next.Init(0, NumAgents);
		for (const FCandidate& Candidate : Candidates)
		{
			Next[Candidate.Index] = 1;
			const double Dist = FVector2D::Distance(Walkers[Candidate.Index].Agent.Location, Viewer);
			if (InSet[Candidate.Index] == 0)
			{
				++OutEntered;
				bOutNewcomerOutside = bOutNewcomerOutside || Dist > EnterRadius;
			}
			bOutMemberTooFar = bOutMemberTooFar || Dist > CrowdNet::InterestExitRadius(EnterRadius);
		}
		for (int32 i = 0; i < NumAgents; ++i)
		{
			OutLeft += (InSet[i] != 0 && Next[i] == 0) ? 1 : 0;
		}
		InSet = Next;
		OutSize = Candidates.Num();
	};

	TArray<uint8> SetNaive;
	TArray<uint8> SetHysteresis;
	SetNaive.Init(0, NumAgents);
	SetHysteresis.Init(0, NumAgents);

	int32 NaiveEntered = 0, NaiveLeft = 0, HysteresisEntered = 0, HysteresisLeft = 0;
	int32 NaiveSize = 0, HysteresisSize = 0, LargestSet = 0;
	bool bNewcomerOutside = false;
	bool bMemberTooFar = false;
	int32 Ticks = 0;
	for (double Now = 0.0; Now < Seconds; Now += Dt)
	{
		for (FWalker& Walker : Walkers)
		{
			CrowdWander::Advance(Walker.Agent.Location, Walker.Agent, static_cast<float>(Dt), Walker.Home, Tuning, CrowdWander::TurnSignForId(Walker.Agent.NetId));
		}

		int32 EnteredNaive = 0, LeftNaive = 0, EnteredHysteresis = 0, LeftHysteresis = 0;
		Update(false, SetNaive, EnteredNaive, LeftNaive, NaiveSize, bNewcomerOutside, bMemberTooFar);
		Update(true, SetHysteresis, EnteredHysteresis, LeftHysteresis, HysteresisSize, bNewcomerOutside, bMemberTooFar);
		LargestSet = FMath::Max(LargestSet, FMath::Max(NaiveSize, HysteresisSize));

		if (Ticks++ > 0) // the very first update fills the set from nothing
		{
			NaiveEntered += EnteredNaive;
			NaiveLeft += LeftNaive;
			HysteresisEntered += EnteredHysteresis;
			HysteresisLeft += LeftHysteresis;
		}
	}

	const double PerSecond = 1.0 / (Seconds - Dt);
	AddInfo(FString::Printf(TEXT("%d agents, set of %d: without hysteresis %.1f enter / %.1f leave per second, with hysteresis %.1f / %.1f per second"),
		NumAgents, MaxAgents, NaiveEntered * PerSecond, NaiveLeft * PerSecond, HysteresisEntered * PerSecond, HysteresisLeft * PerSecond));

	TestTrue(TEXT("test is not vacuous: the set is full and agents cross its edge"), NaiveSize == MaxAgents && NaiveEntered > 300);
	TestTrue(TEXT("the set never grows beyond what the bubble holds"), LargestSet <= MaxAgents);
	TestFalse(TEXT("a newcomer was never outside the normal radius"), bNewcomerOutside);
	TestFalse(TEXT("a member was never outside the exit radius"), bMemberTooFar);
	TestTrue(TEXT("hysteresis removes at least a third of the entering and leaving"), HysteresisEntered * 3 < NaiveEntered * 2 && HysteresisLeft * 3 < NaiveLeft * 2);
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
