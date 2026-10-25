#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdLodTable.h"
#include "Crowd/CrowdPersist.h"
#include "Crowd/CrowdSim.h"
#include "Crowd/CrowdTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdSubsystem.generated.h"

struct FCrowdPlanResult;
class AActor;
class ACrowdRegionReplicator;
class UCrowdPCGPlanner;
class UMassBubbleStreamingMonitor;

enum class ECrowdRegionState : uint8
{
	Inactive,   // no agents, only the persisted compact state
	Planning,   // never populated: waiting for its spawn plan (a PCG graph run), no agents yet
	Spawning,   // agents are being created in slices (budgeted per frame)
	Active,     // fully populated and simulated
	Despawning, // state is being persisted in slices
};

struct FCrowdViewer
{
	FVector2D Location = FVector2D::ZeroVector;
	bool bVirtual = false;
};

/**
 * One population region. The region (not an Actor) owns the NPC state, so World Partition cells can
 * unload and reload without losing anything.
 */
struct FCrowdRegion
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	int32 Slot = INDEX_NONE;
	uint32 Seed = 1;
	ECrowdRegionState State = ECrowdRegionState::Inactive;

	/** World time when the region stopped being wanted by every viewer, or < 0 while wanted. */
	double UnwantedSince = -1.0;

	/**
	 * The simulated agents. While Spawning only the part created so far, while Despawning the part before DespawnCursor is
	 * already persisted and no longer simulated.
	 */
	TArray<FCrowdAgentSim> Agents;

	/** Persisted state. While Spawning it is the restore source, while Despawning it is being filled. */
	TArray<FCrowdPackedAgent> Saved;

	/** World time when the region became Inactive, or < 0 (see UCrowdSettings::PersistTTLSec). */
	double InactiveSince = -1.0;

	/**
	 * True once a spawn plan (PCG) decided the population of this region. From then on Saved is the truth, even when it
	 * is empty: a region the graph left empty stays empty instead of being filled randomly the next time it is wanted.
	 */
	bool bPlanned = false;

	int32 SpawnCursor = 0;
	int32 SpawnTarget = 0;
	int32 DespawnCursor = 0;

	/** The replicated state of this region. Null while no real player is near. Authority only. */
	TWeakObjectPtr<ACrowdRegionReplicator> Replicator;

	/** A real player's replication reach covers the region (EvaluateReplication). */
	bool bObserved = false;
	bool bReplicatorQueued = false;

	/** World time when the region left every player's reach while it still has a replicator, or < 0. */
	double ReplicatorUnwantedSince = -1.0;
};

/** Memory held by the region table. Dead = allocated but not needed by the region's current state. */
struct FCrowdMemoryStats
{
	int32 NumRegions = 0;
	int32 NumPersistedAgents = 0;
	int32 NumEvictedRegions = 0;
	int32 NumReplicators = 0;
	SIZE_T RegionTableBytes = 0;
	SIZE_T SavedUsedBytes = 0;
	SIZE_T SavedAllocatedBytes = 0;
	SIZE_T AgentsUsedBytes = 0;
	SIZE_T AgentsAllocatedBytes = 0;
	SIZE_T DeadBytes = 0;
};

/**
 * Server-side crowd state: viewers, the region table with the simulated agents, spawn / despawn scheduling and the
 * replicators that carry the state of the regions to the players.
 *
 * Each frame (game thread drives, worker threads simulate):
 *   regions wanted by viewers -> spawn / despawn slices -> LOD tiers -> movement -> publish to the region replicators.
 * The replication system (Iris) takes it from there: which player receives which region, in which order and how often is
 * decided by its spatial filter and prioritizer (see Net/CrowdRegionReplicator.h).
 */
UCLASS()
class MASSBUBBLE_API UCrowdSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// ---- USubsystem / UWorldSubsystem / FTickableGameObject ----
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// ---- Viewers ----
	void RegisterVirtualViewer(AActor* Actor);
	void UnregisterVirtualViewer(AActor* Actor);
	const TArray<FCrowdViewer>& GetViewers() const { return Viewers; }

	// ---- Load-test bots ----
	// Bots are created / removed one per opt.crowd.BotRampSec, so the engine never has to load
	// (or unload and garbage collect) dozens of cells in a single frame.
	/** Schedules Count more bots (AMassBubbleStreamingAnchor::SpawnBot, one per interval). */
	void RequestBots(int32 Count);
	/** Schedules the removal of every runtime bot, one per interval. Cancels bots that were not spawned yet. */
	void RequestClearBots();
	int32 GetNumPendingBotOps() const;

	// ---- Stats / debugging ----
	void SetTierCounts(const int32 (&InCounts)[CrowdLODCount]);
	int32 GetTierCount(ECrowdLOD Tier) const { return TierCounts[static_cast<int32>(Tier)]; }
	int32 GetNumAliveAgents() const { return AliveAgents; }
	int32 GetNumRegionsInState(ECrowdRegionState State) const;
	int32 GetNumReplicators() const;
	void DumpStats() const;

	FCrowdMemoryStats GetMemoryStats() const;
	FString GetMemorySummary() const;

	/** One line: region states, queues, replicators, pending bot operations. Used by the hitch log. */
	FString GetDebugSummary() const;

	/** One line about the PCG spawn planner: graph, queue, runs in flight, results, latency. */
	FString GetPlannerSummary() const;

	/**
	 * Forgets the planned / persisted population of every region that is not active, so that the next visit generates it
	 * again (opt.crowd.PCG.Replan, after editing the PCG graph). Returns how many regions were reset.
	 */
	int32 ForgetInactivePlans();

