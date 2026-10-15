#include "Core/MassBubbleDiet.h"

#include "Core/MassBubbleBoot.h"
#include "Core/MassBubbleMemory.h"
#include "MassBubble.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "ModuleDescriptor.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	constexpr double DietBytesPerMB = 1024.0 * 1024.0;

	// ---------------------------------------------------------------------------------------------
	// What the hooks recorded (-OptDietModules / -OptDietReport)
	// ---------------------------------------------------------------------------------------------

	struct FDietModuleLoad
	{
		double MsSinceProcess = -1.0;
		double ProcessMB = -1.0;
	};

	struct FDietMemorySample
	{
		FString At;
		double MsSinceProcess = 0.0;
		double ProcessMB = 0.0;
	};

	FCriticalSection GDietLock;
	TMap<FName, FDietModuleLoad> GLoadedAfterHook; // modules whose load the hook saw
	TSet<FName> GLoadedBeforeHook;             // modules that were up before the hook was installed: no timing for those
	TArray<FDietMemorySample> GDietSamples;
	double GHookMs = -1.0;
	FString GPhaseAtHook;

	double DietSecondsSinceProcessStart()
	{
		return FPlatformTime::Seconds() - GStartTime;
	}

	double DietProcessMB()
	{
		return static_cast<double>(MassBubbleMem::GetProcessUsedBytes()) / DietBytesPerMB;
	}

	void DietAddSample(const TCHAR* At)
	{
		FDietMemorySample Sample;
		Sample.At = At;
		Sample.MsSinceProcess = DietSecondsSinceProcessStart() * 1000.0;
		Sample.ProcessMB = DietProcessMB();

		FScopeLock Lock(&GDietLock);
		GDietSamples.Add(Sample);
	}

	const TCHAR* DietBuildConfigName()
	{
#if UE_BUILD_SHIPPING
		return TEXT("Shipping");
#elif UE_BUILD_TEST
		return TEXT("Test");
#elif UE_BUILD_DEVELOPMENT
		return TEXT("Development");
#else
		return TEXT("Debug");
#endif
	}

	const TCHAR* DietTargetTypeName()
	{
		if (IsRunningDedicatedServer())
		{
			return TEXT("Server");
		}
		if (IsRunningClientOnly())
		{
			return TEXT("Client");
		}
		if (GIsEditor)
		{
			return TEXT("Editor");
		}
		return TEXT("Game");
	}

	// ---------------------------------------------------------------------------------------------
	// Minimal JSON text: enough for this report, and no dependency on the Json module (a module that is
	// loaded only for the diet tooling would be a poor way to start a module diet).
	// ---------------------------------------------------------------------------------------------

	FString DietJsonQuote(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 2);
		Out.AppendChar(TEXT('"'));
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR C = In[Index];
			switch (C)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (static_cast<uint32>(C) < 0x20u)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
				break;
			}
		}
		Out.AppendChar(TEXT('"'));
		return Out;
	}

	FString DietJsonNumber(double Value)
	{
		return FString::Printf(TEXT("%.3f"), FMath::IsFinite(Value) ? Value : 0.0);
	}

	/** One JSON object. Keys are string literals, so they need no escaping. */
	struct FDietJson
	{
		TArray<FString> Members;

		FDietJson& Str(const TCHAR* Key, const FString& Value)
		{
			Members.Add(FString::Printf(TEXT("\"%s\":%s"), Key, *DietJsonQuote(Value)));
			return *this;
		}

		FDietJson& Num(const TCHAR* Key, double Value)
		{
			Members.Add(FString::Printf(TEXT("\"%s\":%s"), Key, *DietJsonNumber(Value)));
			return *this;
		}

		FDietJson& Int(const TCHAR* Key, int64 Value)
		{
			Members.Add(FString::Printf(TEXT("\"%s\":%lld"), Key, Value));
			return *this;
		}

		FDietJson& Flag(const TCHAR* Key, bool Value)
		{
			Members.Add(FString::Printf(TEXT("\"%s\":%s"), Key, Value ? TEXT("true") : TEXT("false")));
			return *this;
		}

		/** Already valid JSON (an array or an object). */
		FDietJson& Raw(const TCHAR* Key, const FString& Json)
		{
			Members.Add(FString::Printf(TEXT("\"%s\":%s"), Key, *Json));
			return *this;
		}

		FString ToString() const
		{
			return FString::Printf(TEXT("{%s}"), *FString::Join(Members, TEXT(",")));
		}
	};

	/** One element per line: the report stays readable and diffable. */
	FString DietJsonArray(const TArray<FString>& Items)
	{
		return FString::Printf(TEXT("[\n%s\n]"), *FString::Join(Items, TEXT(",\n")));
	}

	// ---------------------------------------------------------------------------------------------
	// Is anything using the module? Live objects whose class comes from /Script/<Module> (CDOs do not count: every
	// loaded class has one), and objects that live in a plugin's mounted content.
	// ---------------------------------------------------------------------------------------------

	struct FDietObjectCounts
	{
		TMap<FName, int32> InstancesByClassPackage; // /Script/<Module> -> live instances of its classes
		TMap<FName, int32> ClassesByPackage;        // /Script/<Module> -> UClass objects it registered
		TMap<FName, int32> ObjectsByOuterPackage;   // package -> objects that live in it
		int32 TotalObjects = 0;
		int32 TotalClasses = 0;
	};

	void DietCountObjects(FDietObjectCounts& Out)
	{
		// TObjectIterator skips class default objects by default. One pass over a few hundred thousand objects: a few ms,
		// once, when the report is written.
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Object = *It;
			++Out.TotalObjects;

			if (const UClass* Class = Object->GetClass())
			{
				if (const UPackage* ClassPackage = Class->GetOutermost())
				{
					++Out.InstancesByClassPackage.FindOrAdd(ClassPackage->GetFName());
				}
			}

			if (const UPackage* OuterPackage = Object->GetOutermost())
			{
				++Out.ObjectsByOuterPackage.FindOrAdd(OuterPackage->GetFName());
			}

			if (UClass* AsClass = Cast<UClass>(Object))
			{
				++Out.TotalClasses;
				if (const UPackage* Package = AsClass->GetOutermost())
				{
					++Out.ClassesByPackage.FindOrAdd(Package->GetFName());
				}
			}
		}
	}

	FName DietScriptPackageOf(const FString& ModuleName)
	{
		return FName(*FString::Printf(TEXT("/Script/%s"), *ModuleName));
	}

	int32 DietFindCount(const TMap<FName, int32>& Map, const FName& Key)
	{
		const int32* Found = Map.Find(Key);
		return Found != nullptr ? *Found : 0;
	}

	// ---------------------------------------------------------------------------------------------
	// Report sections
	// ---------------------------------------------------------------------------------------------

	FString DietBuildMilestonesJson()
	{
		TArray<MassBubbleBoot::FMilestoneInfo> Timeline;
		MassBubbleBoot::GetTimeline(Timeline);
		const double OffsetSec = MassBubbleBoot::GetProcessOffsetSec();

		TArray<FString> Items;
		for (const MassBubbleBoot::FMilestoneInfo& Milestone : Timeline)
		{
			FDietJson Object;
			Object.Str(TEXT("name"), Milestone.Name)
				.Num(TEXT("ms_since_first"), Milestone.Seconds * 1000.0)
				.Num(TEXT("ms_since_process"), (Milestone.Seconds + OffsetSec) * 1000.0);
			Items.Add(Object.ToString());
		}
		return DietJsonArray(Items);
	}

	FString DietBuildSamplesJson(double NowProcessMB)
	{
		TArray<FString> Items;
		{
			FScopeLock Lock(&GDietLock);
			for (const FDietMemorySample& Sample : GDietSamples)
			{
				FDietJson Object;
				Object.Str(TEXT("at"), Sample.At).Num(TEXT("ms_since_process"), Sample.MsSinceProcess).Num(TEXT("process_mb"), Sample.ProcessMB);
				Items.Add(Object.ToString());
			}
		}

		FDietJson Last;
		Last.Str(TEXT("at"), TEXT("Report")).Num(TEXT("ms_since_process"), DietSecondsSinceProcessStart() * 1000.0).Num(TEXT("process_mb"), NowProcessMB);
		Items.Add(Last.ToString());
		return DietJsonArray(Items);
	}

	FString DietBuildModulesJson(const FDietObjectCounts& Counts, int32& OutLoaded)
	{
		TArray<FModuleStatus> Statuses;
		FModuleManager::Get().QueryModules(Statuses);
		Statuses.Sort([](const FModuleStatus& A, const FModuleStatus& B) { return A.Name < B.Name; });

		TArray<FString> Items;
		FScopeLock Lock(&GDietLock);
		for (const FModuleStatus& Status : Statuses)
		{
			if (!Status.bIsLoaded)
			{
				continue;
			}

			const FName ModuleName(*Status.Name);
			const FDietModuleLoad* Load = GLoadedAfterHook.Find(ModuleName);
			const FName ScriptPackage = DietScriptPackageOf(Status.Name);
			const int64 FileBytes = Status.FilePath.IsEmpty() ? -1 : IFileManager::Get().FileSize(*Status.FilePath);

			FDietJson Object;
			Object.Str(TEXT("name"), Status.Name)
				.Str(TEXT("plugin"), MassBubbleDiet::GetModuleOwner(ModuleName))
				.Flag(TEXT("game_module"), Status.bIsGameModule)
				.Flag(TEXT("loaded_before_hook"), GLoadedBeforeHook.Contains(ModuleName))
				.Num(TEXT("loaded_ms"), Load != nullptr ? Load->MsSinceProcess : -1.0)
				.Num(TEXT("mb_at_load"), Load != nullptr ? Load->ProcessMB : -1.0)
				.Num(TEXT("file_mb"), FileBytes > 0 ? static_cast<double>(FileBytes) / DietBytesPerMB : 0.0)
				.Int(TEXT("uclasses"), DietFindCount(Counts.ClassesByPackage, ScriptPackage))
				.Int(TEXT("instances"), DietFindCount(Counts.InstancesByClassPackage, ScriptPackage));
			Items.Add(Object.ToString());
			++OutLoaded;
		}
		return DietJsonArray(Items);
	}

	FString DietBuildPluginsJson(const FDietObjectCounts& Counts)
	{
		const TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();

		// Objects that live in a plugin's mounted content, e.g. /Niagara/...
		TArray<TPair<FString, FString>> Mounts; // plugin name, mount path
		for (const TSharedRef<IPlugin>& Plugin : Plugins)
		{
			if (Plugin->CanContainContent())
			{
				const FString Mount = Plugin->GetMountedAssetPath();
				if (!Mount.IsEmpty())
				{
					Mounts.Emplace(Plugin->GetName(), Mount);
				}
			}
		}

		TMap<FString, int32> ContentObjects;
		for (const TPair<FName, int32>& Pair : Counts.ObjectsByOuterPackage)
		{
			const FString PackageName = Pair.Key.ToString();
			if (PackageName.StartsWith(TEXT("/Script/")))
			{
				continue;
			}
			for (const TPair<FString, FString>& Mount : Mounts)
			{
				if (PackageName.StartsWith(Mount.Value))
				{
					ContentObjects.FindOrAdd(Mount.Key) += Pair.Value;
					break;
				}
			}
		}

		TArray<FString> Items;
		for (const TSharedRef<IPlugin>& Plugin : Plugins)
		{
			const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();

			int32 Declared = 0;
			int32 Loaded = 0;
			int32 Instances = 0;
			TArray<FString> ModuleItems;
			for (const FModuleDescriptor& Module : Descriptor.Modules)
			{
				const bool bLoaded = FModuleManager::Get().IsModuleLoaded(Module.Name);
				++Declared;
				Loaded += bLoaded ? 1 : 0;
				Instances += DietFindCount(Counts.InstancesByClassPackage, DietScriptPackageOf(Module.Name.ToString()));

				FDietJson ModuleObject;
				ModuleObject.Str(TEXT("name"), Module.Name.ToString())
					.Str(TEXT("type"), EHostType::ToString(Module.Type))
					.Str(TEXT("phase"), ELoadingPhase::ToString(Module.LoadingPhase))
					.Flag(TEXT("loaded"), bLoaded);
				ModuleItems.Add(ModuleObject.ToString());
			}

			// Hard and optional plugin dependencies, so the tools can keep everything a kept plugin needs.
			TArray<FString> DependencyItems;
			for (const FPluginReferenceDescriptor& Reference : Descriptor.Plugins)
			{
				if (!Reference.bEnabled)
				{
					continue;
				}
				FDietJson DependencyObject;
				DependencyObject.Str(TEXT("name"), Reference.Name).Flag(TEXT("optional"), Reference.bOptional);
				DependencyItems.Add(DependencyObject.ToString());
			}

			const int32* Content = ContentObjects.Find(Plugin->GetName());

			FDietJson Object;
			Object.Str(TEXT("name"), Plugin->GetName())
				.Str(TEXT("friendly_name"), Descriptor.FriendlyName)
				.Str(TEXT("category"), Descriptor.Category)
				.Str(TEXT("origin"), Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine ? TEXT("engine") : TEXT("project"))
				.Str(TEXT("descriptor"), Plugin->GetDescriptorFileName())
				.Flag(TEXT("enabled_by_default"), Plugin->IsEnabledByDefault(true))
				.Flag(TEXT("can_contain_content"), Plugin->CanContainContent())
				.Int(TEXT("content_objects"), Content != nullptr ? *Content : 0)
				.Int(TEXT("instances"), Instances)
				.Int(TEXT("modules_declared"), Declared)
				.Int(TEXT("modules_loaded"), Loaded)
				.Raw(TEXT("depends_on"), FString::Printf(TEXT("[%s]"), *FString::Join(DependencyItems, TEXT(","))))
				.Raw(TEXT("modules"), FString::Printf(TEXT("[%s]"), *FString::Join(ModuleItems, TEXT(","))));
			Items.Add(Object.ToString());
		}
		return DietJsonArray(Items);
	}
}

