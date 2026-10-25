#include "Game/MassBubbleGameMode.h"

#include "Game/MassBubbleCharacter.h"
#include "World/MassBubbleStreamingAnchor.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

AMassBubbleGameMode::AMassBubbleGameMode()
{
	DefaultPawnClass = AMassBubbleCharacter::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
}

void AMassBubbleGameMode::StartPlay()
{
	Super::StartPlay();

	// Headless load test: MassBubbleServer.exe /Game/Maps/L_MassBubbleWorld -log -OptBots=8
	int32 NumBots = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("OptBots="), NumBots) && NumBots > 0)
	{
		AMassBubbleStreamingAnchor::SpawnBots(GetWorld(), NumBots);
	}
}
