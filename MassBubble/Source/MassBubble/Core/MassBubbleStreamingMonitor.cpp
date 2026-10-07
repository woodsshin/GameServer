#include "Core/MassBubbleStreamingMonitor.h"

#include "Core/MassBubbleRuntimeConfig.h"
#include "Core/MassBubbleStats.h"
#include "Crowd/CrowdSubsystem.h"
#include "MassBubble.h"

#include "Engine/Engine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "UObject/UObjectGlobals.h"

bool UMassBubbleStreamingMonitor::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningCommandlet();
}

bool UMassBubbleStreamingMonitor::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UMassBubbleStreamingMonitor::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// First world of the process: spread out the engine's own level streaming work (see MassBubbleRuntimeConfig.cpp).
	MassBubbleConfig::ApplyStreamingProfileOnce();

	const double Now = FPlatformTime::Seconds();
	LastBusyTime = Now;
	LastGCTime = Now;

	// The delegate is global (all worlds): events of other worlds (PIE server + clients in one process) are ignored.
	StreamingStateHandle = FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddWeakLambda(this,
		[this](UWorld* InWorld, const ULevelStreaming* InStreamingLevel, ULevel* /*InLevelIfLoaded*/,
			ELevelStreamingState InPreviousState, ELevelStreamingState InNewState)
		{
			if (InWorld != GetWorld() || InStreamingLevel == nullptr)
			{
				return;
			}

			const TWeakObjectPtr<const UObject> Key(static_cast<const UObject*>(InStreamingLevel));
			const bool bBusyNow = InNewState == ELevelStreamingState::Loading
				|| InNewState == ELevelStreamingState::MakingVisible
				|| InNewState == ELevelStreamingState::MakingInvisible;

			if (bBusyNow)
			{
				BusyCells.Add(Key);
				LastBusyTime = FPlatformTime::Seconds();
			}
			else
			{
				BusyCells.Remove(Key);
			}
			NumBusyCells = BusyCells.Num();

			// What happened since the last tick. The hitch log reports it for the frame that just ended, because a cell
			// that finished inside a slow frame is no longer "busy" by the time the log is written.
			++StateChangesSinceTick;
			PeakBusySinceTick = FMath::Max(PeakBusySinceTick, NumBusyCells);

			// A cell that went away leaves garbage behind: remember since when, ScheduleQuietGC picks the moment.
			// (A level that is newly registered can report PreviousState == NewState, that is not an unload.)
			const bool bGone = InNewState == ELevelStreamingState::Unloaded || InNewState == ELevelStreamingState::Removed;
			if (bGone && InPreviousState != InNewState)
			{
				++UnloadsSinceTick;
				if (PendingGCSince < 0.0)
				{
					PendingGCSince = FPlatformTime::Seconds();
				}
			}
		});

	// "Before the GC lock is acquired": the time from here to the post delegate is what the game thread is held for
	// (waiting for the lock + reachability analysis, plus the purge when it is not incremental).
	PreGCHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddWeakLambda(this, [this]()
	{
		GCStartTime = FPlatformTime::Seconds();
	});

	PostGCHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddWeakLambda(this, [this]()
	{
		const double PostTime = FPlatformTime::Seconds();
		LastGCDurationSec = (GCStartTime > 0.0) ? (PostTime - GCStartTime) : 0.0;
		LastGCIntervalSec = (PreviousGCStartTime > 0.0 && GCStartTime > 0.0) ? (GCStartTime - PreviousGCStartTime) : -1.0;
		PreviousGCStartTime = GCStartTime;

		bLastGCWasOurs = bGCRequestedByUs;
		bGCRequestedByUs = false;
		++NumGCs;

		LastGCFrame = GFrameCounter;
		LastGCTime = PostTime;
		PendingGCSince = -1.0; // whatever was waiting for a GC has been handled now
	});
}

void UMassBubbleStreamingMonitor::Deinitialize()
{
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(StreamingStateHandle);
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGCHandle);
	FCoreUObjectDelegates::GetPostGarbageCollect().Remove(PostGCHandle);
	StreamingStateHandle.Reset();
	PreGCHandle.Reset();
	PostGCHandle.Reset();

	BusyCells.Reset();
	NumBusyCells = 0;

	Super::Deinitialize();
}

TStatId UMassBubbleStreamingMonitor::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMassBubbleStreamingMonitor, STATGROUP_Tickables);
}

