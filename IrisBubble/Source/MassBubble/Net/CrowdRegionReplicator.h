#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Net/CrowdNetMath.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "CrowdRegionReplicator.generated.h"

class ACrowdRegionReplicator;

/** Broadcast on every machine with a screen while a region replicator is in the world. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCrowdRegionReplicator, ACrowdRegionReplicator*);

/**
 * One replicated agent, fixed-size integers only: 7 B of payload (plus the FastArray per-item header).
 * The index of the item is the index of the agent in its region. The meaning of the fields is in Net/CrowdNetMath.h.
 */
USTRUCT()
struct MASSBUBBLE_API FCrowdAgentItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	/** Offset from the lower corner of the region, 0..RegionSizeCm in 65535 steps. */
	UPROPERTY()
	uint16 X = 0;

	UPROPERTY()
	uint16 Y = 0;

	/** Velocity in 5 cm/s units. */
	UPROPERTY()
	int8 VX = 0;

	UPROPERTY()
	int8 VY = 0;

	/** CrowdNet::StampOf(server time) of the moment the state was published. */
	UPROPERTY()
	uint8 Stamp = 0;

	/** Iris (and FastArray change detection) compares items by value. */
	bool operator==(const FCrowdAgentItem& Other) const
	{
		return X == Other.X && Y == Other.Y && VX == Other.VX && VY == Other.VY && Stamp == Other.Stamp;
	}
};

/**
 * Standard FFastArraySerializer: both replication systems handle it (legacy NetDriver via NetDeltaSerialize below,
 * Iris through its support for existing fast array definitions), so one build can be tested with
 * -UseIrisReplication=0 / 1. Only the items that were marked dirty are sent.
 */
USTRUCT()
struct MASSBUBBLE_API FCrowdAgentArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdAgentItem> Items;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FCrowdAgentItem, FCrowdAgentArray>(Items, DeltaParms, *this);
	}
};

template <>
struct TStructOpsTypeTraits<FCrowdAgentArray> : public TStructOpsTypeTraitsBase2<FCrowdAgentArray>
{
	enum
	{
		WithNetDeltaSerializer = true,
	};
};

/**
 * The replicated state of ONE crowd region: the quantized position and velocity of every agent in it.
 *
 * The crowd is simulated on the server only. What a client sees of it is the set of replicators that the replication system
 * has in scope for it:
 *   * the replicator is a spatially filtered object: its location is the center of its region and its net cull distance
 *     covers the replication radius plus half a region diagonal, so the Iris grid filter (or the legacy distance check)
 *     decides which regions a player receives, and the prioritizer decides which of them go first when bandwidth is short;
 *   * it is push model driven: UCrowdSubsystem marks the array dirty only when it published an agent;
 *   * the array is a FastArray: only the items that changed are sent. The number of agents of a region is fixed while the
 *     region is active, so the array never changes size after it has been filled;
 *   * every item has a time stamp and the replicator one for its newest update, so that the age of a state is the same on
 *     every client, whenever the region came into its scope (see Net/CrowdNetMath.h).
 *
 * Created by UCrowdSubsystem while a real player is near its region, destroyed when the region goes away.
 */
UCLASS(NotPlaceable)
class MASSBUBBLE_API ACrowdRegionReplicator : public AActor
{
	GENERATED_BODY()

public:
	ACrowdRegionReplicator();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- Authority ----

	/**
	 * Between SpawnActorDeferred and FinishSpawning: names the region, sizes the array and sets how far away the actor
	 * is relevant. The state itself is written after FinishSpawning.
	 */
	void ServerInitialize(const FIntPoint& InCoord, double InRegionSizeCm, int32 NumAgents, double CullDistanceCm, float UpdateHz);

	/** Writes the state of one agent and marks its item dirty. Pos is in world space. */
	void ServerWriteAgent(int32 Index, const FVector2D& Pos, const FVector2f& Vel, uint8 Stamp);

	/** Publishes what ServerWriteAgent wrote: marks the replicated properties dirty and takes the stamp of this update. */
	void ServerCommit(uint8 InNewestStamp);

	// ---- Any machine with a screen ----

	int32 GetNumAgents() const { return AgentArray.Items.Num(); }
	const FIntPoint& GetRegionCoord() const { return RegionCoord; }
	double GetRegionSizeCm() const { return RegionSizeCm; }

	/** Lower corner of the region in world space. */
	FVector2D GetOriginWorld() const;

	/** The state of an agent as published, in world space (no extrapolation). */
	FVector2D GetAgentPos(int32 Index) const;
	FVector2f GetAgentVel(int32 Index) const;

	/** Seconds since the state of the agent was valid on the server, as far as this machine can tell. */
	double GetAgentAgeSec(int32 Index, double Now) const;

	/** Where the agent is believed to be at Now (CrowdNet::Extrapolate) and the velocity to walk a drawn agent with. */
	FVector2D GetExtrapolatedPos(int32 Index, double Now, FVector2f& OutDrivingVelocity) const;

	/** Lets the renderer follow the replicators that exist instead of polling for them. */
	static FOnCrowdRegionReplicator& OnRegionBeginPlay();
	static FOnCrowdRegionReplicator& OnRegionEndPlay();

private:
	UFUNCTION()
	void OnRep_NewestStamp();

	UPROPERTY(Replicated)
	FCrowdAgentArray AgentArray;

	/** Region this replicator carries. Sent once. */
	UPROPERTY(Replicated)
	FIntPoint RegionCoord = FIntPoint::ZeroValue;

	UPROPERTY(Replicated)
	float RegionSizeCm = 12800.f;

	/** Stamp of the newest update of the array. */
	UPROPERTY(ReplicatedUsing = OnRep_NewestStamp)
	uint8 NewestStamp = 0;

	/** Platform seconds at which the newest update of this machine's copy arrived (or was written, on the authority). */
	double RecvTime = 0.0;

	bool bHeaderMarked = false;
};
