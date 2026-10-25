#include "Crowd/CrowdSubsystem.h"

#include "Core/MassBubbleBoot.h"
#include "Core/MassBubbleMemory.h"
#include "Core/MassBubbleStats.h"
#include "Core/MassBubbleStreamingMonitor.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdPCGPlanner.h"
#include "Crowd/CrowdPlan.h"
#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdWander.h"
#include "MassBubble.h"
#include "Net/CrowdRegionReplicator.h"
#include "World/MassBubbleStreamingAnchor.h"

#include "Async/ParallelFor.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

namespace
{
	/** Spawning, Active and Despawning regions hold agents. */
	FORCEINLINE bool HoldsAgents(ECrowdRegionState State)
	{
		return State == ECrowdRegionState::Spawning || State == ECrowdRegionState::Active || State == ECrowdRegionState::Despawning;
	}

	/** While a region despawns, the agents before the cursor are persisted already: they stop simulating. */
	FORCEINLINE int32 FirstSimulated(const FCrowdRegion& Region)
	{
		return Region.State == ECrowdRegionState::Despawning ? Region.DespawnCursor : 0;
	}

	/** The live agents are the truth once a region has spawned: its restore source is not needed any more. */
	void MarkRegionActive(FCrowdRegion& Region)
	{
		if (CrowdCVars::TrimRegions != 0)
		{
			Region.Saved.Empty();
		}
		else
		{
			Region.Saved.Reset();
		}
		Region.State = ECrowdRegionState::Active;
		OPT_BOOT_ONCE("Crowd.FirstRegionActive");
	}
}

bool UCrowdSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Authority only (dedicated server, listen host or standalone).
	// Pure clients render what their region replicators carry.
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->GetNetMode() != NM_Client;
}

bool UCrowdSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UCrowdSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Tells us when World Partition is busy: spawn / despawn budgets shrink while it is.
	Monitor = Collection.InitializeDependency<UMassBubbleStreamingMonitor>();

	ReloadCrowdTuning();
	const FCrowdTuning& T = GetCrowdTuning();

	// Spawn plans: a PCG graph tells a new region where its agents start. The planner only exists while a graph is
	// configured; without one (or with opt.crowd.PCG 0) regions are populated randomly.
	if (T.bUsePCG)
	{
		UCrowdPCGPlanner* NewPlanner = NewObject<UCrowdPCGPlanner>(this);
		if (NewPlanner->Initialize())
		{
			Planner = NewPlanner;
		}
	}

	MassBubbleBoot::Mark(TEXT("Crowd.SubsystemReady"));
	UE_LOG(LogMassBubble, Log, TEXT("CrowdSubsystem up (NetMode=%d). Region=%.0fcm, ActiveRadius=%d, AgentsPerRegion=%d, ReplicationHz=%.1f, ReplicationRadius=%.0fcm"),
		static_cast<int32>(GetWorld()->GetNetMode()), T.RegionSizeCm, T.ActiveRegionRadius, T.AgentsPerRegion, T.ReplicationHz, T.ReplicationRadiusCm);
}

void UCrowdSubsystem::Deinitialize()
{
	if (Planner != nullptr)
	{
		Planner->Shutdown();
		Planner = nullptr;
	}

	// The replicator actors die with the world: just drop the references.
	Regions.Empty();
	RegionSlotByCoord.Empty();
	SpawnQueue.Empty();
	DespawnQueue.Empty();
	ReplicatorQueue.Empty();
	SimulatedScratch.Empty();
	TierScratch.Empty();
	MoveScratch.Empty();
	LodTable = CrowdLod::FTable();
	LodViewers.Empty();
	VirtualViewers.Empty();
	Viewers.Empty();
	BotJobs.Empty();
	Monitor.Reset();

	Super::Deinitialize();
}

TStatId UCrowdSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdSubsystem, STATGROUP_Tickables);
}

double UCrowdSubsystem::GetNow() const
{
	return static_cast<double>(GetWorld()->GetTimeSeconds());
}

// ------------------------------------------------------------------------------------------------
// Tick
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::Tick(float DeltaTime)
{
	{
		OPT_SCOPE(STAT_OptCrowd_Director, Director, "Opt.Crowd.Director");

		// Bots run even with opt.crowd.Enable 0, so the engine's streaming cost can be measured on its own.
		TickBotRamp(DeltaTime);

		if (!CrowdCVars::Enable)
		{
			return;
		}

		RefreshViewers();

		// Region desire is evaluated at 4 Hz: viewers cross 128 m regions slowly compared to the frame rate.
		RegionEvalAccumulator += DeltaTime;
		if (RegionEvalAccumulator >= 0.25f)
		{
			RegionEvalAccumulator = 0.f;
			const double Now = GetNow();
			EvaluateRegions(Now);
			EvaluateReplication(Now);
		}

		// Spawn plans (PCG graph runs) that finished since the last frame turn into spawn jobs. Cheap when nothing is pending.
		PumpPlans();
		PumpRegionJobs();
		PumpReplicators();
	}

	CollectSimulatedRegions();
	UpdateLod();
	MoveAgents(DeltaTime);
	PublishRegions(DeltaTime);

	const int32 ActiveRegions = GetNumRegionsInState(ECrowdRegionState::Active);
	SET_DWORD_STAT(STAT_OptCrowd_Agents, AliveAgents);
	SET_DWORD_STAT(STAT_OptCrowd_Regions, ActiveRegions);
	CSV_CUSTOM_STAT(OptCrowd, Agents, AliveAgents, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, ActiveRegions, ActiveRegions, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, Replicators, GetNumReplicators(), ECsvCustomStatOp::Set);

	MemorySampleAccumulator += DeltaTime;
	if (MemorySampleAccumulator >= 1.f)
	{
		MemorySampleAccumulator = 0.f;
		PublishMemoryStats();
	}
}

// ------------------------------------------------------------------------------------------------
// Viewers
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::RegisterVirtualViewer(AActor* Actor)
{
	if (Actor != nullptr)
	{
		VirtualViewers.AddUnique(Actor);
	}
}

