#include "Crowd/CrowdPCGPlanner.h"

#include "Crowd/CrowdPCGHost.h"
#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdSubsystem.h"
#include "MassBubble.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "PCGGraph.h"

bool UCrowdPCGPlanner::Initialize()
{
	const UCrowdSettings* Settings = GetDefault<UCrowdSettings>();
	GraphPath = Settings->SpawnGraph.ToSoftObjectPath();
	if (GraphPath.IsNull())
	{
		UE_LOG(LogMassBubble, Warning, TEXT("[CrowdPCG] 'Use PCG' is on, but no Spawn Graph is set (Project Settings > MassBubble Crowd > PCG). Regions are populated randomly."));
		State = EState::Disabled;
		return false;
	}

	// Loaded in the background; requests that arrive before it is ready simply wait in the queue.
	GraphHandle = StreamableManager.RequestAsyncLoad(GraphPath);
	if (!GraphHandle.IsValid())
	{
		UE_LOG(LogMassBubble, Error, TEXT("[CrowdPCG] the spawn graph '%s' could not be requested. Regions are populated randomly."), *GraphPath.ToString());
		State = EState::Failed;
		return false;
	}

	State = EState::Loading;
	UE_LOG(LogMassBubble, Log, TEXT("[CrowdPCG] loading spawn graph '%s'"), *GraphPath.ToString());
	return true;
}

void UCrowdPCGPlanner::Shutdown()
{
	// The transient host actors die with the world: just drop the references.
	Queue.Empty();
	Running.Empty();
	Results.Empty();
	GraphHandle.Reset();
	Graph = nullptr;
	State = EState::Disabled;
}

void UCrowdPCGPlanner::PollGraphLoad()
{
	if (!GraphHandle.IsValid() || GraphHandle->WasCanceled())
	{
		UE_LOG(LogMassBubble, Error, TEXT("[CrowdPCG] the request for the spawn graph '%s' was cancelled. Regions are populated randomly."), *GraphPath.ToString());
		State = EState::Failed;
		return;
	}
	if (!GraphHandle->HasLoadCompleted())
	{
		return; // still loading
	}

	UObject* Loaded = GraphHandle->GetLoadedAsset();
	if (Cast<UPCGGraphInterface>(Loaded) == nullptr)
	{
		UE_LOG(LogMassBubble, Error, TEXT("[CrowdPCG] '%s' did not load as a PCG graph (not cooked into this build? wrong asset type?). Regions are populated randomly."), *GraphPath.ToString());
		State = EState::Failed;
		return;
	}

	Graph = Loaded;
	State = EState::Ready;
	UE_LOG(LogMassBubble, Log, TEXT("[CrowdPCG] spawn graph ready: %s"), *GetNameSafe(Loaded));
}

void UCrowdPCGPlanner::Request(const FCrowdPlanRequest& InRequest)
{
	// One plan per region at a time.
	for (const FJob& Job : Queue)
	{
		if (Job.Request.Coord == InRequest.Coord)
		{
			return;
		}
	}
	for (const FJob& Job : Running)
	{
		if (Job.Request.Coord == InRequest.Coord)
		{
			return;
		}
	}

	FJob& Job = Queue.AddDefaulted_GetRef();
	Job.Request = InRequest;
	Job.QueuedAt = FPlatformTime::Seconds();
	++NumRequested;
}

void UCrowdPCGPlanner::Cancel(const FIntPoint& Coord)
{
	Queue.RemoveAll([&Coord](const FJob& Job) { return Job.Request.Coord == Coord; });
	Results.RemoveAll([&Coord](const FCrowdPlanResult& Result) { return Result.Coord == Coord; });

	for (int32 i = Running.Num() - 1; i >= 0; --i)
	{
		if (Running[i].Request.Coord != Coord)
		{
			continue;
		}
		if (ACrowdPCGHost* Host = Running[i].Host.Get())
		{
			Host->Destroy(); // PCG cancels the run of a component that is destroyed
		}
		Running.RemoveAtSwap(i);
		++NumCancelled;
	}
}

