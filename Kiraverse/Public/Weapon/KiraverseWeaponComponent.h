#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AbilitySystemComponent.h"
#include "KiraverseWeaponComponent.generated.h"

class AKiraverseWeaponBase;

// Owns the character's current weapon and grants/revokes its fire ability on switch.
UCLASS(ClassGroup = (Kiraverse), meta = (BlueprintSpawnableComponent))
class KIRAVERSE_API UKiraverseWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKiraverseWeaponComponent();

	// Destroys the current weapon (if any) and equips a fresh instance of WeaponClass.
	UFUNCTION(BlueprintCallable, Category = "Kiraverse|Weapon")
	void EquipWeapon(TSubclassOf<AKiraverseWeaponBase> WeaponClass);

	// Convenience wrapper over EquipWeapon, e.g. bound to a "switch weapon" input.
	UFUNCTION(BlueprintCallable, Category = "Kiraverse|Weapon")
	void EquipWeaponAtIndex(int32 Index);

	AKiraverseWeaponBase* GetCurrentWeapon() const { return CurrentWeapon; }

	//~ Begin UActorComponent interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End UActorComponent interface

protected:
	virtual void BeginPlay() override;

	// Weapon classes available to this character; index 0 is equipped on BeginPlay.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Weapon")
	TArray<TSubclassOf<AKiraverseWeaponBase>> DefaultWeaponClasses;

	// Socket on the owner's mesh the equipped weapon attaches to.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Weapon")
	FName WeaponSocketName = TEXT("WeaponSocket");

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CurrentWeapon, Category = "Kiraverse|Weapon")
	TObjectPtr<AKiraverseWeaponBase> CurrentWeapon;

private:
	void UnequipCurrentWeapon();

	UFUNCTION()
	void OnRep_CurrentWeapon(AKiraverseWeaponBase* OldWeapon);

	FGameplayAbilitySpecHandle CurrentFireAbilityHandle;
};
