#pragma once

#include "CoreMinimal.h"
#include "MassSubsystemBase.h"
#include "MassEntityTypes.h"
#include "MassEntitySubsystem.h" // provides FMassEntityManager / FMassArchetypeHandle
#include "Crowd/CrowdTypes.h"
#include "Crowd/CrowdCellGrid.h"
#include "CrowdSubsystem.generated.h"

class AActor;
class UMassBubbleStreamingMonitor;

enum class ECrowdRegionState : uint8
{
	Inactive,   // no entities, only the persisted compact state
	Spawning,   // entities are being created in slices (budgeted per frame)
	Active,     // fully populated and simulated
	Despawning, // state is being persisted and entities destroyed in slices
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

	TArray<FMassEntityHandle> Entities;
	/** Persisted state. While Spawning it is the restore source, while Despawning it is being filled. */
	TArray<FCrowdSavedAgent> Saved;

	int32 SpawnCursor = 0;
	int32 SpawnTarget = 0;
	int32 DespawnCursor = 0;

	/** Replication snapshot of this region (rebuilt at ReplicationHz by UCrowdSnapshotProcessor). */
	FCrowdCellGrid Grid;
};

/**
 * Server-side crowd state that lives outside of the entity manager:
 * viewers, region table, spawn / despawn scheduling, and the spatial snapshot used by replication.
 *
 * Derives from UMassSubsystemBase so Mass processors can declare it as a requirement. All access happens on the
 * game thread (see the traits specialisation below).
 */
UCLASS()
class MASSBUBBLE_API UCrowdSubsystem : public UMassSubsystemBase
{
	GENERATED_BODY()

public:
	// ---- USubsystem / UWorldSubsystem ----
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- Driven by UCrowdDirector (game thread) ----
	void TickDirector(float DeltaSeconds);

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

	// ---- Snapshot protocol used by UCrowdSnapshotProcessor ----
	bool IsSnapshotDue() const { return bSnapshotDue; }
	void BeginSnapshot();
	void AddToSnapshot(int32 RegionSlot, const FVector2D& Pos, const FVector2f& Vel, uint32 NetId);
	void EndSnapshot();
	uint32 GetSnapshotSerial() const { return SnapshotSerial; }

	/** True when the region was part of the last snapshot (a real player can see it) and its grid is fresh. */
	bool IsRegionSnapshotted(int32 RegionSlot) const
	{
		return RegionSnapshotMask.IsValidIndex(RegionSlot) && RegionSnapshotMask[RegionSlot] != 0;
	}
	int32 GetNumSnapshotRegions() const;

	/** Calls Fn(const FCrowdGridAgent&, double DistSquared) for each agent in the circle. Game thread only. */
	template <typename FuncType>
	void ForEachAgentInCircle(const FVector2D& Center, double Radius, FuncType&& Fn) const
	{
		const float Size = CachedRegionSizeCm;
		const int32 MinRX = FMath::FloorToInt((Center.X - Radius) / Size);
		const int32 MaxRX = FMath::FloorToInt((Center.X + Radius) / Size);
		const int32 MinRY = FMath::FloorToInt((Center.Y - Radius) / Size);
		const int32 MaxRY = FMath::FloorToInt((Center.Y + Radius) / Size);

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
				// A region outside the snapshot scope has a stale grid: never hand out stale agents.
				if ((Region.State == ECrowdRegionState::Active || Region.State == ECrowdRegionState::Spawning) && IsRegionSnapshotted(*Slot))
				{
					Region.Grid.ForEachInCircle(Center, Radius, Fn);
				}
			}
		}
	}

	// ---- Stats / debugging ----
	void SetTierCounts(const int32 (&InCounts)[CrowdLODCount]);
	int32 GetTierCount(ECrowdLOD Tier) const { return TierCounts[static_cast<int32>(Tier)]; }
	int32 GetNumAliveAgents() const { return AliveAgents; }
	int32 GetNumRegionsInState(ECrowdRegionState State) const;
	void DumpStats() const;

	/** One line: region states, queues, snapshot scope, pending bot operations. Used by the hitch log. */
	FString GetDebugSummary() const;

	/** Forces a snapshot on the next processor run (used by tests and the stats command). */
	void RequestSnapshot() { bSnapshotDue = true; }

private:
	void RefreshViewers();
	void EvaluateRegions(double Now);
	int32 FindOrAddRegion(const FIntPoint& Coord);
	void BeginSpawn(FCrowdRegion& Region);
	void BeginDespawn(FCrowdRegion& Region);
	int32 SpawnSlice(FCrowdRegion& Region, int32 Budget);
	int32 DespawnSlice(FCrowdRegion& Region, int32 Budget);
	void PumpRegionJobs();
	void TickBotRamp(float DeltaSeconds);
	FCrowdSavedAgent MakeAgent(const FCrowdRegion& Region, int32 Index);
	FMassArchetypeHandle GetAgentArchetype();
	double GetNow() const;

	TSharedPtr<FMassEntityManager> EntityManager;
	FMassArchetypeHandle AgentArchetype;

	TArray<FCrowdViewer> Viewers;
	TArray<TWeakObjectPtr<AActor>> VirtualViewers;

	TArray<FCrowdRegion> Regions;
	TMap<FIntPoint, int32> RegionSlotByCoord;
	TSet<FIntPoint> WantedScratch;
	TArray<int32> SpawnQueue;
	TArray<int32> DespawnQueue;

	/** Tells us whether World Partition is busy (spawn / despawn budgets shrink then). Null in commandlets. */
	TWeakObjectPtr<UMassBubbleStreamingMonitor> Monitor;

	struct FBotJob
	{
		int32 Total = 0;
		int32 Next = 0;
		bool bClear = false;
	};
	TArray<FBotJob> BotJobs;
	float BotRampTimer = 0.f;

	/** 1 = the region is part of the current snapshot. Indexed by region slot, rebuilt by BeginSnapshot(). */
	TArray<uint8> RegionSnapshotMask;

	uint32 NextNetId = 1;
	int32 AliveAgents = 0;
	int32 TierCounts[CrowdLODCount] = { 0, 0, 0, 0 };

	float CachedRegionSizeCm = 12800.f;
	float CachedGridCellSizeCm = 1600.f;
	float RegionEvalAccumulator = 0.f;
	float SnapshotAccumulator = 0.f;
	bool bSnapshotDue = false;
	uint32 SnapshotSerial = 0;
};

/** Processors declare this subsystem as a requirement; GameThreadOnly keeps them off worker threads. */
template <>
struct TMassExternalSubsystemTraits<UCrowdSubsystem> final
{
	enum
	{
		GameThreadOnly = true,
		ThreadSafeWrite = false,
	};
};
