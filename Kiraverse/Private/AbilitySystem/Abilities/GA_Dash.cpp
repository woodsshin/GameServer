#include "AbilitySystem/Abilities/GA_Dash.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

UGA_Dash::UGA_Dash()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Dash);
	ActivationOwnedTags.AddTag(KiraverseGameplayTags::State_Dashing);
	// Refuse to re-trigger while mid-dash or while the cooldown GE is still applied.
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::State_Dashing);
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::Cooldown_Dash);
}

void UGA_Dash::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	ACharacter* Character = Cast<ACharacter>(ActorInfo->AvatarActor.Get());
	if (!Character || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// Launch along current facing; keep whatever vertical velocity already exists.
	const FVector DashDirection = Character->GetActorForwardVector();
	Character->LaunchCharacter(DashDirection * DashImpulse, true, false);

	// Start the cooldown effect so Cooldown.Dash blocks re-activation while it runs.
	if (CooldownEffectClass && ActorInfo->AbilitySystemComponent.IsValid())
	{
		FGameplayEffectContextHandle EffectContext = ActorInfo->AbilitySystemComponent->MakeEffectContext();
		EffectContext.AddSourceObject(this);
		const FGameplayEffectSpecHandle SpecHandle =
			ActorInfo->AbilitySystemComponent->MakeOutgoingSpec(CooldownEffectClass, GetAbilityLevel(), EffectContext);
		if (SpecHandle.IsValid())
		{
			ActorInfo->AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
		}
	}

	// End the ability once the short active window of the dash has elapsed.
	Character->GetWorldTimerManager().SetTimer(DashTimerHandle, [this, Handle, ActorInfo, ActivationInfo]()
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
	}, DashDuration, false);
}

void UGA_Dash::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (ACharacter* Character = Cast<ACharacter>(ActorInfo->AvatarActor.Get()))
	{
		Character->GetWorldTimerManager().ClearTimer(DashTimerHandle);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