private:
	/** Tier counts of one region, summed after the parallel pass. */
	struct FTierCounts
	{
		int32 Count[CrowdLODCount] = { 0, 0, 0, 0 };
	};

	double GetNow() const;

	// ---- Region scheduling ----
	void RefreshViewers();
	void EvaluateRegions(double Now);
	int32 FindOrAddRegion(const FIntPoint& Coord);
	void BeginSpawn(FCrowdRegion& Region);
	void StartSpawning(FCrowdRegion& Region);
	void BeginPlan(FCrowdRegion& Region);
	void CancelPlan(FCrowdRegion& Region);
	void PumpPlans();
	void ApplyPlan(FCrowdRegion& Region, FCrowdPlanResult& Result);
	bool CanPlanWithPCG() const;
	void BeginDespawn(FCrowdRegion& Region);
	int32 SpawnSlice(FCrowdRegion& Region, int32 Budget);
	int32 DespawnSlice(FCrowdRegion& Region, int32 Budget);
	void PumpRegionJobs();
	void ForgetRegion(FCrowdRegion& Region);
	int32 EvictStalePersistence(double Now);
	void TickBotRamp(float DeltaSeconds);
	FCrowdAgentSim MakeAgent(const FCrowdRegion& Region, int32 Index);

	// ---- Simulation ----
	void CollectSimulatedRegions();
	void BuildLodTable(const FCrowdTuning& Tuning);
	void UpdateLod();
	void MoveAgents(float DeltaSeconds);
	void RunPerRegion(int32 Num, TFunctionRef<void(int32)> Body) const;

	// ---- Replication ----
	void EvaluateReplication(double Now);
	void PumpReplicators();
	void CreateReplicator(FCrowdRegion& Region);
	void DestroyReplicator(FCrowdRegion& Region);
	void PublishRegions(float DeltaSeconds);

	// ---- Stats ----
	void PublishMemoryStats();

	TArray<FCrowdViewer> Viewers;
	TArray<TWeakObjectPtr<AActor>> VirtualViewers;

	TArray<FCrowdRegion> Regions;
	TMap<FIntPoint, int32> RegionSlotByCoord;
	TSet<FIntPoint> WantedScratch;
	TArray<int32> SpawnQueue;
	TArray<int32> DespawnQueue;
	TArray<int32> ReplicatorQueue;

	/** Reused every frame: no allocation after warm-up. */
	TArray<int32> SimulatedScratch;
	TArray<FTierCounts> TierScratch;
	TArray<CrowdSim::FMoveStats> MoveScratch;
	TArray<FCrowdSavedAgent> PlanScratch;

	/** Rebuilt every frame the LOD table is used. */
	CrowdLod::FTable LodTable;
	TArray<FVector2D> LodViewers;

	/** Tells us whether World Partition is busy (spawn / despawn budgets shrink then). Null in commandlets. */
	TWeakObjectPtr<UMassBubbleStreamingMonitor> Monitor;

	/** Answers "where do the agents of a new region start?" with a PCG graph. Null when no graph is configured. */
	UPROPERTY(Transient)
	TObjectPtr<UCrowdPCGPlanner> Planner;

	struct FBotJob
	{
		int32 Total = 0;
		int32 Next = 0;
		bool bClear = false;
	};
	TArray<FBotJob> BotJobs;
	float BotRampTimer = 0.f;

	uint32 NextNetId = 1;
	int32 AliveAgents = 0;
	int32 TierCounts[CrowdLODCount] = { 0, 0, 0, 0 };

	float RegionEvalAccumulator = 0.f;
	float PublishAccumulator = 0.f;
	float MemorySampleAccumulator = 0.f;
	int32 NumEvictedRegions = 0;
};
