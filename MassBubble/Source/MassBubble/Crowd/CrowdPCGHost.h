#pragma once

#include "CoreMinimal.h"
#include "Crowd/CrowdPlan.h"
#include "GameFramework/Actor.h"
#include "CrowdPCGHost.generated.h"

class UBoxComponent;
class UPCGComponent;
class UPCGGraphInterface;

/**
 * Server side, transient host of ONE PCG graph run.
 *
 * A PCG component takes its generation bounds from the actor it lives on. The planner (UCrowdPCGPlanner) therefore spawns
 * one of these per region: a box as big as the region plus a PCG component that runs the project's spawn graph once
 * (GenerateOnDemand, triggered from code). When the graph is done the planner reads the points and destroys the host.
 *
 * A new host per run instead of a pool is deliberate: PCG caches the input data it derives from the actor (its bounds), and
 * an actor that is moved to the next region would hand the next run the cached bounds of the previous one.
 *
 * Never replicated, never saved, no tick.
 */
UCLASS(NotPlaceable, Transient)
class MASSBUBBLE_API ACrowdPCGHost : public AActor
{
	GENERATED_BODY()

public:
	ACrowdPCGHost();

	virtual void PostInitializeComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Between SpawnActorDeferred and FinishSpawning: sizes the generation bounds (SizeCm x SizeCm, +-HalfHeightCm tall). */
	void SetGenerationBounds(double SizeCm, double HalfHeightCm);

	/** After FinishSpawning: runs Graph with the given seed. False if there is nothing to run. */
	bool StartGeneration(UPCGGraphInterface* Graph, int32 Seed);

	/** True once PCG reported the end of the run (successfully or cancelled). */
	bool IsFinished() const { return bFinished; }
	bool Succeeded() const { return bSucceeded; }

	/** Appends the points the graph delivered on its output node (world space). Returns how many were appended. */
	int32 CollectPoints(TArray<FCrowdPlanPoint>& OutPoints) const;

private:
	void HandleGenerated(UPCGComponent* Component);
	void HandleCancelled(UPCGComponent* Component);

	UPROPERTY(VisibleAnywhere, Category = "Crowd")
	TObjectPtr<UBoxComponent> RegionBox;

	UPROPERTY(VisibleAnywhere, Category = "Crowd")
	TObjectPtr<UPCGComponent> PCGRunner;

	FDelegateHandle GeneratedHandle;
	FDelegateHandle CancelledHandle;
	bool bFinished = false;
	bool bSucceeded = false;
};
