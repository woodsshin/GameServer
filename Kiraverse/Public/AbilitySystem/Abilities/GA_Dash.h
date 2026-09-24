#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Dash.generated.h"

class UGameplayEffect;

// Launches the character forward; ActivationBlockedTags enforce the cooldown window.
UCLASS()
class KIRAVERSE_API UGA_Dash : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Dash();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	// Impulse strength and how long the dash keeps its active state tag applied.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Dash")
	float DashImpulse = 1600.f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Dash")
	float DashDuration = 0.25f;

	// Self-applied on activation; its own duration is what actually gates the cooldown.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Dash")
	TSubclassOf<UGameplayEffect> CooldownEffectClass;

private:
	FTimerHandle DashTimerHandle;
};
