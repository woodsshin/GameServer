#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KiraverseBombSite.generated.h"

class UBoxComponent;
class USceneComponent;
class AKiraverseCharacter;

// A designer-placed volume marking where the bomb can be planted. Purely a location/query
// object — it holds no bomb-state itself (that lives on AKiraverseBomb) and does no team
// gating itself (that's ActivationBlockedTags on GA_Bomb_Plant/Defuse). Its only job is
// "is this character standing inside a plantable zone right now."
UCLASS()
class KIRAVERSE_API AKiraverseBombSite : public AActor
{
	GENERATED_BODY()

public:
	AKiraverseBombSite();

	// Checked by GA_Bomb_Plant before allowing the channel to start.
	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	bool IsCharacterInPlantZone(const AKiraverseCharacter* Character) const;

	// Attach point the bomb snaps to on a successful plant.
	USceneComponent* GetPlantSocketComponent() const { return PlantSocket; }

	// Optional identifying label, e.g. "A" / "B" for multi-site maps. Not used for any logic
	// in this pass (GiveRandomAttackerTheBomb doesn't route to a specific site), just surfaced
	// for UI/HUD callouts.
	UFUNCTION(BlueprintPure, Category = "Kiraverse|Bomb")
	FName GetSiteLabel() const { return SiteLabel; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<UBoxComponent> PlantZone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Kiraverse|Bomb")
	TObjectPtr<USceneComponent> PlantSocket;

	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bomb")
	FName SiteLabel = TEXT("A");
};
