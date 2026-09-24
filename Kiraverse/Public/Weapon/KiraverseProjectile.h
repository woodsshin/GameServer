#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KiraverseProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class AKiraverseCharacter;

// Simple travelling projectile; applies GE_Damage to whatever it first hits.
UCLASS()
class KIRAVERSE_API AKiraverseProjectile : public AActor
{
	GENERATED_BODY()

public:
	AKiraverseProjectile();

	// Sets shooter/damage/speed right after spawning, before the projectile starts moving.
	void InitializeProjectile(AKiraverseCharacter* InShooter, float InDamage, float InSpeed);

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Projectile")
	TObjectPtr<USphereComponent> CollisionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Projectile")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	// Safety net so stray projectiles despawn even if they never hit anything.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Projectile")
	float LifeSpanSeconds = 5.f;

private:
	UPROPERTY()
	TObjectPtr<AKiraverseCharacter> Shooter;

	float Damage = 0.f;
};
