#include "AbilitySystem/Abilities/GA_Fire_Projectile.h"
#include "Weapon/KiraverseWeaponBase.h"

void UGA_Fire_Projectile::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo) || !CommitFireCost(ActorInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (AKiraverseWeaponBase* Weapon = GetEquippedWeapon(ActorInfo))
	{
		// Weapon::Fire spawns the AKiraverseProjectile; damage is applied on its hit.
		Weapon->Fire(GetKiraverseCharacter(ActorInfo));
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