void UCrowdSubsystem::UnregisterVirtualViewer(AActor* Actor)
{
	VirtualViewers.RemoveAll([Actor](const TWeakObjectPtr<AActor>& Ptr)
	{
		return !Ptr.IsValid() || Ptr.Get() == Actor;
	});
}

void UCrowdSubsystem::RefreshViewers()
{
	Viewers.Reset();

	const UWorld* World = GetWorld();
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (PC == nullptr)
		{
			continue;
		}
		const AActor* Target = PC->GetPawn() != nullptr ? static_cast<const AActor*>(PC->GetPawn()) : PC->GetViewTarget();
		if (Target != nullptr)
		{
			const FVector Loc = Target->GetActorLocation();
			FCrowdViewer& V = Viewers.AddDefaulted_GetRef();
			V.Location = FVector2D(Loc.X, Loc.Y);
			V.bVirtual = false;
		}
	}

	for (int32 i = VirtualViewers.Num() - 1; i >= 0; --i)
	{
		const AActor* Actor = VirtualViewers[i].Get();
		if (Actor == nullptr)
		{
			VirtualViewers.RemoveAtSwap(i);
			continue;
		}
		const FVector Loc = Actor->GetActorLocation();
		FCrowdViewer& V = Viewers.AddDefaulted_GetRef();
		V.Location = FVector2D(Loc.X, Loc.Y);
		V.bVirtual = true;
	}
}

// ------------------------------------------------------------------------------------------------
// Load-test bots (ramped)
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::RequestBots(int32 Count)
{
	if (Count <= 0)
	{
		return;
	}

	FBotJob& Job = BotJobs.AddDefaulted_GetRef();
	Job.Total = Count;
	Job.Next = 0;
	Job.bClear = false;
}

void UCrowdSubsystem::RequestClearBots()
{
	// Bots that were not spawned yet are simply cancelled; the existing ones are removed one by one.
	BotJobs.Reset();

	FBotJob& Job = BotJobs.AddDefaulted_GetRef();
	Job.bClear = true;
}

int32 UCrowdSubsystem::GetNumPendingBotOps() const
{
	int32 Pending = 0;
	for (const FBotJob& Job : BotJobs)
	{
		Pending += Job.bClear ? 1 : FMath::Max(Job.Total - Job.Next, 0);
	}
	return Pending;
}

