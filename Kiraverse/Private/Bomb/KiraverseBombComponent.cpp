#include "Bomb/KiraverseBombComponent.h"
#include "Bomb/KiraverseBomb.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"

UKiraverseBombComponent::UKiraverseBombComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UKiraverseBombComponent::AttachBombToCarrier(AKiraverseBomb* Bomb)
{
	const AKiraverseCharacter* OwningCharacter = Cast<AKiraverseCharacter>(GetOwner());
	if (!OwningCharacter || !Bomb || !OwningCharacter->HasAuthority())
	{
		return;
	}

	CarriedBomb = Bomb;
	Bomb->AttachToComponent(OwningCharacter->GetMesh(),
		FAttachmentTransformRules(EAttachmentRule::SnapToTarget, true), BombCarrySocketName);

	if (UAbilitySystemComponent* ASC = OwningCharacter->GetAbilitySystemComponent())
	{
		ASC->AddLooseGameplayTag(KiraverseGameplayTags::State_CarryingBomb);
	}
}

void UKiraverseBombComponent::ReleaseCarriedBomb()
{
	const AKiraverseCharacter* OwningCharacter = Cast<AKiraverseCharacter>(GetOwner());
	if (!OwningCharacter || !CarriedBomb || !OwningCharacter->HasAuthority())
	{
		return;
	}

	if (UAbilitySystemComponent* ASC = OwningCharacter->GetAbilitySystemComponent())
	{
		ASC->RemoveLooseGameplayTag(KiraverseGameplayTags::State_CarryingBomb);
	}

	CarriedBomb = nullptr;
	// Note: does NOT detach the bomb actor itself — see header comment. Caller is responsible for
	// AKiraverseBomb::DropAtCurrentLocation() or PlantAtSite() immediately after this returns.
}

void UKiraverseBombComponent::OnRep_CarriedBomb(AKiraverseBomb* OldBomb)
{
	// Client-side hook for HUD ("you have the bomb" prompt, carry-socket VFX). Left as a no-op
	// beyond replication, same as AKiraverseBomb::OnRep_BombState — task summary doesn't specify
	// bomb-carry UI.
}

void UKiraverseBombComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UKiraverseBombComponent, CarriedBomb);
}
