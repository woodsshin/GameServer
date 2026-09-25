#pragma once

#include "CoreMinimal.h"

// Blackboard key names shared by the C++ service/tasks and the editor-authored Blackboard asset; names must match exactly.
namespace KiraverseBB
{
	// Object: closest visible enemy character, unset when none is in sight.
	inline const FName TargetEnemy(TEXT("TargetEnemy"));

	// Object: the bomb site this bot should move to or hold.
	inline const FName TargetSite(TEXT("TargetSite"));

	// Object: the live bomb actor (loose or planted), unset when none exists.
	inline const FName Bomb(TEXT("Bomb"));

	// Vector: where the bot should stand while guarding a planted bomb.
	inline const FName GuardLocation(TEXT("GuardLocation"));

	// Bool: this bot is carrying the bomb.
	inline const FName HasBomb(TEXT("HasBomb"));

	// Bool: bot is standing inside a plantable zone.
	inline const FName InPlantZone(TEXT("InPlantZone"));

	// Bool: a bomb in the Planted state exists.
	inline const FName BombPlanted(TEXT("BombPlanted"));

	// Bool: a bomb in the Idle state lies loose in the world (not carried, not planted).
	inline const FName BombLoose(TEXT("BombLoose"));

	// Bool: an enemy is close enough that the bot must abandon a plant/defuse channel to fight.
	inline const FName ThreatClose(TEXT("ThreatClose"));

	// Bool: the bot is currently inside a plant/defuse channel.
	inline const FName IsChanneling(TEXT("IsChanneling"));
}