void UCrowdSubsystem::TickBotRamp(float DeltaSeconds)
{
	if (BotJobs.Num() == 0)
	{
		BotRampTimer = 0.f;
		return;
	}

	UWorld* World = GetWorld();
	const float Interval = GetBotRampIntervalSec();

	BotRampTimer -= DeltaSeconds;

	// With Interval 0 every pending operation runs in this frame; the guard only prevents an endless loop.
	for (int32 Guard = 0; Guard < 4096 && BotJobs.Num() > 0 && BotRampTimer <= 0.f; ++Guard)
	{
		FBotJob& Job = BotJobs[0];
		if (Job.bClear)
		{
			if (AMassBubbleStreamingAnchor::DestroyOneBot(World) <= 0)
			{
				BotJobs.RemoveAt(0); // that was the last one
			}
		}
		else
		{
			AMassBubbleStreamingAnchor::SpawnBot(World, Job.Next, Job.Total);
			if (++Job.Next >= Job.Total)
			{
				BotJobs.RemoveAt(0);
			}
		}

		if (Interval > 0.f)
		{
			BotRampTimer = Interval; // one operation per interval
			break;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Region scheduling
// ------------------------------------------------------------------------------------------------

int32 UCrowdSubsystem::FindOrAddRegion(const FIntPoint& Coord)
{
	if (const int32* Found = RegionSlotByCoord.Find(Coord))
	{
		return *Found;
	}

	const FCrowdTuning& T = GetCrowdTuning();
	const int32 Slot = Regions.AddDefaulted();
	FCrowdRegion& Region = Regions[Slot];
	Region.Coord = Coord;
	Region.Slot = Slot;

	const uint32 Mixed = (static_cast<uint32>(Coord.X) * 73856093u) ^ (static_cast<uint32>(Coord.Y) * 19349663u);
	Region.Seed = CrowdMath::NonZeroSeed(CrowdMath::Hash32(Mixed ^ static_cast<uint32>(T.WorldSeed)));

	RegionSlotByCoord.Add(Coord, Slot);
	return Slot;
}

void UCrowdSubsystem::EvaluateRegions(double Now)
{
	const FCrowdTuning& T = GetCrowdTuning();

	WantedScratch.Reset();
	const int32 Radius = T.ActiveRegionRadius;
	for (const FCrowdViewer& Viewer : Viewers)
	{
		const FIntPoint Center = CrowdMath::WorldToRegion(Viewer.Location, T.RegionSizeCm);
		for (int32 DY = -Radius; DY <= Radius; ++DY)
		{
			for (int32 DX = -Radius; DX <= Radius; ++DX)
			{
				WantedScratch.Add(FIntPoint(Center.X + DX, Center.Y + DY));
			}
		}
	}

	// Newly wanted regions: closest to any viewer first, so the player's own surroundings fill in first.
	TArray<TPair<double, int32>, TInlineAllocator<32>> ToActivate;
	for (const FIntPoint& Coord : WantedScratch)
	{
		const int32 Slot = FindOrAddRegion(Coord);
		FCrowdRegion& Region = Regions[Slot];
		Region.UnwantedSince = -1.0;

		if (Region.State == ECrowdRegionState::Inactive)
		{
			const FVector2D RegionCenter = CrowdMath::RegionMin(Coord, T.RegionSizeCm) + FVector2D(T.RegionSizeCm * 0.5, T.RegionSizeCm * 0.5);
			double Best = TNumericLimits<double>::Max();
			for (const FCrowdViewer& Viewer : Viewers)
			{
				Best = FMath::Min(Best, FVector2D::DistSquared(RegionCenter, Viewer.Location));
			}
			ToActivate.Emplace(Best, Slot);
		}
	}
	ToActivate.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
	for (const TPair<double, int32>& Item : ToActivate)
	{
		BeginSpawn(Regions[Item.Value]);
	}

	// Regions nobody wants any more: despawn after a grace period (hysteresis against boundary flicker). A region that is
	// still waiting for its spawn plan has nothing to despawn: after the same grace period its plan is cancelled instead.
	for (FCrowdRegion& Region : Regions)
	{
		const bool bLive = Region.State == ECrowdRegionState::Active || Region.State == ECrowdRegionState::Planning;
		if (!bLive || WantedScratch.Contains(Region.Coord))
		{
			continue;
		}
		if (Region.UnwantedSince < 0.0)
		{
			Region.UnwantedSince = Now;
		}
		else if (Now - Region.UnwantedSince >= T.RegionDeactivateDelaySec)
		{
			if (Region.State == ECrowdRegionState::Active)
			{
				BeginDespawn(Region);
			}
			else
			{
				CancelPlan(Region);
			}
		}
	}

	EvictStalePersistence(Now);
}

void UCrowdSubsystem::BeginSpawn(FCrowdRegion& Region)
{
	// A region whose population is known (persisted agents, or a plan that placed nobody) is restored as it is.
	if (Region.bPlanned || Region.Saved.Num() > 0)
	{
		StartSpawning(Region);
		return;
	}

	// Never populated: ask the PCG graph where the agents start. The region waits in Planning until the answer arrives.
	if (CanPlanWithPCG())
	{
		BeginPlan(Region);
		return;
	}

	// No PCG: generate the crowd deterministically from the region seed (CrowdWander::InitNewAgent).
	StartSpawning(Region);
}

void UCrowdSubsystem::StartSpawning(FCrowdRegion& Region)
{
	const FCrowdTuning& T = GetCrowdTuning();
	Region.State = ECrowdRegionState::Spawning;
	Region.SpawnCursor = 0;
	// Restore the persisted / planned population if there is one, otherwise generate it deterministically from the seed.
	Region.SpawnTarget = (Region.bPlanned || Region.Saved.Num() > 0) ? Region.Saved.Num() : T.AgentsPerRegion;
	Region.InactiveSince = -1.0;
	if (CrowdCVars::TrimRegions != 0)
	{
		Region.Agents.Reserve(Region.SpawnTarget);
	}
	SpawnQueue.Add(Region.Slot);
}

// ------------------------------------------------------------------------------------------------
// Spawn plans (PCG): where the agents of a region that was never populated start
// ------------------------------------------------------------------------------------------------

bool UCrowdSubsystem::CanPlanWithPCG() const
{
	return CrowdCVars::PCG != 0 && Planner != nullptr && Planner->IsEnabled();
}

void UCrowdSubsystem::BeginPlan(FCrowdRegion& Region)
{
	const FCrowdTuning& T = GetCrowdTuning();

	Region.State = ECrowdRegionState::Planning;

	FCrowdPlanRequest Request;
	Request.Coord = Region.Coord;
	Request.Min = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);
	Request.SizeCm = T.RegionSizeCm;
	Request.Seed = Region.Seed;
	Request.MaxAgents = T.AgentsPerRegion;
	Planner->Request(Request);
}

void UCrowdSubsystem::CancelPlan(FCrowdRegion& Region)
{
	if (Planner != nullptr)
	{
		Planner->Cancel(Region.Coord);
	}

	// Nothing was created and nothing is persisted: the region simply goes back to waiting for a viewer.
	Region.State = ECrowdRegionState::Inactive;
	Region.UnwantedSince = -1.0;
}

void UCrowdSubsystem::PumpPlans()
{
	if (Planner == nullptr)
	{
		return;
	}

	OPT_SCOPE(STAT_OptCrowd_Plan, Plan, "Opt.Crowd.Plan");

	Planner->Pump(FPlatformTime::Seconds());

	FCrowdPlanResult Result;
	while (Planner->PopResult(Result))
	{
		const int32* Slot = RegionSlotByCoord.Find(Result.Coord);
		if (Slot == nullptr)
		{
			continue;
		}

		FCrowdRegion& Region = Regions[*Slot];
		if (Region.State != ECrowdRegionState::Planning)
		{
			continue; // cancelled while its graph was running
		}
		ApplyPlan(Region, Result);
	}

	CSV_CUSTOM_STAT(OptCrowd, PlansPending, Planner->GetNumPending(), ECsvCustomStatOp::Set);
}

void UCrowdSubsystem::ApplyPlan(FCrowdRegion& Region, FCrowdPlanResult& Result)
{
	const FCrowdTuning& T = GetCrowdTuning();

	if (Result.Status != ECrowdPlanStatus::Ok)
	{
		// The planner already logged why. Fall back to the plain random crowd (StartSpawning generates it from the region
		// seed), or leave the region empty if the project asks for that.
		if (!T.bPCGFallbackToRandom)
		{
			Region.bPlanned = true;
			Region.Saved.Reset();
		}
		StartSpawning(Region);
		return;
	}

	const FVector2D Min = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);
	const CrowdWander::FBox2 Home{ Min, Min + FVector2D(T.RegionSizeCm, T.RegionSizeCm) };

	// The plan becomes the region's persisted population: from here on it is spawned by exactly the code that restores
	// a despawned region (MakeAgent unpacks Saved[Index]).
	const CrowdPlan::FBuildStats Stats = CrowdPlan::BuildAgents(
		Result.Points, Home, Region.Seed, T.AgentsPerRegion, T.bPCGUsePointYaw, T, NextNetId, PlanScratch);

	Region.Saved.Reset();
	Region.Saved.Reserve(PlanScratch.Num());
	for (const FCrowdSavedAgent& Agent : PlanScratch)
	{
		Region.Saved.Add(CrowdPersist::Pack(Agent, Min));
	}
	PlanScratch.Reset();
	OPT_BOOT_ONCE("PCG.FirstPlanApplied");

	// Saved is the truth now, even if the graph left this region empty.
	Region.bPlanned = true;

	UE_LOG(LogMassBubble, Verbose, TEXT("[Crowd] region (%d,%d) planned: %d points -> %d agents (%d off open ground, %d without density, %d not finite, %d thinned to the cap) after %.0f ms"),
		Region.Coord.X, Region.Coord.Y, Stats.Received, Stats.Created, Stats.OffGround, Stats.NoDensity, Stats.NotFinite, Stats.Thinned,
		Result.LatencySec * 1000.0);

	StartSpawning(Region);
}

FString UCrowdSubsystem::GetPlannerSummary() const
{
	if (Planner == nullptr)
	{
		return TEXT("pcg: off (no spawn graph configured, or 'Use PCG' is off in the settings)");
	}
	return Planner->GetDebugSummary();
}

int32 UCrowdSubsystem::ForgetInactivePlans()
{
	int32 Count = 0;
	for (FCrowdRegion& Region : Regions)
	{
		if (Region.State != ECrowdRegionState::Inactive || (!Region.bPlanned && Region.Saved.Num() == 0))
		{
			continue;
		}
		ForgetRegion(Region);
		++Count;
	}
	return Count;
}

void UCrowdSubsystem::ForgetRegion(FCrowdRegion& Region)
{
	Region.bPlanned = false;
	Region.Saved.Empty();
	Region.InactiveSince = -1.0;
}

int32 UCrowdSubsystem::EvictStalePersistence(double Now)
{
	const float TtlSec = GetCrowdTuning().PersistTTLSec;
	if (TtlSec <= 0.f)
	{
		return 0;
	}

	int32 Evicted = 0;
	for (FCrowdRegion& Region : Regions)
	{
		if (Region.State != ECrowdRegionState::Inactive || Region.InactiveSince < 0.0)
		{
			continue;
		}
		if (!Region.bPlanned && Region.Saved.Num() == 0)
		{
			continue;
		}
		if (Now - Region.InactiveSince >= static_cast<double>(TtlSec))
		{
			ForgetRegion(Region);
			++Evicted;
		}
	}

	NumEvictedRegions += Evicted;
	return Evicted;
}

void UCrowdSubsystem::BeginDespawn(FCrowdRegion& Region)
{
	// The replicator goes first: players must stop receiving agents that are about to be persisted.
	DestroyReplicator(Region);
	Region.bObserved = false;

	Region.State = ECrowdRegionState::Despawning;
	Region.DespawnCursor = 0;
	Region.Saved.Reset();
	Region.Saved.Reserve(Region.Agents.Num());
	DespawnQueue.Add(Region.Slot);
}

void UCrowdSubsystem::PumpRegionJobs()
{
	const FCrowdTuning& T = GetCrowdTuning();

	// opt.crowd.BudgetedPump 0: fixed slices of MaxSpawn / MaxDespawnPerFrame agents per frame,
	// no wall clock budget.
	const bool bBudgeted = CrowdCVars::BudgetedPump != 0;

	// While World Partition loads / adds / removes cells it already eats the frame: take less of it ourselves.
	const UMassBubbleStreamingMonitor* StreamMonitor = Monitor.Get();
	const bool bStreamingBusy = StreamMonitor != nullptr && StreamMonitor->IsBusy();

	// ---- spawn: agent creation, in slices, until the wall clock budget is used up ----
	{
		const int32 Batch = bBudgeted ? T.StructuralBatchSize : T.MaxSpawnPerFrame;
		const double BudgetMs = static_cast<double>(bStreamingBusy ? T.SpawnBudgetBusyMs : T.SpawnBudgetMs);
		const double Deadline = FPlatformTime::Seconds() + 0.001 * BudgetMs;

		int32 Budget = T.MaxSpawnPerFrame;
		bool bDidWork = false;
		while (Budget > 0 && SpawnQueue.Num() > 0)
		{
			// Always make progress (one batch), but never run past the wall clock budget.
			if (bBudgeted && bDidWork && FPlatformTime::Seconds() >= Deadline)
			{
				break;
			}

			FCrowdRegion& Region = Regions[SpawnQueue[0]];
			const int32 Used = SpawnSlice(Region, FMath::Min(Budget, Batch));
			Budget -= FMath::Max(Used, 1); // the max() guarantees the loop ends even if a slice created nothing
			bDidWork = true;

			if (Region.State == ECrowdRegionState::Active)
			{
				SpawnQueue.RemoveAt(0);
			}
			// else: the region still has agents left, the next iteration (or the next frame) continues with it
		}
	}

	// ---- despawn: persist the compact state, then free the agents, same rules ----
	{
		const int32 Batch = bBudgeted ? T.StructuralBatchSize : T.MaxDespawnPerFrame;
		const double BudgetMs = static_cast<double>(bStreamingBusy ? T.DespawnBudgetBusyMs : T.DespawnBudgetMs);
		const double Deadline = FPlatformTime::Seconds() + 0.001 * BudgetMs;

		int32 Budget = T.MaxDespawnPerFrame;
		bool bDidWork = false;
		while (Budget > 0 && DespawnQueue.Num() > 0)
		{
			if (bBudgeted && bDidWork && FPlatformTime::Seconds() >= Deadline)
			{
				break;
			}

			FCrowdRegion& Region = Regions[DespawnQueue[0]];
			const int32 Used = DespawnSlice(Region, FMath::Min(Budget, Batch));
			Budget -= FMath::Max(Used, 1);
			bDidWork = true;

			if (Region.State == ECrowdRegionState::Inactive)
			{
				DespawnQueue.RemoveAt(0);
			}
		}
	}
}

FCrowdAgentSim UCrowdSubsystem::MakeAgent(const FCrowdRegion& Region, int32 Index)
{
	const FCrowdTuning& T = GetCrowdTuning();
	const FVector2D Min = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);

	if (Region.Saved.IsValidIndex(Index))
	{
		return CrowdSim::FromSaved(CrowdPersist::Unpack(Region.Saved[Index], Min)); // restore what was persisted
	}

	FCrowdSavedAgent Agent;
	Agent.NetId = NextNetId++;

	// Position, heading, cruise speed and the first pause are decided by CrowdWander::InitNewAgent (unit tested): new
	// agents start on open ground, never inside the band along the region border.
	const CrowdWander::FBox2 Home{ Min, Min + FVector2D(T.RegionSizeCm, T.RegionSizeCm) };
	CrowdWander::InitNewAgent(Agent, Region.Seed + static_cast<uint32>(Index) * 2654435761u, Home, T);

	// The tier stays Off until UpdateLod classifies the agent.
	return CrowdSim::FromSaved(Agent);
}

