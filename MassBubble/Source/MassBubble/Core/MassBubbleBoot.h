#pragma once

#include "CoreMinimal.h"

/**
 * Startup timeline: named milestones with the time since the first one (module startup). Every milestone is also an
 * Unreal Insights bookmark and a CSV profiler event, so the same names appear in both tools.
 *
 *   opt.boot.Report   the timeline
 *   opt.boot.IO       file system layers, async loading, IoStore and the loading related console variables
 *   opt.boot.Modules  how many modules the process loaded (opt.boot.Modules 1 lists them)
 *   opt.boot.Reset    forgets the milestones, e.g. between two PIE sessions
 */
namespace MassBubbleBoot
{
	MASSBUBBLE_API void Mark(const TCHAR* Name);

	/** Same as Mark, ignored when the milestone is already on the timeline. */
	MASSBUBBLE_API void MarkOnce(const TCHAR* Name);

	/** Changes on Reset(): call sites that record a milestone once use it to record it again. */
	MASSBUBBLE_API uint32 GetGeneration();

	/** One recorded milestone: seconds since the first milestone (module startup). */
	struct FMilestoneInfo
	{
		FString Name;
		double Seconds = 0.0;
	};

	/** Copy of the timeline in the order the milestones were recorded (the server diet report writes it out). */
	MASSBUBBLE_API void GetTimeline(TArray<FMilestoneInfo>& OutMilestones);

	/** Seconds from process start (GStartTime) to the first milestone: add it to a milestone's Seconds for time since process start. */
	MASSBUBBLE_API double GetProcessOffsetSec();

	MASSBUBBLE_API void Reset();
	MASSBUBBLE_API FString GetReport();
	MASSBUBBLE_API void LogIoDiagnostics();
	MASSBUBBLE_API void LogModules(bool bList);
}

/** Records the milestone once per timeline; cheap after the first call. Name must be a string literal. */
#define OPT_BOOT_ONCE(Name) \
	do \
	{ \
		static uint32 OptBootGeneration = 0xFFFFFFFFu; \
		if (OptBootGeneration != MassBubbleBoot::GetGeneration()) \
		{ \
			OptBootGeneration = MassBubbleBoot::GetGeneration(); \
			MassBubbleBoot::MarkOnce(TEXT(Name)); \
		} \
	} while (0)
