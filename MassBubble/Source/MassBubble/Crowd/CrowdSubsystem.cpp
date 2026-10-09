#include "Crowd/CrowdSubsystem.h"

#include "Core/MassBubbleStats.h"
#include "Core/MassBubbleStreamingMonitor.h"
#include "Crowd/CrowdFragments.h"
#include "Crowd/CrowdMath.h"
#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdWander.h"
#include "MassBubble.h"
#include "World/MassBubbleStreamingAnchor.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "MassEntitySubsystem.h"

bool UCrowdSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Authority only (dedicated server, listen host or standalone).
	// Pure clients render what their CrowdBubble replicates.
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

	UMassEntitySubsystem* MassSubsystem = Collection.InitializeDependency<UMassEntitySubsystem>();
	check(MassSubsystem);
	EntityManager = MassSubsystem->GetMutableEntityManager().AsShared();

	// Tells us when World Partition is busy: spawn / despawn budgets shrink while it is.
	Monitor = Collection.InitializeDependency<UMassBubbleStreamingMonitor>();

	ReloadCrowdTuning();
	const FCrowdTuning& T = GetCrowdTuning();
	CachedRegionSizeCm = T.RegionSizeCm;
	CachedGridCellSizeCm = T.GridCellSizeCm;

	UE_LOG(LogMassBubble, Log, TEXT("CrowdSubsystem up (NetMode=%d). Region=%.0fcm, ActiveRadius=%d, AgentsPerRegion=%d, ReplicationHz=%.1f"),
		static_cast<int32>(GetWorld()->GetNetMode()), T.RegionSizeCm, T.ActiveRegionRadius, T.AgentsPerRegion, T.ReplicationHz);
}

void UCrowdSubsystem::Deinitialize()
{
	Regions.Empty();
	RegionSlotByCoord.Empty();
	SpawnQueue.Empty();
	DespawnQueue.Empty();
	VirtualViewers.Empty();
	Viewers.Empty();
	BotJobs.Empty();
	RegionSnapshotMask.Empty();
	Monitor.Reset();
	AgentArchetype = FMassArchetypeHandle();
	EntityManager.Reset();

	Super::Deinitialize();
}

double UCrowdSubsystem::GetNow() const
{
	return static_cast<double>(GetWorld()->GetTimeSeconds());
}

