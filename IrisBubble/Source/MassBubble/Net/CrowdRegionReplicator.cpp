#include "Net/CrowdRegionReplicator.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Net/UnrealNetwork.h"
#include "Net/Core/PushModel/PushModel.h"

ACrowdRegionReplicator::ACrowdRegionReplicator()
{
	PrimaryActorTick.bCanEverTick = false; // UCrowdSubsystem writes the state; nothing here ticks
	bReplicates = true;
	bAlwaysRelevant = false;
	bOnlyRelevantToOwner = false;
	bNetLoadOnClient = false;
	SetCanBeDamaged(false);
	// bReplicateMovement stays at its AActor default (false): the replicator never moves.

	// Iris replicates sub objects through the registered sub object list only. The replicator has none today;
	// the flag keeps it correct if one is added.
	bReplicateUsingRegisteredSubObjectList = true;

	// Push model: only properties marked dirty are compared. The update frequency caps how often the object is polled.
	SetNetUpdateFrequency(10.f);
	SetMinNetUpdateFrequency(2.f);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void ACrowdRegionReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	FDoRepLifetimeParams ArrayParams;
	ArrayParams.bIsPushBased = true;
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdRegionReplicator, AgentArray, ArrayParams);

	// The stamp is what ties the items to the clock of the server: every update has to reach OnRep_NewestStamp.
	FDoRepLifetimeParams StampParams;
	StampParams.bIsPushBased = true;
	StampParams.RepNotifyCondition = REPNOTIFY_Always;
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdRegionReplicator, NewestStamp, StampParams);

	FDoRepLifetimeParams InitialParams;
	InitialParams.bIsPushBased = true;
	InitialParams.Condition = COND_InitialOnly;
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdRegionReplicator, RegionCoord, InitialParams);
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdRegionReplicator, RegionSizeCm, InitialParams);
}

void ACrowdRegionReplicator::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority() && RecvTime <= 0.0)
	{
		RecvTime = FPlatformTime::Seconds();
	}

	if (GetNetMode() != NM_DedicatedServer)
	{
		OnRegionBeginPlay().Broadcast(this);
	}
}

void ACrowdRegionReplicator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetNetMode() != NM_DedicatedServer)
	{
		OnRegionEndPlay().Broadcast(this);
	}

	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// Authority
// =================================================================================================

void ACrowdRegionReplicator::ServerInitialize(const FIntPoint& InCoord, double InRegionSizeCm, int32 NumAgents, double CullDistanceCm, float UpdateHz)
{
	RegionCoord = InCoord;
	RegionSizeCm = static_cast<float>(InRegionSizeCm);
	AgentArray.Items.SetNum(FMath::Max(NumAgents, 0));

	SetNetCullDistanceSquared(static_cast<float>(CullDistanceCm * CullDistanceCm));
	SetNetUpdateFrequency(UpdateHz);
}

void ACrowdRegionReplicator::ServerWriteAgent(int32 Index, const FVector2D& Pos, const FVector2f& Vel, uint8 Stamp)
{
	if (!AgentArray.Items.IsValidIndex(Index))
	{
		return;
	}

	const FVector2D Origin = GetOriginWorld();
	const double Size = RegionSizeCm;

	FCrowdAgentItem& Item = AgentArray.Items[Index];
	Item.X = CrowdNet::QuantizeRegionOffset(Pos.X - Origin.X, Size);
	Item.Y = CrowdNet::QuantizeRegionOffset(Pos.Y - Origin.Y, Size);
	Item.VX = CrowdNet::QuantizeVelocity(Vel.X);
	Item.VY = CrowdNet::QuantizeVelocity(Vel.Y);
	Item.Stamp = Stamp;
	AgentArray.MarkItemDirty(Item);
}

void ACrowdRegionReplicator::ServerCommit(uint8 InNewestStamp)
{
	NewestStamp = InNewestStamp;
	RecvTime = FPlatformTime::Seconds(); // the authority's own copy is as fresh as the update it just wrote

	// Push Model: without this the properties would not even be compared.
	MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdRegionReplicator, AgentArray, this);
	MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdRegionReplicator, NewestStamp, this);

	if (!bHeaderMarked)
	{
		bHeaderMarked = true;
		MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdRegionReplicator, RegionCoord, this);
		MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdRegionReplicator, RegionSizeCm, this);
	}
}

// =================================================================================================
// Reading the state
// =================================================================================================

FVector2D ACrowdRegionReplicator::GetOriginWorld() const
{
	return FVector2D(static_cast<double>(RegionCoord.X) * RegionSizeCm, static_cast<double>(RegionCoord.Y) * RegionSizeCm);
}

FVector2D ACrowdRegionReplicator::GetAgentPos(int32 Index) const
{
	const FCrowdAgentItem& Item = AgentArray.Items[Index];
	const FVector2D Origin = GetOriginWorld();
	const double Size = RegionSizeCm;
	return FVector2D(Origin.X + CrowdNet::DequantizeRegionOffset(Item.X, Size), Origin.Y + CrowdNet::DequantizeRegionOffset(Item.Y, Size));
}

FVector2f ACrowdRegionReplicator::GetAgentVel(int32 Index) const
{
	const FCrowdAgentItem& Item = AgentArray.Items[Index];
	return FVector2f(CrowdNet::DequantizeVelocity(Item.VX), CrowdNet::DequantizeVelocity(Item.VY));
}

double ACrowdRegionReplicator::GetAgentAgeSec(int32 Index, double Now) const
{
	return CrowdNet::StampAgeSec(NewestStamp, AgentArray.Items[Index].Stamp) + FMath::Max(Now - RecvTime, 0.0);
}

FVector2D ACrowdRegionReplicator::GetExtrapolatedPos(int32 Index, double Now, FVector2f& OutDrivingVelocity) const
{
	const double Age = GetAgentAgeSec(Index, Now);
	const FVector2f Velocity = GetAgentVel(Index);
	OutDrivingVelocity = CrowdNet::DrivingVelocity(Velocity, Age);
	return CrowdNet::Extrapolate(GetAgentPos(Index), Velocity, Age);
}

void ACrowdRegionReplicator::OnRep_NewestStamp()
{
	RecvTime = FPlatformTime::Seconds();
}

FOnCrowdRegionReplicator& ACrowdRegionReplicator::OnRegionBeginPlay()
{
	static FOnCrowdRegionReplicator Delegate;
	return Delegate;
}

FOnCrowdRegionReplicator& ACrowdRegionReplicator::OnRegionEndPlay()
{
	static FOnCrowdRegionReplicator Delegate;
	return Delegate;
}
