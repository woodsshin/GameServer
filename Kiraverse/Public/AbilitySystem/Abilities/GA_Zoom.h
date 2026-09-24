#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Zoom.generated.h"

// Sets the equipped weapon's ZoomFOV while held; restores the default FOV on release.
UCLASS()
class KIRAVERSE_API UGA_Zoom : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Zoom();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// Bound to the WaitInputRelease task; ends the ability as soon as the button lifts.
	UFUNCTION()
	void OnZoomInputReleased(float TimeHeld);

	FGameplayAbilitySpecHandle CachedHandle;
	FGameplayAbilityActorInfo CachedActorInfo;
	FGameplayAbilityActivationInfo CachedActivationInfo;
};
