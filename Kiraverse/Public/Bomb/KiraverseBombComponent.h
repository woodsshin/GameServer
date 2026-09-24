#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "KiraverseBombComponent.generated.h"

class AKiraverseBomb;

// Owns the character's reference to whichever AKiraverseBomb they're currently carrying, if any.
// Deliberately mirrors KiraverseWeaponComponent's CurrentWeapon pattern: a single replicated
// TObjectPtr with an OnRep hook, authority-gated mutators, and no per-tick logic. Unlike weapons,
// there's no "equip on BeginPlay" — a character starts with no bomb; GameMode attaches one via
// AttachBombToCarrier for whichever attacker is randomly chosen at round start.
UCLASS(ClassGroup = (Kiraverse), meta = (BlueprintSpawnableComponent))
class KIRAVERSE_API UKiraverseBombComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKiraverseBombComponent();

	AKiraverseBomb* GetCarriedBomb() const { return CarriedBomb; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	bool HasBomb() const { return CarriedBomb != nullptr; }

	// Authority only. Called by GameMode at round start (initial hand-off) and by GA_Bomb_Interact's
	// pickup branch (picking up a dropped bomb). Attaches Bomb to this owner's carry socket and
	// grants State.CarryingBomb on the owner's ASC, same tag-on-attach idea as PlayerState's team tag.
	void AttachBombToCarrier(AKiraverseBomb* Bomb);

	// Authority only. Called by GA_Bomb_Interact's drop branch and by GA_Bomb_Plant right before
	// AKiraverseBomb::PlantAtSite (planting also clears the carry reference — the bomb stops being
	// "carried" the instant it's attached to a site, regardless of channel outcome). Detaches the
	// carry-side reference only; does NOT itself call AKiraverseBomb::DropAtCurrentLocation or
	// PlantAtSite — callers pick which of those two happens to the bomb itself, this just lets go.
	void ReleaseCarriedBomb();

	//~ Begin UActorComponent interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End UActorComponent interface

protected:
	// Socket on the owner's mesh the carried bomb attaches to. Separate from WeaponSocketName on
	// KiraverseWeaponComponent since a character can carry a bomb and have a weapon equipped
	// simultaneously (task summary doesn't say weapons are dropped on bomb pickup, so they aren't).
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	FName BombCarrySocketName = TEXT("BombSocket");

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CarriedBomb, Category = "Kiraverse|Bomb")
	TObjectPtr<AKiraverseBomb> CarriedBomb;

private:
	UFUNCTION()
	void OnRep_CarriedBomb(AKiraverseBomb* OldBomb);
};
