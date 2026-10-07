#include "CrowdRenderHost.h"

#include "Components/InstancedStaticMeshComponent.h"

ACrowdRenderHost::ACrowdRenderHost()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Instances"));
	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision); // no physics state for hundreds of instances
	Instances->SetCastShadow(false);
	Instances->SetCanEverAffectNavigation(false);
	SetRootComponent(Instances);
}