bool UCrowdPCGPlanner::StartJob(FJob& Job, double Now)
{
	UWorld* World = GetWorld();
	UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(Graph);
	if (World == nullptr || GraphInterface == nullptr)
	{
		return false;
	}

	const FCrowdTuning& T = GetCrowdTuning();
	const double Half = 0.5 * Job.Request.SizeCm;
	const FTransform Transform(FVector(Job.Request.Min.X + Half, Job.Request.Min.Y + Half, static_cast<double>(T.AgentGroundZ)));

	// Deferred spawn: the bounds have to be right before the component registers.
	ACrowdPCGHost* Host = World->SpawnActorDeferred<ACrowdPCGHost>(
		ACrowdPCGHost::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Host == nullptr)
	{
		return false;
	}

	Host->SetFlags(RF_Transient);
	Host->SetGenerationBounds(Job.Request.SizeCm, static_cast<double>(T.PCGBoundsHalfHeightCm));
	Host->FinishSpawning(Transform);

	if (!Host->StartGeneration(GraphInterface, static_cast<int32>(Job.Request.Seed)))
	{
		Host->Destroy();
		return false;
	}

	Job.Host = Host;
	Job.StartedAt = Now;
	return true;
}

void UCrowdPCGPlanner::PushResult(const FJob& Job, ECrowdPlanStatus Status, TArray<FCrowdPlanPoint>&& Points, double Now)
{
	FCrowdPlanResult& Result = Results.AddDefaulted_GetRef();
	Result.Coord = Job.Request.Coord;
	Result.Status = Status;
	Result.Points = MoveTemp(Points);
	Result.LatencySec = Now - Job.QueuedAt;

	switch (Status)
	{
	case ECrowdPlanStatus::Ok:
		++NumOk;
		TotalLatencySec += Result.LatencySec;
		MaxLatencySec = FMath::Max(MaxLatencySec, Result.LatencySec);
		if (!bLoggedFirstOk)
		{
			bLoggedFirstOk = true;
			UE_LOG(LogMassBubble, Log, TEXT("[CrowdPCG] first plan arrived: region (%d,%d), %d points, %.0f ms"),
				Result.Coord.X, Result.Coord.Y, Result.Points.Num(), Result.LatencySec * 1000.0);
		}
		break;

	case ECrowdPlanStatus::TimedOut:
		++NumTimedOut;
		if (NumFailed + NumTimedOut <= 5)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("[CrowdPCG] region (%d,%d): the graph did not finish within %.0f s. Is the PCG plugin enabled, does the graph have an Output node with point data, and does the PCG subsystem run in this world? The region is populated %s."),
				Result.Coord.X, Result.Coord.Y, GetCrowdTuning().PCGTimeoutSec,
				GetCrowdTuning().bPCGFallbackToRandom ? TEXT("randomly instead") : TEXT("NOT (it stays empty)"));
		}
		break;

	default:
		++NumFailed;
		if (NumFailed + NumTimedOut <= 5)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("[CrowdPCG] region (%d,%d): the graph run failed or was cancelled. The region is populated %s."),
				Result.Coord.X, Result.Coord.Y,
				GetCrowdTuning().bPCGFallbackToRandom ? TEXT("randomly instead") : TEXT("NOT (it stays empty)"));
		}
		break;
	}
}

void UCrowdPCGPlanner::Pump(double Now)
{
	if (State == EState::Disabled)
	{
		return;
	}
	if (State == EState::Loading)
	{
		PollGraphLoad();
	}
	if (State == EState::Failed)
	{
		// The graph is not coming. Answer everything that waits; the subsystem populates those regions without PCG.
		for (const FJob& Job : Queue)
		{
			PushResult(Job, ECrowdPlanStatus::Failed, TArray<FCrowdPlanPoint>(), Now);
		}
		Queue.Reset();
		return;
	}
	if (Running.Num() == 0 && Queue.Num() == 0)
	{
		return; // most frames end here
	}

	const FCrowdTuning& T = GetCrowdTuning();

	// ---- 1) runs that finished, failed or took too long ----
	for (int32 i = Running.Num() - 1; i >= 0; --i)
	{
		const FJob& Job = Running[i];
		ACrowdPCGHost* Host = Job.Host.Get();

		ECrowdPlanStatus Status = ECrowdPlanStatus::Ok;
		if (Host == nullptr)
		{
			Status = ECrowdPlanStatus::Failed; // the host died behind our back
		}
		else if (Host->IsFinished())
		{
			Status = Host->Succeeded() ? ECrowdPlanStatus::Ok : ECrowdPlanStatus::Failed;
		}
		else if (Now - Job.StartedAt > static_cast<double>(T.PCGTimeoutSec))
		{
			Status = ECrowdPlanStatus::TimedOut;
		}
		else
		{
			continue; // still running
		}

		TArray<FCrowdPlanPoint> Points;
		if (Status == ECrowdPlanStatus::Ok)
		{
			Host->CollectPoints(Points);
		}
		if (Host != nullptr)
		{
			Host->Destroy();
		}

		PushResult(Job, Status, MoveTemp(Points), Now);
		Running.RemoveAtSwap(i);
	}

	// ---- 2) start waiting runs, in request order ----
	if (State == EState::Ready)
	{
		while (Queue.Num() > 0 && Running.Num() < T.PCGMaxConcurrent)
		{
			FJob Job = MoveTemp(Queue[0]);
			Queue.RemoveAt(0, 1, EAllowShrinking::No);

			if (StartJob(Job, Now))
			{
				Running.Add(MoveTemp(Job));
			}
			else
			{
				PushResult(Job, ECrowdPlanStatus::Failed, TArray<FCrowdPlanPoint>(), Now);
			}
		}
	}
}

