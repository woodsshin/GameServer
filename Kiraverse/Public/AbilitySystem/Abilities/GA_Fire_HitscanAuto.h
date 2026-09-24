#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Abilities/GA_Fire_Base.h"
#include "GA_Fire_HitscanAuto.generated.h"

// Repeats the trace on a timer, tied to the weapon's fire rate, until input is released.
UCLASS()
class KIRAVERSE_API UGA_Fire_HitscanAuto : public UGA_Fire_Base
{
	GENERATED_BODY()

public:
	UGA_Fire_HitscanAuto();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// One tick of the auto-fire loop: commits cost/cooldown, then fires again.
	void FireTick();

	// Bound to the WaitInputRelease task; ends the ability as soon as the button lifts.
	UFUNCTION()
	void OnFireInputReleased(float TimeHeld);

	FTimerHandle AutoFireTimerHandle;
	FGameplayAbilitySpecHandle CachedHandle;
	FGameplayAbilityActorInfo CachedActorInfo;
	FGameplayAbilityActivationInfo CachedActivationInfo;
};
