#include "World/MassBubbleStreamingAnchor.h"

#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdSubsystem.h"
#include "MassBubble.h"

#include "Components/SceneComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AMassBubbleStreamingAnchor::AMassBubbleStreamingAnchor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false; // only orbiting anchors need a tick; StartOrbit switches it on
	bReplicates = false;
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Root);

	StreamingSource = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("StreamingSource"));
}

void AMassBubbleStreamingAnchor::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (World->GetNetMode() == NM_Client)
	{
		// A placed anchor also exists in the client's copy of the level. It must not stream anything there.
		SetActorTickEnabled(false);
		if (StreamingSource != nullptr)
		{
			StreamingSource->DisableStreamingSource();
		}
		return;
	}

	if (StreamingSource != nullptr)
	{
		if (bStreamWorldPartition)
		{
			StreamingSource->EnableStreamingSource();
		}
		else
		{
			StreamingSource->DisableStreamingSource();
		}
	}

	if (bRegisterAsCrowdViewer)
	{
		if (UCrowdSubsystem* Crowd = World->GetSubsystem<UCrowdSubsystem>())
		{
			Crowd->RegisterVirtualViewer(this);
		}
	}
}

void AMassBubbleStreamingAnchor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UCrowdSubsystem* Crowd = World->GetSubsystem<UCrowdSubsystem>())
		{
			Crowd->UnregisterVirtualViewer(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void AMassBubbleStreamingAnchor::ConfigureAsRuntimeBot()
{
	bRuntimeBot = true;

	if (StreamingSource != nullptr)
	{
		// Bots are load generators, not players: the cells a real player waits for are loaded first.
		StreamingSource->Priority = EStreamingSourcePriority::Low;

		// By default a bot only loads cells (no component registration, BeginPlay, tick or physics state);
		// the crowd is made of Mass entities and needs none of that.
		StreamingSource->TargetState = (CrowdCVars::BotActivateCells != 0)
			? EStreamingSourceTargetState::Activated
			: EStreamingSourceTargetState::Loaded;
	}
}

void AMassBubbleStreamingAnchor::StartOrbit(const FVector2D& Center, double RadiusCm, double SpeedCmPerSec, double StartAngleRad)
{
	OrbitCenter = Center;
	OrbitRadius = FMath::Max(RadiusCm, 1.0);
	OrbitAngularSpeed = SpeedCmPerSec / OrbitRadius;
	OrbitAngle = StartAngleRad;
	OrbitZ = GetActorLocation().Z;
	bOrbiting = true;
	SetActorTickEnabled(true);
}

void AMassBubbleStreamingAnchor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bOrbiting)
	{
		return;
	}

	OrbitAngle += OrbitAngularSpeed * static_cast<double>(DeltaSeconds);
	SetActorLocation(FVector(
		OrbitCenter.X + FMath::Cos(OrbitAngle) * OrbitRadius,
		OrbitCenter.Y + FMath::Sin(OrbitAngle) * OrbitRadius,
		OrbitZ));
}

AMassBubbleStreamingAnchor* AMassBubbleStreamingAnchor::SpawnBot(UWorld* World, int32 Index, int32 Count)
{
	if (World == nullptr || Count <= 0 || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	const FCrowdTuning& Tuning = GetCrowdTuning();

	// Spread the bots over the circle, and over three concentric orbits so their active areas overlap less.
	const double Angle = FMath::DegreesToRadians(360.0 * static_cast<double>(Index) / static_cast<double>(Count));
	const double Radius = static_cast<double>(Tuning.BotOrbitRadiusCm) * (1.0 - 0.25 * static_cast<double>(Index % 3));
	const double Speed = static_cast<double>(Tuning.BotSpeedCmPerSec);

	const FVector Location(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Tuning.AgentGroundZ);
	const FTransform SpawnTransform(Location);

	// Deferred spawn: the streaming source must be configured before BeginPlay switches it on.
	AMassBubbleStreamingAnchor* Bot = World->SpawnActorDeferred<AMassBubbleStreamingAnchor>(
		AMassBubbleStreamingAnchor::StaticClass(), SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Bot == nullptr)
	{
		return nullptr;
	}

	Bot->SetFlags(RF_Transient);
	Bot->ConfigureAsRuntimeBot();
	Bot->FinishSpawning(SpawnTransform);
	Bot->StartOrbit(FVector2D::ZeroVector, Radius, Speed, Angle);

	UE_LOG(LogMassBubble, Verbose, TEXT("Streaming anchor bot %d/%d spawned (orbit radius %.0f cm, speed %.0f cm/s)."),
		Index + 1, Count, Radius, Speed);
	return Bot;
}

int32 AMassBubbleStreamingAnchor::DestroyOneBot(UWorld* World)
{
	if (World == nullptr)
	{
		return 0;
	}

	AMassBubbleStreamingAnchor* Victim = nullptr;
	int32 Remaining = 0;
	for (TActorIterator<AMassBubbleStreamingAnchor> It(World); It; ++It)
	{
		if (!It->IsRuntimeBot())
		{
			continue;
		}
		if (Victim == nullptr)
		{
			Victim = *It;
		}
		else
		{
			++Remaining;
		}
	}

	if (Victim != nullptr)
	{
		Victim->Destroy();
	}
	return Remaining;
}

int32 AMassBubbleStreamingAnchor::SpawnBots(UWorld* World, int32 Count)
{
	if (World == nullptr || Count <= 0)
	{
		return 0;
	}
	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogMassBubble, Warning, TEXT("SpawnBots ignored: this is a client. Run it on the server / standalone / listen host."));
		return 0;
	}

	if (UCrowdSubsystem* Crowd = World->GetSubsystem<UCrowdSubsystem>())
	{
		Crowd->RequestBots(Count);

		const FCrowdTuning& Tuning = GetCrowdTuning();
		const float Interval = GetBotRampIntervalSec();
		UE_LOG(LogMassBubble, Log, TEXT("Scheduled %d streaming anchor bots (%s, orbit radius %.0f cm, speed %.0f cm/s, cells %s)."),
			Count,
			Interval > 0.f ? *FString::Printf(TEXT("one every %.1f s"), Interval) : TEXT("ALL AT ONCE"),
			Tuning.BotOrbitRadiusCm, Tuning.BotSpeedCmPerSec,
			CrowdCVars::BotActivateCells != 0 ? TEXT("activated") : TEXT("loaded only"));
		return Count;
	}

	// No crowd subsystem to ramp them (should not happen on a server): spawn right away.
	int32 Spawned = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (SpawnBot(World, Index, Count) != nullptr)
		{
			++Spawned;
		}
	}
	return Spawned;
}

int32 AMassBubbleStreamingAnchor::ClearBots(UWorld* World)
{
	if (World == nullptr)
	{
		return 0;
	}

	int32 Existing = 0;
	for (TActorIterator<AMassBubbleStreamingAnchor> It(World); It; ++It)
	{
		if (It->IsRuntimeBot())
		{
			++Existing;
		}
	}

	if (UCrowdSubsystem* Crowd = World->GetSubsystem<UCrowdSubsystem>())
	{
		Crowd->RequestClearBots();

		const float Interval = GetBotRampIntervalSec();
		UE_LOG(LogMassBubble, Log, TEXT("Removing %d streaming anchor bots (%s)."),
			Existing, Interval > 0.f ? *FString::Printf(TEXT("one every %.1f s"), Interval) : TEXT("ALL AT ONCE"));
		return Existing;
	}

	while (DestroyOneBot(World) > 0)
	{
	}
	return Existing;
}
