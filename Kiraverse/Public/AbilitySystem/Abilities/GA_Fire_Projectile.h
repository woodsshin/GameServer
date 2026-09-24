#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Abilities/GA_Fire_Base.h"
#include "GA_Fire_Projectile.generated.h"

// Single activation spawns one projectile actor; the projectile carries the damage.
UCLASS()
class KIRAVERSE_API UGA_Fire_Projectile : public UGA_Fire_Base
{
	GENERATED_BODY()

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