// ------------------------------------------------------------------------------------------------
// Director tick
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::TickDirector(float DeltaSeconds)
{
	OPT_SCOPE(STAT_OptCrowd_Director, Director, "Opt.Crowd.Director");

	// Bots run even with opt.crowd.Enable 0, so the engine's streaming cost can be measured on its own.
	TickBotRamp(DeltaSeconds);

	if (!CrowdCVars::Enable || !EntityManager.IsValid())
	{
		return;
	}

	const FCrowdTuning& T = GetCrowdTuning();

	RefreshViewers();

	// Region desire is evaluated at 4 Hz: viewers cross 128 m regions slowly compared to the frame rate.
	RegionEvalAccumulator += DeltaSeconds;
	if (RegionEvalAccumulator >= 0.25f)
	{
		RegionEvalAccumulator = 0.f;
		EvaluateRegions(GetNow());
	}

	// Structural changes (create / destroy) are only legal while Mass is not processing.
	if (!EntityManager->IsProcessing())
	{
		PumpRegionJobs();
	}

	// Replication snapshot cadence. The processor does the work when it sees the flag.
	const float Hz = CrowdCVars::ReplicationHz > 0.f ? CrowdCVars::ReplicationHz : T.ReplicationHz;
	const float Interval = 1.f / FMath::Max(Hz, 1.f);
	SnapshotAccumulator += DeltaSeconds;
	if (CrowdCVars::Replicate && SnapshotAccumulator >= Interval)
	{
		SnapshotAccumulator = FMath::Min(SnapshotAccumulator - Interval, Interval);
		bSnapshotDue = true;
	}

	const int32 ActiveRegions = GetNumRegionsInState(ECrowdRegionState::Active);
	SET_DWORD_STAT(STAT_OptCrowd_Agents, AliveAgents);
	SET_DWORD_STAT(STAT_OptCrowd_Regions, ActiveRegions);
	CSV_CUSTOM_STAT(OptCrowd, Agents, AliveAgents, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(OptCrowd, ActiveRegions, ActiveRegions, ECsvCustomStatOp::Set);
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
	Region.Grid.Init(CrowdMath::RegionMin(Coord, T.RegionSizeCm), T.RegionSizeCm, T.CellsPerSide);

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
	TArray<TPair<double, int32>> ToActivate;
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

	// Regions nobody wants any more: despawn after a grace period (hysteresis against boundary flicker).
	for (FCrowdRegion& Region : Regions)
	{
		if (Region.State != ECrowdRegionState::Active || WantedScratch.Contains(Region.Coord))
		{
			continue;
		}
		if (Region.UnwantedSince < 0.0)
		{
			Region.UnwantedSince = Now;
		}
		else if (Now - Region.UnwantedSince >= T.RegionDeactivateDelaySec)
		{
			BeginDespawn(Region);
		}
	}
}

void UCrowdSubsystem::BeginSpawn(FCrowdRegion& Region)
{
	const FCrowdTuning& T = GetCrowdTuning();
	Region.State = ECrowdRegionState::Spawning;
	Region.SpawnCursor = 0;
	// Restore the persisted population if there is one, otherwise generate it deterministically from the seed.
	Region.SpawnTarget = Region.Saved.Num() > 0 ? Region.Saved.Num() : T.AgentsPerRegion;
	SpawnQueue.Add(Region.Slot);
}

void UCrowdSubsystem::BeginDespawn(FCrowdRegion& Region)
{
	Region.State = ECrowdRegionState::Despawning;
	Region.DespawnCursor = 0;
	Region.Saved.Reset();
	Region.Saved.Reserve(Region.Entities.Num());
	Region.Grid.Clear(); // replication must stop seeing these agents immediately
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

	// ---- spawn: Mass entity creation, in sub-slices, until the wall clock budget is used up ----
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

	// ---- despawn: persist the compact state, then destroy the entities, same rules ----
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

FMassArchetypeHandle UCrowdSubsystem::GetAgentArchetype()
{
	if (!AgentArchetype.IsValid())
	{
		AgentArchetype = EntityManager->CreateArchetype({
			FCrowdAgentTag::StaticStruct(),
			FCrowdIdFragment::StaticStruct(),
			FCrowdLocationFragment::StaticStruct(),
			FCrowdMotionFragment::StaticStruct(),
			FCrowdLODFragment::StaticStruct() });
	}
	return AgentArchetype;
}

FCrowdSavedAgent UCrowdSubsystem::MakeAgent(const FCrowdRegion& Region, int32 Index)
{
	if (Region.Saved.IsValidIndex(Index))
	{
		return Region.Saved[Index]; // restore exactly what was persisted
	}

	const FCrowdTuning& T = GetCrowdTuning();

	FCrowdSavedAgent Agent;
	Agent.NetId = NextNetId++;

	// Position, heading, cruise speed and the first pause are decided by CrowdWander::InitNewAgent (unit tested): new
	// agents start on open ground, never inside the band along the region border.
	const FVector2D Min = CrowdMath::RegionMin(Region.Coord, T.RegionSizeCm);
	const CrowdWander::FBox2 Home{ Min, Min + FVector2D(T.RegionSizeCm, T.RegionSizeCm) };
	CrowdWander::InitNewAgent(Agent, Region.Seed + static_cast<uint32>(Index) * 2654435761u, Home, T);
	return Agent;
}

int32 UCrowdSubsystem::SpawnSlice(FCrowdRegion& Region, int32 Budget)
{
	OPT_SCOPE(STAT_OptCrowd_Spawn, SpawnSlice, "Opt.Crowd.SpawnSlice");

	const int32 Remaining = Region.SpawnTarget - Region.SpawnCursor;
	const int32 Count = FMath::Min(Budget, Remaining);
	if (Count <= 0)
	{
		Region.Saved.Reset();
		Region.State = ECrowdRegionState::Active;
		return 0;
	}

	FMassEntityManager& EM = *EntityManager;

	TArray<FMassEntityHandle> NewEntities;
	NewEntities.Reserve(Count);
	{
		// Observers (none today) are notified when this context goes out of scope, i.e. after the data is filled in.
		const auto CreationContext = EM.BatchCreateEntities(GetAgentArchetype(), Count, NewEntities);

		for (int32 i = 0; i < NewEntities.Num(); ++i)
		{
			const FCrowdSavedAgent Agent = MakeAgent(Region, Region.SpawnCursor + i);
			const FMassEntityHandle Entity = NewEntities[i];

			FCrowdIdFragment& Id = EM.GetFragmentDataChecked<FCrowdIdFragment>(Entity);
			Id.NetId = Agent.NetId;
			Id.HomeX = Region.Coord.X;
			Id.HomeY = Region.Coord.Y;
			Id.RegionSlot = Region.Slot;

			EM.GetFragmentDataChecked<FCrowdLocationFragment>(Entity).Location = Agent.Location;

			FCrowdMotionFragment& Motion = EM.GetFragmentDataChecked<FCrowdMotionFragment>(Entity);
			Motion.Velocity = Agent.Velocity;
			Motion.RetargetTimer = Agent.RetargetTimer;
			Motion.Rng = CrowdMath::NonZeroSeed(Agent.Rng);
			Motion.Heading = Agent.Heading;
			Motion.CruiseSpeed = Agent.CruiseSpeed;
			// FCrowdLODFragment keeps its default (Off) until UCrowdLODProcessor classifies the agent.
		}
	}

	Region.Entities.Append(NewEntities);
	Region.SpawnCursor += NewEntities.Num();
	AliveAgents += NewEntities.Num();

	if (Region.SpawnCursor >= Region.SpawnTarget)
	{
		Region.Saved.Reset(); // live entities are the truth again
		Region.State = ECrowdRegionState::Active;
	}
	return NewEntities.Num();
}

int32 UCrowdSubsystem::DespawnSlice(FCrowdRegion& Region, int32 Budget)
{
	OPT_SCOPE(STAT_OptCrowd_Despawn, DespawnSlice, "Opt.Crowd.DespawnSlice");

	FMassEntityManager& EM = *EntityManager;

	const int32 Remaining = Region.Entities.Num() - Region.DespawnCursor;
	const int32 Count = FMath::Min(Budget, Remaining);

	if (Count > 0)
	{
		// 1) persist the compact state of this slice
		for (int32 i = Region.DespawnCursor; i < Region.DespawnCursor + Count; ++i)
		{
			const FMassEntityHandle Entity = Region.Entities[i];
			const FCrowdIdFragment& Id = EM.GetFragmentDataChecked<FCrowdIdFragment>(Entity);
			const FCrowdLocationFragment& Loc = EM.GetFragmentDataChecked<FCrowdLocationFragment>(Entity);
			const FCrowdMotionFragment& Motion = EM.GetFragmentDataChecked<FCrowdMotionFragment>(Entity);

			FCrowdSavedAgent& Saved = Region.Saved.AddDefaulted_GetRef();
			Saved.NetId = Id.NetId;
			Saved.Location = Loc.Location;
			Saved.Velocity = Motion.Velocity;
			Saved.RetargetTimer = Motion.RetargetTimer;
			Saved.Rng = Motion.Rng;
			Saved.Heading = Motion.Heading;
			Saved.CruiseSpeed = Motion.CruiseSpeed;
		}

		// 2) destroy them in one batch
		EM.BatchDestroyEntities(TConstArrayView<FMassEntityHandle>(Region.Entities.GetData() + Region.DespawnCursor, Count));
		Region.DespawnCursor += Count;
		AliveAgents -= Count;
	}

	if (Region.DespawnCursor >= Region.Entities.Num())
	{
		Region.Entities.Reset();
		Region.DespawnCursor = 0;
		Region.State = ECrowdRegionState::Inactive;
	}
	return Count;
}

// ------------------------------------------------------------------------------------------------
// Snapshot (written by UCrowdSnapshotProcessor, read by ACrowdBubble)
// ------------------------------------------------------------------------------------------------

void UCrowdSubsystem::BeginSnapshot()
{
	const FCrowdTuning& T = GetCrowdTuning();

	RegionSnapshotMask.Init(0, Regions.Num());

	if (CrowdCVars::SnapshotScope == 0)
	{
		// opt.crowd.SnapshotScope 0: snapshot every live region, whoever is looking at it.
		for (const FCrowdRegion& Region : Regions)
		{
			if (Region.State == ECrowdRegionState::Active || Region.State == ECrowdRegionState::Spawning)
			{
				RegionSnapshotMask[Region.Slot] = 1;
			}
		}
	}
	else
	{
		// Only regions that can reach into the interest circle of a real player. Regions that only load-test bots
		// look at are never replicated, so their grids are not built. The margin covers the player's movement
		// between RefreshViewers() and the bubble rebuild of the same frame.
		const double Reach = static_cast<double>(T.BubbleRadiusCm) + static_cast<double>(T.SnapshotMarginCm);
		const float Size = CachedRegionSizeCm;

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
					const FCrowdRegion& Region = Regions[*Slot];
					if (Region.State == ECrowdRegionState::Active || Region.State == ECrowdRegionState::Spawning)
					{
						RegionSnapshotMask[*Slot] = 1;
					}
				}
			}
		}
	}

	for (int32 Slot = 0; Slot < Regions.Num(); ++Slot)
	{
		if (RegionSnapshotMask[Slot] != 0)
		{
			Regions[Slot].Grid.BeginBuild();
		}
	}
}

