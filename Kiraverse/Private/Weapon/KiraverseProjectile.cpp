#include "Weapon/KiraverseProjectile.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystem/Effects/GE_Damage.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"

AKiraverseProjectile::AKiraverseProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionComponent"));
	CollisionComponent->InitSphereRadius(10.f);
	CollisionComponent->SetCollisionProfileName(TEXT("Projectile"));
	CollisionComponent->OnComponentHit.AddDynamic(this, &AKiraverseProjectile::OnHit);
	SetRootComponent(CollisionComponent);

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->UpdatedComponent = CollisionComponent;
	ProjectileMovement->InitialSpeed = 3000.f;
	ProjectileMovement->MaxSpeed = 3000.f;
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
	ProjectileMovement->ProjectileGravityScale = 0.f;

	SetReplicates(true);
}

void AKiraverseProjectile::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(LifeSpanSeconds);
}

void AKiraverseProjectile::InitializeProjectile(AKiraverseCharacter* InShooter, float InDamage, float InSpeed)
{
	Shooter = InShooter;
	Damage = InDamage;

	if (ProjectileMovement)
	{
		ProjectileMovement->InitialSpeed = InSpeed;
		ProjectileMovement->MaxSpeed = InSpeed;
		ProjectileMovement->Velocity = GetActorForwardVector() * InSpeed;
	}

	if (Shooter)
	{
		// Prevents the projectile from immediately colliding with the actor that fired it.
		CollisionComponent->IgnoreActorWhenMoving(Shooter, true);
	}
}

void AKiraverseProjectile::OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	if (OtherActor && OtherActor != this && OtherActor != Shooter && Shooter)
	{
		UAbilitySystemComponent* SourceASC = Shooter->GetAbilitySystemComponent();
		IAbilitySystemInterface* TargetASI = Cast<IAbilitySystemInterface>(OtherActor);
		UAbilitySystemComponent* TargetASC = TargetASI ? TargetASI->GetAbilitySystemComponent() : nullptr;

		if (SourceASC && TargetASC)
		{
			FGameplayEffectContextHandle EffectContext = SourceASC->MakeEffectContext();
			EffectContext.AddInstigator(Shooter, this);
			EffectContext.AddHitResult(Hit);

			const FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(UGE_Damage::StaticClass(), 1.f, EffectContext);
			if (SpecHandle.IsValid())
			{
				SpecHandle.Data->SetSetByCallerMagnitude(KiraverseGameplayTags::Data_Damage, Damage);
				SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
			}
		}
	}

	Destroy();
}
