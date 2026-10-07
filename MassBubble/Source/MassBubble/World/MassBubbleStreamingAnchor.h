#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassBubbleStreamingAnchor.generated.h"

class UWorldPartitionStreamingSourceComponent;

/**
 * A "virtual player" that exists only on the server. At one location it provides
 *   1. a UWorldPartitionStreamingSourceComponent  => World Partition keeps the cells around it loaded
 *   2. a crowd viewer registration                => UCrowdSubsystem keeps the Mass population around it spawned
 *
 * Usage:
 *   * load test:    opt.crowd.SpawnBots N (or -OptBots=N on the command line) spawns N anchors circling the map
 *   * level design: place one in the map to keep an area alive. A placed anchor needs "Is Spatially Loaded"
 *                   disabled in its World Partition details, otherwise it lives in the cell it is supposed
 *                   to keep loaded.
 *
 * Load-test bots:
 *   * are created and removed one by one (opt.crowd.BotRampSec), never all in one frame, so there is no burst
 *     of cells loading / unloading (+ GC)
 *   * use a Low priority streaming source, so the cells real players wait for are loaded first
 *   * only load cells (TargetState Loaded: no component registration, BeginPlay or tick) unless
 *     opt.crowd.BotActivateCells is 1
 *   * do not make the server wait for slow loading (FWorldPartitionStreamingSource::bBlockOnSlowLoading);
 *     the streaming profile in MassBubbleRuntimeConfig.cpp sets wp.Runtime.BlockOnSlowStreaming=0 on dedicated servers
 *
 * Inert on clients: a client only streams around its own player.
 */
UCLASS()
class MASSBUBBLE_API AMassBubbleStreamingAnchor : public AActor
{
	GENERATED_BODY()

public:
	AMassBubbleStreamingAnchor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Circle around Center (world cm) at constant linear speed. Starts ticking. */
	void StartOrbit(const FVector2D& Center, double RadiusCm, double SpeedCmPerSec, double StartAngleRad);

	bool IsRuntimeBot() const { return bRuntimeBot; }

	/**
	 * Server / standalone only. Schedules Count bots; they appear one by one (opt.crowd.BotRampSec, 0 = all at once).
	 * Orbit radius and speed come from the crowd settings. Returns how many were scheduled (spawned, if there is no
	 * crowd subsystem to ramp them).
	 */
	static int32 SpawnBots(UWorld* World, int32 Count);

	/** Schedules the removal of all anchors created by SpawnBots, one by one. Returns how many bots exist right now. */
	static int32 ClearBots(UWorld* World);

	/** Immediate versions, used by UCrowdSubsystem's ramp. SpawnBot places bot Index of Count on the orbit. */
	static AMassBubbleStreamingAnchor* SpawnBot(UWorld* World, int32 Index, int32 Count);

	/** Destroys one runtime bot. Returns how many bots are left afterwards. */
	static int32 DestroyOneBot(UWorld* World);

protected:
	/** Keep the World Partition cells around this actor loaded on the server. */
	UPROPERTY(EditAnywhere, Category = "Anchor")
	bool bStreamWorldPartition = true;

	/** Keep the Mass crowd populated around this actor (UCrowdSubsystem::RegisterVirtualViewer). */
	UPROPERTY(EditAnywhere, Category = "Anchor")
	bool bRegisterAsCrowdViewer = true;

private:
	/** Must run BEFORE BeginPlay (SpawnBot uses deferred spawning): BeginPlay is what switches the source on. */
	void ConfigureAsRuntimeBot();

	UPROPERTY(VisibleAnywhere, Category = "Anchor")
	TObjectPtr<UWorldPartitionStreamingSourceComponent> StreamingSource;

	bool bOrbiting = false;
	bool bRuntimeBot = false;
	FVector2D OrbitCenter = FVector2D::ZeroVector;
	double OrbitRadius = 0.0;
	double OrbitAngularSpeed = 0.0; // radians per second
	double OrbitAngle = 0.0;
	double OrbitZ = 0.0;
};
