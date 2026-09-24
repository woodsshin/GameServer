#include "Weapon/KiraverseWeapon_Projectile.h"
#include "Weapon/KiraverseProjectile.h"
#include "Character/KiraverseCharacter.h"
#include "Components/SkeletalMeshComponent.h"

void AKiraverseWeapon_Projectile::Fire(AKiraverseCharacter* Shooter)
{
	if (!Shooter || !ProjectileClass || !GetWorld())
	{
		return;
	}

	// Authority spawns the replicated projectile; the firing client's predicted call returns here.
	if (!Shooter->HasAuthority())
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = FindComponentByClass<USkeletalMeshComponent>();
	FTransform SpawnTransform = (Mesh && Mesh->DoesSocketExist(MuzzleSocketName))
		? Mesh->GetSocketTransform(MuzzleSocketName)
		: GetActorTransform();

	if (SpreadAngle > 0.f)
	{
		const FVector SpreadDirection = FMath::VRandCone(SpawnTransform.GetRotation().Vector(), FMath::DegreesToRadians(SpreadAngle));
		SpawnTransform.SetRotation(SpreadDirection.ToOrientationQuat());
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Shooter;
	SpawnParams.Instigator = Shooter;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (AKiraverseProjectile* Projectile = GetWorld()->SpawnActor<AKiraverseProjectile>(ProjectileClass, SpawnTransform, SpawnParams))
	{
		Projectile->InitializeProjectile(Shooter, GetBaseDamage(), ProjectileSpeed);
	}
}
