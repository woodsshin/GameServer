#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Game/KiraverseTypes.h"
#include "GameplayTagContainer.h"
#include "KiraverseAIController.generated.h"

class AKiraverseCharacter;
class UBehaviorTree;

// Bot controller: hosts the behavior tree and owns frame-rate aiming; all decisions live in the tree's nodes.
UCLASS()
class KIRAVERSE_API AKiraverseAIController : public AAIController
{
	GENERATED_BODY()

public:
	AKiraverseAIController();

	//~ Begin AController interface
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void UpdateControlRotation(float DeltaTime, bool bUpdatePawn = true) override;
	//~ End AController interface

	// Called by the engage task each tick: aim at Target with a fresh random error, or pass nullptr to stop aiming.
	void SetAimTarget(const AActor* Target);

	// Rolls a new yaw/pitch error; called by tasks on their own cadence so aim wobbles instead of jittering per frame.
	void RerollAimError();

	// Looks toward a world point with no aim error, for scanning; cleared by SetAimTarget(nullptr) or ClearLookLocation.
	void SetLookLocation(const FVector& WorldLocation);
	void ClearLookLocation();

	// Starts a plant/defuse/fire/pickup ability by tag on the possessed character; false if activation failed.
	bool TryActivateAbilityByTag(const FGameplayTag& Tag);

	// Cancels every ability matching Tag on the possessed character.
	void CancelAbilitiesByTag(const FGameplayTag& Tag);

	ETeam GetOwnTeam() const;

protected:
	// Blackboard and tree assigned in the Blueprint subclass; the tree must use the KiraverseBB key names.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bot")
	TObjectPtr<UBehaviorTree> BehaviorTreeAsset;

	// Max degrees per second the bot can turn toward its target; keeps flick-shots out of reach.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bot")
	float AimTurnRateDegrees = 360.f;

	// Degrees of random aim error so bots are beatable.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bot")
	float AimErrorDegrees = 4.f;

private:
	// Weak so a destroyed target (a killed enemy, a round-end respawn) can never leave a dangling aim.
	TWeakObjectPtr<const AActor> AimTarget;

	// Yaw/pitch error added on top of the true direction to the target.
	FRotator AimErrorOffset = FRotator::ZeroRotator;

	// Passive look point used when there is no AimTarget; bHasLookLocation gates it.
	FVector LookLocation = FVector::ZeroVector;
	bool bHasLookLocation = false;
};
