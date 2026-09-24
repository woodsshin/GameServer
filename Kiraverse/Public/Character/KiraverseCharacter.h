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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Abilities")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Abilities")
	TObjectPtr<UKiraverseAttributeSet> AttributeSet;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Weapon")
	TObjectPtr<UKiraverseWeaponComponent> WeaponComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<UKiraverseBombComponent> BombComponent;

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
};