// =================================================================================================
// Public
// =================================================================================================

FString MassBubbleDiet::GetModuleOwner(FName ModuleName)
{
	TSharedPtr<IPlugin> Owner = nullptr;

#if WITH_EDITOR
	Owner = IPluginManager::Get().GetModuleOwnerPlugin(ModuleName);
#else
	for (const TSharedPtr<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		if (Plugin.IsValid())
		{
			const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
			bool bFound = Descriptor.Modules.ContainsByPredicate([&ModuleName](const FModuleDescriptor& Module) {
				return Module.Name == ModuleName;
				});

			if (bFound)
			{
				Owner = Plugin;
				break;
			}
		}
	}
#endif

	return Owner.IsValid() ? Owner->GetName() : FString();
}

FString MassBubbleDiet::BuildReportJson(const FString& Label)
{
	// Memory first: counting the objects below allocates.
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	const double ProcessMBNow = static_cast<double>(Memory.UsedPhysical) / DietBytesPerMB;
	const double UptimeSec = DietSecondsSinceProcessStart();

	FDietObjectCounts Counts;
	DietCountObjects(Counts);

	int32 LoadedModules = 0;
	const FString ModulesJson = DietBuildModulesJson(Counts, LoadedModules);

	const int64 ExecutableBytes = IFileManager::Get().FileSize(FPlatformProcess::ExecutablePath());

	FDietJson Timing;
	Timing.Num(TEXT("uptime_sec"), UptimeSec)
		.Num(TEXT("process_to_first_milestone_ms"), MassBubbleBoot::GetProcessOffsetSec() * 1000.0)
		.Num(TEXT("module_hook_installed_ms"), GHookMs);

	FDietJson MemoryObject;
	MemoryObject.Num(TEXT("process_mb"), ProcessMBNow) // the same quantity as the OptCrowd/ProcessMB CSV stat
		.Num(TEXT("peak_physical_mb"), static_cast<double>(Memory.PeakUsedPhysical) / DietBytesPerMB)
		.Num(TEXT("virtual_mb"), static_cast<double>(Memory.UsedVirtual) / DietBytesPerMB)
		.Num(TEXT("peak_virtual_mb"), static_cast<double>(Memory.PeakUsedVirtual) / DietBytesPerMB);

	FDietJson Objects;
	Objects.Int(TEXT("total"), Counts.TotalObjects).Int(TEXT("uclasses"), Counts.TotalClasses);

	FDietJson Root;
	Root.Int(TEXT("schema"), 1)
		.Str(TEXT("label"), Label)
		.Str(TEXT("generated_utc"), FDateTime::UtcNow().ToIso8601())
		.Str(TEXT("project"), FApp::GetProjectName())
		.Str(TEXT("engine_version"), FEngineVersion::Current().ToString())
		.Str(TEXT("platform"), ANSI_TO_TCHAR(FPlatformProperties::PlatformName()))
		.Str(TEXT("configuration"), DietBuildConfigName())
		.Str(TEXT("target_type"), DietTargetTypeName())
		.Str(TEXT("command_line"), FCommandLine::Get())
		.Str(TEXT("executable"), FPlatformProcess::ExecutablePath())
		.Num(TEXT("executable_mb"), ExecutableBytes > 0 ? static_cast<double>(ExecutableBytes) / DietBytesPerMB : 0.0)
		.Str(TEXT("last_completed_phase_at_hook"), GPhaseAtHook)
		.Raw(TEXT("timing"), Timing.ToString())
		.Raw(TEXT("memory"), MemoryObject.ToString())
		.Raw(TEXT("objects"), Objects.ToString())
		.Raw(TEXT("memory_samples"), DietBuildSamplesJson(ProcessMBNow))
		.Raw(TEXT("milestones"), DietBuildMilestonesJson())
		.Raw(TEXT("modules"), ModulesJson)
		.Raw(TEXT("plugins"), DietBuildPluginsJson(Counts));

	return Root.ToString() + TEXT("\n");
}

