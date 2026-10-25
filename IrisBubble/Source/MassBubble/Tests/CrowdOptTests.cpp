// Unit tests for the LOD region table (Crowd/CrowdLodTable.h) and the packed persistence record (Crowd/CrowdPersist.h).
// Run in the editor:  Session Frontend > Automation > filter "MassBubble.Crowd.Opt"
// or headless:        UnrealEditor-Cmd MassBubble.uproject -ExecCmds="Automation RunTests MassBubble.Crowd.Opt; Quit" -unattended -nullrhi -log

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Crowd/CrowdLodTable.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdPersist.h"
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdWander.h"

#include "Misc/AutomationTest.h"

namespace CrowdOptTestSupport
{
	constexpr double RegionCm = 12800.0;

	inline CrowdWander::FBox2 HomeBox(const FVector2D& Min)
	{
		return CrowdWander::FBox2{ Min, Min + FVector2D(RegionCm, RegionCm) };
	}

	/** Reference: the plain loop over every viewer. */
	inline ECrowdLOD MeasureTier(const TArray<FVector2D>& Viewers, const FVector2D& Pos, ECrowdLOD Current, const FCrowdTuning& Tuning)
	{
		float Distance = 1.0e12f;
		if (Viewers.Num() > 0)
		{
			double BestSq = TNumericLimits<double>::Max();
			for (const FVector2D& Viewer : Viewers)
			{
				BestSq = FMath::Min(BestSq, FVector2D::DistSquared(Pos, Viewer));
			}
			Distance = static_cast<float>(FMath::Sqrt(BestSq));
		}
		return CrowdMath::ComputeTier(Distance, Current, Tuning);
	}

	/** The regions the crowd keeps alive: ActiveRegionRadius around every viewer, each once. */
	inline TArray<FIntPoint> WantedRegions(const TArray<FVector2D>& Viewers, const FCrowdTuning& Tuning)
	{
		TArray<FIntPoint> Wanted;
		for (const FVector2D& Viewer : Viewers)
		{
			const FIntPoint Center = CrowdMath::WorldToRegion(Viewer, Tuning.RegionSizeCm);
			for (int32 DY = -Tuning.ActiveRegionRadius; DY <= Tuning.ActiveRegionRadius; ++DY)
			{
				for (int32 DX = -Tuning.ActiveRegionRadius; DX <= Tuning.ActiveRegionRadius; ++DX)
				{
					Wanted.Add(FIntPoint(Center.X + DX, Center.Y + DY));
				}
			}
		}
		Wanted.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });

		TArray<FIntPoint> Unique;
		for (const FIntPoint& Coord : Wanted)
		{
			if (Unique.Num() == 0 || Unique.Last() != Coord)
			{
				Unique.Add(Coord);
			}
		}
		return Unique;
	}

	struct FLodRun
	{
		int64 Agents = 0;
		int64 Mismatches = 0;
		int64 TableEvaluations = 0;
		int64 BruteEvaluations = 0;
		int32 Regions = 0;
		int32 Mixed = 0;
		int32 AllHigh = 0;
		int32 AllOff = 0;
	};

	/** Classifies AgentsPerRegion random agents (and the four corners) of every region with the table and with the reference. */
	inline FLodRun RunLod(const TArray<FVector2D>& Viewers, const TArray<FIntPoint>& Regions, int32 AgentsPerRegion, uint32 Seed, const FCrowdTuning& Tuning)
	{
		CrowdLod::FTable Table;
		Table.Begin(Regions.Num(), Viewers);
		for (int32 Slot = 0; Slot < Regions.Num(); ++Slot)
		{
			Table.AddRegion(Slot, CrowdMath::RegionMin(Regions[Slot], Tuning.RegionSizeCm), Tuning.RegionSizeCm, Tuning);
		}

		FLodRun Run;
		Run.Regions = Regions.Num();
		Run.Mixed = Table.NumMixed;
		Run.AllHigh = Table.NumAllHigh;
		Run.AllOff = Table.NumAllOff;

		uint32 Rng = CrowdMath::NonZeroSeed(Seed);
		for (int32 Slot = 0; Slot < Regions.Num(); ++Slot)
		{
			const FVector2D Min = CrowdMath::RegionMin(Regions[Slot], Tuning.RegionSizeCm);
			const CrowdLod::FRegionEntry& Entry = Table.Entries[Slot];

			for (int32 i = 0; i < AgentsPerRegion; ++i)
			{
				FVector2D Pos(Min.X + CrowdMath::Random01(Rng) * RegionCm, Min.Y + CrowdMath::Random01(Rng) * RegionCm);
				if (i < 4)
				{
					Pos = FVector2D(Min.X + ((i & 1) ? RegionCm : 0.0), Min.Y + ((i & 2) ? RegionCm : 0.0));
				}
				const ECrowdLOD Current = static_cast<ECrowdLOD>(CrowdMath::NextRandom(Rng) & 3u);

				const ECrowdLOD Expected = MeasureTier(Viewers, Pos, Current, Tuning);
				const ECrowdLOD Actual = Table.Classify(Slot, Pos, Current, Tuning);

				++Run.Agents;
				Run.Mismatches += (Actual != Expected) ? 1 : 0;
				Run.BruteEvaluations += Viewers.Num();
				Run.TableEvaluations += (Entry.Kind == CrowdLod::ERegionKind::Mixed) ? Entry.NumViewers : 0;
			}
		}
		return Run;
	}

	inline TArray<FVector2D> RandomViewers(int32 Num, double HalfExtentCm, uint32 Seed)
	{
		uint32 Rng = CrowdMath::NonZeroSeed(Seed);
		TArray<FVector2D> Viewers;
		for (int32 i = 0; i < Num; ++i)
		{
			Viewers.Add(FVector2D((2.0 * CrowdMath::Random01(Rng) - 1.0) * HalfExtentCm, (2.0 * CrowdMath::Random01(Rng) - 1.0) * HalfExtentCm));
		}
		return Viewers;
	}
}

