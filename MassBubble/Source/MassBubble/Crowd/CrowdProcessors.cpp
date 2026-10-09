#include "Crowd/CrowdProcessors.h"

#include "Core/MassBubbleStats.h"
#include "Crowd/CrowdFragments.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdSubsystem.h"
#include "Crowd/CrowdWander.h"

#include "MassExecutionContext.h"

#include <atomic>

// =================================================================================================
// LOD
// =================================================================================================

UCrowdLODProcessor::UCrowdLODProcessor()
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
	ExecutionOrder.ExecuteInGroup = CrowdGroups::LOD;
	bRequiresGameThreadExecution = true;
}

void UCrowdLODProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.Initialize(EntityManager);
	EntityQuery.AddTagRequirement<FCrowdAgentTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FCrowdIdFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdLocationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdLODFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.RegisterWithProcessor(*this);

	ProcessorRequirements.AddSubsystemRequirement<UCrowdSubsystem>(EMassFragmentAccess::ReadWrite);
}

void UCrowdLODProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	OPT_SCOPE(STAT_OptCrowd_LOD, LOD, "Opt.Crowd.LOD");

	if (!CrowdCVars::Enable)
	{
		return;
	}

	UCrowdSubsystem* Crowd = Context.GetMutableSubsystem<UCrowdSubsystem>();
	if (Crowd == nullptr)
	{
		return;
	}

	const FCrowdTuning& Tuning = GetCrowdTuning();
	const TArray<FCrowdViewer>& Viewers = Crowd->GetViewers();
	const bool bUseLOD = CrowdCVars::LOD != 0;
	const uint32 Frame = static_cast<uint32>(GFrameCounter);

	int32 Counts[CrowdLODCount] = { 0, 0, 0, 0 };

	EntityQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& ChunkContext)
	{
		const int32 NumEntities = ChunkContext.GetNumEntities();
		const TConstArrayView<FCrowdIdFragment> Ids = ChunkContext.GetFragmentView<FCrowdIdFragment>();
		const TConstArrayView<FCrowdLocationFragment> Locations = ChunkContext.GetFragmentView<FCrowdLocationFragment>();
		const TArrayView<FCrowdLODFragment> LODs = ChunkContext.GetMutableFragmentView<FCrowdLODFragment>();

		for (int32 i = 0; i < NumEntities; ++i)
		{
			FCrowdLODFragment& LOD = LODs[i];

			if (!bUseLOD)
			{
				LOD.Tier = ECrowdLOD::High;
			}
			else if (((Frame + Ids[i].NetId) & 3u) == 0u)
			{
				// Time-sliced LOD: each agent is re-classified every 4th frame.
				float Distance = 1.0e12f; // no viewer => far away => Off
				if (Viewers.Num() > 0)
				{
					double BestSq = TNumericLimits<double>::Max();
					for (const FCrowdViewer& Viewer : Viewers)
					{
						BestSq = FMath::Min(BestSq, FVector2D::DistSquared(Locations[i].Location, Viewer.Location));
					}
					Distance = static_cast<float>(FMath::Sqrt(BestSq));
				}
				LOD.Tier = CrowdMath::ComputeTier(Distance, LOD.Tier, Tuning);
			}

			++Counts[static_cast<int32>(LOD.Tier)];
		}
	});

	Crowd->SetTierCounts(Counts);
}

// =================================================================================================
// Movement
// =================================================================================================

UCrowdMovementProcessor::UCrowdMovementProcessor()
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
	ExecutionOrder.ExecuteInGroup = CrowdGroups::Move;
	ExecutionOrder.ExecuteAfter.Add(CrowdGroups::LOD);
	bRequiresGameThreadExecution = false;
}

void UCrowdMovementProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.Initialize(EntityManager);
	EntityQuery.AddTagRequirement<FCrowdAgentTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FCrowdIdFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdLocationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FCrowdMotionFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FCrowdLODFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.RegisterWithProcessor(*this);
}

void UCrowdMovementProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	OPT_SCOPE(STAT_OptCrowd_Move, Move, "Opt.Crowd.Move");

	if (!CrowdCVars::Enable)
	{
		return;
	}

	const FCrowdTuning& Tuning = GetCrowdTuning();
	const uint32 Frame = static_cast<uint32>(GFrameCounter);
	const float DeltaTime = Context.GetDeltaTimeSeconds();
	const bool bTimeSlice = CrowdCVars::TimeSlicing != 0;

	std::atomic<int32> Simulated{ 0 };
	std::atomic<int32> Clamped{ 0 };

	// Only touches data of its own chunk, so it is safe to run concurrently on several chunks.
	auto ProcessChunk = [&](FMassExecutionContext& ChunkContext)
	{
		const int32 NumEntities = ChunkContext.GetNumEntities();
		const TConstArrayView<FCrowdIdFragment> Ids = ChunkContext.GetFragmentView<FCrowdIdFragment>();
		const TArrayView<FCrowdLocationFragment> Locations = ChunkContext.GetMutableFragmentView<FCrowdLocationFragment>();
		const TArrayView<FCrowdMotionFragment> Motions = ChunkContext.GetMutableFragmentView<FCrowdMotionFragment>();
		const TArrayView<FCrowdLODFragment> LODs = ChunkContext.GetMutableFragmentView<FCrowdLODFragment>();

		int32 LocalSimulated = 0;
		int32 LocalClamped = 0;

		for (int32 i = 0; i < NumEntities; ++i)
		{
			FCrowdLODFragment& LOD = LODs[i];

			// 0 = frozen, 1 = every frame, N = every N-th frame (agents are spread over frames by NetId).
			const int32 Interval = bTimeSlice ? Tuning.LODIntervalFrames[static_cast<int32>(LOD.Tier)] : 1;
			if (Interval == 0)
			{
				continue;
			}
			if (Interval > 1 && ((Frame + Ids[i].NetId) % static_cast<uint32>(Interval)) != 0u)
			{
				LOD.PendingDelta += DeltaTime; // remember how much time this agent still owes
				continue;
			}

			const float Step = FMath::Min(LOD.PendingDelta + DeltaTime, Tuning.MaxStepDeltaSec);
			LOD.PendingDelta = 0.f;

			// Wander inside the home region (Crowd/CrowdWander.h). Pure function of this agent's own data, so it is
			// safe on any chunk in parallel. The step is split at segment boundaries: skipping frames (time slicing)
			// walks the same path as stepping every frame.
			const double MinX = static_cast<double>(Ids[i].HomeX) * Tuning.RegionSizeCm;
			const double MinY = static_cast<double>(Ids[i].HomeY) * Tuning.RegionSizeCm;
			const CrowdWander::FBox2 Home{ FVector2D(MinX, MinY), FVector2D(MinX + Tuning.RegionSizeCm, MinY + Tuning.RegionSizeCm) };

			if (CrowdWander::Advance(Locations[i].Location, Motions[i], Step, Home, Tuning, CrowdWander::TurnSignForId(Ids[i].NetId)))
			{
				++LocalClamped;
			}
			++LocalSimulated;
		}

		Simulated.fetch_add(LocalSimulated, std::memory_order_relaxed);
		Clamped.fetch_add(LocalClamped, std::memory_order_relaxed);
	};

	if (CrowdCVars::ParallelMove != 0)
	{
		EntityQuery.ParallelForEachEntityChunk(Context, ProcessChunk);
	}
	else
	{
		EntityQuery.ForEachEntityChunk(Context, ProcessChunk);
	}

	const int32 SimulatedCount = Simulated.load(std::memory_order_relaxed);
	INC_DWORD_STAT_BY(STAT_OptCrowd_Simulated, SimulatedCount);
	CSV_CUSTOM_STAT(OptCrowd, Simulated, SimulatedCount, ECsvCustomStatOp::Set);
	// Agents the safety clamp had to pull back into their region. The planner keeps them inside, so this reads 0.
	CSV_CUSTOM_STAT(OptCrowd, WanderClamped, Clamped.load(std::memory_order_relaxed), ECsvCustomStatOp::Set);
}

// =================================================================================================
// Snapshot
// =================================================================================================

UCrowdSnapshotProcessor::UCrowdSnapshotProcessor()
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
	ExecutionOrder.ExecuteInGroup = CrowdGroups::Snapshot;
	ExecutionOrder.ExecuteAfter.Add(CrowdGroups::Move);
	bRequiresGameThreadExecution = true;
}

void UCrowdSnapshotProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.Initialize(EntityManager);
	EntityQuery.AddTagRequirement<FCrowdAgentTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FCrowdIdFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdLocationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdMotionFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FCrowdLODFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.RegisterWithProcessor(*this);

	ProcessorRequirements.AddSubsystemRequirement<UCrowdSubsystem>(EMassFragmentAccess::ReadWrite);
}

void UCrowdSnapshotProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	UCrowdSubsystem* Crowd = Context.GetMutableSubsystem<UCrowdSubsystem>();
	if (Crowd == nullptr || !Crowd->IsSnapshotDue())
	{
		return; // most frames end here: no snapshot due, the query is not even run
	}

	OPT_SCOPE(STAT_OptCrowd_Snapshot, Snapshot, "Opt.Crowd.Snapshot");

	Crowd->BeginSnapshot();

	EntityQuery.ForEachEntityChunk(Context, [Crowd](FMassExecutionContext& ChunkContext)
	{
		const int32 NumEntities = ChunkContext.GetNumEntities();
		const TConstArrayView<FCrowdIdFragment> Ids = ChunkContext.GetFragmentView<FCrowdIdFragment>();
		const TConstArrayView<FCrowdLocationFragment> Locations = ChunkContext.GetFragmentView<FCrowdLocationFragment>();
		const TConstArrayView<FCrowdMotionFragment> Motions = ChunkContext.GetFragmentView<FCrowdMotionFragment>();
		const TConstArrayView<FCrowdLODFragment> LODs = ChunkContext.GetFragmentView<FCrowdLODFragment>();

		for (int32 i = 0; i < NumEntities; ++i)
		{
			// A frozen agent must not look like it is walking: clients extrapolate with this velocity.
			const FVector2f Velocity = (LODs[i].Tier == ECrowdLOD::Off) ? FVector2f::ZeroVector : Motions[i].Velocity;
			Crowd->AddToSnapshot(Ids[i].RegionSlot, Locations[i].Location, Velocity, Ids[i].NetId);
		}
	});

	Crowd->EndSnapshot();
}