FString MassBubbleDiet::WriteReport(const FString& Label, const FString& OutPath)
{
	FString Path = OutPath;
	if (Path.IsEmpty())
	{
		Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Diet"), Label + TEXT(".json"));
	}
	Path = FPaths::ConvertRelativePathToFull(Path);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);

	const double Start = FPlatformTime::Seconds();
	const FString Json = BuildReportJson(Label);
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMassBubble, Error, TEXT("[Diet] could not write the report to %s"), *Path);
		return FString();
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Diet] report '%s' written to %s (%.1f ms, %d bytes)"),
		*Label, *Path, (FPlatformTime::Seconds() - Start) * 1000.0, Json.Len());
	return Path;
}

void MassBubbleDiet::LogPlugins(bool bListModules)
{
	struct FRow
	{
		FString Name;
		FString Origin;
		int32 Declared = 0;
		int32 Loaded = 0;
		TArray<FString> Modules;
	};

	TArray<FRow> Rows;
	const TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		FRow& Row = Rows.AddDefaulted_GetRef();
		Row.Name = Plugin->GetName();
		Row.Origin = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine ? TEXT("engine") : TEXT("project");
		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			const bool bLoaded = FModuleManager::Get().IsModuleLoaded(Module.Name);
			++Row.Declared;
			Row.Loaded += bLoaded ? 1 : 0;
			Row.Modules.Add(FString::Printf(TEXT("%s (%s, %s)%s"), *Module.Name.ToString(), EHostType::ToString(Module.Type),
				ELoadingPhase::ToString(Module.LoadingPhase), bLoaded ? TEXT("") : TEXT("  <not loaded>")));
		}
	}

	Rows.Sort([](const FRow& A, const FRow& B)
	{
		return A.Loaded != B.Loaded ? A.Loaded > B.Loaded : A.Name < B.Name;
	});

	int32 WithCode = 0;
	for (const FRow& Row : Rows)
	{
		WithCode += Row.Loaded > 0 ? 1 : 0;
	}

	UE_LOG(LogMassBubble, Log, TEXT("[Diet] %d enabled plugins, %d with loaded code, %d without (target: %s)"),
		Rows.Num(), WithCode, Rows.Num() - WithCode, DietTargetTypeName());
	for (const FRow& Row : Rows)
	{
		UE_LOG(LogMassBubble, Log, TEXT("[Diet]   %-40s %-8s modules loaded %d / %d"), *Row.Name, *Row.Origin, Row.Loaded, Row.Declared);
		if (bListModules)
		{
			for (const FString& Module : Row.Modules)
			{
				UE_LOG(LogMassBubble, Log, TEXT("[Diet]       %s"), *Module);
			}
		}
	}
}

