#pragma once

#include "CoreMinimal.h"
#include "Game/KiraverseTypes.h"

class UWorld;
class AKiraverseCharacter;
class AKiraverseBomb;
class AKiraverseBombSite;

// Stateless world queries shared by every bot BT node so the sight, site and bomb rules live in one place.
namespace KiraverseAIQueries
{
	bool IsDead(const AKiraverseCharacter* Character);

	// True while the character is inside a plant or defuse channel.
	bool IsChanneling(const AKiraverseCharacter* Character);

	// Nothing solid between the viewer's eyes and the target's location.
	bool HasLineOfSight(const AKiraverseCharacter* Viewer, const AActor* Target);

	// Closest living, opposing, visible character within SightRange; nullptr if none.
	AKiraverseCharacter* FindClosestVisibleEnemy(const AKiraverseCharacter* Self, ETeam OwnTeam, float SightRange);

	// Closest bomb site by straight-line distance; nullptr if the level has none.
	AKiraverseBombSite* FindClosestBombSite(const UWorld* World, const FVector& From);

	// The single live bomb (Idle or Planted); nullptr once it has exploded or been defused.
	AKiraverseBomb* FindLiveBomb(const UWorld* World);
}
