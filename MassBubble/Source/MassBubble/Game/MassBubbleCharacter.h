#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MassBubbleCharacter.generated.h"

class UCameraComponent;

/**
 * Fly-through test pawn built on CharacterMovementComponent (client prediction + server correction).
 *
 * It is always in flying mode, configured identically on every machine. Toggling the mode locally would be a
 * state change the server does not know about (correction storms / rubber banding); a fly toggle would have
 * to travel in the saved-move compressed flags.
 */
UCLASS()
class MASSBUBBLE_API AMassBubbleCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AMassBubbleCharacter();

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveUp(float Value);
	void Turn(float Value);
	void LookUp(float Value);

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> Camera;
};
