#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassBubbleGameMode.generated.h"

/**
 * Server-only class (a GameMode never exists on a client).
 * Spawns one ACrowdBubble per player; the bubble destroys itself when its owner goes away.
 */
UCLASS()
class MASSBUBBLE_API AMassBubbleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMassBubbleGameMode();

	virtual void StartPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
};