// =================================================================================================
// LOD region table vs. the loop over every viewer
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdOptLodTableTest, "MassBubble.Crowd.Opt.LodTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdOptLodTableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdOptTestSupport;

	FCrowdTuning Tuning;

	// ---- one viewer: the 5 x 5 regions around it ----
	{
		const TArray<FVector2D> Viewers = RandomViewers(1, 3.0 * RegionCm, 11u);
		const FLodRun Run = RunLod(Viewers, WantedRegions(Viewers, Tuning), 400, 21u, Tuning);
		AddInfo(FString::Printf(TEXT("1 viewer: %d regions (%d mixed, %d off), %lld agents, %lld distance evaluations (reference %lld)"),
			Run.Regions, Run.Mixed, Run.AllOff, Run.Agents, Run.TableEvaluations, Run.BruteEvaluations));
		TestEqual(TEXT("one viewer: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
		TestTrue(TEXT("one viewer: regions in the corners of the block are out of reach"), Run.AllOff > 0);
	}

	// ---- 64 viewers spread over 40 x 40 regions: the load test with bots ----
	{
		const TArray<FVector2D> Viewers = RandomViewers(64, 20.0 * RegionCm, 12u);
		const FLodRun Run = RunLod(Viewers, WantedRegions(Viewers, Tuning), 200, 22u, Tuning);
		const double Share = Run.BruteEvaluations > 0 ? static_cast<double>(Run.TableEvaluations) / static_cast<double>(Run.BruteEvaluations) : 0.0;
		AddInfo(FString::Printf(TEXT("64 viewers: %d regions (%d mixed, %d off), %lld agents, distance evaluations %.2f%% of the reference"),
			Run.Regions, Run.Mixed, Run.AllOff, Run.Agents, 100.0 * Share));
		TestEqual(TEXT("64 viewers: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
		TestTrue(TEXT("64 viewers: at least 8 times fewer distance evaluations"), Share < 0.125);
	}

	// ---- 64 viewers close together: long candidate lists ----
	{
		const TArray<FVector2D> Viewers = RandomViewers(64, 1.5 * RegionCm, 13u);
		const FLodRun Run = RunLod(Viewers, WantedRegions(Viewers, Tuning), 200, 23u, Tuning);
		TestEqual(TEXT("clustered viewers: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
	}

	// ---- viewers on region corners and exactly at the distance where a region stops being out of reach ----
	{
		TArray<FVector2D> Viewers;
		for (int32 X = -3; X <= 3; ++X)
		{
			for (int32 Y = -3; Y <= 3; ++Y)
			{
				Viewers.Add(FVector2D(RegionCm * X, RegionCm * Y));
			}
		}
		const double Reach = static_cast<double>(Tuning.LODDistanceCm[2]) + static_cast<double>(Tuning.LODHysteresisCm) + CrowdLod::SafetyCm + CrowdLod::PadCm;
		for (const double Delta : { -0.5, 0.0, 0.5 })
		{
			Viewers.Add(FVector2D(-(Reach + Delta), 0.5 * RegionCm));
			Viewers.Add(FVector2D(RegionCm + Reach + Delta, 0.5 * RegionCm));
			Viewers.Add(FVector2D(0.5 * RegionCm, -(Reach + Delta)));
		}
		const FLodRun Run = RunLod(Viewers, WantedRegions(Viewers, Tuning), 100, 24u, Tuning);
		TestEqual(TEXT("boundary cases: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
	}

	// ---- wide tiers: whole regions inside the innermost boundary ----
	{
		FCrowdTuning Wide = Tuning;
		Wide.LODDistanceCm[0] = 30000.f;
		Wide.LODDistanceCm[1] = 40000.f;
		Wide.LODDistanceCm[2] = 60000.f;
		const TArray<FVector2D> Viewers = RandomViewers(8, 6.0 * RegionCm, 14u);
		const FLodRun Run = RunLod(Viewers, WantedRegions(Viewers, Wide), 200, 25u, Wide);
		AddInfo(FString::Printf(TEXT("wide tiers: %d regions (%d all High, %d mixed, %d off)"), Run.Regions, Run.AllHigh, Run.Mixed, Run.AllOff));
		TestEqual(TEXT("wide tiers: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
		TestTrue(TEXT("wide tiers: some regions are entirely High"), Run.AllHigh > 0);
	}

	// ---- no viewer ----
	{
		const TArray<FVector2D> Viewers;
		TArray<FIntPoint> Regions;
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				Regions.Add(FIntPoint(X, Y));
			}
		}
		const FLodRun Run = RunLod(Viewers, Regions, 100, 26u, Tuning);
		TestEqual(TEXT("no viewer: agents whose tier differs from the reference"), static_cast<int32>(Run.Mismatches), 0);
		TestEqual(TEXT("no viewer: every region is out of reach"), Run.AllOff, Run.Regions);
	}

	// ---- a slot without an entry falls back to every viewer ----
	{
		const TArray<FVector2D> Viewers = RandomViewers(5, 2.0 * RegionCm, 15u);
		CrowdLod::FTable Table;
		Table.Begin(2, Viewers);
		const FVector2D Pos(1000.0, 2000.0);
		TestTrue(TEXT("slot without an entry: same tier as the reference"),
			Table.Classify(1, Pos, ECrowdLOD::Medium, Tuning) == MeasureTier(Viewers, Pos, ECrowdLOD::Medium, Tuning)
			&& Table.Classify(INDEX_NONE, Pos, ECrowdLOD::Low, Tuning) == MeasureTier(Viewers, Pos, ECrowdLOD::Low, Tuning));
	}

	return true;
}

// =================================================================================================
// Packed persistence record
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdOptPersistTest, "MassBubble.Crowd.Opt.Persist",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdOptPersistTest::RunTest(const FString& Parameters)
{
	using namespace CrowdOptTestSupport;

	FCrowdTuning Tuning;

	TestEqual(TEXT("packed record is 36 bytes"), static_cast<int32>(sizeof(FCrowdPackedAgent)), 36);
	TestTrue(TEXT("... and smaller than the record it replaces"), sizeof(FCrowdPackedAgent) < sizeof(FCrowdSavedAgent));

	// Regions near the origin and very far from it: the offset to the region origin keeps float precision either way.
	const FVector2D Mins[3] =
	{
		FVector2D(0.0, 0.0),
		FVector2D(RegionCm * 37.0, -RegionCm * 91.0),
		FVector2D(-RegionCm * 1000.0, RegionCm * 2000.0),
	};

	int32 Agents = 0;
	int32 FieldMismatches = 0;
	double WorstOffsetCm = 0.0;
	int32 Diverged = 0;
	double WorstFinalCm = 0.0;
	uint32 Rng = 5150u;

	for (const FVector2D& Min : Mins)
	{
		const CrowdWander::FBox2 Home = HomeBox(Min);
		for (uint32 i = 0; i < 300u; ++i)
		{
			FCrowdSavedAgent Agent;
			Agent.NetId = i + 1u;
			CrowdWander::InitNewAgent(Agent, 99u + i * 2654435761u, Home, Tuning);

			// Walk for a while so that timers, headings and velocities are in all kinds of states.
			const float TurnSign = CrowdWander::TurnSignForId(Agent.NetId);
			const int32 WarmSteps = static_cast<int32>(CrowdMath::Random01(Rng) * 400.f);
			for (int32 Step = 0; Step < WarmSteps; ++Step)
			{
				CrowdWander::Advance(Agent.Location, Agent, 0.05f, Home, Tuning, TurnSign);
			}

			FCrowdSavedAgent Restored = CrowdPersist::Unpack(CrowdPersist::Pack(Agent, Min), Min);
			++Agents;

			const bool bSameFields = Restored.NetId == Agent.NetId && Restored.Velocity.X == Agent.Velocity.X && Restored.Velocity.Y == Agent.Velocity.Y
				&& Restored.RetargetTimer == Agent.RetargetTimer && Restored.Rng == Agent.Rng && Restored.Heading == Agent.Heading
				&& Restored.CruiseSpeed == Agent.CruiseSpeed;
			FieldMismatches += bSameFields ? 0 : 1;
			WorstOffsetCm = FMath::Max(WorstOffsetCm, FMath::Max(FMath::Abs(Restored.Location.X - Agent.Location.X), FMath::Abs(Restored.Location.Y - Agent.Location.Y)));

			// Both copies walk for 30 s: the restored one has to stay on the original's path.
			FCrowdSavedAgent Original = Agent;
			for (int32 Step = 0; Step < 30 * 20; ++Step)
			{
				CrowdWander::Advance(Original.Location, Original, 0.05f, Home, Tuning, TurnSign);
				CrowdWander::Advance(Restored.Location, Restored, 0.05f, Home, Tuning, TurnSign);
			}
			const double FinalCm = FVector2D::Distance(Original.Location, Restored.Location);
			WorstFinalCm = FMath::Max(WorstFinalCm, FinalCm);
			Diverged += (FinalCm > 1.0) ? 1 : 0;
		}
	}

	AddInfo(FString::Printf(TEXT("%d agents: largest location error %.5f cm, after 30 s of walking %d are more than 1 cm apart (worst %.4f cm)"),
		Agents, WorstOffsetCm, Diverged, WorstFinalCm));
	TestEqual(TEXT("fields that are not bit for bit the same after the round trip"), FieldMismatches, 0);
	TestTrue(TEXT("location error below 0.001 cm"), WorstOffsetCm < 1.0e-3);
	TestTrue(TEXT("restored agents keep walking where the originals walk (at most 1% end up more than 1 cm apart)"), Diverged * 100 <= Agents);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
