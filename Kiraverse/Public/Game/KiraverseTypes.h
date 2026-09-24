#pragma once

#include "CoreMinimal.h"
#include "KiraverseTypes.generated.h"

// Which side a player is on for the current round. None is the pre-assignment default.
UENUM(BlueprintType)
enum class ETeam : uint8
{
	None		UMETA(DisplayName = "None"),
	Attackers	UMETA(DisplayName = "Attackers"),
	Defenders	UMETA(DisplayName = "Defenders"),
};

// Server-authoritative round lifecycle; GameState replicates this so clients can drive UI off it.
UENUM(BlueprintType)
enum class ERoundState : uint8
{
	WaitingToStart	UMETA(DisplayName = "Waiting To Start"),
	InProgress		UMETA(DisplayName = "In Progress"),		// Bomb not yet planted; round timer running.
	BombPlanted		UMETA(DisplayName = "Bomb Planted"),		// Fuse timer running; defenders can defuse.
	RoundEnded		UMETA(DisplayName = "Round Ended"),		// Brief settle state before next round starts.
};
