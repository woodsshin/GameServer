#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Jump.generated.h"

// Thin wrapper so Jump can be triggered like any other ability, through tags/input.
UCLASS()
class KIRAVERSE_API UGA_Jump : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Jump();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
