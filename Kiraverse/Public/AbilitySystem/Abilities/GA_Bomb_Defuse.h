#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/Abilities/GA_Bomb_Channeled.h"
#include "GA_Bomb_Defuse.generated.h"

class AKiraverseBomb;

// Defender-only channel that defuses a planted bomb the character is standing near. Mirror of
// GA_Bomb_Plant: team gating via ActivationBlockedTags(Team.Attackers), same channel shape.
UCLASS()
class KIRAVERSE_API UGA_Bomb_Defuse : public UGA_Bomb_Channeled
{
	GENERATED_BODY()

public:
	UGA_Bomb_Defuse();

protected:
	// Must be within DefuseRange of a bomb whose state is EBombState::Planted.
	virtual bool CanStartChannel(const AKiraverseCharacter* Character) const override;

	// Calls AKiraverseBomb::Defuse on the nearby planted bomb. Returns Defuse()'s own result:
	// false if no such bomb could be re-resolved (e.g. it detonated in the gap between the last
	// CheckInterrupt tick and this call — see the base class's OnChannelCompleted doc comment).
	virtual bool OnChannelCompleted(AKiraverseCharacter* Character) override;

private:
	class AKiraverseBomb* FindPlantedBombInRange(const AKiraverseCharacter* Character) const;

	// How close the character must be to the planted bomb to start (and, via the base class's
	// CheckInterrupt, to keep) defusing.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	float DefuseRange = 150.f;
};
