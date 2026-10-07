#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// One set of profiling stats, used by three tools:
//   * "stat OptCrowd"        -> in-game numbers (Development builds)
//   * Unreal Insights        -> TRACE_CPUPROFILER_EVENT_SCOPE_STR names below (-trace=cpu,net,frame)
//   * CSV Profiler           -> "csvprofile start / stop", category OptCrowd (also works in Test builds)
DECLARE_STATS_GROUP(TEXT("Opt Crowd"), STATGROUP_OptCrowd, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Director Tick"),       STAT_OptCrowd_Director, STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Region Spawn Slice"),  STAT_OptCrowd_Spawn,    STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Region Despawn Slice"),STAT_OptCrowd_Despawn,  STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("LOD Processor"),       STAT_OptCrowd_LOD,      STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Movement Processor"),  STAT_OptCrowd_Move,     STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Snapshot Processor"),  STAT_OptCrowd_Snapshot, STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Bubble Update"),       STAT_OptCrowd_Bubble,   STATGROUP_OptCrowd, MASSBUBBLE_API);

// Accumulators keep their value (set with SET_DWORD_STAT); counters are reset every frame (add with INC_DWORD_STAT_BY).
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Agents Alive"),            STAT_OptCrowd_Agents,      STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Active Regions"),          STAT_OptCrowd_Regions,     STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Agents Simulated / frame"),    STAT_OptCrowd_Simulated,   STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bubble Items (all players)"),  STAT_OptCrowd_BubbleItems, STATGROUP_OptCrowd, MASSBUBBLE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bubble Dirty Items"),          STAT_OptCrowd_BubbleDirty, STATGROUP_OptCrowd, MASSBUBBLE_API);

CSV_DECLARE_CATEGORY_EXTERN(OptCrowd);

/** Cycle stat + CSV timing + Insights event in one line. TraceName must be a string literal. */
#define OPT_SCOPE(StatId, CsvName, TraceName) \
	SCOPE_CYCLE_COUNTER(StatId); \
	CSV_SCOPED_TIMING_STAT(OptCrowd, CsvName); \
	TRACE_CPUPROFILER_EVENT_SCOPE_STR(TraceName)
