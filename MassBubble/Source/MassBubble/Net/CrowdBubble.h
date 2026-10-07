#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdCellGrid.h"
#include "GameFramework/Actor.h"
#include "Net/CrowdNetMath.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "CrowdBubble.generated.h"

class UCrowdSubsystem;
struct FCrowdAgentArray;

/**
 * One replicated agent, fixed-size integers only:
 *   NetId 4 B + X,Y int16 + VX,VY int8 = 10 B (plus the FastArray per-item header).
 * Fields without UPROPERTY are NOT replicated; they are bookkeeping on either side.
 */
USTRUCT()
struct MASSBUBBLE_API FCrowdAgentItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 NetId = 0;

	/** Offset from the bubble origin in 1 cm units (int16 => +-327 m). */
	UPROPERTY()
	int16 X = 0;

	UPROPERTY()
	int16 Y = 0;

	/** Velocity in 5 cm/s units. */
	UPROPERTY()
	int8 VX = 0;

	UPROPERTY()
	int8 VY = 0;

	// ---- server only: what the client currently believes (dequantized), for the dead-reckoning decision ----
	FVector2D SentPos = FVector2D::ZeroVector;
	FVector2f SentVel = FVector2f::ZeroVector;
	double SentTime = 0.0;
	uint32 SeenEpoch = 0;
	/** Index into ACrowdBubble::Candidates of the current rebuild, so a moved item can be refreshed with CURRENT data. */
	int32 CandidateIndex = INDEX_NONE;

	// ---- client (and standalone): when this state arrived, for extrapolation ----
	double RecvTime = 0.0;

	// ---- client (and standalone): the replicated values RecvTime belongs to (see MarkReceived) ----
	uint32 AppliedNetId = 0;
	int16 AppliedX = 0;
	int16 AppliedY = 0;
	int8 AppliedVX = 0;
	int8 AppliedVY = 0;

	/** Iris (and FastArray change detection) compares items by value. Only replicated fields take part. */
	bool operator==(const FCrowdAgentItem& Other) const
	{
		return NetId == Other.NetId && X == Other.X && Y == Other.Y && VX == Other.VX && VY == Other.VY;
	}

	void PostReplicatedAdd(const struct FCrowdAgentArray& InArraySerializer);
	void PostReplicatedChange(const struct FCrowdAgentArray& InArraySerializer);

	/**
	 * Records that this state arrived at Now. The extrapolation clock restarts only if the replicated values changed:
	 * Iris can report items that carry unchanged values, and restarting their clock would make the agent jump back
	 * to its last base position and walk the same distance again.
	 */
	void MarkReceived(double Now)
	{
		const bool bFirstTime = RecvTime <= 0.0;
		if (bFirstTime || NetId != AppliedNetId || X != AppliedX || Y != AppliedY || VX != AppliedVX || VY != AppliedVY)
		{
			RecvTime = Now;
			AppliedNetId = NetId;
			AppliedX = X;
			AppliedY = Y;
			AppliedVX = VX;
			AppliedVY = VY;
		}
	}

	/** Client side view of the agent: last received state plus velocity * elapsed (capped). */
	FVector2D GetExtrapolatedPosition(const FVector2D& OriginWorld, double Now, double MaxExtrapolationSec) const;
	FVector2f GetVelocity() const;
};

/**
 * Standard FFastArraySerializer: both replication systems handle it (legacy NetDriver via NetDeltaSerialize below,
 * Iris through its support for existing fast array definitions), so one build can be tested with
 * -UseIrisReplication=0 / 1.
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
 * Per-player area-of-interest replicator. The agents around a player are not replicated as individual
 * Actors; one bubble Actor per player carries them. It is
 *   * only relevant to its owner               (bOnlyRelevantToOwner + COND_OwnerOnly)
 *   * push model driven                        (compared only when marked dirty)
 *   * a FastArray of 10 byte quantized records (only changed records are sent)
 *   * filtered by dead reckoning               (an agent walking straight sends nothing)
 *
 * Created by AMassBubbleGameMode on login, destroyed on logout.
 */
UCLASS(NotPlaceable)
class MASSBUBBLE_API ACrowdBubble : public AActor
{
	GENERATED_BODY()

public:
	ACrowdBubble();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	const TArray<FCrowdAgentItem>& GetItems() const { return AgentArray.Items; }
	FVector2D GetOriginWorld() const { return CrowdNet::LatticeToWorld(OriginLattice); }

	/** The bubble owned by a locally controlled PlayerController, if it exists (client / standalone / listen host). */
	static ACrowdBubble* FindLocalBubble(const UWorld* World);

private:
	void ServerRebuild(UCrowdSubsystem& Crowd);

	UPROPERTY(Replicated)
	FCrowdAgentArray AgentArray;

	/** Origin of the int16 offsets, as a 200 m lattice index. Changes rarely. */
	UPROPERTY(Replicated)
	FIntPoint OriginLattice = FIntPoint::ZeroValue;

	// ---- server only bookkeeping (not replicated) ----
	struct FCandidate
	{
		FCrowdGridAgent Agent;
		double DistSq = 0.0;
	};

	TArray<FCandidate> Candidates;
	TMap<uint32, int32> IdToIndex;
	uint32 Epoch = 0;
	uint32 LastSnapshotSerial = 0;
	bool bOriginInitialized = false;
};