int32 UCrowdSubsystem::SpawnSlice(FCrowdRegion& Region, int32 Budget)
{
	OPT_SCOPE(STAT_OptCrowd_Spawn, SpawnSlice, "Opt.Crowd.SpawnSlice");
	OPT_LLM_SCOPE(OptCrowd_Agents);
	OPT_BOOT_ONCE("Crowd.FirstSpawnSlice");

	const int32 Remaining = Region.SpawnTarget - Region.SpawnCursor;
	const int32 Count = FMath::Min(Budget, Remaining);
	if (Count <= 0)
	{
		MarkRegionActive(Region);
		return 0;
	}

	for (int32 i = 0; i < Count; ++i)
	{
		Region.Agents.Add(MakeAgent(Region, Region.SpawnCursor + i));
	}

	Region.SpawnCursor += Count;
	AliveAgents += Count;

	if (Region.SpawnCursor >= Region.SpawnTarget)
	{
		MarkRegionActive(Region);
	}
	return Count;
}

int32 UCrowdSubsystem::DespawnSlice(FCrowdRegion& Region, int32 Budget)
{
	OPT_SCOPE(STAT_OptCrowd_Despawn, DespawnSlice, "Opt.Crowd.DespawnSlice");
	OPT_LLM_SCOPE(OptCrowd_Persist);

	const int32 Remaining = Region.Agents.Num() - Region.DespawnCursor;
	const int32 Count = FMath::Min(Budget, Remaining);

	if (Count > 0)
	{
		// Persist the compact state of this slice. From here on the agents of the slice stop simulating.
		const FVector2D RegionMin = CrowdMath::RegionMin(Region.Coord, GetCrowdTuning().RegionSizeCm);
		for (int32 i = Region.DespawnCursor; i < Region.DespawnCursor + Count; ++i)
		{
			Region.Saved.Add(CrowdPersist::Pack(CrowdSim::ToSaved(Region.Agents[i]), RegionMin));
		}

		Region.DespawnCursor += Count;
		AliveAgents -= Count;
	}

	if (Region.DespawnCursor >= Region.Agents.Num())
	{
		if (CrowdCVars::TrimRegions != 0)
		{
			Region.Agents.Empty();
		}
		else
		{
			Region.Agents.Reset();
		}
		Region.DespawnCursor = 0;
		Region.State = ECrowdRegionState::Inactive;
		Region.InactiveSince = GetNow();
	}
	return Count;
}

