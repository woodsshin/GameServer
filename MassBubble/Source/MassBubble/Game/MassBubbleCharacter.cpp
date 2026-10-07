#include "Game/MassBubbleCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"

AMassBubbleCharacter::AMassBubbleCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	GetCapsuleComponent()->InitCapsuleSize(40.f, 90.f);

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->DefaultLandMovementMode = MOVE_Flying;
	Movement->GravityScale = 0.f;
	Movement->MaxFlySpeed = 3000.f;               // 30 m/s: crosses a 128 m region in ~4 s, good for stress tests
	Movement->MaxAcceleration = 8000.f;
	Movement->BrakingDecelerationFlying = 6000.f;
	Movement->bOrientRotationToMovement = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
	Camera->bUsePawnControlRotation = true;
}

void AMassBubbleCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Same call on server and client => no desync of the movement mode.
	GetCharacterMovement()->SetMovementMode(MOVE_Flying);
}

void AMassBubbleCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Same trick ADefaultPawn uses: register engine-defined mappings once, so no DefaultInput.ini is needed.
	static bool bMappingsAdded = false;
	if (!bMappingsAdded)
	{
		bMappingsAdded = true;
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveForward", EKeys::W, 1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveForward", EKeys::S, -1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveRight", EKeys::D, 1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveRight", EKeys::A, -1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveUp", EKeys::E, 1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveUp", EKeys::SpaceBar, 1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_MoveUp", EKeys::Q, -1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_Turn", EKeys::MouseX, 1.f));
		UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping("Opt_LookUp", EKeys::MouseY, -1.f));
	}

	PlayerInputComponent->BindAxis("Opt_MoveForward", this, &AMassBubbleCharacter::MoveForward);
	PlayerInputComponent->BindAxis("Opt_MoveRight", this, &AMassBubbleCharacter::MoveRight);
	PlayerInputComponent->BindAxis("Opt_MoveUp", this, &AMassBubbleCharacter::MoveUp);
	PlayerInputComponent->BindAxis("Opt_Turn", this, &AMassBubbleCharacter::Turn);
	PlayerInputComponent->BindAxis("Opt_LookUp", this, &AMassBubbleCharacter::LookUp);
}

void AMassBubbleCharacter::MoveForward(float Value)
{
	if (Value != 0.f && Controller != nullptr)
	{
		AddMovementInput(Controller->GetControlRotation().Vector(), Value);
	}
}

void AMassBubbleCharacter::MoveRight(float Value)
{
	if (Value != 0.f && Controller != nullptr)
	{
		const FRotator Yaw(0.f, Controller->GetControlRotation().Yaw, 0.f);
		AddMovementInput(FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y), Value);
	}
}

void AMassBubbleCharacter::MoveUp(float Value)
{
	if (Value != 0.f)
	{
		AddMovementInput(FVector::UpVector, Value);
	}
}

void AMassBubbleCharacter::Turn(float Value)
{
	AddControllerYawInput(Value);
}

void AMassBubbleCharacter::LookUp(float Value)
{
	AddControllerPitchInput(Value);
}
