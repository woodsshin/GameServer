#include "AbilitySystem/KiraverseGameplayAbility.h"

UKiraverseGameplayAbility::UKiraverseGameplayAbility()
{
	// One instance per actor, predicted locally and confirmed by the server.
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
}
