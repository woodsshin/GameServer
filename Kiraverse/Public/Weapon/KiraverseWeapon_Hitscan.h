#pragma once

#include "CoreMinimal.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "KiraverseWeapon_Hitscan.generated.h"

// Delivers damage with an instant line trace; used by both single-shot and auto weapons.
UCLASS()
class KIRAVERSE_API AKiraverseWeapon_Hitscan : public AKiraverseWeaponBase
{
	GENERATED_BODY()

public:
	virtual void Fire(AKiraverseCharacter* Shooter) override;

protected:
	// Max trace distance and the collision channel the line trace tests against.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Hitscan")
	float TraceRange = 10000.f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Hitscan")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;
};
