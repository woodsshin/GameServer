#include "AbilitySystem/Abilities/GA_Fire_HitscanAuto.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Abilities/Tasks/AbilityTask_WaitInputRelease.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "TimerManager.h"

UGA_Fire_HitscanAuto::UGA_Fire_HitscanAuto()
{
	ActivationOwnedTags.AddTag(KiraverseGameplayTags::State_Firing);
}

void UGA_Fire_HitscanAuto::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// Cache context: the timer callback fires outside the normal ability call stack.
	CachedHandle = Handle;
	CachedActorInfo = *ActorInfo;
	CachedActivationInfo = ActivationInfo;

	UAbilityTask_WaitInputRelease* WaitRelease = UAbilityTask_WaitInputRelease::WaitInputRelease(this, false);
	WaitRelease->OnRelease.AddDynamic(this, &UGA_Fire_HitscanAuto::OnFireInputReleased);
	WaitRelease->ReadyForActivation();

	const AKiraverseWeaponBase* Weapon = GetEquippedWeapon(ActorInfo);
	const float Interval = Weapon ? Weapon->GetFireRate() : 0.1f;

	FireTick();
	GetWorld()->GetTimerManager().SetTimer(AutoFireTimerHandle, this, &UGA_Fire_HitscanAuto::FireTick, Interval, true);
}

void UGA_Fire_HitscanAuto::FireTick()
{
	if (!CommitAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo) || !CommitFireCost(&CachedActorInfo))
	{
		EndAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo, true, true);
		return;
	}

	if (AKiraverseWeaponBase* Weapon = GetEquippedWeapon(&CachedActorInfo))
	{
		Weapon->Fire(GetKiraverseCharacter(&CachedActorInfo));
	}
}

void UGA_Fire_HitscanAuto::OnFireInputReleased(float TimeHeld)
{
	EndAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo, true, false);
}

void UGA_Fire_HitscanAuto::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(AutoFireTimerHandle);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
