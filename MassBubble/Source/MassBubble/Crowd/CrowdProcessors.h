#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "CrowdProcessors.generated.h"

/**
 * Execution order inside the PrePhysics Mass phase:
 *
 *     OptCrowd.LOD  ->  OptCrowd.Move  ->  OptCrowd.Snapshot
 *
 * All three run only on server / standalone worlds (ExecutionFlags). Clients never simulate the crowd.
 */
namespace CrowdGroups
{
	static const FName LOD(TEXT("OptCrowd.LOD"));
	static const FName Move(TEXT("OptCrowd.Move"));
	static const FName Snapshot(TEXT("OptCrowd.Snapshot"));
}

/**
 * Classifies every agent into a LOD tier from the distance to the nearest viewer.
 * Only 1/4 of the agents are evaluated per frame (bucketed by NetId), and tiers use hysteresis.
 * Game thread: it reads the viewer list owned by UCrowdSubsystem.
 */
UCLASS()
class MASSBUBBLE_API UCrowdLODProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdLODProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

	/** The first agent is spawned at runtime, so no archetype matches at startup: never prune this processor. */
	virtual bool ShouldAllowQueryBasedPruning(const bool bRuntimeMode) const override { return false; }

private:
	FMassEntityQuery EntityQuery;
};

/**
 * Wander simulation with time slicing: an agent is stepped every N-th frame (N from its LOD tier),
 * using the delta time it accumulated while it was skipped. Runs on worker threads
 * (ParallelForEachEntityChunk) because it touches nothing but its own chunk.
 */
UCLASS()
class MASSBUBBLE_API UCrowdMovementProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdMovementProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;
	virtual bool ShouldAllowQueryBasedPruning(const bool bRuntimeMode) const override { return false; }

private:
	FMassEntityQuery EntityQuery;
};

/**
 * Copies position / velocity / id of every live agent into the per-region grids of UCrowdSubsystem at
 * ReplicationHz. Replication then reads those flat arrays and never touches the entity manager.
 * Game thread, and early-outs (no query run at all) on every frame where no snapshot is due.
 */
UCLASS()
class MASSBUBBLE_API UCrowdSnapshotProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdSnapshotProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;
	virtual bool ShouldAllowQueryBasedPruning(const bool bRuntimeMode) const override { return false; }

private:
	FMassEntityQuery EntityQuery;
};