// ------------------------------------------------------------------------------------------------
// Simulation: LOD tiers and movement. One task per region, regions share no data.
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::CollectSimulatedRegions()
{
	SimulatedScratch.Reset();
	for (const FCrowdRegion& Region : Regions)
	{
		if (HoldsAgents(Region.State) && Region.Agents.Num() > FirstSimulated(Region))
		{
			SimulatedScratch.Add(Region.Slot);
		}
	}
}

void UCrowdSubsystem::RunPerRegion(int32 Num, TFunctionRef<void(int32)> Body) const
{
	if (CrowdCVars::ParallelMove != 0 && Num > 1)
	{
		ParallelFor(TEXT("OptCrowd.Regions"), Num, 1, Body);
		return;
	}

	for (int32 Index = 0; Index < Num; ++Index)
	{
		Body(Index);
	}
}

void UCrowdSubsystem::BuildLodTable(const FCrowdTuning& Tuning)
{
	LodTable.Begin(Regions.Num(), LodViewers);
	for (const FCrowdRegion& Region : Regions)
	{
		if (HoldsAgents(Region.State))
		{
			LodTable.AddRegion(Region.Slot, CrowdMath::RegionMin(Region.Coord, Tuning.RegionSizeCm), Tuning.RegionSizeCm, Tuning);
		}
	}

	CSV_CUSTOM_STAT(OptCrowd, LodMixedRegions, LodTable.NumMixed, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, LodOffRegions, LodTable.NumAllOff, ECsvCustomStatOp::Set);
}

void UCrowdSubsystem::UpdateLod()
{
	OPT_SCOPE(STAT_OptCrowd_LOD, LOD, "Opt.Crowd.LOD");

	const FCrowdTuning& Tuning = GetCrowdTuning();
	const bool bUseLOD = CrowdCVars::LOD != 0;

	LodViewers.Reset();
	for (const FCrowdViewer& Viewer : Viewers)
	{
		LodViewers.Add(Viewer.Location);
	}

	// Per-region bounds: with several viewers (players and load-test bots) most agents are measured against one or two of them.
	const bool bUseTable = bUseLOD && CrowdCVars::LODPrepass != 0 && Viewers.Num() >= CrowdLod::MinViewersForTable;
	if (bUseTable)
	{
		BuildLodTable(Tuning);
	}

	CrowdSim::FClassifyParams Params;
	Params.Tuning = &Tuning;
	Params.Table = bUseTable ? &LodTable : nullptr;
	Params.Viewers = LodViewers;
	Params.Frame = static_cast<uint32>(GFrameCounter);
	Params.bUseLOD = bUseLOD;

	TierScratch.SetNum(SimulatedScratch.Num());
	for (FTierCounts& Counts : TierScratch)
	{
		Counts = FTierCounts();
	}

	RunPerRegion(SimulatedScratch.Num(), [this, &Params](int32 Index)
	{
		FCrowdRegion& Region = Regions[SimulatedScratch[Index]];
		const int32 First = FirstSimulated(Region);
		CrowdSim::ClassifyRegion(
			TArrayView<FCrowdAgentSim>(Region.Agents.GetData() + First, Region.Agents.Num() - First),
			Region.Slot, Params, TierScratch[Index].Count);
	});

	int32 Total[CrowdLODCount] = { 0, 0, 0, 0 };
	for (const FTierCounts& Counts : TierScratch)
	{
		for (int32 Tier = 0; Tier < CrowdLODCount; ++Tier)
		{
			Total[Tier] += Counts.Count[Tier];
		}
	}
	SetTierCounts(Total);
}

