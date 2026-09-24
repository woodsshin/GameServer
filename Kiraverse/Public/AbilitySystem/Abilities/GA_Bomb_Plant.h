#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Abilities/GA_Bomb_Channeled.h"
#include "GA_Bomb_Plant.generated.h"

class AKiraverseBombSite;

// Attacker-only channel that plants the carried bomb at whichever AKiraverseBombSite the
// character is standing in. Team gating is ActivationBlockedTags(Team.Defenders), same idiom
// GA_Dash uses for its own cooldown gate — not a runtime Cast/branch inside ActivateAbility.
UCLASS()
class KIRAVERSE_API UGA_Bomb_Plant : public UGA_Bomb_Channeled
{
	GENERATED_BODY()

public:
	UGA_Bomb_Plant();

protected:
	// Must be carrying the bomb AND standing inside a bomb site's plant zone.
	virtual bool CanStartChannel(const AKiraverseCharacter* Character) const override;

	// Calls AKiraverseBombComponent::ReleaseCarriedBomb then AKiraverseBomb::PlantAtSite(Site,
	// FuseTime). AKiraverseBomb::PlantAtSite broadcasts OnPlanted itself, which is what actually
	// tells GameMode to switch phases — this class doesn't reach into GameMode directly, matching
	// how no other UKiraverseGameplayAbility subclass references AKiraverseGameMode. Returns
	// PlantAtSite's own result: false if the bomb/component/site couldn't all be re-resolved, or
	// if PlantAtSite itself refused (e.g. bomb state was no longer Idle by the time this ran).
	virtual bool OnChannelCompleted(AKiraverseCharacter* Character) override;

	// NOTE: intentionally duplicated with AKiraverseGameMode::BombFuseTime rather than the ability
	// reading it off GameMode. Keeping abilities GameMode-agnostic (consistent with every other
	// GA_* class in this codebase) means these two values must be kept in sync by whoever configures
	// the project's Blueprint defaults — there's no code-level enforcement of that here. Flagging
	// this explicitly since it's the one place this design trades a single-source-of-truth value
	// for looser coupling.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	float BombFuseTime = 45.f;

private:
	// Resolves the AKiraverseBombSite the character is currently standing in, if any. Shared by
	// CanStartChannel (existence check) and OnChannelCompleted (actual plant target) so both
	// agree on the same site even if the character is technically overlapping more than one.
	AKiraverseBombSite* FindPlantSite(const AKiraverseCharacter* Character) const;
};
