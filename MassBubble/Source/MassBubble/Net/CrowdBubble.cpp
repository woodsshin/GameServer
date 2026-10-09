#include "Net/CrowdBubble.h"

#include "Core/MassBubbleStats.h"
#include "Crowd/CrowdSettings.h"
#include "Crowd/CrowdSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Net/UnrealNetwork.h"
#include "Net/Core/PushModel/PushModel.h"

// =================================================================================================
// FCrowdAgentItem
// =================================================================================================

void FCrowdAgentItem::PostReplicatedAdd(const FCrowdAgentArray& InArraySerializer)
{
	MarkReceived(FPlatformTime::Seconds());
}

void FCrowdAgentItem::PostReplicatedChange(const FCrowdAgentArray& InArraySerializer)
{
	MarkReceived(FPlatformTime::Seconds());
}

FVector2f FCrowdAgentItem::GetVelocity() const
{
	return FVector2f(CrowdNet::DequantizeVelocity(VX), CrowdNet::DequantizeVelocity(VY));
}

FVector2D FCrowdAgentItem::GetExtrapolatedPosition(const FVector2D& OriginWorld, double Now) const
{
	const FVector2D Base = OriginWorld + FVector2D(CrowdNet::DequantizeOffset(X), CrowdNet::DequantizeOffset(Y));
	return CrowdNet::Extrapolate(Base, GetVelocity(), Now - RecvTime);
}

FVector2f FCrowdAgentItem::GetDrivingVelocity(double Now) const
{
	return CrowdNet::DrivingVelocity(GetVelocity(), Now - RecvTime);
}

// =================================================================================================
// ACrowdBubble
// =================================================================================================

namespace
{
	/** Writes the replicated fields and refreshes the server's model of what the client believes. */
	void FillItem(FCrowdAgentItem& Item, const FCrowdGridAgent& Agent, const FVector2D& OriginWorld, double Now)
	{
		Item.NetId = Agent.NetId;
		Item.X = CrowdNet::QuantizeOffset(Agent.Pos.X - OriginWorld.X);
		Item.Y = CrowdNet::QuantizeOffset(Agent.Pos.Y - OriginWorld.Y);
		Item.VX = CrowdNet::QuantizeVelocity(Agent.Vel.X);
		Item.VY = CrowdNet::QuantizeVelocity(Agent.Vel.Y);

		// Round trip through the quantizer: the dead-reckoning check must compare against what the client sees.
		Item.SentPos = OriginWorld + FVector2D(CrowdNet::DequantizeOffset(Item.X), CrowdNet::DequantizeOffset(Item.Y));
		Item.SentVel = FVector2f(CrowdNet::DequantizeVelocity(Item.VX), CrowdNet::DequantizeVelocity(Item.VY));
		Item.SentTime = Now;
		Item.MarkReceived(Now); // standalone / listen host never gets PostReplicated* callbacks for its own data
	}
}

ACrowdBubble::ACrowdBubble()
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	bAlwaysRelevant = false;
	bNetLoadOnClient = false;
	// bReplicateMovement stays at its AActor default (false): the bubble never moves.

	// Iris replicates sub objects through the registered sub object list only. The bubble has none today;
	// the flag keeps it correct if one is added.
	bReplicateUsingRegisteredSubObjectList = true;

	// Push model: only properties marked dirty are compared, so a generous update frequency is cheap.
	SetNetUpdateFrequency(20.f);
	SetMinNetUpdateFrequency(2.f);

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// After the PrePhysics Mass phase that builds the snapshot, so we always see this frame's data.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void ACrowdBubble::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	FDoRepLifetimeParams Params;
	Params.bIsPushBased = true;
	Params.Condition = COND_OwnerOnly;
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdBubble, AgentArray, Params);
	DOREPLIFETIME_WITH_PARAMS_FAST(ACrowdBubble, OriginLattice, Params);
}

void ACrowdBubble::BeginPlay()
{
	Super::BeginPlay();

	// Only the authority simulates the interest set; remote copies just hold the replicated data.
	SetActorTickEnabled(HasAuthority());
}

