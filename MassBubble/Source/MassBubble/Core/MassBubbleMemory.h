#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"

/**
 * Memory tooling of the crowd.
 *   * LLM tags (-llm, Memory Insights): the crowd's allocations are listed under OptCrowd_* instead of "untracked".
 *   * Allocator and process memory for the CSV profiler: metadata "Allocator", stats SavedKB / DeadKB / ProcessMB.
 *   * Console (non-shipping): opt.mem.Info, opt.mem.Bench, opt.mem.Trim.
 * Define MASSBUBBLE_LLM as 0 to compile the tags out (the LLM macros differ between engine versions).
 */

#ifndef MASSBUBBLE_LLM
#define MASSBUBBLE_LLM ENABLE_LOW_LEVEL_MEM_TRACKER
#endif

#if MASSBUBBLE_LLM
#define OPT_LLM_DECLARE_TAG(Tag) LLM_DECLARE_TAG(Tag)
#define OPT_LLM_SCOPE(Tag) LLM_SCOPE_BYTAG(Tag)
#else
#define OPT_LLM_DECLARE_TAG(Tag)
#define OPT_LLM_SCOPE(Tag)
#endif

OPT_LLM_DECLARE_TAG(OptCrowd_Entities);
OPT_LLM_DECLARE_TAG(OptCrowd_Persist);
OPT_LLM_DECLARE_TAG(OptCrowd_Grids);
OPT_LLM_DECLARE_TAG(OptCrowd_Bubble);

namespace MassBubbleMem
{
	/** Physical memory used by this process, in bytes. */
	MASSBUBBLE_API uint64 GetProcessUsedBytes();

	/** Writes the active allocator into the CSV profiler metadata. */
	MASSBUBBLE_API void PublishCsvMetadata();

	/** Allocator name, process memory and the allocator's own statistics. */
	MASSBUBBLE_API void LogInfo();

	/** Times alloc / free of the crowd's typical block sizes, single threaded and on several threads at once. */
	MASSBUBBLE_API void RunBench();

	/** FMemory::Trim, with the time it took and the physical memory it returned. */
	MASSBUBBLE_API void Trim();
}