void MassBubbleDiet::Install()
{
	static bool bInstalled = false;
	if (bInstalled)
	{
		return;
	}
	bInstalled = true;

	const TCHAR* CommandLine = FCommandLine::Get();

	FString ReportLabel;
	FString ReportPath;
	float SettleSec = 15.f;
	FParse::Value(CommandLine, TEXT("OptDietReport="), ReportLabel);
	FParse::Value(CommandLine, TEXT("OptDietOut="), ReportPath);
	FParse::Value(CommandLine, TEXT("OptDietSettleSec="), SettleSec);
	SettleSec = FMath::Max(SettleSec, 0.f);

	const bool bReport = !ReportLabel.IsEmpty();
	const bool bQuit = bReport && FParse::Param(CommandLine, TEXT("OptDietQuit"));
	const bool bModules = bReport || FParse::Param(CommandLine, TEXT("OptDietModules"));

	if (bModules)
	{
		// Everything that is already up has no load time: it was loaded before this module started.
		{
			TArray<FModuleStatus> Modules;
			FModuleManager::Get().QueryModules(Modules);

			FScopeLock Lock(&GDietLock);
			for (const FModuleStatus& Module : Modules)
			{
				if (Module.bIsLoaded)
				{
					GLoadedBeforeHook.Add(FName(*Module.Name));
				}
			}
			GHookMs = DietSecondsSinceProcessStart() * 1000.0;
			GPhaseAtHook = ELoadingPhase::ToString(IPluginManager::Get().GetLastCompletedLoadingPhase());
		}
		DietAddSample(TEXT("Diet.HookInstalled"));

		// Every module that loads from now on becomes a milestone, with the size of the process at that moment: the
		// difference to the previous load is what the module (and whatever else happened in between) cost.
		FModuleManager::Get().OnModulesChanged().AddLambda([](FName ModuleName, EModuleChangeReason Reason)
		{
			if (Reason != EModuleChangeReason::ModuleLoaded)
			{
				return;
			}

			FDietModuleLoad Load;
			Load.MsSinceProcess = DietSecondsSinceProcessStart() * 1000.0;
			Load.ProcessMB = DietProcessMB();
			{
				FScopeLock Lock(&GDietLock);
				GLoadedAfterHook.Add(ModuleName, Load);
			}
			MassBubbleBoot::Mark(*FString::Printf(TEXT("Module.%s.Loaded"), *ModuleName.ToString()));
		});

		// Plugin loading phases, to see which phase the time goes to.
		IPluginManager::Get().OnLoadingPhaseComplete().AddLambda([](ELoadingPhase::Type Phase, bool bSuccess)
		{
			MassBubbleBoot::Mark(*FString::Printf(TEXT("Module.Phase.%s.%s"), ELoadingPhase::ToString(Phase), bSuccess ? TEXT("Done") : TEXT("Failed")));
		});
	}

	FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([bReport, bQuit, ReportLabel, ReportPath, SettleSec]()
	{
		MassBubbleBoot::Mark(TEXT("Engine.InitComplete"));
		DietAddSample(TEXT("Engine.InitComplete"));

		if (!bReport)
		{
			return;
		}

		UE_LOG(LogMassBubble, Log, TEXT("[Diet] report '%s' in %.1f s%s"), *ReportLabel, SettleSec, bQuit ? TEXT(", then quit") : TEXT(""));
		FTSTicker::GetCoreTicker().AddTicker(TEXT("OptDietReport"), SettleSec, [bQuit, ReportLabel, ReportPath](float)
		{
			MassBubbleBoot::Mark(TEXT("Diet.ReportDue"));
			WriteReport(ReportLabel, ReportPath);
			if (bQuit)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false; // once
		});
	});
}

#if !UE_BUILD_SHIPPING

namespace
{
	FAutoConsoleCommand GCmdDietReport(
		TEXT("opt.diet.Report"),
		TEXT("opt.diet.Report [label]  Write the module / plugin report now to <Saved>/Diet/<label>.json (default label: manual). Load times need -OptDietModules at startup."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			MassBubbleDiet::WriteReport(Args.Num() > 0 ? Args[0] : FString(TEXT("manual")), FString());
		}));

	FAutoConsoleCommand GCmdDietPlugins(
		TEXT("opt.diet.Plugins"),
		TEXT("opt.diet.Plugins [1]  Log the enabled plugins with how many of their modules are loaded. 1 also lists the modules."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			MassBubbleDiet::LogPlugins(Args.Num() > 0 && Args[0] != TEXT("0"));
		}));
}

#endif // !UE_BUILD_SHIPPING