void UCrowdSubsystem::AddToSnapshot(int32 RegionSlot, const FVector2D& Pos, const FVector2f& Vel, uint32 NetId)
{
	// The mask only contains Active / Spawning regions (BeginSnapshot) and is sized to the region table.
	if (!IsRegionSnapshotted(RegionSlot))
	{
		return;
	}
	Regions[RegionSlot].Grid.Add(Pos, Vel, NetId);
}

void UCrowdSubsystem::EndSnapshot()
{
	for (int32 Slot = 0; Slot < Regions.Num(); ++Slot)
	{
		if (RegionSnapshotMask.IsValidIndex(Slot) && RegionSnapshotMask[Slot] != 0)
		{
			Regions[Slot].Grid.EndBuild();
		}
	}
	++SnapshotSerial;
	bSnapshotDue = false;
}

int32 UCrowdSubsystem::GetNumSnapshotRegions() const
{
	int32 Count = 0;
	for (const uint8 Flag : RegionSnapshotMask)
	{
		Count += (Flag != 0) ? 1 : 0;
	}
	return Count;
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

FString UCrowdSubsystem::GetDebugSummary() const
{
	return FString::Printf(TEXT("crowd: regions spawning=%d despawning=%d | queues spawn=%d despawn=%d | snapshot regions=%d | bot ops pending=%d"),
		GetNumRegionsInState(ECrowdRegionState::Spawning), GetNumRegionsInState(ECrowdRegionState::Despawning),
		SpawnQueue.Num(), DespawnQueue.Num(), GetNumSnapshotRegions(), GetNumPendingBotOps());
}

void UCrowdSubsystem::DumpStats() const
{
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] viewers=%d regions(total=%d active=%d spawning=%d despawning=%d inactive=%d) alive=%d"),
		Viewers.Num(), Regions.Num(),
		GetNumRegionsInState(ECrowdRegionState::Active), GetNumRegionsInState(ECrowdRegionState::Spawning),
		GetNumRegionsInState(ECrowdRegionState::Despawning), GetNumRegionsInState(ECrowdRegionState::Inactive),
		AliveAgents);
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] LOD tiers: High=%d Medium=%d Low=%d Off=%d | spawnQueue=%d despawnQueue=%d snapshotSerial=%u"),
		TierCounts[0], TierCounts[1], TierCounts[2], TierCounts[3], SpawnQueue.Num(), DespawnQueue.Num(), SnapshotSerial);
	UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *GetDebugSummary());
	if (const UMassBubbleStreamingMonitor* StreamMonitor = Monitor.Get())
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Crowd] %s"), *StreamMonitor->GetDebugSummary());
	}
}
