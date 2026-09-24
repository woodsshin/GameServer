#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombSite.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"

AKiraverseBomb::AKiraverseBomb()
{
	PrimaryActorTick.bCanEverTick = false;

	CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionComponent"));
	CollisionComponent->InitSphereRadius(12.f);
	CollisionComponent->SetCollisionProfileName(TEXT("PhysicsActor"));
	SetRootComponent(CollisionComponent);

	BombMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BombMesh"));
	BombMesh->SetupAttachment(CollisionComponent);
	BombMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	SetReplicates(true);
}

void AKiraverseBomb::DropAtCurrentLocation()
{
	if (!HasAuthority() || BombState != EBombState::Idle)
	{
		return;
	}

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	// State is already Idle (drop is only valid from Idle-while-carried); no state change or
	// OnRep needed, this just detaches. Kept as its own function so GA_Bomb_Interact's drop
	// branch and carrier-death teardown both have one call site.
}

bool AKiraverseBomb::PlantAtSite(AKiraverseBombSite* Site, float FuseTimeSeconds)
{
	if (!HasAuthority() || !Site || BombState != EBombState::Idle)
	{
		return false;
	}

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	AttachToComponent(Site->GetPlantSocketComponent(),
		FAttachmentTransformRules(EAttachmentRule::SnapToTarget, true));

	PlantedAtSite = Site;
	BombState = EBombState::Planted;
	OnRep_BombState(); // Server doesn't get its own OnRep firing; drive local FX/logic explicitly.

	FuseEndTime = GetWorld()->GetTimeSeconds() + FuseTimeSeconds;
	GetWorldTimerManager().SetTimer(FuseTimerHandle, this, &AKiraverseBomb::Detonate, FuseTimeSeconds, false);

	OnPlanted.Broadcast();
	return true;
}

bool AKiraverseBomb::Defuse()
{
	if (!HasAuthority() || BombState != EBombState::Planted)
	{
		return false;
	}

	GetWorldTimerManager().ClearTimer(FuseTimerHandle);
	FuseEndTime = 0.f;
	BombState = EBombState::Defused;
	OnRep_BombState();

	OnDefused.Broadcast();
	return true;
}

void AKiraverseBomb::Detonate()
{
	if (!HasAuthority() || BombState != EBombState::Planted)
	{
		return;
	}

	FuseEndTime = 0.f;
	BombState = EBombState::Exploded;
	OnRep_BombState();

	OnExploded.Broadcast();
}

float AKiraverseBomb::GetFuseTimeRemaining() const
{
	if (BombState != EBombState::Planted || FuseEndTime <= 0.f || !GetWorld())
	{
		return 0.f;
	}

	return FMath::Max(0.f, FuseEndTime - GetWorld()->GetTimeSeconds());
}

void AKiraverseBomb::OnRep_BombState()
{
	// Hook for client-side presentation (planted beep FX, defused/exploded VFX-swap, etc.).
	// Left as a no-op beyond the replication itself; the task summary doesn't specify bomb FX.
}

void AKiraverseBomb::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKiraverseBomb, BombState);
	DOREPLIFETIME(AKiraverseBomb, FuseEndTime);
}
