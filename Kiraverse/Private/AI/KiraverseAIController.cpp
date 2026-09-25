#include "AI/KiraverseAIController.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Character/KiraverseCharacter.h"
#include "Game/KiraversePlayerState.h"
#include "AbilitySystemComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"

AKiraverseAIController::AKiraverseAIController()
{
	// Team lives on PlayerState, so a bot needs one just like a human player.
	bWantsPlayerState = true;
	PrimaryActorTick.bCanEverTick = false;
}

void AKiraverseAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	AimTarget = nullptr;
	AimErrorOffset = FRotator::ZeroRotator;
	bHasLookLocation = false;

	if (!BehaviorTreeAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("KiraverseAIController: %s has no BehaviorTreeAsset; the bot will stand still."), *GetNameSafe(this));
		return;
	}

	// RunBehaviorTree creates the blackboard from the tree's asset and restarts the tree, so each round begins with clean keys.
	if (!RunBehaviorTree(BehaviorTreeAsset))
	{
		UE_LOG(LogTemp, Warning, TEXT("KiraverseAIController: %s failed to run %s; check that the tree has a Blackboard Asset assigned."),
			*GetNameSafe(this), *GetNameSafe(BehaviorTreeAsset));
	}
}

void AKiraverseAIController::OnUnPossess()
{
	if (UBehaviorTreeComponent* TreeComponent = Cast<UBehaviorTreeComponent>(BrainComponent))
	{
		TreeComponent->StopTree(EBTStopMode::Safe);
	}

	CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Fire.GetTag());
	CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Bomb_Plant.GetTag());
	CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Bomb_Defuse.GetTag());

	AimTarget = nullptr;
	bHasLookLocation = false;
	Super::OnUnPossess();
}

ETeam AKiraverseAIController::GetOwnTeam() const
{
	const AKiraversePlayerState* PS = GetPlayerState<AKiraversePlayerState>();
	return PS ? PS->GetTeam() : ETeam::None;
}

void AKiraverseAIController::SetAimTarget(const AActor* Target)
{
	AimTarget = Target;
	if (Target)
	{
		RerollAimError();
	}
}

void AKiraverseAIController::RerollAimError()
{
	AimErrorOffset.Yaw = FMath::FRandRange(-AimErrorDegrees, AimErrorDegrees);
	AimErrorOffset.Pitch = FMath::FRandRange(-AimErrorDegrees, AimErrorDegrees);
}

void AKiraverseAIController::SetLookLocation(const FVector& WorldLocation)
{
	LookLocation = WorldLocation;
	bHasLookLocation = true;
}

void AKiraverseAIController::ClearLookLocation()
{
	bHasLookLocation = false;
}

void AKiraverseAIController::UpdateControlRotation(float DeltaTime, bool bUpdatePawn)
{
	// Replaces the engine's focus-based rotation so AimErrorDegrees is not overwritten by perfect aim.
	const APawn* ControlledPawn = GetPawn();
	const AActor* Target = AimTarget.Get();
	if (!ControlledPawn || (!Target && !bHasLookLocation))
	{
		Super::UpdateControlRotation(DeltaTime, bUpdatePawn);
		return;
	}

	// An aim target wins over a passive look point; only aiming at an enemy carries the random error.
	const FVector ToPoint = (Target ? Target->GetActorLocation() : LookLocation) - ControlledPawn->GetPawnViewLocation();
	FRotator Desired = ToPoint.Rotation();
	if (Target)
	{
		Desired.Yaw += AimErrorOffset.Yaw;
		Desired.Pitch += AimErrorOffset.Pitch;
	}

	const FRotator NewRotation = FMath::RInterpConstantTo(GetControlRotation(), Desired, DeltaTime, AimTurnRateDegrees);
	SetControlRotation(NewRotation);

	if (bUpdatePawn)
	{
		GetPawn()->FaceRotation(NewRotation, DeltaTime);
	}
}

bool AKiraverseAIController::TryActivateAbilityByTag(const FGameplayTag& Tag)
{
	// Named Self, not Character: AAIController already declares a Character member, which a same-named local would hide.
	const AKiraverseCharacter* Self = Cast<AKiraverseCharacter>(GetPawn());
	UAbilitySystemComponent* ASC = Self ? Self->GetAbilitySystemComponent() : nullptr;
	if (!ASC)
	{
		return false;
	}

	return ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(Tag));
}

void AKiraverseAIController::CancelAbilitiesByTag(const FGameplayTag& Tag)
{
	const AKiraverseCharacter* Self = Cast<AKiraverseCharacter>(GetPawn());
	UAbilitySystemComponent* ASC = Self ? Self->GetAbilitySystemComponent() : nullptr;
	if (!ASC)
	{
		return;
	}

	const FGameplayTagContainer TagContainer(Tag);
	ASC->CancelAbilities(&TagContainer);
}
