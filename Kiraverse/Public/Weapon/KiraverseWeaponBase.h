#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KiraverseWeaponBase.generated.h"

class UGA_Fire_Base;
class AKiraverseCharacter;
class USkeletalMeshComponent;

// Abstract weapon actor; concrete subclasses decide how a shot is actually delivered.
UCLASS(Abstract)
class KIRAVERSE_API AKiraverseWeaponBase : public AActor
{
	GENERATED_BODY()

public:
	AKiraverseWeaponBase();

	// Called by the currently granted fire ability; implemented per weapon type.
	virtual void Fire(AKiraverseCharacter* Shooter) PURE_VIRTUAL(AKiraverseWeaponBase::Fire, );

	float GetBaseDamage() const { return BaseDamage; }
	float GetFireRate() const { return FireRate; }
	TSubclassOf<UGA_Fire_Base> GetFireAbilityClass() const { return FireAbilityClass; }

	float GetRecoilPitch() const { return RecoilPitch; }
	float GetRecoilYaw() const { return RecoilYaw; }
	float GetRecoilRecoverySpeed() const { return RecoilRecoverySpeed; }
	float GetSpreadAngle() const { return SpreadAngle; }
	float GetZoomFOV() const { return ZoomFOV; }
	float GetAmmoCost() const { return AmmoCost; }
	float GetStaminaCost() const { return StaminaCost; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Weapon")
	TObjectPtr<USkeletalMeshComponent> WeaponMesh;

	// Damage per hit and seconds between shots; FireRate drives the Auto ability's timer.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Weapon")
	float BaseDamage = 10.f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Weapon")
	float FireRate = 0.15f;

	// Ability WeaponComponent grants on equip; this is what makes Single/Auto/Projectile differ.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Weapon")
	TSubclassOf<UGA_Fire_Base> FireAbilityClass;

	// Camera kick per shot, in degrees; consumed by the character's own camera/recoil system.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Recoil")
	float RecoilPitch = 1.5f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Recoil")
	float RecoilYaw = 0.5f;

	// Degrees/second the camera kick decays back toward zero once the recoil system reads it.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Recoil")
	float RecoilRecoverySpeed = 8.f;

	// Half-angle in degrees of the cone shots are randomized within; 0 is a straight line.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Spread")
	float SpreadAngle = 1.f;

	// Field of view while this weapon's zoom ability (GA_Zoom) is held.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Zoom")
	float ZoomFOV = 45.f;

	// Ammo and stamina spent per shot; read by GA_Fire_Base::CommitFireCost.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Cost")
	float AmmoCost = 1.f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Cost")
	float StaminaCost = 5.f;
};