void UMassBubbleStreamingMonitor::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	// Forget cells whose streaming object died without reporting a final state.
	for (auto It = BusyCells.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	NumBusyCells = BusyCells.Num();
	if (NumBusyCells > 0)
	{
		LastBusyTime = Now;
	}

	// What the cells did during the frame that just ended, then start counting the next one.
	StateChangesLastFrame = StateChangesSinceTick;
	UnloadsLastFrame = UnloadsSinceTick;
	PeakBusyLastFrame = FMath::Max(PeakBusySinceTick, NumBusyCells);
	StateChangesSinceTick = 0;
	UnloadsSinceTick = 0;
	PeakBusySinceTick = NumBusyCells;

	LogHitchIfAny(DeltaTime);
	ScheduleQuietGC(Now);

	CSV_CUSTOM_STAT(OptCrowd, StreamingBusyCells, NumBusyCells, ECsvCustomStatOp::Set);
}

void UMassBubbleStreamingMonitor::LogHitchIfAny(float DeltaTime)
{
	const double Frame = static_cast<double>(DeltaTime);

	// Slow average: tells "everything is slow" apart from "one spike".
	SmoothedFrameSec = (SmoothedFrameSec <= 0.0) ? Frame : FMath::Lerp(SmoothedFrameSec, Frame, 0.02);

	const double ThresholdSec = 0.001 * static_cast<double>(MassBubbleStreamCVars::HitchLogMs);
	if (ThresholdSec <= 0.0 || Frame < ThresholdSec)
	{
		return;
	}

	FString CrowdInfo(TEXT("no crowd subsystem in this world"));
	if (const UWorld* World = GetWorld())
	{
		if (const UCrowdSubsystem* Crowd = World->GetSubsystem<UCrowdSubsystem>())
		{
			CrowdInfo = Crowd->GetDebugSummary();
		}
	}

	// The delta that arrives here is the length of the PREVIOUS frame, i.e. the hitch that just happened.
	UE_LOG(LogMassBubble, Warning, TEXT("[Hitch] previous frame %.1f ms (average %.1f ms) | GC in the last 2 frames: %s | %s | %s"),
		Frame * 1000.0, SmoothedFrameSec * 1000.0,
		DidGarbageCollectRecently(2) ? TEXT("YES") : TEXT("no"),
		*GetDebugSummary(), *CrowdInfo);
}

void UMassBubbleStreamingMonitor::ScheduleQuietGC(double Now)
{
	if (MassBubbleStreamCVars::QuietGC == 0 || PendingGCSince < 0.0 || GEngine == nullptr)
	{
		return;
	}

	const bool bQuiet = NumBusyCells == 0 && (Now - LastBusyTime) >= static_cast<double>(MassBubbleStreamCVars::GCQuietSec);
	const bool bOverdue = (Now - PendingGCSince) >= static_cast<double>(MassBubbleStreamCVars::GCMaxDeferSec);
	const bool bSpaced = (Now - LastGCTime) >= static_cast<double>(MassBubbleStreamCVars::GCMinSpacingSec);

	if (bSpaced && (bQuiet || bOverdue))
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Stream] GC after cell unload: %s (waited %.1f s)"),
			bQuiet ? TEXT("World Partition is quiet") : TEXT("waited long enough"), Now - PendingGCSince);

		// Remembered so the hitch log can say "started by: our quiet GC" for the GC that follows.
		bGCRequestedByUs = true;

		// false = normal GC, the purge stays incremental. A full purge would make the freeze longer, not shorter.
		GEngine->ForceGarbageCollection(/*bForcePurge=*/false);
		PendingGCSince = -1.0;
		LastGCTime = Now; // do not ask again before the GC has actually run
	}
}

bool UMassBubbleStreamingMonitor::DidGarbageCollectRecently(uint64 Frames) const
{
	return LastGCFrame != 0 && GFrameCounter >= LastGCFrame && (GFrameCounter - LastGCFrame) <= Frames;
}

FString UMassBubbleStreamingMonitor::GetDebugSummary() const
{
	FString GC;
	if (NumGCs == 0)
	{
		GC = TEXT("no GC seen yet");
	}
	else
	{
		GC = FString::Printf(TEXT("last GC held the game thread %.1f ms (lock wait + reachability analysis), started %s after the previous one, started by: %s, GCs seen: %u, %llu frames ago"),
			LastGCDurationSec * 1000.0,
			LastGCIntervalSec >= 0.0 ? *FString::Printf(TEXT("%.1f s"), LastGCIntervalSec) : TEXT("n/a"),
			bLastGCWasOurs ? TEXT("our quiet GC") : TEXT("engine / other"),
			NumGCs, GFrameCounter - LastGCFrame);
	}

	return FString::Printf(TEXT("world partition: cells busy now=%d, peak during the last frame=%d, state changes=%d, unloads=%d | %s | GC waiting after unload: %s"),
		NumBusyCells, PeakBusyLastFrame, StateChangesLastFrame, UnloadsLastFrame, *GC,
		PendingGCSince >= 0.0 ? TEXT("yes") : TEXT("no"));
}
