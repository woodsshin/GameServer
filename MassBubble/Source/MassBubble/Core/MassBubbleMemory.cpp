#include "Core/MassBubbleMemory.h"

#include "MassBubble.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/MemoryBase.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"
#include "Misc/OutputDeviceRedirector.h"
#include "ProfilingDebugging/CsvProfiler.h"

#if MASSBUBBLE_LLM
LLM_DEFINE_TAG(OptCrowd_Entities);
LLM_DEFINE_TAG(OptCrowd_Persist);
LLM_DEFINE_TAG(OptCrowd_Grids);
LLM_DEFINE_TAG(OptCrowd_Bubble);
#endif

uint64 MassBubbleMem::GetProcessUsedBytes()
{
	return FPlatformMemory::GetStats().UsedPhysical;
}

void MassBubbleMem::PublishCsvMetadata()
{
#if defined(CSV_METADATA)
	CSV_METADATA(TEXT("Allocator"), FPlatformMemory::BaseAllocator()->GetDescriptiveName());
#endif
}

void MassBubbleMem::LogInfo()
{
	const double MB = 1024.0 * 1024.0;
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();

	UE_LOG(LogMassBubble, Log, TEXT("[Mem] allocator: %s"), FPlatformMemory::BaseAllocator()->GetDescriptiveName());
	UE_LOG(LogMassBubble, Log, TEXT("[Mem] process: physical %.1f MB used (peak %.1f MB), virtual %.1f MB used (peak %.1f MB), %.1f MB physical available"),
		static_cast<double>(Stats.UsedPhysical) / MB, static_cast<double>(Stats.PeakUsedPhysical) / MB,
		static_cast<double>(Stats.UsedVirtual) / MB, static_cast<double>(Stats.PeakUsedVirtual) / MB,
		static_cast<double>(Stats.AvailablePhysical) / MB);
	UE_LOG(LogMassBubble, Log, TEXT("[Mem] the allocator is a command line choice (-binnedmalloc2, -binnedmalloc3, -mimalloc, -ansimalloc, depending on platform and engine version). More detail: memreport -full, -llm with Memory Insights."));

	FPlatformMemory::BaseAllocator()->DumpAllocatorStats(*GLog);
}

namespace
{
	struct FBenchScenario
	{
		const TCHAR* Name;
		SIZE_T Bytes;
		int32 Batch;
		int32 Rounds;
	};

	const FBenchScenario GScenarios[] =
	{
		{ TEXT("entity slice scratch, 512 B"), 512, 256, 4000 },
		{ TEXT("bubble item array, 4 KB"), 4096, 128, 2000 },
		{ TEXT("saved agents of a region, 14.4 KB"), 14400, 64, 600 },
	};

	/**
	 * ns per allocation + free. Blocks are allocated in a batch and freed in a strided order, like the arrays that
	 * live and die with their regions.
	 */
	double TimeBatches(SIZE_T Bytes, int32 Batch, int32 Rounds)
	{
		TArray<void*> Blocks;
		Blocks.Init(nullptr, Batch);

		const double Start = FPlatformTime::Seconds();
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			for (int32 i = 0; i < Batch; ++i)
			{
				Blocks[i] = FMemory::Malloc(Bytes);
				*static_cast<volatile uint8*>(Blocks[i]) = static_cast<uint8>(i); // the block is really used
			}
			for (int32 Phase = 0; Phase < 4; ++Phase)
			{
				for (int32 i = Phase; i < Batch; i += 4)
				{
					FMemory::Free(Blocks[i]);
				}
			}
		}
		const double Seconds = FPlatformTime::Seconds() - Start;
		return Seconds * 1.0e9 / (static_cast<double>(Batch) * static_cast<double>(Rounds));
	}
}

void MassBubbleMem::RunBench()
{
	const int32 NumThreads = FMath::Clamp(FPlatformMisc::NumberOfWorkerThreadsToSpawn(), 2, 8);
	UE_LOG(LogMassBubble, Log, TEXT("[Mem] bench on %s, %d threads for the contended runs"), FPlatformMemory::BaseAllocator()->GetDescriptiveName(), NumThreads);

	for (const FBenchScenario& Scenario : GScenarios)
	{
		const double Single = TimeBatches(Scenario.Bytes, Scenario.Batch, Scenario.Rounds);

		TArray<TFuture<double>> Futures;
		for (int32 Thread = 0; Thread < NumThreads; ++Thread)
		{
			Futures.Add(Async(EAsyncExecution::ThreadPool, [&Scenario]()
			{
				return TimeBatches(Scenario.Bytes, Scenario.Batch, Scenario.Rounds);
			}));
		}

		double Sum = 0.0;
		for (TFuture<double>& Future : Futures)
		{
			Sum += Future.Get();
		}
		const double Contended = Sum / static_cast<double>(NumThreads);

		UE_LOG(LogMassBubble, Log, TEXT("[Mem]   %-36s single thread %6.1f ns | %d threads %6.1f ns each (%.2fx)"),
			Scenario.Name, Single, NumThreads, Contended, Contended / FMath::Max(Single, 1.0e-3));
	}
}

void MassBubbleMem::Trim()
{
	const double MB = 1024.0 * 1024.0;
	const double Before = static_cast<double>(GetProcessUsedBytes()) / MB;
	const double Start = FPlatformTime::Seconds();

	FMemory::Trim();

	const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
	const double After = static_cast<double>(GetProcessUsedBytes()) / MB;
	UE_LOG(LogMassBubble, Log, TEXT("[Mem] FMemory::Trim took %.1f ms, physical memory %.1f MB -> %.1f MB"), Ms, Before, After);
}

#if !UE_BUILD_SHIPPING

namespace
{
	FAutoConsoleCommand GCmdMemInfo(
		TEXT("opt.mem.Info"),
		TEXT("Log the active allocator, process memory and the allocator's own statistics."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleMem::LogInfo));

	FAutoConsoleCommand GCmdMemBench(
		TEXT("opt.mem.Bench"),
		TEXT("Time alloc / free of the crowd's block sizes on the active allocator, single threaded and contended. Blocks the game thread while it runs."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleMem::RunBench));

	FAutoConsoleCommand GCmdMemTrim(
		TEXT("opt.mem.Trim"),
		TEXT("Call FMemory::Trim and log how long it took and how much physical memory it returned."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleMem::Trim));
}

#endif // !UE_BUILD_SHIPPING
