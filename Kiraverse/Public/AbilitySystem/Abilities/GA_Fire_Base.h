#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Fire_Base.generated.h"

class AKiraverseWeaponBase;
class AKiraverseCharacter;

// Shared plumbing for every fire ability: resolves the currently equipped weapon.
UCLASS(Abstract)
class KIRAVERSE_API UGA_Fire_Base : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Fire_Base();

protected:
	// Returns nullptr if the owning character has no weapon equipped right now.
	AKiraverseWeaponBase* GetEquippedWeapon(const FGameplayAbilityActorInfo* ActorInfo) const;

	AKiraverseCharacter* GetKiraverseCharacter(const FGameplayAbilityActorInfo* ActorInfo) const;

	// Applies GE_Cost_Fire using the equipped weapon's AmmoCost/StaminaCost. Returns false
	// (no effect applied) if either resource is currently insufficient.
	bool CommitFireCost(const FGameplayAbilityActorInfo* ActorInfo) const;
};
