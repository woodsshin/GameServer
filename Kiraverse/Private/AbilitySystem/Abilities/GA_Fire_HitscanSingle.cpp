#include "AbilitySystem/Abilities/GA_Fire_HitscanSingle.h"
#include "Weapon/KiraverseWeaponBase.h"

void UGA_Fire_HitscanSingle::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo) || !CommitFireCost(ActorInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (AKiraverseWeaponBase* Weapon = GetEquippedWeapon(ActorInfo))
	{
		Weapon->Fire(GetKiraverseCharacter(ActorInfo));
	}

	// A single shot has no window to track; end right after firing.
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
