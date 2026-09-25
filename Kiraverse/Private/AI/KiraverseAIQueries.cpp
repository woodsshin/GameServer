#include "AI/KiraverseAIQueries.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombSite.h"
#include "Character/KiraverseCharacter.h"
#include "Game/KiraversePlayerState.h"
#include "AbilitySystemComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace KiraverseAIQueries
{
	bool IsDead(const AKiraverseCharacter* Character)
	{
		return !Character || Character->IsDead();
	}

	bool IsChanneling(const AKiraverseCharacter* Character)
	{
		const UAbilitySystemComponent* ASC = Character ? Character->GetAbilitySystemComponent() : nullptr;
		return ASC && (ASC->HasMatchingGameplayTag(KiraverseGameplayTags::State_Planting)
			|| ASC->HasMatchingGameplayTag(KiraverseGameplayTags::State_Defusing));
	}

	bool HasLineOfSight(const AKiraverseCharacter* Viewer, const AActor* Target)
	{
		const UWorld* World = Viewer ? Viewer->GetWorld() : nullptr;
		if (!World || !Target)
		{
			return false;
		}

		FCollisionQueryParams Params;
		Params.AddIgnoredActor(Viewer);
		Params.AddIgnoredActor(Target);

		FHitResult Hit;
		const bool bBlocked = World->LineTraceSingleByChannel(
			Hit, Viewer->GetPawnViewLocation(), Target->GetActorLocation(), ECC_Visibility, Params);
		return !bBlocked;
	}

	AKiraverseCharacter* FindClosestVisibleEnemy(const AKiraverseCharacter* Self, ETeam OwnTeam, float SightRange)
	{
		const UWorld* World = Self ? Self->GetWorld() : nullptr;
		if (!World || OwnTeam == ETeam::None)
		{
			return nullptr;
		}

		AKiraverseCharacter* Closest = nullptr;
		float ClosestDistSq = FMath::Square(SightRange);
		const FVector SelfLocation = Self->GetActorLocation();

		for (TActorIterator<AKiraverseCharacter> It(World); It; ++It)
		{
			AKiraverseCharacter* Other = *It;
			if (Other == Self || Other->IsDead())
			{
				continue;
			}

			const AKiraversePlayerState* OtherState = Other->GetPlayerState<AKiraversePlayerState>();
			if (!OtherState || OtherState->GetTeam() == ETeam::None || OtherState->GetTeam() == OwnTeam)
			{
				continue;
			}

			const float DistSq = FVector::DistSquared(SelfLocation, Other->GetActorLocation());
			if (DistSq < ClosestDistSq && HasLineOfSight(Self, Other))
			{
				ClosestDistSq = DistSq;
				Closest = Other;
			}
		}

		return Closest;
	}

	AKiraverseBombSite* FindClosestBombSite(const UWorld* World, const FVector& From)
	{
		if (!World)
		{
			return nullptr;
		}

		AKiraverseBombSite* Closest = nullptr;
		float ClosestDistSq = TNumericLimits<float>::Max();
		for (TActorIterator<AKiraverseBombSite> It(World); It; ++It)
		{
			const float DistSq = FVector::DistSquared(From, It->GetActorLocation());
			if (DistSq < ClosestDistSq)
			{
				ClosestDistSq = DistSq;
				Closest = *It;
			}
		}

		return Closest;
	}

	AKiraverseBomb* FindLiveBomb(const UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}

		// GameMode spawns exactly one bomb per round and destroys the previous one, so the first live match is the bomb.
		for (TActorIterator<AKiraverseBomb> It(World); It; ++It)
		{
			const EBombState State = It->GetBombState();
			if (State == EBombState::Idle || State == EBombState::Planted)
			{
				return *It;
			}
		}

		return nullptr;
	}
}
