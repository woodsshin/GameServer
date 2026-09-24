#include "AbilitySystem/Abilities/GA_Bomb_Plant.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombComponent.h"
#include "Bomb/KiraverseBombSite.h"
#include "Character/KiraverseCharacter.h"
#include "Kismet/GameplayStatics.h"

UGA_Bomb_Plant::UGA_Bomb_Plant()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Bomb_Plant);
	ActivationOwnedTags.AddTag(KiraverseGameplayTags::State_Planting);

	// Attackers-only: block if the activating ASC does NOT have Team.Attackers. GAS's
	// ActivationBlockedTags blocks when the tag IS present, so a straight "block Team.Defenders"
	// also correctly allows activation for a player with neither tag (e.g. Team.None pre-assignment,
	// or spectators) — that's intentional here since GameMode always assigns a real team before
	// RestartPlayer grants any abilities, so a Team.None activator shouldn't be reachable in practice.
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::Team_Defenders);
}

bool UGA_Bomb_Plant::CanStartChannel(const AKiraverseCharacter* Character) const
{
	const UKiraverseBombComponent* BombComponent = Character ? Character->FindComponentByClass<UKiraverseBombComponent>() : nullptr;
	return BombComponent && BombComponent->HasBomb() && FindPlantSite(Character) != nullptr;
}

bool UGA_Bomb_Plant::OnChannelCompleted(AKiraverseCharacter* Character)
{
	UKiraverseBombComponent* BombComponent = Character ? Character->FindComponentByClass<UKiraverseBombComponent>() : nullptr;
	AKiraverseBomb* Bomb = BombComponent ? BombComponent->GetCarriedBomb() : nullptr;
	AKiraverseBombSite* Site = FindPlantSite(Character);

	// Re-resolve Site here rather than trusting whatever CanStartChannel saw at activation time:
	// the channel ran for RequiredChannelTime seconds, so re-confirm the character is still
	// actually in a site's zone right now. In practice CheckInterrupt's movement radius should
	// already have caught a character who wandered out, but this is the actual plant-time
	// authority check and shouldn't rely solely on that.
	if (!BombComponent || !Bomb || !Site)
	{
		return false;
	}

	// Order matters here: call PlantAtSite BEFORE ReleaseCarriedBomb, not after. PlantAtSite can
	// still fail its own internal guard (e.g. BombState no longer Idle — someone else interacted
	// with this exact bomb in the same frame), and if ReleaseCarriedBomb had already run at that
	// point, the component's CarriedBomb reference would already be cleared while the bomb actor
	// itself is still physically attached to this character's mesh (PlantAtSite's early-out
	// doesn't touch attachment — see AKiraverseBomb::PlantAtSite). That leaves the bomb both
	// "not carried" (per the component) and "still attached to a carrier" (per the actor) at the
	// same time — un-pickupable (GA_Bomb_Interact's pickup check requires no attach parent) and
	// un-droppable (nothing still references it as CarriedBomb to drop). Calling PlantAtSite first
	// means ReleaseCarriedBomb only ever runs after a plant that's actually going to stick.
	if (!Bomb->PlantAtSite(Site, BombFuseTime))
	{
		return false;
	}

	BombComponent->ReleaseCarriedBomb();
	return true;
}

AKiraverseBombSite* UGA_Bomb_Plant::FindPlantSite(const AKiraverseCharacter* Character) const
{
	if (!Character || !GetWorld())
	{
		return nullptr;
	}

	TArray<AActor*> Sites;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AKiraverseBombSite::StaticClass(), Sites);

	for (AActor* SiteActor : Sites)
	{
		if (AKiraverseBombSite* Site = Cast<AKiraverseBombSite>(SiteActor))
		{
			if (Site->IsCharacterInPlantZone(Character))
			{
				return Site;
			}
		}
	}

	return nullptr;
}
