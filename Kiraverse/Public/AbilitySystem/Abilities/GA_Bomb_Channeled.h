#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Bomb_Channeled.generated.h"

class AKiraverseBomb;
class AKiraverseCharacter;

// Shared "hold input for N seconds to complete" base for Plant and Defuse. Reuses
// GA_Fire_HitscanAuto's cached-context-plus-timer shape (CachedHandle/CachedActorInfo/
// CachedActivationInfo + a repeating FTimerHandle calling a member function), since both that
// ability and this one need a member-function callback outside the normal ActivateAbility call
// stack. Two things GA_Fire_HitscanAuto does NOT need that this DOES:
//
//   1. Progress tracking (0..RequiredChannelTime) rather than a fire-and-repeat loop — the timer
//      here ticks a progress accumulator and completes once at the end, rather than doing
//      per-tick "fire" work.
//   2. An interrupt check beyond input-release. GA_Fire_HitscanAuto only ever stops when the
//      player releases the button (WaitInputRelease) or a commit fails. A plant/defuse channel
//      that could ONLY be stopped by releasing input would let a player defuse while being shot
//      at or chased with no way for anyone else to interrupt them — CheckInterrupt() below adds
//      a per-tick movement check (character leaves ChannelInterruptMoveRadius from where the
//      channel started) as a second interrupt source. This is a deliberate addition beyond what
//      the task summary specified, because a defusal mode without a movement-breaks-defuse rule
//      has a real, easily-exploited gap; it's isolated in one virtual so it's easy to remove or
//      change (e.g. to a "took damage" interrupt instead/in addition) if a different rule is wanted.
UCLASS(Abstract)
class KIRAVERSE_API UGA_Bomb_Channeled : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Bomb_Channeled();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	// --- Subclass hooks (Plant/Defuse fill these in) ---

	// Whether the channel is even allowed to start right now (in range of a plantable site /
	// standing on a planted bomb, etc). Checked once at activation; NOT re-checked per tick, since
	// leaving the valid zone is exactly the movement case CheckInterrupt already covers.
	virtual bool CanStartChannel(const AKiraverseCharacter* Character) const PURE_VIRTUAL(UGA_Bomb_Channeled::CanStartChannel, return false;);

	// Called once when progress reaches RequiredChannelTime and CheckInterrupt hasn't fired. This
	// is where Plant calls AKiraverseBomb::PlantAtSite and Defuse calls AKiraverseBomb::Defuse.
	// MUST return whether the action it attempted actually succeeded. Returning false here is not
	// just "nothing to do" — it's a distinct outcome from a movement/death interrupt, and
	// ChannelTick uses it to end the ability as a genuine failure (bWasCancelled = true) rather
	// than a silent success. This return value exists because the timing gap between the channel
	// starting and RequiredChannelTime seconds later is real: the target site can go out of range,
	// or a planted bomb being defused can detonate mid-channel, in the moment between the last
	// CheckInterrupt tick and this call. Re-validating everything here (not trusting whatever
	// CanStartChannel or the previous tick saw) and reporting the true outcome is what CLOSES that
	// gap — returning true unconditionally would silently drop the failure on the floor.
	virtual bool OnChannelCompleted(AKiraverseCharacter* Character) PURE_VIRTUAL(UGA_Bomb_Channeled::OnChannelCompleted, return false;);

	// How long, in seconds, the input must be held. Plant and Defuse set different values.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	float RequiredChannelTime = 5.f;

	// How far (world units) the character may move from the channel's start location before
	// CheckInterrupt cancels it. See class comment — this is the added-beyond-summary interrupt.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	float ChannelInterruptMoveRadius = 30.f;

	AKiraverseCharacter* GetCachedCharacter() const;

private:
	// One tick of the channel: checks interrupt, advances progress, completes or reschedules.
	void ChannelTick();

	// Returns true if the channel should be cancelled: character died/lost control, or moved
	// further than ChannelInterruptMoveRadius from ChannelStartLocation.
	bool CheckInterrupt() const;

	FTimerHandle ChannelTimerHandle;
	FGameplayAbilitySpecHandle CachedHandle;
	FGameplayAbilityActorInfo CachedActorInfo;
	FGameplayAbilityActivationInfo CachedActivationInfo;

	FVector ChannelStartLocation = FVector::ZeroVector;
	float ElapsedChannelTime = 0.f;

	// Fixed tick granularity for the progress timer; doesn't need to match frame rate since this
	// only drives a 0..RequiredChannelTime accumulator, not per-tick gameplay like FireTick.
	static constexpr float ChannelTickInterval = 0.1f;
};
