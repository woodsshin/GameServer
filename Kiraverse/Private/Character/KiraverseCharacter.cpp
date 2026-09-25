#include "Character/KiraverseCharacter.h"
#include "AbilitySystem/KiraverseAttributeSet.h"
#include "AbilitySystem/KiraverseGameplayAbility.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Weapon/KiraverseWeaponComponent.h"
#include "Bomb/KiraverseBombComponent.h"
#include "Bomb/KiraverseBomb.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"
#include "AbilitySystemComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"

AKiraverseCharacter::AKiraverseCharacter()
{
	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	AttributeSet = CreateDefaultSubobject<UKiraverseAttributeSet>(TEXT("AttributeSet"));

	WeaponComponent = CreateDefaultSubobject<UKiraverseWeaponComponent>(TEXT("WeaponComponent"));
	BombComponent = CreateDefaultSubobject<UKiraverseBombComponent>(TEXT("BombComponent"));

	bReplicates = true;
}

UAbilitySystemComponent* AKiraverseCharacter::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AKiraverseCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	InitializeAbilitySystem();
	GrantDefaultAbilities();

	if (const APlayerController* PlayerController = Cast<APlayerController>(NewController))
	{
		if (ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				if (DefaultMappingContext)
				{
					Subsystem->AddMappingContext(DefaultMappingContext, 0);
				}
			}
		}
	}
}

void AKiraverseCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	InitializeAbilitySystem();
}

void AKiraverseCharacter::InitializeAbilitySystem()
{
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}
}

void AKiraverseCharacter::GrantDefaultAbilities()
{
	if (!HasAuthority() || !AbilitySystemComponent)
	{
		return;
	}

	for (const TSubclassOf<UKiraverseGameplayAbility>& AbilityClass : DefaultAbilities)
	{
		if (AbilityClass)
		{
			AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1));
		}
	}
}

void AKiraverseCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (JumpAction)
		{
			EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateAbilitiesWithTag, KiraverseGameplayTags::Ability_Jump.GetTag());
		}
		if (DashAction)
		{
			EnhancedInput->BindAction(DashAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateAbilitiesWithTag, KiraverseGameplayTags::Ability_Dash.GetTag());
		}
		if (FireAction)
		{
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateAbilitiesWithTag, KiraverseGameplayTags::Ability_Fire.GetTag());
		}
		if (ZoomAction)
		{
			// Release is handled inside GA_Zoom via WaitInputRelease; only Started is bound here.
			EnhancedInput->BindAction(ZoomAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateAbilitiesWithTag, KiraverseGameplayTags::Ability_Zoom.GetTag());
		}
		if (BombInteractAction)
		{
			// GA_Bomb_Interact owns both PickUp and Drop tags, so either tag reaches it.
			EnhancedInput->BindAction(BombInteractAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateAbilitiesWithTag, KiraverseGameplayTags::Ability_Bomb_PickUp.GetTag());
		}
		if (BombActionAction)
		{
			// One key covers both Plant (attackers) and Defuse (defenders).
			EnhancedInput->BindAction(BombActionAction, ETriggerEvent::Started, this,
				&AKiraverseCharacter::ActivateBombChannelAbility);
		}
	}
}

void AKiraverseCharacter::ActivateAbilitiesWithTag(FGameplayTag Tag)
{
	if (AbilitySystemComponent && AbilitySystemComponent->AbilityActorInfo.IsValid()
		&& AbilitySystemComponent->AbilityActorInfo->IsLocallyControlled())
	{
		AbilitySystemComponent->TryActivateAbilitiesByTag(FGameplayTagContainer(Tag));
	}
}

void AKiraverseCharacter::ActivateBombChannelAbility()
{
	if (AbilitySystemComponent && AbilitySystemComponent->AbilityActorInfo.IsValid()
		&& AbilitySystemComponent->AbilityActorInfo->IsLocallyControlled())
	{
		// Only the team's granted ability (Plant or Defuse) will match.
		FGameplayTagContainer BombChannelTags;
		BombChannelTags.AddTag(KiraverseGameplayTags::Ability_Bomb_Plant);
		BombChannelTags.AddTag(KiraverseGameplayTags::Ability_Bomb_Defuse);
		AbilitySystemComponent->TryActivateAbilitiesByTag(BombChannelTags);
	}
}

void AKiraverseCharacter::HandleDeath(AKiraverseCharacter* Killer)
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	// Cancel first so an in-flight plant/defuse/fire ends cleanly before State.Dead blocks new activations.
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->CancelAllAbilities();
		AbilitySystemComponent->AddLooseGameplayTag(KiraverseGameplayTags::State_Dead);
	}

	// A carrier who dies must leave the bomb pickable; a bomb left attached to a corpse can never be picked up.
	if (BombComponent && BombComponent->HasBomb())
	{
		AKiraverseBomb* Bomb = BombComponent->GetCarriedBomb();
		BombComponent->ReleaseCarriedBomb();
		if (Bomb)
		{
			Bomb->DropAtCurrentLocation();
		}
	}

	bIsDead = true;
	EnterRagdoll();

	// Server does not receive its own OnRep, so broadcast here; clients broadcast from OnRep_IsDead.
	OnCharacterDied.Broadcast(this, Killer);
}

void AKiraverseCharacter::OnRep_IsDead()
{
	if (bIsDead)
	{
		EnterRagdoll();
		OnCharacterDied.Broadcast(this, nullptr);
	}
}

void AKiraverseCharacter::EnterRagdoll()
{
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	// Capsule stops blocking so the ragdoll (and other players) are not held up by an invisible cylinder.
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
	{
		SkeletalMesh->SetCollisionProfileName(RagdollCollisionProfileName);
		SkeletalMesh->SetAllBodiesSimulatePhysics(true);
		SkeletalMesh->WakeAllRigidBodies();
	}

	// Stop routing input to a corpse; the controller's view moves to the kill cam instead.
	if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		DisableInput(PlayerController);
	}

	if (RagdollFreezeDelay > 0.f)
	{
		GetWorldTimerManager().SetTimer(RagdollFreezeTimerHandle, [this]()
		{
			if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
			{
				SkeletalMesh->SetSimulatePhysics(false);
			}
		}, RagdollFreezeDelay, false);
	}
}

void AKiraverseCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKiraverseCharacter, bIsDead);
}
