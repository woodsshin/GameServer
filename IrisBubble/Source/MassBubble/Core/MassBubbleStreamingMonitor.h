#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MassBubbleStreamingMonitor.generated.h"

/**
 * Watches World Partition cell streaming (cells are streaming levels) and garbage collection.
 * Exists in every game / PIE world.
 *
 *   1. Busy cells.   A cell is "busy" while it is Loading, MakingVisible (AddToWorld) or MakingInvisible
 *                    (RemoveFromWorld). UCrowdSubsystem shrinks its spawn / despawn budgets while any cell is busy.
 *   2. Quiet GC.     The engine's forced GC after cell unloads (s.ForceGCAfterLevelStreamedOut) is switched off by
 *                    the profile in MassBubbleRuntimeConfig.cpp. This class runs the GC once no cell is busy
 *                    (opt.stream.QuietGC), or after opt.stream.GCMaxDeferSec at the latest.
 *   3. Hitch log.    With opt.hitch.LogMs > 0, every frame longer than that many ms is logged, together with
 *                      * GC info: duration, time since the previous GC, who started it (our quiet GC or the engine),
 *                      * cell activity during the frame (peak busy cells, state changes, unloads),
 *                      * the crowd state.
 */
UCLASS()
class MASSBUBBLE_API UMassBubbleStreamingMonitor : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Cells of THIS world that are loading, being added to or being removed from the world right now. */
	int32 GetNumBusyCells() const { return NumBusyCells; }
	bool IsBusy() const { return NumBusyCells > 0; }

	/** True when a garbage collection ran within the last Frames frames. */
	bool DidGarbageCollectRecently(uint64 Frames) const;

	FString GetDebugSummary() const;

private:
	void LogHitchIfAny(float DeltaTime);
	void ScheduleQuietGC(double Now);

	/** Weak: a streaming level that dies without a final state change must not keep the world "busy" forever. */
	TSet<TWeakObjectPtr<const UObject>> BusyCells;
	int32 NumBusyCells = 0;

	// ---- cell activity accumulated since the previous tick ----
	int32 StateChangesSinceTick = 0;
	int32 UnloadsSinceTick = 0;
	int32 PeakBusySinceTick = 0;
	// Same counters for the frame that just ended (what the hitch log reports)
	int32 StateChangesLastFrame = 0;
	int32 UnloadsLastFrame = 0;
	int32 PeakBusyLastFrame = 0;

	double LastBusyTime = 0.0;
	/** Since when cells have been unloaded without a GC, < 0 = nothing waiting. */
	double PendingGCSince = -1.0;
	double LastGCTime = 0.0;
	uint64 LastGCFrame = 0;

	// ---- the last garbage collection: who started it and how long the game thread was held ----
	/** Pre-GC delegate: before the GC lock is taken. Pre -> post therefore includes waiting for the lock. */
	double GCStartTime = 0.0;
	double PreviousGCStartTime = 0.0;
	double LastGCDurationSec = 0.0;
	double LastGCIntervalSec = -1.0;
	uint32 NumGCs = 0;
	bool bGCRequestedByUs = false;
	bool bLastGCWasOurs = false;

	double SmoothedFrameSec = 0.0;

	FDelegateHandle StreamingStateHandle;
	FDelegateHandle PreGCHandle;
	FDelegateHandle PostGCHandle;
};
