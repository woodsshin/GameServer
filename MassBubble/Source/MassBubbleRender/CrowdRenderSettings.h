#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CrowdRenderSettings.generated.h"

class UMaterialInterface;
class UStaticMesh;

/**
 * Client-side presentation tunables (Project Settings > Game > MassBubble Crowd Rendering).
 * Lives in the ClientOnly module: the dedicated server never loads this class, so it never touches meshes.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "MassBubble Crowd Rendering"))
class MASSBUBBLERENDER_API UCrowdRenderSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCrowdRenderSettings();

	/** Mesh drawn for every agent. Default: the engine cylinder (100 x 100 x 100 cm, pivot in the middle). */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> AgentMesh;

	/** Optional. Empty = use the mesh's own material. */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh")
	TSoftObjectPtr<UMaterialInterface> AgentMaterial;

	/** Default scale turns the unit cylinder into a 50 cm wide, 180 cm tall "person". */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh")
	FVector AgentScale = FVector(0.5, 0.5, 1.8);

	/** Lifts the pivot so the mesh stands on the ground (half the scaled mesh height for the default cylinder). */
	UPROPERTY(Config, EditAnywhere, Category = "Mesh")
	float PivotOffsetCm = 90.f;

	UPROPERTY(Config, EditAnywhere, Category = "Mesh")
	bool bCastShadow = false;

	/** Hard cap on drawn instances; the bubble holds at most MaxAgentsPerBubble anyway. */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "1", ClampMax = "20000"))
	int32 MaxInstances = 2048;

	/** Never extrapolate further than this past the last received state (a stalled connection must not fling agents away). */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float MaxExtrapolationSec = 1.5f;

	/**
	 * Exponential smoothing rate (1/s) towards the extrapolated position. Hides the pops caused by dead-reckoning
	 * corrections. 0 disables it (agents snap). Costs a TMap lookup per agent per frame.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float SmoothingRate = 15.f;

	/** How often the client looks for its replicated ACrowdBubble while it has none. */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.1"))
	float BubbleSearchIntervalSec = 1.f;
};