void UCrowdSubsystem::MoveAgents(float DeltaSeconds)
{
	OPT_SCOPE(STAT_OptCrowd_Move, Move, "Opt.Crowd.Move");

	const FCrowdTuning& Tuning = GetCrowdTuning();

	CrowdSim::FMoveParams Params;
	Params.Tuning = &Tuning;
	Params.Frame = static_cast<uint32>(GFrameCounter);
	Params.DeltaSeconds = DeltaSeconds;
	Params.bTimeSlice = CrowdCVars::TimeSlicing != 0;

	MoveScratch.SetNum(SimulatedScratch.Num());

	RunPerRegion(SimulatedScratch.Num(), [this, &Params](int32 Index)
	{
		FCrowdRegion& Region = Regions[SimulatedScratch[Index]];
		const int32 First = FirstSimulated(Region);
		MoveScratch[Index] = CrowdSim::MoveRegion(
			TArrayView<FCrowdAgentSim>(Region.Agents.GetData() + First, Region.Agents.Num() - First), Region.Coord, Params);
	});

	int32 Simulated = 0;
	int32 Clamped = 0;
	for (const CrowdSim::FMoveStats& Stats : MoveScratch)
	{
		Simulated += Stats.Simulated;
		Clamped += Stats.Clamped;
	}

	INC_DWORD_STAT_BY(STAT_OptCrowd_Simulated, Simulated);
	CSV_CUSTOM_STAT(OptCrowd, Simulated, Simulated, ECsvCustomStatOp::Set);
	// Agents the safety clamp had to pull back into their region. The planner keeps them inside, so this reads 0.
	CSV_CUSTOM_STAT(OptCrowd, WanderClamped, Clamped, ECsvCustomStatOp::Set);
}

// ------------------------------------------------------------------------------------------------
// Replication: one replicator per region that a real player can reach
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::EvaluateReplication(double Now)
{
	const FCrowdTuning& T = GetCrowdTuning();

	for (FCrowdRegion& Region : Regions)
	{
		Region.bObserved = false;
	}

	if (CrowdCVars::SnapshotScope == 0)
	{
		// opt.crowd.SnapshotScope 0: every active region has a replicator, whoever is looking at it.
		for (FCrowdRegion& Region : Regions)
		{
			Region.bObserved = (Region.State == ECrowdRegionState::Active);
		}
	}
	else
	{
		// Only regions that can reach into the replication radius of a real player. Regions that only load-test bots look
		// at are simulated but never replicated. The margin gives the replicator time to exist before the player gets there.
		const double Reach = static_cast<double>(T.ReplicationRadiusCm) + static_cast<double>(T.ReplicatorMarginCm);
		const double ReachSq = Reach * Reach;
		const double Size = T.RegionSizeCm;

		for (const FCrowdViewer& Viewer : Viewers)
		{
			if (Viewer.bVirtual)
			{
				continue;
			}

			const int32 MinRX = FMath::FloorToInt((Viewer.Location.X - Reach) / Size);
			const int32 MaxRX = FMath::FloorToInt((Viewer.Location.X + Reach) / Size);
			const int32 MinRY = FMath::FloorToInt((Viewer.Location.Y - Reach) / Size);
			const int32 MaxRY = FMath::FloorToInt((Viewer.Location.Y + Reach) / Size);

			for (int32 RY = MinRY; RY <= MaxRY; ++RY)
			{
				for (int32 RX = MinRX; RX <= MaxRX; ++RX)
				{
					const int32* Slot = RegionSlotByCoord.Find(FIntPoint(RX, RY));
					if (Slot == nullptr)
					{
						continue;
					}

					FCrowdRegion& Region = Regions[*Slot];
					if (Region.State != ECrowdRegionState::Active || Region.bObserved)
					{
						continue;
					}

					const FVector2D BoxMin = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);
					const FVector2D BoxMax = BoxMin + FVector2D(Size, Size);
					if (CrowdLod::NearSq(BoxMin, BoxMax, Viewer.Location) <= ReachSq)
					{
						Region.bObserved = true;
					}
				}
			}
		}
	}

	// Replicators of regions in reach are created, nearest first. Those that left every player's reach go after a while.
	TArray<TPair<double, int32>, TInlineAllocator<32>> ToCreate;
	for (FCrowdRegion& Region : Regions)
	{
		if (Region.State != ECrowdRegionState::Active)
		{
			continue; // a region that despawns takes its replicator with it (BeginDespawn)
		}

		if (Region.bObserved)
		{
			Region.ReplicatorUnwantedSince = -1.0;
			if (!Region.Replicator.IsValid() && !Region.bReplicatorQueued && Region.Agents.Num() > 0)
			{
				const FVector2D RegionCenter = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm) + FVector2D(T.RegionSizeCm * 0.5, T.RegionSizeCm * 0.5);
				double Best = 0.0;
				bool bAnyPlayer = false;
				for (const FCrowdViewer& Viewer : Viewers)
				{
					if (!Viewer.bVirtual)
					{
						const double DistSq = FVector2D::DistSquared(RegionCenter, Viewer.Location);
						Best = bAnyPlayer ? FMath::Min(Best, DistSq) : DistSq;
						bAnyPlayer = true;
					}
				}

				Region.bReplicatorQueued = true;
				ToCreate.Emplace(Best, Region.Slot);
			}
		}
		else if (Region.Replicator.IsValid())
		{
			if (Region.ReplicatorUnwantedSince < 0.0)
			{
				Region.ReplicatorUnwantedSince = Now;
			}
			else if (Now - Region.ReplicatorUnwantedSince >= static_cast<double>(T.ReplicatorLingerSec))
			{
				DestroyReplicator(Region);
			}
		}
	}

	ToCreate.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
	for (const TPair<double, int32>& Item : ToCreate)
	{
		ReplicatorQueue.Add(Item.Value);
	}
}

