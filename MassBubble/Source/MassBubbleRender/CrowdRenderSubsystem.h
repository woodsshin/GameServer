#pragma once

#include "CoreMinimal.h"
#include "Engine/StreamableManager.h"
#include "Net/CrowdSmoothing.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdRenderSubsystem.generated.h"

class ACrowdBubble;
class ACrowdRenderHost;
class UCrowdRenderSettings;
class UInstancedStaticMeshComponent;

/**
 * Client-side presentation of the replicated crowd.
 *
 * The server owns the truth (Mass entities); the client only ever sees ACrowdBubble's quantized FastArray.
 * Every frame this subsystem
 *   1. extrapolates each received agent with its velocity (dead reckoning, client half; the cap is the shared one in
 *      CrowdNet, so the server knows exactly what is drawn),
 *   2. smooths (CrowdSmoothing: eased velocity + velocity feed-forward + position correction + yaw) so that new walking
 *      segments become curves and corrections never show as jumps,
 *   3. writes all transforms into ONE instanced static mesh with a single batch call.
 *
 * It exists only where there is a screen: the module is ClientOnly and ShouldCreateSubsystem also rejects
 * dedicated servers and commandlets.
 *
 * The agent mesh / material are loaded asynchronously the first time a bubble shows up
 * (a synchronous load would stall the game thread).
 */
UCLASS()
class MASSBUBBLERENDER_API UCrowdRenderSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	bool EnsureHost(const UCrowdRenderSettings& Settings);
	void ResizeInstances(int32 NewCount);
	void ClearAll();

	UPROPERTY(Transient)
	TObjectPtr<ACrowdRenderHost> Host;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	TWeakObjectPtr<ACrowdBubble> Bubble;

	/** Keeps the agent mesh / material alive; the load itself runs in the background. */
	FStreamableManager StreamableManager;
	TSharedPtr<FStreamableHandle> AssetHandle;

	/** Reused every frame: no allocation after warm-up. */
	TArray<FTransform> Transforms;
	TArray<FTransform> AddScratch;

	/** What is drawn for one agent (position, eased velocity, yaw), per NetId. */
	struct FVisual
	{
		CrowdSmoothing::FState State;
		uint32 Epoch = 0;
	};
	TMap<uint32, FVisual> Visuals;
	uint32 VisualEpoch = 0;

	int32 NumInstances = 0;
	double NextBubbleSearchTime = 0.0;
	bool bHostFailed = false;
};
