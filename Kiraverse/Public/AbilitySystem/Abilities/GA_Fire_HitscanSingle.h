#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Abilities/GA_Fire_Base.h"
#include "GA_Fire_HitscanSingle.generated.h"

// One trace per activation; holding the input down does not repeat the shot.
UCLASS()
class KIRAVERSE_API UGA_Fire_HitscanSingle : public UGA_Fire_Base
{
	GENERATED_BODY()

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
