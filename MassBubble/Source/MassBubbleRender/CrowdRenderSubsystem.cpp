#include "CrowdRenderSubsystem.h"

#include "CrowdRenderHost.h"
#include "CrowdRenderSettings.h"
#include "MassBubbleRender.h"

#include "Core/MassBubbleRuntimeConfig.h"
#include "Crowd/CrowdSettings.h"
#include "Net/CrowdBubble.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Stats/Stats.h"

// This module has its own stat group and CSV category, so no CSV category symbols
// have to be shared across the module boundary.
DECLARE_STATS_GROUP(TEXT("Opt Render"), STATGROUP_OptRender, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Crowd Render Update"), STAT_OptRender_Update, STATGROUP_OptRender);
DECLARE_DWORD_COUNTER_STAT(TEXT("Agents Drawn"), STAT_OptRender_Drawn, STATGROUP_OptRender);
CSV_DEFINE_CATEGORY(OptRender, true);

namespace
{
	int32 GRenderEnable = 1;
	FAutoConsoleVariableRef CVarRenderEnable(
		TEXT("opt.crowd.Render"), GRenderEnable,
		TEXT("1 = draw the replicated crowd. 0 = hide it (measure network cost without render cost)."),
		ECVF_Default);

	int32 GRenderSmoothing = 1;
	FAutoConsoleVariableRef CVarRenderSmoothing(
		TEXT("opt.crowd.RenderSmoothing"), GRenderSmoothing,
		TEXT("1 = eased velocity + velocity feed-forward + correction + yaw (CrowdSmoothing). 0 = draw the extrapolated position as it is (A/B comparison)."),
		ECVF_Default);

	int32 GRenderStats = 0;
	FAutoConsoleVariableRef CVarRenderStats(
		TEXT("opt.crowd.RenderStats"), GRenderStats,
		TEXT("1 = print client-side crowd numbers on screen (non-shipping builds)."),
		ECVF_Default);

	/** A standing agent has no velocity to face; derive a stable pseudo random heading from its id instead. */
	double StationaryYawDegrees(uint32 NetId)
	{
		const uint32 Hash = NetId * 2654435761u;
		return static_cast<double>(Hash >> 24) * (360.0 / 256.0);
	}
}

bool UCrowdRenderSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	if (IsRunningDedicatedServer() || IsRunningCommandlet())
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->GetNetMode() != NM_DedicatedServer;
}

bool UCrowdRenderSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UCrowdRenderSubsystem::Deinitialize()
{
	// The transient host actor dies with the world; just drop our references.
	Host = nullptr;
	Instances = nullptr;
	Bubble.Reset();
	AssetHandle.Reset();
	Visuals.Empty();
	Transforms.Empty();
	AddScratch.Empty();
	NumInstances = 0;
	Super::Deinitialize();
}

TStatId UCrowdRenderSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdRenderSubsystem, STATGROUP_Tickables);
}

