#include "CrowdRenderSubsystem.h"

#include "CrowdRenderHost.h"
#include "CrowdRenderSettings.h"
#include "MassBubbleRender.h"

#include "Core/MassBubbleBoot.h"
#include "Core/MassBubbleRuntimeConfig.h"
#include "Crowd/CrowdSettings.h"
#include "Net/CrowdRegionReplicator.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Stats/Stats.h"

#include <algorithm>

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
	double StationaryYawDegrees(uint32 AgentId)
	{
		const uint32 Hash = AgentId * 2654435761u;
		return static_cast<double>(Hash >> 24) * (360.0 / 256.0);
	}

	/** Identifies an agent on this machine: its region and its index in it. */
	uint32 AgentIdOf(const FIntPoint& Coord, int32 Index)
	{
		return static_cast<uint32>(Index) ^ (static_cast<uint32>(Coord.X) * 73856093u) ^ (static_cast<uint32>(Coord.Y) * 19349663u);
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

void UCrowdRenderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	RegionBeginPlayHandle = ACrowdRegionReplicator::OnRegionBeginPlay().AddUObject(this, &UCrowdRenderSubsystem::HandleRegionBeginPlay);
	RegionEndPlayHandle = ACrowdRegionReplicator::OnRegionEndPlay().AddUObject(this, &UCrowdRenderSubsystem::HandleRegionEndPlay);

	const UCrowdRenderSettings* Settings = GetDefault<UCrowdRenderSettings>();
	if (Settings->bPrewarmAssets)
	{
		RequestAssets(*Settings);
	}
}

void UCrowdRenderSubsystem::Deinitialize()
{
	ACrowdRegionReplicator::OnRegionBeginPlay().Remove(RegionBeginPlayHandle);
	ACrowdRegionReplicator::OnRegionEndPlay().Remove(RegionEndPlayHandle);
	RegionBeginPlayHandle.Reset();
	RegionEndPlayHandle.Reset();

	// The transient host actor dies with the world; just drop our references.
	Host = nullptr;
	Instances = nullptr;
	Regions.Empty();
	VisualsByRegion.Empty();
	Candidates.Empty();
	AssetHandle.Reset();
	Transforms.Empty();
	AddScratch.Empty();
	NumInstances = 0;
	Super::Deinitialize();
}

TStatId UCrowdRenderSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdRenderSubsystem, STATGROUP_Tickables);
}

void UCrowdRenderSubsystem::HandleRegionBeginPlay(ACrowdRegionReplicator* Region)
{
	if (Region == nullptr || Region->GetWorld() != GetWorld())
	{
		return;
	}

	Regions.AddUnique(Region);
	RequestAssets(*GetDefault<UCrowdRenderSettings>());
}

void UCrowdRenderSubsystem::HandleRegionEndPlay(ACrowdRegionReplicator* Region)
{
	if (Region == nullptr)
	{
		return;
	}

	Regions.RemoveAll([Region](const TWeakObjectPtr<ACrowdRegionReplicator>& Ptr)
	{
		return !Ptr.IsValid() || Ptr.Get() == Region;
	});
	VisualsByRegion.Remove(Region);
}

void UCrowdRenderSubsystem::RequestAssets(const UCrowdRenderSettings& Settings)
{
	// Requested once, loaded in the background; the crowd simply appears a few frames later.
	if (AssetHandle.IsValid() || bHostFailed)
	{
		return;
	}

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
		return;
	}
	OPT_BOOT_ONCE("Render.AssetsRequested");
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

	// ---- assets: loaded in the background; the crowd simply appears a few frames later ----
	RequestAssets(Settings);
	if (bHostFailed || !AssetHandle.IsValid())
	{
		return false;
	}
	if (!AssetHandle->HasLoadCompleted())
	{
		return false; // still loading, try again next frame
	}
	OPT_BOOT_ONCE("Render.AssetsReady");

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
	VisualsByRegion.Reset();
}