void ACrowdBubble::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return;
	}
	if (!IsValid(GetOwner()))
	{
		Destroy();
		return;
	}
	if (!CrowdCVars::Replicate)
	{
		return;
	}

	UCrowdSubsystem* Crowd = GetWorld()->GetSubsystem<UCrowdSubsystem>();
	if (Crowd == nullptr)
	{
		return;
	}

	const uint32 Serial = Crowd->GetSnapshotSerial();
	if (Serial == LastSnapshotSerial)
	{
		return; // no new snapshot since the last rebuild: nothing to do this frame
	}
	LastSnapshotSerial = Serial;

	ServerRebuild(*Crowd);
}

void ACrowdBubble::ServerRebuild(UCrowdSubsystem& Crowd)
{
	OPT_SCOPE(STAT_OptCrowd_Bubble, Bubble, "Opt.Crowd.Bubble");

	const FCrowdTuning& Tuning = GetCrowdTuning();

	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC == nullptr)
	{
		return;
	}
	const AActor* Target = PC->GetPawn() != nullptr ? static_cast<const AActor*>(PC->GetPawn()) : PC->GetViewTarget();
	if (Target == nullptr)
	{
		return;
	}

	const FVector Location = Target->GetActorLocation();
	const FVector2D Center(Location.X, Location.Y);
	const double Now = FPlatformTime::Seconds();

	// ---- 1) origin lattice: offsets are relative to it, so moving it re-sends everything ----
	// The origin only moves once the player is more than 110 m away from it, so a player jittering on a
	// lattice border does not cause repeated full resends.
	const bool bRebase = !bOriginInitialized || CrowdNet::NeedsRebase(Center, CrowdNet::LatticeToWorld(OriginLattice));
	if (bRebase)
	{
		OriginLattice = CrowdNet::WorldToLattice(Center);
		bOriginInitialized = true;
		MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdBubble, OriginLattice, this);
	}
	const FVector2D OriginWorld = CrowdNet::LatticeToWorld(OriginLattice);

	// ---- 2) interest set: everything within the radius, nearest MaxAgentsPerBubble ----
	// With hysteresis (CrowdNet::InterestRank / InterestExitRadius): an agent that is already replicated counts as 10 %
	// closer and may stay up to 10 % beyond the radius a newcomer has to be inside. An agent that hovers around the
	// edge of the set otherwise enters and leaves again and again, and each time costs a full record plus a gap in
	// the client's smoothing (the agent pops out and in).
	const double EnterRadius = Tuning.BubbleRadiusCm;
	Candidates.Reset();
	Crowd.ForEachAgentInCircle(Center, CrowdNet::InterestExitRadius(EnterRadius), [this, EnterRadius](const FCrowdGridAgent& Agent, double DistSq)
	{
		const int32* Existing = IdToIndex.Find(Agent.NetId);
		double Rank = 0.0;
		if (!CrowdNet::InterestCandidate(DistSq, Existing != nullptr, EnterRadius, Rank))
		{
			return; // a newcomer has to be inside the normal radius
		}

		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Agent = Agent;
		Candidate.DistSq = DistSq;
		Candidate.ExistingIndex = (Existing != nullptr) ? *Existing : INDEX_NONE;
		Candidate.Rank = Rank;
	});

	if (Candidates.Num() > Tuning.MaxAgentsPerBubble)
	{
		Candidates.Sort([](const FCandidate& L, const FCandidate& R) { return L.Rank < R.Rank; });
		Candidates.SetNum(Tuning.MaxAgentsPerBubble, EAllowShrinking::No);
	}

	// ---- 3) diff against what the client already has ----
	++Epoch;
	TArray<FCrowdAgentItem>& Items = AgentArray.Items;
	const bool bDeadReckoning = CrowdCVars::DeadReckoning != 0;
	const double NearDistSq = static_cast<double>(Tuning.NearDistanceCm) * Tuning.NearDistanceCm;

	int32 NumDirty = 0;
	bool bStructural = false;

	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const FCandidate& Candidate = Candidates[CandidateIndex];
		const FCrowdGridAgent& Agent = Candidate.Agent;

		if (Candidate.ExistingIndex == INDEX_NONE)
		{
			// entered the interest set
			const int32 NewIndex = Items.AddDefaulted();
			FCrowdAgentItem& NewItem = Items[NewIndex];
			FillItem(NewItem, Agent, OriginWorld, Now);
			NewItem.SeenEpoch = Epoch;
			NewItem.CandidateIndex = CandidateIndex;
			IdToIndex.Add(Agent.NetId, NewIndex);
			AgentArray.MarkItemDirty(NewItem);
			++NumDirty;
			bStructural = true;
			continue;
		}

		FCrowdAgentItem& Item = Items[Candidate.ExistingIndex]; // still valid: items are only removed in step 4
		Item.SeenEpoch = Epoch;
		Item.CandidateIndex = CandidateIndex;

		bool bResend = bRebase;
		if (!bResend)
		{
			if (bDeadReckoning)
			{
				const float Tolerance = (Candidate.DistSq < NearDistSq) ? Tuning.NearErrorCm : Tuning.FarErrorCm;
				bResend = CrowdNet::ShouldResend(Agent.Pos, Agent.Vel, Item.SentPos, Item.SentVel,
					Now - Item.SentTime, Tolerance, Tuning.VelocityEpsCmPerSec);
			}
			else
			{
				// Without dead reckoning: resend everything that moves (or just stopped).
				bResend = !Agent.Vel.IsNearlyZero() || !Item.SentVel.IsNearlyZero();
			}
		}

		if (bResend)
		{
			FillItem(Item, Agent, OriginWorld, Now);
			AgentArray.MarkItemDirty(Item);
			++NumDirty;
		}
	}

	// ---- 4) left the interest set ----
	for (int32 i = Items.Num() - 1; i >= 0; --i)
	{
		if (Items[i].SeenEpoch == Epoch)
		{
			continue;
		}
		IdToIndex.Remove(Items[i].NetId);
		Items.RemoveAtSwap(i);
		if (i < Items.Num())
		{
			// The former last element now lives at slot i. It was already visited (we walk backwards),
			// so it is still in the interest set and Candidates[CandidateIndex] holds its current state.
			//
			// The legacy serializer follows an item by its ReplicationID, so moving it costs nothing.
			// Iris addresses array elements by index: slot i has new content and must be sent. The item
			// is refreshed with the agent's current state first, otherwise the client would restart its
			// extrapolation from a stale base position (a visible jump back).
			FCrowdAgentItem& Moved = Items[i];
			IdToIndex[Moved.NetId] = i;
			if (Candidates.IsValidIndex(Moved.CandidateIndex))
			{
				FillItem(Moved, Candidates[Moved.CandidateIndex].Agent, OriginWorld, Now);
			}
			AgentArray.MarkItemDirty(Moved);
			++NumDirty;
		}
		bStructural = true;
	}

	if (bStructural)
	{
		AgentArray.MarkArrayDirty();
	}
	if (bStructural || NumDirty > 0)
	{
		// Push Model: without this the property would not even be compared.
		MARK_PROPERTY_DIRTY_FROM_NAME(ACrowdBubble, AgentArray, this);
	}

	INC_DWORD_STAT_BY(STAT_OptCrowd_BubbleItems, Items.Num());
	INC_DWORD_STAT_BY(STAT_OptCrowd_BubbleDirty, NumDirty);
	CSV_CUSTOM_STAT(OptCrowd, BubbleItems, Items.Num(), ECsvCustomStatOp::Accumulate);
	CSV_CUSTOM_STAT(OptCrowd, BubbleDirty, NumDirty, ECsvCustomStatOp::Accumulate);
}

ACrowdBubble* ACrowdBubble::FindLocalBubble(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	for (TActorIterator<ACrowdBubble> It(World); It; ++It)
	{
		const APlayerController* PC = Cast<APlayerController>(It->GetOwner());
		if (PC != nullptr && PC->IsLocalController())
		{
			return *It;
		}
	}
	return nullptr;
}
