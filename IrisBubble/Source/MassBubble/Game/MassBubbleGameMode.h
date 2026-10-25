#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassBubbleGameMode.generated.h"

/**
 * Server-only class (a GameMode never exists on a client).
 * Starts the headless load test (-OptBots=N). The crowd itself reaches the players through the region replicators
 * that UCrowdSubsystem creates; nothing per player has to be spawned.
 */
UCLASS()
class MASSBUBBLE_API AMassBubbleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMassBubbleGameMode();

	virtual void StartPlay() override;
};
