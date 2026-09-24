#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "GA_Bomb_Interact.generated.h"

class AKiraverseBomb;

// One input, two branches, decided by current carry state — same "single tag, branch inside
// ActivateAbility" shape as AKiraverseCharacter::ActivateAbilitiesWithTag routing to whichever
// ability owns Ability.Fire. No team gating: any character can pick up or drop the bomb (a
// dropped bomb is fair game for either side to grab — standard for this genre, and the task
// summary explicitly calls this out as unrestricted).
UCLASS()
class KIRAVERSE_API UGA_Bomb_Interact : public UKiraverseGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_Bomb_Interact();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

private:
	// Branch: owner already carrying a bomb -> drop it in place.
	void DoDrop(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo);

	// Branch: owner not carrying, and a pickup-able bomb is nearby -> pick it up.
	void DoPickUp(AKiraverseBomb* NearbyBomb, const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo);

	// Finds an Idle, unattached-to-a-carrier bomb within PickUpRadius of the owner. Returns
	// nullptr if none in range. Sweeps AKiraverseBomb actors in the world rather than relying on
	// overlap events, since the bomb has no overlap-generating trigger of its own (only its
	// CollisionComponent, which is a physics/visibility sphere, not a pickup trigger).
	AKiraverseBomb* FindNearbyPickupableBomb(const AActor* AvatarActor) const;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	float PickUpRadius = 150.f;
};
