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

	/**
	 * The three rates below belong to the client-side smoothing (Net/CrowdSmoothing.h). The replicated state is a
	 * position and a constant velocity per walking segment; drawn as it is, every segment would be a corner. All rates
	 * are in 1/s (the time constant is 1/rate); 0 turns the respective filter off.
	 *
	 * How fast the drawn velocity follows the replicated one: a turn becomes a short curve, a pause a short slide.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float VelocityEasingRate = 7.f;

	/** How fast the drawn position is pulled onto the position the server predicts. This is what keeps the error bounded. */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float CorrectionRate = 4.f;

	/** How fast the agent turns to face the direction it walks in. */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float YawRate = 10.f;

	/** Farther than this from where it should be (cm): teleport instead of gliding (first sighting, a long gap in the data). */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "50"))
	float SnapDistanceCm = 500.f;

	/** How often the client looks for its replicated ACrowdBubble while it has none. */
	UPROPERTY(Config, EditAnywhere, Category = "Presentation", meta = (ClampMin = "0.1"))
	float BubbleSearchIntervalSec = 1.f;
};
