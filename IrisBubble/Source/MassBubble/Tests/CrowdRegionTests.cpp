// Unit tests for the region replication format (Net/CrowdNetMath.h) and the region simulation (Crowd/CrowdSim.h).
// Run in the editor:  Session Frontend > Automation > filter "MassBubble.Crowd.Region"
// or headless:        UnrealEditor-Cmd MassBubble.uproject -ExecCmds="Automation RunTests MassBubble.Crowd.Region; Quit" -unattended -nullrhi -log
//
// Like the rest of the crowd logic these functions need no world, no replication system and no network connection:
// ACrowdRegionReplicator only stores what CrowdNet quantizes and UCrowdSubsystem only calls CrowdSim once per region.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Crowd/CrowdLodTable.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdSim.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"
#include "Net/CrowdNetMath.h"

#include "Misc/AutomationTest.h"

namespace CrowdRegionTestSupport
{
	constexpr double RegionCm = 12800.0;

	inline CrowdWander::FBox2 HomeBox(const FIntPoint& Coord)
	{
		const FVector2D Min = CrowdMath::RegionMin(Coord, static_cast<float>(RegionCm));
		return CrowdWander::FBox2{ Min, Min + FVector2D(RegionCm, RegionCm) };
	}

	/** New agents exactly as UCrowdSubsystem::MakeAgent creates them. */
	inline TArray<FCrowdAgentSim> MakeAgents(const FIntPoint& Coord, int32 Num, uint32 Seed, const FCrowdTuning& Tuning)
	{
		TArray<FCrowdAgentSim> Agents;
		Agents.Reserve(Num);
		for (int32 i = 0; i < Num; ++i)
		{
			FCrowdSavedAgent Saved;
			Saved.NetId = static_cast<uint32>(i) + 1u;
			CrowdWander::InitNewAgent(Saved, Seed + static_cast<uint32>(i) * 2654435761u, HomeBox(Coord), Tuning);
			Agents.Add(CrowdSim::FromSaved(Saved));
		}
		return Agents;
	}
}

// =================================================================================================
// Quantization
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdRegionQuantizationTest, "MassBubble.Crowd.Region.Quantization",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdRegionQuantizationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdRegionTestSupport;

	const double Step = CrowdNet::RegionOffsetStepCm(RegionCm);

	// ---- position: the error is at most half a step, over the whole region ----
	double MaxPosError = 0.0;
	for (double Cm = 0.0; Cm <= RegionCm; Cm += 0.37)
	{
		const double Back = CrowdNet::DequantizeRegionOffset(CrowdNet::QuantizeRegionOffset(Cm, RegionCm), RegionCm);
		MaxPosError = FMath::Max(MaxPosError, FMath::Abs(Back - Cm));
	}
	AddInfo(FString::Printf(TEXT("one step is %.4f cm, largest round trip error %.4f cm"), Step, MaxPosError));
	TestTrue(TEXT("position round trip error <= half a step"), MaxPosError <= 0.5 * Step + 1.0e-9);
	TestTrue(TEXT("a step is finer than a quarter of a centimetre in a 128 m region"), Step < 0.25);

	TestEqual(TEXT("the lower border is step 0"), static_cast<int32>(CrowdNet::QuantizeRegionOffset(0.0, RegionCm)), 0);
	TestEqual(TEXT("the upper border is the last step"), static_cast<int32>(CrowdNet::QuantizeRegionOffset(RegionCm, RegionCm)), 65535);
	TestEqual(TEXT("offset clamps high"), static_cast<int32>(CrowdNet::QuantizeRegionOffset(1.0e9, RegionCm)), 65535);
	TestEqual(TEXT("offset clamps low"), static_cast<int32>(CrowdNet::QuantizeRegionOffset(-1.0e9, RegionCm)), 0);

	// ---- offsets are relative to the region: the precision does not depend on the distance to the world origin ----
	{
		const FIntPoint Far(-1000, 2000);
		const FVector2D Min = CrowdMath::RegionMin(Far, static_cast<float>(RegionCm));
		double WorstFar = 0.0;
		for (double Cm = 0.0; Cm <= RegionCm; Cm += 91.3)
		{
			const double World = Min.X + Cm;
			const double Offset = World - Min.X;
			const double Back = Min.X + CrowdNet::DequantizeRegionOffset(CrowdNet::QuantizeRegionOffset(Offset, RegionCm), RegionCm);
			WorstFar = FMath::Max(WorstFar, FMath::Abs(Back - World));
		}
		TestTrue(TEXT("a region 128 km from the origin is as precise as the first one"), WorstFar <= 0.5 * Step + 1.0e-6);
	}

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

	return true;
}