bool UCrowdRenderSubsystem::GetViewLocation(FVector2D& OutLocation) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC == nullptr)
	{
		return false;
	}

	FVector Location = FVector::ZeroVector;
	if (PC->PlayerCameraManager != nullptr)
	{
		Location = PC->PlayerCameraManager->GetCameraLocation();
	}
	else if (const APawn* Pawn = PC->GetPawn())
	{
		Location = Pawn->GetActorLocation();
	}
	else
	{
		return false;
	}

	OutLocation = FVector2D(Location.X, Location.Y);
	return true;
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

	// ---- 1) the replicators that exist on this machine: the ones that began play before this subsystem listened, once ----
	if (!bScannedRegions)
	{
		bScannedRegions = true;
		for (TActorIterator<ACrowdRegionReplicator> It(World); It; ++It)
		{
			Regions.AddUnique(*It);
		}
	}
	Regions.RemoveAll([](const TWeakObjectPtr<ACrowdRegionReplicator>& Ptr) { return !Ptr.IsValid(); });

	if (Regions.Num() == 0 || GRenderEnable == 0)
	{
		ClearAll();
		return;
	}
	OPT_BOOT_ONCE("Render.RegionFound");
	if (!EnsureHost(*Settings))
	{
		return;
	}

	// ---- 2) the agents to draw: inside the draw radius of the local player, at most MaxInstances of them ----
	FVector2D ViewLocation = FVector2D::ZeroVector;
	const bool bHasView = GetViewLocation(ViewLocation);
	const double DrawRadius = static_cast<double>(Settings->DrawRadiusCm);
	const double DrawRadiusSq = DrawRadius * DrawRadius;

	Candidates.Reset();
	int32 NumItems = 0;
	for (const TWeakObjectPtr<ACrowdRegionReplicator>& Weak : Regions)
	{
		const ACrowdRegionReplicator* Region = Weak.Get();
		if (Region == nullptr)
		{
			continue;
		}

		const int32 NumAgents = Region->GetNumAgents();
		NumItems += NumAgents;

		// Agents stay inside their region: a region whose box lies beyond the draw radius has nothing to draw.
		if (bHasView)
		{
			const FVector2D BoxMin = Region->GetOriginWorld();
			const double Size = Region->GetRegionSizeCm();
			const double DX = FMath::Max(FMath::Max(BoxMin.X - ViewLocation.X, ViewLocation.X - (BoxMin.X + Size)), 0.0);
			const double DY = FMath::Max(FMath::Max(BoxMin.Y - ViewLocation.Y, ViewLocation.Y - (BoxMin.Y + Size)), 0.0);
			if (DX * DX + DY * DY > DrawRadiusSq)
			{
				continue;
			}
		}

		TUniquePtr<FRegionVisuals>& Visuals = VisualsByRegion.FindOrAdd(Region);
		if (!Visuals.IsValid())
		{
			Visuals = MakeUnique<FRegionVisuals>();
		}
		if (Visuals->Agents.Num() != NumAgents)
		{
			Visuals->Agents.SetNum(NumAgents);
		}
		FRegionVisuals* RegionVisuals = Visuals.Get();

		for (int32 Index = 0; Index < NumAgents; ++Index)
		{
			FCandidate Candidate;
			// Where the server believes the client draws this agent, and the velocity to walk it with until the next update.
			Candidate.Target = Region->GetExtrapolatedPos(Index, Now, Candidate.Drive);
			Candidate.DistSq = bHasView ? FVector2D::DistSquared(Candidate.Target, ViewLocation) : 0.0;
			if (bHasView && Candidate.DistSq > DrawRadiusSq)
			{
				continue;
			}

			Candidate.Region = Region;
			Candidate.Visuals = RegionVisuals;
			Candidate.Index = Index;
			Candidates.Add(Candidate);
		}
	}

	const int32 MaxInstances = Settings->MaxInstances;
	if (Candidates.Num() > MaxInstances)
	{
		std::nth_element(Candidates.GetData(), Candidates.GetData() + MaxInstances, Candidates.GetData() + Candidates.Num(),
			[](const FCandidate& A, const FCandidate& B) { return A.DistSq < B.DistSq; });
		Candidates.SetNum(MaxInstances, EAllowShrinking::No);
	}

	// ---- 3) one instance per agent that is drawn ----
	const int32 Count = Candidates.Num();
	if (Count != NumInstances)
	{
		ResizeInstances(Count);
	}
	INC_DWORD_STAT_BY(STAT_OptRender_Drawn, Count);
	if (Count == 0)
	{
		return;
	}

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
	for (const FCandidate& Candidate : Candidates)
	{
		FVisual& Visual = Candidate.Visuals->Agents[Candidate.Index];
		const uint32 AgentId = AgentIdOf(Candidate.Region->GetRegionCoord(), Candidate.Index);

		FVector2D Shown;
		double Yaw;

		if (bSmooth)
		{
			if (Visual.Epoch == 0u)
			{
				// first sighting: appear exactly where the server says
				Visual.State = CrowdSmoothing::Begin(Candidate.Target, Candidate.Drive, StationaryYawDegrees(AgentId));
			}
			else if (Visual.Epoch + 1u != VisualEpoch)
			{
				// back inside the draw radius after a gap: do not fly in from the old spot (but keep facing the way it faced)
				Visual.State = CrowdSmoothing::Begin(Candidate.Target, Candidate.Drive, Visual.State.YawDeg);
			}
			else
			{
				CrowdSmoothing::Step(Visual.State, Candidate.Target, Candidate.Drive, DeltaTime, Frame, Params);
			}
			Visual.Epoch = VisualEpoch;

			Shown = Visual.State.Pos;
			Yaw = Visual.State.YawDeg;
		}
		else
		{
			Shown = Candidate.Target;
			Yaw = (Candidate.Drive.SizeSquared() > 1.f)
				? FMath::RadiansToDegrees(FMath::Atan2(static_cast<double>(Candidate.Drive.Y), static_cast<double>(Candidate.Drive.X)))
				: StationaryYawDegrees(AgentId);
		}

		Transforms.Emplace(FRotator(0.0, Yaw, 0.0), FVector(Shown.X, Shown.Y, Z), Scale);
	}

	// ---- 4) one call, one render-state update for the whole crowd ----
	Instances->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/true, /*bTeleport=*/true);
	OPT_BOOT_ONCE("Render.FirstDraw");

#if !UE_BUILD_SHIPPING
	if (GRenderStats != 0 && GEngine != nullptr)
	{
		// Replication system this process asked for (Iris / legacy).
		static const FString ReplicationMode = MassBubbleConfig::GetReplicationModeString();

		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x0FC20001), 0.f, FColor::Yellow,
			FString::Printf(TEXT("Crowd (client)  regions=%d  items=%d  drawn=%d  net=%s"),
				Regions.Num(), NumItems, Count, *ReplicationMode));
	}
#endif
}
