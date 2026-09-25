#include "AI/BehaviorTree/BTService_UpdateWorldState.h"
#include "AI/KiraverseAIQueries.h"
#include "AI/KiraverseBlackboardKeys.h"
#include "AI/KiraverseAIController.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombComponent.h"
#include "Bomb/KiraverseBombSite.h"
#include "Character/KiraverseCharacter.h"
#include "Game/KiraversePlayerState.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "NavigationSystem.h"
#include "Engine/World.h"

namespace
{
	// Number of candidate stand-off angles tried around the bomb before falling back to the closest navigable one.
	constexpr int32 GuardCandidateCount = 8;
}

UBTService_UpdateWorldState::UBTService_UpdateWorldState()
{
	NodeName = TEXT("Update World State");
	Interval = 0.25f;
	RandomDeviation = 0.05f;
}

FString UBTService_UpdateWorldState::GetStaticServiceDescription() const
{
	return TEXT("Writes enemy, bomb, site, threat and guard facts to the blackboard.");
}

void UBTService_UpdateWorldState::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	const AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	// Non-const: UNavigationSystemV1::GetCurrent only overloads UObject*/UWorld*, not const UWorld*.
	UWorld* World = Self ? Self->GetWorld() : nullptr;
	if (!Blackboard || !Self || !World)
	{
		return;
	}

	const AKiraversePlayerState* PlayerState = Controller->GetPlayerState<AKiraversePlayerState>();
	const ETeam Team = PlayerState ? PlayerState->GetTeam() : ETeam::None;

	// --- Enemy and threat ---
	AKiraverseCharacter* Enemy = KiraverseAIQueries::FindClosestVisibleEnemy(Self, Team, EnemySightRange);
	const bool bChanneling = KiraverseAIQueries::IsChanneling(Self);
	const bool bThreatClose = Enemy
		&& FVector::DistSquared(Self->GetActorLocation(), Enemy->GetActorLocation()) <= FMath::Square(ChannelAbortRange);

	if (Enemy)
	{
		Blackboard->SetValueAsObject(KiraverseBB::TargetEnemy, Enemy);
	}
	else
	{
		Blackboard->ClearValue(KiraverseBB::TargetEnemy);
	}
	Blackboard->SetValueAsBool(KiraverseBB::ThreatClose, bThreatClose);
	Blackboard->SetValueAsBool(KiraverseBB::IsChanneling, bChanneling);

	// --- Bomb ---
	AKiraverseBomb* Bomb = KiraverseAIQueries::FindLiveBomb(World);
	const bool bPlanted = Bomb && Bomb->GetBombState() == EBombState::Planted;
	const bool bLoose = Bomb && Bomb->GetBombState() == EBombState::Idle && !Bomb->GetAttachParentActor();

	if (Bomb)
	{
		Blackboard->SetValueAsObject(KiraverseBB::Bomb, Bomb);
	}
	else
	{
		Blackboard->ClearValue(KiraverseBB::Bomb);
	}
	Blackboard->SetValueAsBool(KiraverseBB::BombPlanted, bPlanted);
	Blackboard->SetValueAsBool(KiraverseBB::BombLoose, bLoose);

	const UKiraverseBombComponent* BombComponent = Self->GetBombComponent();
	Blackboard->SetValueAsBool(KiraverseBB::HasBomb, BombComponent && BombComponent->HasBomb());

	// --- Site ---
	AKiraverseBombSite* Site = KiraverseAIQueries::FindClosestBombSite(World, Self->GetActorLocation());
	if (Site)
	{
		Blackboard->SetValueAsObject(KiraverseBB::TargetSite, Site);
	}
	else
	{
		Blackboard->ClearValue(KiraverseBB::TargetSite);
	}
	Blackboard->SetValueAsBool(KiraverseBB::InPlantZone, Site && Site->IsCharacterInPlantZone(Self));

	// --- Guard location: only meaningful once the bomb is planted ---
	if (!bPlanted)
	{
		Blackboard->ClearValue(KiraverseBB::GuardLocation);
		return;
	}

	// Each bot gets its own starting angle from its name so guards fan out instead of stacking on one spot.
	const FVector BombLocation = Bomb->GetActorLocation();
	const float BaseAngleDegrees = static_cast<float>(GetTypeHash(Controller->GetName()) % 360);
	UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(World);

	FVector BestLocation = BombLocation;
	bool bFoundVisible = false;
	for (int32 Index = 0; Index < GuardCandidateCount; ++Index)
	{
		const float AngleRadians = FMath::DegreesToRadians(BaseAngleDegrees + Index * (360.f / GuardCandidateCount));
		const FVector Candidate = BombLocation + FVector(FMath::Cos(AngleRadians), FMath::Sin(AngleRadians), 0.f) * GuardStandOffDistance;

		FVector Navigable = Candidate;
		if (NavSystem)
		{
			FNavLocation NavLocation;
			if (!NavSystem->ProjectPointToNavigation(Candidate, NavLocation, FVector(100.f, 100.f, 250.f)))
			{
				continue;
			}
			Navigable = NavLocation.Location;
		}

		// A guard position is only useful if the bomb is actually visible from it.
		FCollisionQueryParams Params;
		Params.AddIgnoredActor(Bomb);
		Params.AddIgnoredActor(Self);
		FHitResult Hit;
		const FVector EyeOffset(0.f, 0.f, 80.f);
		if (!World->LineTraceSingleByChannel(Hit, Navigable + EyeOffset, BombLocation + EyeOffset, ECC_Visibility, Params))
		{
			BestLocation = Navigable;
			bFoundVisible = true;
			break;
		}
	}

	// No visible navigable spot around the bomb: stand at the bomb site itself rather than leave the key unset.
	if (!bFoundVisible && Site)
	{
		BestLocation = Site->GetActorLocation();
	}
	Blackboard->SetValueAsVector(KiraverseBB::GuardLocation, BestLocation);
}