// =================================================================================================
// Time stamps: the age of a shared state
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdRegionStampTest, "MassBubble.Crowd.Region.Stamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdRegionStampTest::RunTest(const FString& Parameters)
{
	using namespace CrowdRegionTestSupport;

	const double Tick = 1.0 / CrowdNet::StampHz;

	// ---- the age between two stamps is the time between them to within one tick, also across the 8 bit wrap ----
	double WorstError = 0.0;
	for (double Now = 3.5; Now < 60.0; Now += 0.0137)
	{
		for (const double Age : { 0.0, 0.1, 0.5, 1.0, 2.0, 3.0 })
		{
			const double Measured = CrowdNet::StampAgeSec(CrowdNet::StampOf(Now), CrowdNet::StampOf(Now - Age));
			WorstError = FMath::Max(WorstError, FMath::Abs(Measured - Age));
		}
	}
	AddInfo(FString::Printf(TEXT("stamp tick %.1f ms, worst age error %.1f ms"), Tick * 1000.0, WorstError * 1000.0));
	TestTrue(TEXT("the stamp age matches the elapsed time within one tick"), WorstError < Tick + 1.0e-9);
	TestEqual(TEXT("a stamp is the same again exactly 8 s later (the wrap the constants stay clear of)"),
		static_cast<int32>(CrowdNet::StampAgeSec(CrowdNet::StampOf(8.0 + 1.0), CrowdNet::StampOf(1.0)) * CrowdNet::StampHz), 0);
	TestTrue(TEXT("a state survives heartbeat plus extrapolation cap before its stamp wraps"),
		255.0 * Tick > CrowdNet::HeartbeatSec + CrowdNet::MaxExtrapolationSec);

	// ---- a client that joins late receives the state of the last publish. With stamps it draws every agent where it is. ----
	{
		FCrowdTuning Tuning;
		constexpr int32 NumAgents = 300;
		constexpr double Dt = 1.0 / 60.0;
		const FIntPoint Coord(0, 0);

		TArray<FCrowdAgentSim> Agents = MakeAgents(Coord, NumAgents, 900u, Tuning);
		for (FCrowdAgentSim& Agent : Agents)
		{
			Agent.Tier = ECrowdLOD::High;
		}

		struct FItem
		{
			uint16 X = 0;
			uint16 Y = 0;
			int8 VX = 0;
			int8 VY = 0;
			uint8 Stamp = 0;
		};
		TArray<FItem> Items;
		Items.SetNum(NumAgents);

		// What UCrowdSubsystem::PublishRegions + the replicator do for one agent.
		auto Write = [&](int32 Index, const FVector2f& Velocity, double Now)
		{
			FCrowdAgentSim& Agent = Agents[Index];
			FItem& Item = Items[Index];
			Item.X = CrowdNet::QuantizeRegionOffset(Agent.Location.X, RegionCm);
			Item.Y = CrowdNet::QuantizeRegionOffset(Agent.Location.Y, RegionCm);
			Item.VX = CrowdNet::QuantizeVelocity(Velocity.X);
			Item.VY = CrowdNet::QuantizeVelocity(Velocity.Y);
			Item.Stamp = CrowdNet::StampOf(Now);
			Agent.SentPos = FVector2D(CrowdNet::DequantizeRegionOffset(Item.X, RegionCm), CrowdNet::DequantizeRegionOffset(Item.Y, RegionCm));
			Agent.SentVel = FVector2f(CrowdNet::DequantizeVelocity(Item.VX), CrowdNet::DequantizeVelocity(Item.VY));
			Agent.SentTime = Now;
		};

		for (int32 i = 0; i < NumAgents; ++i)
		{
			Write(i, Agents[i].Velocity, 0.0);
		}
		uint8 Newest = CrowdNet::StampOf(0.0);
		double NewestTime = 0.0;

		CrowdSim::FMoveParams Move;
		Move.Tuning = &Tuning;
		Move.bTimeSlice = false;
		Move.DeltaSeconds = static_cast<float>(Dt);

		double Now = 0.0;
		for (int32 Frame = 1; Frame <= 3600; ++Frame) // 60 s, replication at 10 Hz
		{
			Now = static_cast<double>(Frame) * Dt;
			Move.Frame = static_cast<uint32>(Frame);
			CrowdSim::MoveRegion(Agents, Coord, Move);

			if (Frame % 6 == 0)
			{
				bool bAnyWritten = false;
				for (int32 i = 0; i < NumAgents; ++i)
				{
					const FVector2f Velocity = CrowdSim::PublishedVelocity(Agents[i]);
					if (CrowdSim::NeedsPublish(Agents[i], Velocity, Now, Tuning, /*bDeadReckoning=*/true))
					{
						Write(i, Velocity, Now);
						bAnyWritten = true;
					}
				}
				if (bAnyWritten)
				{
					Newest = CrowdNet::StampOf(Now);
					NewestTime = Now;
				}
			}
		}

		// The client gets the whole array at once and has no history: what it draws at this moment, against the truth.
		const double SinceNewest = Now - NewestTime;
		double WorstWithStamps = 0.0;
		double WorstWithoutStamps = 0.0;
		for (int32 i = 0; i < NumAgents; ++i)
		{
			const FItem& Item = Items[i];
			const FVector2D Base(CrowdNet::DequantizeRegionOffset(Item.X, RegionCm), CrowdNet::DequantizeRegionOffset(Item.Y, RegionCm));
			const FVector2f Velocity(CrowdNet::DequantizeVelocity(Item.VX), CrowdNet::DequantizeVelocity(Item.VY));

			const double Age = CrowdNet::StampAgeSec(Newest, Item.Stamp) + SinceNewest;
			WorstWithStamps = FMath::Max(WorstWithStamps, FVector2D::Distance(CrowdNet::Extrapolate(Base, Velocity, Age), Agents[i].Location));
			WorstWithoutStamps = FMath::Max(WorstWithoutStamps, FVector2D::Distance(Base, Agents[i].Location)); // age = time since arrival = 0
		}

		AddInfo(FString::Printf(TEXT("late joiner, %d agents: worst error with stamps %.1f cm, if the arrival time were the age %.1f cm"),
			NumAgents, WorstWithStamps, WorstWithoutStamps));
		TestTrue(TEXT("with stamps every agent is drawn within the tolerance (plus a tick and the quantizers)"), WorstWithStamps < Tuning.NearErrorCm + 20.f);
		TestTrue(TEXT("(for contrast) without stamps a late joiner would see agents metres from where they are"), WorstWithoutStamps > 150.0);
	}

	return true;
}