bool UCrowdPCGPlanner::PopResult(FCrowdPlanResult& OutResult)
{
	if (Results.Num() == 0)
	{
		return false;
	}
	OutResult = MoveTemp(Results[0]);
	Results.RemoveAt(0, 1, EAllowShrinking::No);
	return true;
}

FString UCrowdPCGPlanner::GetDebugSummary() const
{
	const TCHAR* StateName = TEXT("off");
	switch (State)
	{
	case EState::Loading: StateName = TEXT("loading"); break;
	case EState::Ready:   StateName = TEXT("ready");   break;
	case EState::Failed:  StateName = TEXT("FAILED");  break;
	default: break;
	}

	return FString::Printf(TEXT("pcg: graph %s (%s) | queued=%d running=%d | requested=%d ok=%d failed=%d timed out=%d cancelled=%d | latency avg=%.0f ms max=%.0f ms"),
		*GraphPath.ToString(), StateName, Queue.Num(), Running.Num(),
		NumRequested, NumOk, NumFailed, NumTimedOut, NumCancelled,
		NumOk > 0 ? 1000.0 * TotalLatencySec / static_cast<double>(NumOk) : 0.0, 1000.0 * MaxLatencySec);
}

// ------------------------------------------------------------------------------------------------
// Console commands (the A/B switch opt.crowd.PCG itself lives with the other switches in CrowdSettings.cpp)
// ------------------------------------------------------------------------------------------------

#if !UE_BUILD_SHIPPING

namespace
{
	/** Same fallback as CrowdConsole.cpp: a command can arrive without a world. */
	UWorld* ResolveWorldForPCGCommand(UWorld* InWorld)
	{
		if (InWorld != nullptr)
		{
			return InWorld;
		}
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
				{
					return Context.World();
				}
			}
		}
		return nullptr;
	}

	void CmdPCGStats(const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = ResolveWorldForPCGCommand(InWorld);
		UCrowdSubsystem* Crowd = (World != nullptr) ? World->GetSubsystem<UCrowdSubsystem>() : nullptr;
		if (Crowd == nullptr)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("opt.crowd.PCG.Stats: no crowd subsystem in this world (pure client, or not a game world)."));
			return;
		}

		UE_LOG(LogMassBubble, Log, TEXT("[CrowdPCG] opt.crowd.PCG=%d | %s"), CrowdCVars::PCG, *Crowd->GetPlannerSummary());
	}

	void CmdPCGReplan(const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = ResolveWorldForPCGCommand(InWorld);
		UCrowdSubsystem* Crowd = (World != nullptr) ? World->GetSubsystem<UCrowdSubsystem>() : nullptr;
		if (Crowd == nullptr)
		{
			UE_LOG(LogMassBubble, Warning, TEXT("opt.crowd.PCG.Replan: no crowd subsystem in this world."));
			return;
		}

		const int32 Forgotten = Crowd->ForgetInactivePlans();
		UE_LOG(LogMassBubble, Log, TEXT("[CrowdPCG] forgot the population of %d inactive regions: they are planned again the next time a viewer wants them. Active regions keep their agents."), Forgotten);
	}

	FAutoConsoleCommandWithWorldAndArgs GCmdPCGStats(
		TEXT("opt.crowd.PCG.Stats"),
		TEXT("Log the state of the PCG spawn planner: graph, queue, runs in flight, results and latency."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CmdPCGStats));

	FAutoConsoleCommandWithWorldAndArgs GCmdPCGReplan(
		TEXT("opt.crowd.PCG.Replan"),
		TEXT("Forget the planned / persisted population of every inactive region, so that it is generated again (after editing the PCG graph)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CmdPCGReplan));
}

#endif // !UE_BUILD_SHIPPING
