#pragma once

#include "CoreMinimal.h"
#include "Engine/StreamableManager.h"
#include "Net/CrowdSmoothing.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdRenderSubsystem.generated.h"

class ACrowdRegionReplicator;
class ACrowdRenderHost;
class UCrowdRenderSettings;
class UInstancedStaticMeshComponent;

/**
 * Client-side presentation of the replicated crowd.
 *
 * The server owns the truth (the region simulation); the client only ever sees the quantized FastArrays of the
 * ACrowdRegionReplicators that the replication system has in scope for it. Every frame this subsystem
 *   1. extrapolates each received agent with its velocity (dead reckoning, client half; the cap is the shared one in
 *      CrowdNet, so the server knows exactly what is drawn),
 *   2. keeps the agents inside the draw radius of the local player, the nearest MaxInstances of them,
 *   3. smooths (CrowdSmoothing: eased velocity + velocity feed-forward + position correction + yaw) so that new walking
 *      segments become curves and corrections never show as jumps,
 *   4. writes all transforms into ONE instanced static mesh with a single batch call.
 *
 * It exists only where there is a screen: the module is ClientOnly and ShouldCreateSubsystem also rejects
 * dedicated servers and commandlets.
 *
 * The agent mesh / material are loaded asynchronously, when the world starts (UCrowdRenderSettings::bPrewarmAssets)
 * or when the first region replicator begins play; a synchronous load would stall the game thread.
 */
UCLASS()
class MASSBUBBLERENDER_API UCrowdRenderSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	/** What is drawn for one agent (position, eased velocity, yaw). */
	struct FVisual
	{
		CrowdSmoothing::FState State;
		uint32 Epoch = 0;
	};

	/** The drawn state of the agents of one region, by agent index. */
	struct FRegionVisuals
	{
		TArray<FVisual> Agents;
	};

	/** An agent inside the draw radius in this frame. */
	struct FCandidate
	{
		const ACrowdRegionReplicator* Region = nullptr;
		FRegionVisuals* Visuals = nullptr;
		int32 Index = 0;
		FVector2D Target = FVector2D::ZeroVector;
		FVector2f Drive = FVector2f::ZeroVector;
		double DistSq = 0.0;
	};

	void RequestAssets(const UCrowdRenderSettings& Settings);
	void HandleRegionBeginPlay(ACrowdRegionReplicator* Region);
	void HandleRegionEndPlay(ACrowdRegionReplicator* Region);
	bool EnsureHost(const UCrowdRenderSettings& Settings);
	void ResizeInstances(int32 NewCount);
	void ClearAll();

	/** The camera of the first local player, in the ground plane. False while there is none. */
	bool GetViewLocation(FVector2D& OutLocation) const;

	UPROPERTY(Transient)
	TObjectPtr<ACrowdRenderHost> Host;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	/** The replicators that are in the world on this machine. */
	TArray<TWeakObjectPtr<ACrowdRegionReplicator>> Regions;
	TMap<const ACrowdRegionReplicator*, TUniquePtr<FRegionVisuals>> VisualsByRegion;
	FDelegateHandle RegionBeginPlayHandle;
	FDelegateHandle RegionEndPlayHandle;

	/** Keeps the agent mesh / material alive; the load itself runs in the background. */
	FStreamableManager StreamableManager;
	TSharedPtr<FStreamableHandle> AssetHandle;

	/** Reused every frame: no allocation after warm-up. */
	TArray<FTransform> Transforms;
	TArray<FTransform> AddScratch;
	TArray<FCandidate> Candidates;

	uint32 VisualEpoch = 0;
	int32 NumInstances = 0;
	bool bScannedRegions = false;
	bool bHostFailed = false;
};