// =================================================================================================
// Region simulation
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdRegionSimTest, "MassBubble.Crowd.Region.Sim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdRegionSimTest::RunTest(const FString& Parameters)
{
	using namespace CrowdRegionTestSupport;

	FCrowdTuning Tuning;

	// ---- the persistence record round trip ----
	{
		const TArray<FCrowdAgentSim> Agents = MakeAgents(FIntPoint(2, 3), 50, 11u, Tuning);
		bool bSame = true;
		for (const FCrowdAgentSim& Agent : Agents)
		{
			const FCrowdAgentSim Back = CrowdSim::FromSaved(CrowdSim::ToSaved(Agent));
			bSame = bSame && Back.NetId == Agent.NetId && Back.Location == Agent.Location && Back.Velocity == Agent.Velocity
				&& Back.RetargetTimer == Agent.RetargetTimer && Back.Rng == Agent.Rng && Back.Heading == Agent.Heading
				&& Back.CruiseSpeed == Agent.CruiseSpeed;
		}
		TestTrue(TEXT("a saved agent comes back as it was"), bSame);
	}

	// ---- stepping a region is stepping its agents with the wander model, nothing else ----
	{
		const FIntPoint Coord(3, -2);
		TArray<FCrowdAgentSim> Sim = MakeAgents(Coord, 100, 7u, Tuning);
		TArray<FCrowdSavedAgent> Reference;
		for (FCrowdAgentSim& Agent : Sim)
		{
			Reference.Add(CrowdSim::ToSaved(Agent));
			Agent.Tier = ECrowdLOD::High;
		}

		CrowdSim::FMoveParams Params;
		Params.Tuning = &Tuning;
		Params.DeltaSeconds = 1.f / 60.f;
		Params.bTimeSlice = true;
		const CrowdWander::FBox2 Home = HomeBox(Coord);

		int32 Clamped = 0;
		int32 Simulated = 0;
		for (int32 Frame = 0; Frame < 60 * 60; ++Frame)
		{
			Params.Frame = static_cast<uint32>(Frame);
			const CrowdSim::FMoveStats Stats = CrowdSim::MoveRegion(Sim, Coord, Params);
			Clamped += Stats.Clamped;
			Simulated += Stats.Simulated;

			for (FCrowdSavedAgent& Saved : Reference)
			{
				CrowdWander::Advance(Saved.Location, Saved, Params.DeltaSeconds, Home, Tuning, CrowdWander::TurnSignForId(Saved.NetId));
			}
		}

		bool bSamePath = true;
		for (int32 i = 0; i < Sim.Num(); ++i)
		{
			bSamePath = bSamePath && Sim[i].Location == Reference[i].Location && Sim[i].Velocity == Reference[i].Velocity;
		}
		TestTrue(TEXT("every agent of a High region walks exactly where CrowdWander::Advance walks it"), bSamePath);
		TestEqual(TEXT("agents stepped (High tier: every agent every frame)"), Simulated, 100 * 60 * 60);
		TestEqual(TEXT("agents the safety clamp had to rescue"), Clamped, 0);

		// A frozen agent does not move and does not owe time.
		FCrowdAgentSim Frozen = Sim[0];
		Frozen.Tier = ECrowdLOD::Off;
		const FVector2D FrozenAt = Frozen.Location;
		TArray<FCrowdAgentSim> One;
		One.Add(Frozen);
		const CrowdSim::FMoveStats FrozenStats = CrowdSim::MoveRegion(One, Coord, Params);
		TestTrue(TEXT("an Off agent stands still"), One[0].Location == FrozenAt && FrozenStats.Simulated == 0 && One[0].PendingDelta == 0.f);
	}

	// ---- the per-region bounds give the tiers the loop over every viewer gives ----
	{
		const TArray<FIntPoint> Coords = { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(6, 6) };

		TArray<FVector2D> Viewers;
		uint32 Rng = 321u;
		for (int32 i = 0; i < 8; ++i)
		{
			Viewers.Add(FVector2D(CrowdMath::Random01(Rng) * 3.0 * RegionCm, CrowdMath::Random01(Rng) * 2.0 * RegionCm));
		}

		CrowdLod::FTable Table;
		Table.Begin(Coords.Num(), Viewers);
		for (int32 Slot = 0; Slot < Coords.Num(); ++Slot)
		{
			Table.AddRegion(Slot, CrowdMath::RegionMin(Coords[Slot], Tuning.RegionSizeCm), Tuning.RegionSizeCm, Tuning);
		}

		int32 Different = 0;
		int32 SeenTiers[CrowdLODCount] = { 0, 0, 0, 0 };
		for (int32 Slot = 0; Slot < Coords.Num(); ++Slot)
		{
			TArray<FCrowdAgentSim> WithTable = MakeAgents(Coords[Slot], 200, 40u + static_cast<uint32>(Slot), Tuning);
			TArray<FCrowdAgentSim> Brute = WithTable;

			CrowdSim::FClassifyParams WithParams;
			WithParams.Tuning = &Tuning;
			WithParams.Table = &Table;
			WithParams.Viewers = Viewers;

			CrowdSim::FClassifyParams BruteParams = WithParams;
			BruteParams.Table = nullptr;

			int32 CountsWith[CrowdLODCount] = { 0, 0, 0, 0 };
			int32 CountsBrute[CrowdLODCount] = { 0, 0, 0, 0 };
			for (uint32 Frame = 0; Frame < 4; ++Frame) // every agent is classified once in 4 frames
			{
				WithParams.Frame = Frame;
				BruteParams.Frame = Frame;
				CrowdSim::ClassifyRegion(WithTable, Slot, WithParams, CountsWith);
				CrowdSim::ClassifyRegion(Brute, Slot, BruteParams, CountsBrute);
			}

			for (int32 i = 0; i < WithTable.Num(); ++i)
			{
				Different += (WithTable[i].Tier != Brute[i].Tier) ? 1 : 0;
				++SeenTiers[static_cast<int32>(WithTable[i].Tier)];
			}
		}

		int32 TiersUsed = 0;
		for (const int32 Seen : SeenTiers)
		{
			TiersUsed += (Seen > 0) ? 1 : 0;
		}
		TestTrue(TEXT("test is not vacuous: more than one tier occurs"), TiersUsed >= 2);
		TestEqual(TEXT("agents whose tier differs between the table and the loop over every viewer"), Different, 0);

		// No LOD: everybody is High.
		TArray<FCrowdAgentSim> Flat = MakeAgents(Coords[0], 40, 5u, Tuning);
		CrowdSim::FClassifyParams FlatParams;
		FlatParams.Tuning = &Tuning;
		FlatParams.bUseLOD = false;
		int32 FlatCounts[CrowdLODCount] = { 0, 0, 0, 0 };
		CrowdSim::ClassifyRegion(Flat, 0, FlatParams, FlatCounts);
		TestEqual(TEXT("without LOD every agent is High"), FlatCounts[static_cast<int32>(ECrowdLOD::High)], 40);
	}

	// ---- what is published ----
	{
		FCrowdAgentSim Agent;
		Agent.Location = FVector2D(1000.0, 1000.0);
		Agent.SentPos = FVector2D(1040.0, 1000.0); // the clients believe it stands 40 cm away
		Agent.SentTime = 0.0;

		Agent.Tier = ECrowdLOD::High;
		TestTrue(TEXT("High tier: 40 cm off the prediction is more than the near tolerance"),
			CrowdSim::NeedsPublish(Agent, FVector2f::ZeroVector, 1.0, Tuning, true));
		Agent.Tier = ECrowdLOD::Low;
		TestFalse(TEXT("Low tier: 40 cm is inside the far tolerance"),
			CrowdSim::NeedsPublish(Agent, FVector2f::ZeroVector, 1.0, Tuning, true));

		Agent.Velocity = FVector2f(100.f, 0.f);
		Agent.Tier = ECrowdLOD::Off;
		TestTrue(TEXT("a frozen agent is published without velocity"), CrowdSim::PublishedVelocity(Agent).IsNearlyZero());
		Agent.Tier = ECrowdLOD::Medium;
		TestTrue(TEXT("every other tier is published with its velocity"), CrowdSim::PublishedVelocity(Agent) == Agent.Velocity);

		FCrowdAgentSim Standing;
		TestFalse(TEXT("without dead reckoning a standing agent whose clients know it stands is not published"),
			CrowdSim::NeedsPublish(Standing, FVector2f::ZeroVector, 1.0, Tuning, false));
		TestTrue(TEXT("without dead reckoning a moving agent is published every time"),
			CrowdSim::NeedsPublish(Standing, FVector2f(100.f, 0.f), 1.0, Tuning, false));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
