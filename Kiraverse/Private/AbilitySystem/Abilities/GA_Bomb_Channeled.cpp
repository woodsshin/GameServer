#include "AbilitySystem/Abilities/GA_Bomb_Channeled.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystemComponent.h"
#include "TimerManager.h"

UGA_Bomb_Channeled::UGA_Bomb_Channeled()
{
	// Self-stacking guard: refuse to start a second channel while one is already running on this
	// ASC. Same ActivationBlockedTags idiom GA_Dash uses for its own cooldown gate.
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::Cooldown_BombAction);
}

void UGA_Bomb_Channeled::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	AKiraverseCharacter* Character = ActorInfo ? Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!Character || !CanStartChannel(Character))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	CachedHandle = Handle;
	CachedActorInfo = *ActorInfo;
	CachedActivationInfo = ActivationInfo;
	ChannelStartLocation = Character->GetActorLocation();
	ElapsedChannelTime = 0.f;

	// Grants both the state tag (subclass adds it via ActivationOwnedTags in its own constructor,
	// e.g. State.Planting/State.Defusing) and this shared cooldown-style guard tag.
	if (ActorInfo->AbilitySystemComponent.IsValid())
	{
		ActorInfo->AbilitySystemComponent->AddLooseGameplayTag(KiraverseGameplayTags::Cooldown_BombAction);
	}

	GetWorld()->GetTimerManager().SetTimer(ChannelTimerHandle, this,
		&UGA_Bomb_Channeled::ChannelTick, ChannelTickInterval, true);
}

void UGA_Bomb_Channeled::ChannelTick()
{
	if (CheckInterrupt())
	{
		EndAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo, true, true);
		return;
	}

	ElapsedChannelTime += ChannelTickInterval;
	if (ElapsedChannelTime < RequiredChannelTime)
	{
		return;
	}

	// bWasCancelled mirrors whatever OnChannelCompleted actually achieved, not just "we reached
	// RequiredChannelTime". A character with no valid completion target left (see the class-level
	// comment on OnChannelCompleted for how that can happen even after CheckInterrupt passed every
	// tick) ends here exactly like any other failed activation, not as a silent no-op success.
	AKiraverseCharacter* Character = GetCachedCharacter();
	const bool bCompletedSuccessfully = Character && OnChannelCompleted(Character);

	EndAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo, true, !bCompletedSuccessfully);
}

bool UGA_Bomb_Channeled::CheckInterrupt() const
{
	const AKiraverseCharacter* Character = GetCachedCharacter();
	if (!Character)
	{
		return true; // Character gone (died, disconnected) — nothing left to channel.
	}

	// NOTE: this radius is independent of whatever zone CanStartChannel checked (bomb site trigger
	// extent, or defuse-range-of-planted-bomb). If ChannelInterruptMoveRadius is configured larger
	// than the actual plant-zone/defuse-range, a character could walk outside that zone without
	// tripping this check. Keep this value <= the tightest of those zones, or add an explicit
	// "still satisfies CanStartChannel" re-check here instead of/alongside the distance check if
	// zone size and interrupt radius need to vary independently per level.
	const float DistanceMoved = FVector::Dist(Character->GetActorLocation(), ChannelStartLocation);
	return DistanceMoved > ChannelInterruptMoveRadius;
}

void UGA_Bomb_Channeled::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ChannelTimerHandle);
	}

	if (ActorInfo && ActorInfo->AbilitySystemComponent.IsValid())
	{
		ActorInfo->AbilitySystemComponent->RemoveLooseGameplayTag(KiraverseGameplayTags::Cooldown_BombAction);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

AKiraverseCharacter* UGA_Bomb_Channeled::GetCachedCharacter() const
{
	return Cast<AKiraverseCharacter>(CachedActorInfo.AvatarActor.Get());
}
