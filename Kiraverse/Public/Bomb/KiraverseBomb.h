#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KiraverseBomb.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class AKiraverseBombSite;

UENUM(BlueprintType)
enum class EBombState : uint8
{
	Idle,		// Carried by a character, or lying dropped in the world. Not on a timer.
	Planted,	// Attached to a bomb site; fuse timer running server-side.
	Exploded,	// Terminal.
	Defused,	// Terminal.
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKiraverseBombEvent);

// The physical bomb prop. Carried by attaching to a character's hand socket (via
// KiraverseBombComponent, same ownership pattern as KiraverseWeaponComponent/CurrentWeapon),
// dropped by detaching and leaving it in the world, or planted by attaching to a bomb site.
// GameMode owns round-level consequences (score, round-state) by subscribing to OnExploded/
// OnDefused; this actor only owns the bomb's own physical/timer state.
UCLASS()
class KIRAVERSE_API AKiraverseBomb : public AActor
{
	GENERATED_BODY()

public:
	AKiraverseBomb();

	UPROPERTY(BlueprintAssignable)
	FKiraverseBombEvent OnExploded;

	UPROPERTY(BlueprintAssignable)
	FKiraverseBombEvent OnDefused;

	// Broadcast once, right after a successful GA_Bomb_Plant channel calls PlantAtSite. GameMode
	// subscribes to switch the active round-phase timer from "round time limit" to "bomb fuse" —
	// added alongside OnExploded/OnDefused so all three round-relevant bomb events go through the
	// same actor-doesn't-know-about-GameMode delegate pattern, rather than Plant calling into
	// GameMode directly.
	UPROPERTY(BlueprintAssignable)
	FKiraverseBombEvent OnPlanted;

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	EBombState GetBombState() const { return BombState; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	AKiraverseBombSite* GetPlantedAtSite() const { return PlantedAtSite; }

	// Authority only. Called by GA_Bomb_Interact's drop branch and by KiraverseBombComponent when
	// a carrier dies (see UnequipCurrentWeapon-equivalent teardown). Detaches and leaves in place.
	void DropAtCurrentLocation();

	// Authority only. Called by GA_Bomb_Plant on successful channel completion. Attaches to Site,
	// starts the fuse timer, and flips state to Planted. Returns false if already planted/resolved.
	bool PlantAtSite(AKiraverseBombSite* Site, float FuseTimeSeconds);

	// Authority only. Called by GA_Bomb_Defuse on successful channel completion. Clears the fuse
	// timer, flips state to Defused, and broadcasts OnDefused. Returns false if not currently Planted.
	bool Defuse();

	// Authority only. Bound as this bomb's own FuseTimerHandle callback (set in PlantAtSite), so it
	// fires on its own once the fuse runs out — no external caller drives this. Also safe to call
	// directly for testing. Flips state to Exploded and broadcasts OnExploded. No-ops if the bomb
	// isn't in the Planted state (defused/already-exploded bombs can't be force-detonated).
	void Detonate();

	// Seconds remaining on the fuse; meaningless (returns 0) unless BombState == Planted.
	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	float GetFuseTimeRemaining() const;

	// Absolute GetWorld()->GetTimeSeconds() value the fuse ends at (0 if not Planted). Prefer this
	// over reconstructing it from GetFuseTimeRemaining() + now when the caller specifically needs
	// the absolute timestamp (e.g. to mirror onto another actor's own end-timestamp field, as
	// AKiraverseGameState::RoundPhaseEndTime does) — GetFuseTimeRemaining() is for display.
	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	float GetFuseEndTime() const { return FuseEndTime; }

protected:
	UFUNCTION()
	void OnRep_BombState();

	//~ Begin AActor interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<USphereComponent> CollisionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<UStaticMeshComponent> BombMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_BombState, Category = "Kiraverse|Bomb")
	EBombState BombState = EBombState::Idle;

	// Same end-timestamp replication pattern as AKiraverseGameState's round timer, for the same
	// jitter-avoidance reason. 0 while not Planted.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Kiraverse|Bomb")
	float FuseEndTime = 0.f;

	UPROPERTY()
	TObjectPtr<AKiraverseBombSite> PlantedAtSite;

private:
	FTimerHandle FuseTimerHandle;
};
