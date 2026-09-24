#pragma once

#include "CoreMinimal.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "KiraverseWeapon_Projectile.generated.h"

class AKiraverseProjectile;

// Spawns a travelling projectile actor instead of resolving damage instantly.
UCLASS()
class KIRAVERSE_API AKiraverseWeapon_Projectile : public AKiraverseWeaponBase
{
	GENERATED_BODY()

public:
	virtual void Fire(AKiraverseCharacter* Shooter) override;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Projectile")
	TSubclassOf<AKiraverseProjectile> ProjectileClass;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Projectile")
	float ProjectileSpeed = 3000.f;

	// Socket on the weapon mesh the projectile spawns from, e.g. the barrel tip.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Projectile")
	FName MuzzleSocketName = TEXT("Muzzle");
};
