#include "AbilitySystem/Abilities/GA_Jump.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "GameFramework/Character.h"

UGA_Jump::UGA_Jump()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Jump);
}

void UGA_Jump::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (ACharacter* Character = Cast<ACharacter>(ActorInfo->AvatarActor.Get()))
	{
		// CharacterMovementComponent owns the rest of the jump arc from here.
		Character->Jump();
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
