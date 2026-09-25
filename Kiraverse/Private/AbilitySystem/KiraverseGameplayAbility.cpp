#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "AbilitySystem/KiraverseGameplayTags.h"

UKiraverseGameplayAbility::UKiraverseGameplayAbility()
{
	// One instance per actor, predicted locally and confirmed by the server.
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// Every subclass inherits this, so a dead character cannot activate any ability.
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::State_Dead);
}
