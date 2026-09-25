#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "KiraverseCharacter.generated.h"

class UAbilitySystemComponent;
class UKiraverseAttributeSet;
class UKiraverseGameplayAbility;
class UKiraverseWeaponComponent;
class UKiraverseBombComponent;
class UInputMappingContext;
class UInputAction;
class AKiraverseCharacter;

// Fired on every machine (server directly, clients via OnRep) the moment this character dies.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKiraverseCharacterDied, AKiraverseCharacter*, DeadCharacter, AKiraverseCharacter*, Killer);

// Base character for Kiraverse: owns the ASC and binds Enhanced Input to ability tags.
UCLASS()
class KIRAVERSE_API AKiraverseCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AKiraverseCharacter();

	//~ Begin IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~ End IAbilitySystemInterface

	UFUNCTION(BlueprintPure, Category = "Kiraverse")
	UKiraverseWeaponComponent* GetWeaponComponent() const { return WeaponComponent; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse")
	UKiraverseBombComponent* GetBombComponent() const { return BombComponent; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Death")
	bool IsDead() const { return bIsDead; }

	// Authority only. Called by the attribute set when Health reaches 0. Idempotent.
	void HandleDeath(AKiraverseCharacter* Killer);

	UPROPERTY(BlueprintAssignable, Category = "Kiraverse|Death")
	FKiraverseCharacterDied OnCharacterDied;

protected:
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// Inits the ASC's actor info; called from both PossessedBy (server) and
	// OnRep_PlayerState (owning and simulated clients).
	void InitializeAbilitySystem();

	// Grants every ability in DefaultAbilities; authority only.
	void GrantDefaultAbilities();

	// Shared handler for Jump/Dash/Zoom/Fire input: activates whatever ability carries Tag.
	void ActivateAbilitiesWithTag(FGameplayTag Tag);

	// Tries the Plant and Defuse abilities together on the shared bomb-action input.
	void ActivateBombChannelAbility();

	//~ Begin AActor interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	UFUNCTION()
	void OnRep_IsDead();

	// Runs on every machine: disables movement/collision, releases the bomb tag state, and ragdolls the mesh.
	void EnterRagdoll();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Abilities")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Abilities")
	TObjectPtr<UKiraverseAttributeSet> AttributeSet;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Weapon")
	TObjectPtr<UKiraverseWeaponComponent> WeaponComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<UKiraverseBombComponent> BombComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_IsDead, Category = "Kiraverse|Death")
	bool bIsDead = false;

	// Collision profile the mesh switches to while ragdolling; must collide with the world but not pawns.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Death")
	FName RagdollCollisionProfileName = TEXT("Ragdoll");

	// Seconds after death before the ragdoll is frozen to save physics cost; 0 keeps it simulating.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Death", meta = (ClampMin = "0"))
	float RagdollFreezeDelay = 5.f;

	// Jump/Dash/Zoom are granted here; Fire abilities are granted per-weapon by WeaponComponent.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Abilities")
	TArray<TSubclassOf<UKiraverseGameplayAbility>> DefaultAbilities;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> DashAction;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> ZoomAction;

	// Tap input for picking up or dropping the bomb.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> BombInteractAction;

	// Input for planting (attackers) or defusing (defenders) the bomb.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Input")
	TObjectPtr<UInputAction> BombActionAction;

private:
	FTimerHandle RagdollFreezeTimerHandle;
};
