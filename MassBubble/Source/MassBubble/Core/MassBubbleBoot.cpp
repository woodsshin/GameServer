#include "Core/MassBubbleBoot.h"

#include "Core/MassBubbleStats.h"
#include "MassBubble.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
	struct FMilestone
	{
		FString Name;
		double Seconds = 0.0;
	};

	FCriticalSection GBootLock;
	TArray<FMilestone> GMilestones;
	double GBootStart = 0.0;
	std::atomic<uint32> GGeneration{ 0 };

	FString BootCVarText(const TCHAR* Name)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var != nullptr ? Var->GetString() : FString(TEXT("<missing>"));
	}
}

void MassBubbleBoot::Mark(const TCHAR* Name)
{
	const double Now = FPlatformTime::Seconds();
	double SinceStart = 0.0;
	{
		FScopeLock Lock(&GBootLock);
		if (GMilestones.Num() == 0)
		{
			GBootStart = Now;
		}
		FMilestone& Milestone = GMilestones.AddDefaulted_GetRef();
		Milestone.Name = Name;
		Milestone.Seconds = Now - GBootStart;
		SinceStart = Milestone.Seconds;
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Boot] +%.1f ms %s"), SinceStart * 1000.0, Name);
	TRACE_BOOKMARK(TEXT("OptCrowd boot: %s"), Name);
	CSV_EVENT(OptCrowd, TEXT("Boot: %s"), Name);
}

void MassBubbleBoot::MarkOnce(const TCHAR* Name)
{
	{
		FScopeLock Lock(&GBootLock);
		for (const FMilestone& Milestone : GMilestones)
		{
			if (Milestone.Name == Name)
			{
				return;
			}
		}
	}
	Mark(Name);
}

uint32 MassBubbleBoot::GetGeneration()
{
	return GGeneration.load(std::memory_order_relaxed);
}

void MassBubbleBoot::Reset()
{
	FScopeLock Lock(&GBootLock);
	GMilestones.Reset();
	GGeneration.fetch_add(1, std::memory_order_relaxed);
}

FString MassBubbleBoot::GetReport()
{
	FScopeLock Lock(&GBootLock);

	FString Report;
	double Previous = 0.0;
	for (const FMilestone& Milestone : GMilestones)
	{
		Report += FString::Printf(TEXT("\n  +%9.1f ms  (%+8.1f ms)  %s"), Milestone.Seconds * 1000.0, (Milestone.Seconds - Previous) * 1000.0, *Milestone.Name);
		Previous = Milestone.Seconds;
	}
	return Report;
}

void MassBubbleBoot::LogIoDiagnostics()
{
	FString Layers;
	int32 Depth = 0;
	for (IPlatformFile* Layer = &FPlatformFileManager::Get().GetPlatformFile(); Layer != nullptr && Depth < 16; Layer = Layer->GetLowerLevel(), ++Depth)
	{
		Layers += FString::Printf(TEXT("%s%s"), Depth > 0 ? TEXT(" > ") : TEXT(""), Layer->GetName());
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Boot] file system layers (top first): %s"), *Layers);
	UE_LOG(LogMassBubble, Log, TEXT("[Boot] cooked data: %s | async loading thread: %s"),
		FPlatformProperties::RequiresCookedData() ? TEXT("required") : TEXT("optional"),
		IsAsyncLoadingMultithreaded() ? TEXT("ON") : TEXT("off (packages load on the game thread)"));

	static const TCHAR* const CVarNames[] =
	{
		TEXT("s.AsyncLoadingThreadEnabled"),
		TEXT("s.AsyncLoadingTimeLimit"),
		TEXT("s.AsyncLoadingUseFullTimeLimit"),
		TEXT("s.PriorityAsyncLoadingExtraTime"),
		TEXT("s.LevelStreamingActorsUpdateTimeLimit"),
		TEXT("s.ForceGCAfterLevelStreamedOut"),
		TEXT("gc.TimeBetweenPurgingPendingKillObjects"),
		TEXT("wp.Runtime.BlockOnSlowStreaming"),
		TEXT("wp.Runtime.MaxLoadingLevelStreamingCells"),
		TEXT("net.IsPushModelEnabled"),
	};
	for (const TCHAR* Name : CVarNames)
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Boot]   %s = %s"), Name, *BootCVarText(Name));
	}
}

void MassBubbleBoot::LogModules(bool bList)
{
	TArray<FModuleStatus> Modules;
	FModuleManager::Get().QueryModules(Modules);
	Modules.Sort([](const FModuleStatus& A, const FModuleStatus& B) { return A.Name < B.Name; });

	int32 Loaded = 0;
	int32 GameModules = 0;
	for (const FModuleStatus& Module : Modules)
	{
		if (Module.bIsLoaded)
		{
			++Loaded;
			GameModules += Module.bIsGameModule ? 1 : 0;
		}
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Boot] modules: %d loaded (%d game modules) of %d known"), Loaded, GameModules, Modules.Num());
	if (bList)
	{
		for (const FModuleStatus& Module : Modules)
		{
			if (Module.bIsLoaded)
			{
				UE_LOG(LogMassBubble, Log, TEXT("[Boot]   %s"), *Module.Name);
			}
		}
	}
}

#if !UE_BUILD_SHIPPING

namespace
{
	FAutoConsoleCommand GCmdBootReport(
		TEXT("opt.boot.Report"),
		TEXT("Log the startup timeline (milestones with the time since module startup)."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			UE_LOG(LogMassBubble, Log, TEXT("[Boot] timeline:%s"), *MassBubbleBoot::GetReport());
		}));

	FAutoConsoleCommand GCmdBootIo(
		TEXT("opt.boot.IO"),
		TEXT("Log the file system layers, whether packages load on a thread of their own and the loading related console variables."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleBoot::LogIoDiagnostics));

	FAutoConsoleCommand GCmdBootModules(
		TEXT("opt.boot.Modules"),
		TEXT("opt.boot.Modules [1]  Log how many modules the process loaded, with 1 also their names."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			MassBubbleBoot::LogModules(Args.Num() > 0 && Args[0] != TEXT("0"));
		}));

	FAutoConsoleCommand GCmdBootReset(
		TEXT("opt.boot.Reset"),
		TEXT("Forget the startup milestones (between two PIE sessions)."),
		FConsoleCommandDelegate::CreateStatic(&MassBubbleBoot::Reset));
}

#endif // !UE_BUILD_SHIPPING
