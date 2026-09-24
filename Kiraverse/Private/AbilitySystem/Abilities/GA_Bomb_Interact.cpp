#include "AbilitySystem/Abilities/GA_Bomb_Interact.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombComponent.h"
#include "Character/KiraverseCharacter.h"
#include "Kismet/GameplayStatics.h"

UGA_Bomb_Interact::UGA_Bomb_Interact()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Bomb_PickUp);
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Bomb_Drop);
}

void UGA_Bomb_Interact::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get());
	UKiraverseBombComponent* BombComponent = Character ? Character->FindComponentByClass<UKiraverseBombComponent>() : nullptr;
	if (!Character || !BombComponent)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (BombComponent->HasBomb())
	{
		DoDrop(Handle, ActorInfo, ActivationInfo);
		return;
	}

	if (AKiraverseBomb* NearbyBomb = FindNearbyPickupableBomb(Character))
	{
		DoPickUp(NearbyBomb, Handle, ActorInfo, ActivationInfo);
		return;
	}

	// Nothing to do — not carrying, and nothing pickupable nearby.
	EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
}

void UGA_Bomb_Interact::DoDrop(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo)
{
	AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get());
	UKiraverseBombComponent* BombComponent = Character ? Character->FindComponentByClass<UKiraverseBombComponent>() : nullptr;
	AKiraverseBomb* Bomb = BombComponent ? BombComponent->GetCarriedBomb() : nullptr;

	if (BombComponent && Bomb)
	{
		BombComponent->ReleaseCarriedBomb();
		Bomb->DropAtCurrentLocation();
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_Bomb_Interact::DoPickUp(AKiraverseBomb* NearbyBomb, const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo)
{
	AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get());
	UKiraverseBombComponent* BombComponent = Character ? Character->FindComponentByClass<UKiraverseBombComponent>() : nullptr;

	if (BombComponent && NearbyBomb)
	{
		BombComponent->AttachBombToCarrier(NearbyBomb);
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

AKiraverseBomb* UGA_Bomb_Interact::FindNearbyPickupableBomb(const AActor* AvatarActor) const
{
	if (!AvatarActor || !GetWorld())
	{
		return nullptr;
	}

	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AKiraverseBomb::StaticClass(), FoundBombs);

	const FVector OwnerLocation = AvatarActor->GetActorLocation();
	for (AActor* FoundActor : FoundBombs)
	{
		AKiraverseBomb* Bomb = Cast<AKiraverseBomb>(FoundActor);
		// Idle covers both "dropped in the world" and "carried by someone else" — the latter
		// is excluded by the attachment check, since a carried bomb is attached to a character's
		// mesh and a dropped one has no attach parent.
		if (Bomb && Bomb->GetBombState() == EBombState::Idle && !Bomb->GetAttachParentActor()
			&& FVector::DistSquared(OwnerLocation, Bomb->GetActorLocation()) <= FMath::Square(PickUpRadius))
		{
			return Bomb;
		}
	}

	return nullptr;
}
