#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CrowdRenderHost.generated.h"

class UInstancedStaticMeshComponent;

/**
 * Holds the single instanced-mesh component that draws the whole replicated crowd.
 *
 * All agents share one Actor, one component and one draw call.
 * Spawned locally by UCrowdRenderSubsystem; never replicated, never saved.
 */
UCLASS(NotPlaceable, Transient)
class MASSBUBBLERENDER_API ACrowdRenderHost : public AActor
{
	GENERATED_BODY()

public:
	ACrowdRenderHost();

	UInstancedStaticMeshComponent* GetInstances() const { return Instances; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Crowd")
	TObjectPtr<UInstancedStaticMeshComponent> Instances;
};