void UCrowdSubsystem::PumpReplicators()
{
	if (ReplicatorQueue.Num() == 0)
	{
		return;
	}

	const FCrowdTuning& T = GetCrowdTuning();

	// Spawning an actor is the expensive part: take less of the frame while World Partition streams.
	const UMassBubbleStreamingMonitor* StreamMonitor = Monitor.Get();
	const bool bStreamingBusy = StreamMonitor != nullptr && StreamMonitor->IsBusy();

	int32 Budget = bStreamingBusy ? 1 : T.MaxReplicatorSpawnsPerFrame;
	while (Budget > 0 && ReplicatorQueue.Num() > 0)
	{
		const int32 Slot = ReplicatorQueue[0];
		ReplicatorQueue.RemoveAt(0, 1, EAllowShrinking::No);

		FCrowdRegion& Region = Regions[Slot];
		Region.bReplicatorQueued = false;
		if (Region.State != ECrowdRegionState::Active || !Region.bObserved || Region.Replicator.IsValid())
		{
			continue; // the region changed while it waited
		}

		CreateReplicator(Region);
		--Budget;
	}
}

void UCrowdSubsystem::CreateReplicator(FCrowdRegion& Region)
{
	OPT_LLM_SCOPE(OptCrowd_Replicators);

	UWorld* World = GetWorld();
	const FCrowdTuning& T = GetCrowdTuning();
	if (World == nullptr || Region.Agents.Num() == 0)
	{
		return;
	}

	// The actor stands in the middle of its region: that is the location the spatial filter and the prioritizer use.
	const FVector2D Min = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);
	const double Half = 0.5 * static_cast<double>(T.RegionSizeCm);
	const FTransform Transform(FVector(Min.X + Half, Min.Y + Half, static_cast<double>(T.AgentGroundZ)));

	// Deferred spawn: the region, the cull distance and the size of the array are set before the actor registers.
	ACrowdRegionReplicator* Replicator = World->SpawnActorDeferred<ACrowdRegionReplicator>(
		ACrowdRegionReplicator::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Replicator == nullptr)
	{
		return;
	}

	// A player receives the actor while it stands within the cull distance of its center: the replication radius around
	// the region plus the distance from the center to a corner (0.7071 of the edge).
	const double CullDistanceCm = static_cast<double>(T.ReplicationRadiusCm) + 0.70710678 * static_cast<double>(T.RegionSizeCm);

	Replicator->SetFlags(RF_Transient);
	Replicator->ServerInitialize(Region.Coord, T.RegionSizeCm, Region.Agents.Num(), CullDistanceCm, T.ReplicationHz);
	Replicator->FinishSpawning(Transform);

	// Every agent is written once, so the first state a player receives holds all of them.
	const double Now = GetNow();
	const uint8 Stamp = CrowdNet::StampOf(Now);
	for (int32 Index = 0; Index < Region.Agents.Num(); ++Index)
	{
		FCrowdAgentSim& Agent = Region.Agents[Index];
		Replicator->ServerWriteAgent(Index, Agent.Location, CrowdSim::PublishedVelocity(Agent), Stamp);
		Agent.SentPos = Replicator->GetAgentPos(Index);
		Agent.SentVel = Replicator->GetAgentVel(Index);
		Agent.SentTime = Now;
	}
	Replicator->ServerCommit(Stamp);

	Region.Replicator = Replicator;
	Region.ReplicatorUnwantedSince = -1.0;
	OPT_BOOT_ONCE("Net.FirstReplicator");
}

void UCrowdSubsystem::DestroyReplicator(FCrowdRegion& Region)
{
	if (ACrowdRegionReplicator* Replicator = Region.Replicator.Get())
	{
		Replicator->Destroy(); // the replication system removes it from every player that has it
	}
	Region.Replicator.Reset();
	Region.ReplicatorUnwantedSince = -1.0;
}

void UCrowdSubsystem::PublishRegions(float DeltaSeconds)
{
	const FCrowdTuning& T = GetCrowdTuning();

	// Replication cadence: the state of the replicators is refreshed at ReplicationHz.
	const float Hz = CrowdCVars::ReplicationHz > 0.f ? CrowdCVars::ReplicationHz : T.ReplicationHz;
	const float Interval = 1.f / FMath::Max(Hz, 1.f);
	PublishAccumulator += DeltaSeconds;
	if (PublishAccumulator < Interval)
	{
		return;
	}
	PublishAccumulator = FMath::Min(PublishAccumulator - Interval, Interval);

	if (!CrowdCVars::Replicate)
	{
		return;
	}

	OPT_SCOPE(STAT_OptCrowd_Publish, Publish, "Opt.Crowd.Publish");

	const double Now = GetNow();
	const uint8 Stamp = CrowdNet::StampOf(Now);
	const bool bDeadReckoning = CrowdCVars::DeadReckoning != 0;

	int32 Items = 0;
	int32 Dirty = 0;
	for (FCrowdRegion& Region : Regions)
	{
		// A replicator that is only lingering (no player within reach) is not refreshed: dead reckoning catches it up
		// with the next publish after a player comes back.
		if (Region.State != ECrowdRegionState::Active || !Region.bObserved)
		{
			continue;
		}
		ACrowdRegionReplicator* Replicator = Region.Replicator.Get();
		if (Replicator == nullptr)
		{
			continue;
		}

		const int32 Num = FMath::Min(Region.Agents.Num(), Replicator->GetNumAgents());
		int32 RegionDirty = 0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			FCrowdAgentSim& Agent = Region.Agents[Index];
			const FVector2f Velocity = CrowdSim::PublishedVelocity(Agent);
			if (!CrowdSim::NeedsPublish(Agent, Velocity, Now, T, bDeadReckoning))
			{
				continue;
			}

			// Round trip through the quantizer: the dead-reckoning check must compare against what the clients see.
			Replicator->ServerWriteAgent(Index, Agent.Location, Velocity, Stamp);
			Agent.SentPos = Replicator->GetAgentPos(Index);
			Agent.SentVel = Replicator->GetAgentVel(Index);
			Agent.SentTime = Now;
			++RegionDirty;
		}

		if (RegionDirty > 0)
		{
			Replicator->ServerCommit(Stamp);
		}
		Items += Num;
		Dirty += RegionDirty;
	}

	INC_DWORD_STAT_BY(STAT_OptCrowd_ReplItems, Items);
	INC_DWORD_STAT_BY(STAT_OptCrowd_ReplDirty, Dirty);
	CSV_CUSTOM_STAT(OptCrowd, ReplItems, Items, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, ReplDirty, Dirty, ECsvCustomStatOp::Set);
}

