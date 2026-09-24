#include "Weapon/KiraverseWeaponBase.h"
#include "Components/SkeletalMeshComponent.h"

AKiraverseWeaponBase::AKiraverseWeaponBase()
{
	PrimaryActorTick.bCanEverTick = false;

	WeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
	SetRootComponent(WeaponMesh);

	bReplicates = true;
	SetReplicateMovement(true);
}
