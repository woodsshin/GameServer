#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdPlan.h"
#include "Engine/StreamableManager.h"
#include "UObject/Object.h"
#include "CrowdPCGPlanner.generated.h"

class ACrowdPCGHost;

/**
 * Answers "where do the agents of this region start?" with the output of a PCG graph.
 *
 * PCG is the generator, not the scheduler. UCrowdSubsystem still decides WHEN a region is populated (viewers, hysteresis,
 * budgets, persistence); when a region that was never populated becomes wanted it calls Request() and the region waits
 * in the Planning state. Each frame the subsystem calls Pump() and takes the finished plans with PopResult().
 *
 *   * One graph run per region: the graph runs on a transient ACrowdPCGHost whose bounds are the region, with the region
 *     seed as PCG seed. Same region + same graph + same world data = same points.
 *   * At most PCGMaxConcurrent runs are in flight; the rest waits in the queue in request order (the subsystem requests the
 *     regions closest to a viewer first).
 *   * The graph is loaded in the background. Requests made before it is ready simply wait.
 *   * A run that does not finish within PCGTimeoutSec, or fails, is answered with a failed result: the subsystem then
 *     populates the region randomly (or leaves it empty, bPCGFallbackToRandom).
 *
 * Everything here runs on the game thread. The graph must end in point data on its Output node (see the settings).
 */
UCLASS()
class MASSBUBBLE_API UCrowdPCGPlanner : public UObject
{
	GENERATED_BODY()

private:
	enum class EState : uint8
	{
		Disabled, // no graph configured
		Loading,  // graph is being loaded in the background
		Ready,
		Failed,   // the graph could not be loaded: every request is answered with a failure
	};

	struct FJob
	{
		FCrowdPlanRequest Request;
		TWeakObjectPtr<ACrowdPCGHost> Host;
		double QueuedAt = 0.0;
		double StartedAt = 0.0;
	};

public:
	/**
	 * Reads UCrowdSettings (category "PCG") and starts loading the spawn graph. Returns false if there is nothing to run
	 * (no graph configured, or it cannot be requested): the caller then populates regions without PCG.
	 */
	bool Initialize();

	/** Drops every reference. The host actors of runs in flight die with the world. */
	void Shutdown();

	/** A graph is configured: it is loading or ready. Requests made now are accepted and wait for it. */
	bool IsEnabled() const { return State == EState::Loading || State == EState::Ready; }
	bool IsReady() const { return State == EState::Ready; }

	/** Queues the plan of one region (ignored if that region is already queued or running). */
	void Request(const FCrowdPlanRequest& InRequest);

	/** The region is not wanted any more: forgets its plan and stops its graph run. */
	void Cancel(const FIntPoint& Coord);

	/** Starts queued runs (up to the concurrency limit) and collects finished ones. Once per frame. */
	void Pump(double Now);

	/** Takes the next finished plan, if there is one. */
	bool PopResult(FCrowdPlanResult& OutResult);

	int32 GetNumQueued() const { return Queue.Num(); }
	int32 GetNumRunning() const { return Running.Num(); }
	int32 GetNumPending() const { return Queue.Num() + Running.Num() + Results.Num(); }

	FString GetDebugSummary() const;

	/** Cumulative counters since the world started (CSV: OptCrowd/PlansOk, PlansFailed, PlanLatency*Ms). */
	int32 GetNumOk() const { return NumOk; }
	int32 GetNumFailed() const { return NumFailed + NumTimedOut; }
	double GetAvgLatencySec() const { return NumOk > 0 ? TotalLatencySec / static_cast<double>(NumOk) : 0.0; }
	double GetMaxLatencySec() const { return MaxLatencySec; }

private:
	void PollGraphLoad();
	bool StartJob(FJob& Job, double Now);
	void PushResult(const FJob& Job, ECrowdPlanStatus Status, TArray<FCrowdPlanPoint>&& Points, double Now);

	EState State = EState::Disabled;
	FSoftObjectPath GraphPath;

	/** Keeps the graph asset loaded while the background request runs; the loaded graph itself is held by Graph. */
	FStreamableManager StreamableManager;
	TSharedPtr<FStreamableHandle> GraphHandle;

	/** The spawn graph (a UPCGGraphInterface; typed as UObject so that this header needs no PCG include). */
	UPROPERTY(Transient)
	TObjectPtr<UObject> Graph;

	TArray<FJob> Queue;
	TArray<FJob> Running;
	TArray<FCrowdPlanResult> Results;

	// ---- statistics (opt.crowd.PCG.Stats) ----
	int32 NumRequested = 0;
	int32 NumOk = 0;
	int32 NumFailed = 0;
	int32 NumTimedOut = 0;
	int32 NumCancelled = 0;
	double TotalLatencySec = 0.0;
	double MaxLatencySec = 0.0;
	bool bLoggedFirstOk = false;
};