// ------------------------------------------------------------------------------------------------
// Stats / debugging
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::SetTierCounts(const int32 (&InCounts)[CrowdLODCount])
{
	for (int32 i = 0; i < CrowdLODCount; ++i)
	{
		TierCounts[i] = InCounts[i];
	}
}

int32 UCrowdSubsystem::GetNumRegionsInState(ECrowdRegionState State) const
{
	int32 Count = 0;
	for (const FCrowdRegion& Region : Regions)
	{
		Count += (Region.State == State) ? 1 : 0;
	}
	return Count;
}

int32 UCrowdSubsystem::GetNumReplicators() const
{
	int32 Count = 0;
	for (const FCrowdRegion& Region : Regions)
	{
		Count += Region.Replicator.IsValid() ? 1 : 0;
	}
	return Count;
}

FCrowdMemoryStats UCrowdSubsystem::GetMemoryStats() const
{
	FCrowdMemoryStats Stats;
	Stats.NumRegions = Regions.Num();
	Stats.NumEvictedRegions = NumEvictedRegions;
	Stats.RegionTableBytes = Regions.GetAllocatedSize() + RegionSlotByCoord.GetAllocatedSize();

	for (const FCrowdRegion& Region : Regions)
	{
		const SIZE_T SavedUsed = static_cast<SIZE_T>(Region.Saved.Num()) * sizeof(FCrowdPackedAgent);
		const SIZE_T SavedAllocated = Region.Saved.GetAllocatedSize();
		const SIZE_T AgentsUsed = static_cast<SIZE_T>(Region.Agents.Num()) * sizeof(FCrowdAgentSim);
		const SIZE_T AgentsAllocated = Region.Agents.GetAllocatedSize();

		Stats.NumPersistedAgents += Region.Saved.Num();
		Stats.NumReplicators += Region.Replicator.IsValid() ? 1 : 0;
		Stats.SavedUsedBytes += SavedUsed;
		Stats.SavedAllocatedBytes += SavedAllocated;
		Stats.AgentsUsedBytes += AgentsUsed;
		Stats.AgentsAllocatedBytes += AgentsAllocated;

		Stats.DeadBytes += (SavedAllocated - SavedUsed) + (AgentsAllocated - AgentsUsed);
	}
	return Stats;
}

FString UCrowdSubsystem::GetMemorySummary() const
{
	const FCrowdMemoryStats Stats = GetMemoryStats();
	const auto KB = [](SIZE_T Bytes) { return static_cast<double>(Bytes) / 1024.0; };

	return FString::Printf(TEXT("memory: regions=%d (table %.0f KB, evicted %d) | persisted agents=%d, saved %.0f KB used / %.0f KB allocated | simulated agents %.0f / %.0f KB | replicators=%d | dead capacity %.0f KB"),
		Stats.NumRegions, KB(Stats.RegionTableBytes), Stats.NumEvictedRegions, Stats.NumPersistedAgents,
		KB(Stats.SavedUsedBytes), KB(Stats.SavedAllocatedBytes), KB(Stats.AgentsUsedBytes), KB(Stats.AgentsAllocatedBytes),
		Stats.NumReplicators, KB(Stats.DeadBytes));
}

void UCrowdSubsystem::PublishMemoryStats()
{
	const FCrowdMemoryStats Stats = GetMemoryStats();
	CSV_CUSTOM_STAT(OptCrowd, SavedKB, static_cast<float>(Stats.SavedAllocatedBytes) / 1024.f, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, DeadKB, static_cast<float>(Stats.DeadBytes) / 1024.f, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, ProcessMB, static_cast<float>(MassBubbleMem::GetProcessUsedBytes()) / (1024.f * 1024.f), ECsvCustomStatOp::Set);
	MassBubbleMem::PublishCsvMetadata();
}

FString UCrowdSubsystem::GetDebugSummary() const
{
	return FString::Printf(TEXT("crowd: regions planning=%d spawning=%d despawning=%d | queues spawn=%d despawn=%d replicator=%d | replicators=%d | pcg plans pending=%d | bot ops pending=%d"),
		GetNumRegionsInState(ECrowdRegionState::Planning), GetNumRegionsInState(ECrowdRegionState::Spawning),
		GetNumRegionsInState(ECrowdRegionState::Despawning),
		SpawnQueue.Num(), DespawnQueue.Num(), ReplicatorQueue.Num(),
		GetNumReplicators(), Planner != nullptr ? Planner->GetNumPending() : 0, GetNumPendingBotOps());
}

void UCrowdSubsystem::DumpStats() const
{
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] viewers=%d regions(total=%d active=%d planning=%d spawning=%d despawning=%d inactive=%d) alive=%d"),
		Viewers.Num(), Regions.Num(),
		GetNumRegionsInState(ECrowdRegionState::Active), GetNumRegionsInState(ECrowdRegionState::Planning),
		GetNumRegionsInState(ECrowdRegionState::Spawning),
		GetNumRegionsInState(ECrowdRegionState::Despawning), GetNumRegionsInState(ECrowdRegionState::Inactive),
		AliveAgents);
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] LOD tiers: High=%d Medium=%d Low=%d Off=%d | spawnQueue=%d despawnQueue=%d replicatorQueue=%d"),
		TierCounts[0], TierCounts[1], TierCounts[2], TierCounts[3], SpawnQueue.Num(), DespawnQueue.Num(), ReplicatorQueue.Num());
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *GetDebugSummary());
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *GetMemorySummary());
	if (Planner != nullptr)
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *Planner->GetDebugSummary());
	}
	if (const UMassBubbleStreamingMonitor* StreamMonitor = Monitor.Get())
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *StreamMonitor->GetDebugSummary());
	}
}