bool UCrowdRenderSubsystem::EnsureHost(const UCrowdRenderSettings& Settings)
{
	if (IsValid(Host) && Instances != nullptr)
	{
		return true;
	}
	if (bHostFailed)
	{
		return false;
	}

	// The host was destroyed behind our back (or never existed): start from scratch.
	Instances = nullptr;
	NumInstances = 0;

	// ---- assets: requested once, loaded in the background; the crowd simply appears a few frames later ----
	if (!AssetHandle.IsValid())
	{
		TArray<FSoftObjectPath> Paths;
		Paths.Add(Settings.AgentMesh.ToSoftObjectPath());
		if (!Settings.AgentMaterial.IsNull())
		{
			Paths.Add(Settings.AgentMaterial.ToSoftObjectPath());
		}

		AssetHandle = StreamableManager.RequestAsyncLoad(MoveTemp(Paths));
		if (!AssetHandle.IsValid())
		{
			UE_LOG(LogMassBubbleRender, Error, TEXT("AgentMesh '%s' could not be requested - the crowd will not be drawn."),
				*Settings.AgentMesh.ToString());
			bHostFailed = true;
			return false;
		}
	}
	if (!AssetHandle->HasLoadCompleted())
	{
		return false; // still loading, try again next frame
	}

	UStaticMesh* Mesh = Settings.AgentMesh.Get();
	if (Mesh == nullptr)
	{
		UE_LOG(LogMassBubbleRender, Error, TEXT("AgentMesh '%s' could not be loaded - the crowd will not be drawn."),
			*Settings.AgentMesh.ToString());
		bHostFailed = true;
		return false;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Host = GetWorld()->SpawnActor<ACrowdRenderHost>(ACrowdRenderHost::StaticClass(), FTransform::Identity, Params);
	if (!IsValid(Host))
	{
		UE_LOG(LogMassBubbleRender, Error, TEXT("Could not spawn ACrowdRenderHost."));
		bHostFailed = true;
		return false;
	}

	Instances = Host->GetInstances();
	Instances->SetStaticMesh(Mesh);
	if (UMaterialInterface* Material = Settings.AgentMaterial.Get())
	{
		Instances->SetMaterial(0, Material);
	}
	Instances->SetCastShadow(Settings.bCastShadow);
	return true;
}

void UCrowdRenderSubsystem::ResizeInstances(int32 NewCount)
{
	if (NewCount > NumInstances)
	{
		AddScratch.SetNum(NewCount - NumInstances, EAllowShrinking::No); // identity transforms; overwritten right after
		Instances->AddInstances(AddScratch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
	}
	else
	{
		// Index == array slot, so removing from the back never reshuffles survivors.
		for (int32 Index = NumInstances - 1; Index >= NewCount; --Index)
		{
			Instances->RemoveInstance(Index);
		}
	}
	NumInstances = NewCount;
}

void UCrowdRenderSubsystem::ClearAll()
{
	if (Instances != nullptr && NumInstances > 0)
	{
		Instances->ClearInstances();
	}
	NumInstances = 0;
	Visuals.Reset();
}

void UCrowdRenderSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_OptRender_Update);
	CSV_SCOPED_TIMING_STAT(OptRender, Update);
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("Opt.Render.Update");

	UWorld* World = GetWorld();
	if (World == nullptr || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const UCrowdRenderSettings* Settings = GetDefault<UCrowdRenderSettings>();
	const double Now = FPlatformTime::Seconds();

	// ---- 1) the replicated bubble: poll (cheaply, ~1 Hz) until it has arrived ----
	ACrowdBubble* BubblePtr = Bubble.Get();
	if (BubblePtr == nullptr && Now >= NextBubbleSearchTime)
	{
		NextBubbleSearchTime = Now + Settings->BubbleSearchIntervalSec;
		BubblePtr = ACrowdBubble::FindLocalBubble(World);
		Bubble = BubblePtr;
	}

	if (BubblePtr == nullptr || GRenderEnable == 0)
	{
		ClearAll();
		return;
	}
	if (!EnsureHost(*Settings))
	{
		return;
	}

	// ---- 2) one instance per received agent ----
	const TArray<FCrowdAgentItem>& Items = BubblePtr->GetItems();
	const int32 Count = FMath::Min(Items.Num(), Settings->MaxInstances);
	if (Count != NumInstances)
	{
		ResizeInstances(Count);
	}
	INC_DWORD_STAT_BY(STAT_OptRender_Drawn, Count);
	if (Count == 0)
	{
		return;
	}

	const FVector2D Origin = BubblePtr->GetOriginWorld();
	const double Z = static_cast<double>(GetCrowdTuning().AgentGroundZ) + Settings->PivotOffsetCm;
	const FVector Scale = Settings->AgentScale;

	CrowdSmoothing::FParams Params;
	Params.VelocityRate = Settings->VelocityEasingRate;
	Params.CorrectionRate = Settings->CorrectionRate;
	Params.YawRate = Settings->YawRate;
	Params.SnapDistanceCm = Settings->SnapDistanceCm;
	const CrowdSmoothing::FFrame Frame = CrowdSmoothing::MakeFrame(Params, DeltaTime); // once per frame, not per agent
	const bool bSmooth = GRenderSmoothing != 0;

	++VisualEpoch;
	Transforms.Reset(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FCrowdAgentItem& Item = Items[Index];

		// Where the server believes the client draws this agent, and the velocity to walk it with until the next update.
		const FVector2D Target = Item.GetExtrapolatedPosition(Origin, Now);
		const FVector2f TargetVelocity = Item.GetDrivingVelocity(Now);

		FVector2D Shown;
		double Yaw;

		if (bSmooth)
		{
			FVisual* Visual = Visuals.Find(Item.NetId);
			if (Visual == nullptr)
			{
				// first sighting: appear exactly where the server says
				Visual = &Visuals.Add(Item.NetId);
				Visual->State = CrowdSmoothing::Begin(Target, TargetVelocity, StationaryYawDegrees(Item.NetId));
			}
			else if (Visual->Epoch + 1u != VisualEpoch)
			{
				// re-entered the bubble after a gap: do not fly in from the old spot (but keep facing the way it faced)
				Visual->State = CrowdSmoothing::Begin(Target, TargetVelocity, Visual->State.YawDeg);
			}
			else
			{
				CrowdSmoothing::Step(Visual->State, Target, TargetVelocity, DeltaTime, Frame, Params);
			}
			Visual->Epoch = VisualEpoch;

			Shown = Visual->State.Pos;
			Yaw = Visual->State.YawDeg;
		}
		else
		{
			Shown = Target;
			Yaw = (TargetVelocity.SizeSquared() > 1.f)
				? FMath::RadiansToDegrees(FMath::Atan2(static_cast<double>(TargetVelocity.Y), static_cast<double>(TargetVelocity.X)))
				: StationaryYawDegrees(Item.NetId);
		}

		Transforms.Emplace(FRotator(0.0, Yaw, 0.0), FVector(Shown.X, Shown.Y, Z), Scale);
	}

	// Forget agents that left the bubble (only when the map has clearly grown, so the sweep is rare).
	if (Visuals.Num() > Count * 2 + 64)
	{
		for (auto It = Visuals.CreateIterator(); It; ++It)
		{
			if (It->Value.Epoch != VisualEpoch)
			{
				It.RemoveCurrent();
			}
		}
	}

	// ---- 3) one call, one render-state update for the whole crowd ----
	Instances->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);

#if !UE_BUILD_SHIPPING
	if (GRenderStats != 0 && GEngine != nullptr)
	{
		int32 Fresh = 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Now - Items[Index].RecvTime < 1.0)
			{
				++Fresh;
			}
		}

		// Replication system this process asked for (Iris / legacy).
		static const FString ReplicationMode = MassBubbleConfig::GetReplicationModeString();

		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x0FC20001), 0.f, FColor::Yellow,
			FString::Printf(TEXT("Crowd (client)  bubble=%d  drawn=%d  updated<1s=%d  origin=(%.0f, %.0f)  net=%s"),
				Items.Num(), Count, Fresh, Origin.X, Origin.Y, *ReplicationMode));
	}
#endif
}
