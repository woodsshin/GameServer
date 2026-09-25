#include "Game/KiraverseGameMode.h"
#include "Game/KiraverseGameState.h"
#include "Game/KiraversePlayerState.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombComponent.h"
#include "AbilitySystem/Abilities/GA_Bomb_Plant.h"
#include "AbilitySystem/Abilities/GA_Bomb_Defuse.h"
#include "Character/KiraverseCharacter.h"
#include "AI/KiraverseAIController.h"
#include "Player/KiraversePlayerController.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"

AKiraverseGameMode::AKiraverseGameMode()
{
}

void AKiraverseGameMode::StartPlay()
{
	Super::StartPlay();

	CachedGameState = GetGameState<AKiraverseGameState>();
	if (CachedGameState)
	{
		CachedGameState->ResetScores();
	}

	SpawnBots();
	AssignTeams();
	StartRound();
}

void AKiraverseGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// A joiner takes whichever side is smaller (bots included) and becomes bomb-eligible next round.
	if (AKiraversePlayerState* PS = NewPlayer->GetPlayerState<AKiraversePlayerState>())
	{
		TArray<AController*> Participants;
		GetAllParticipants(Participants);

		int32 AttackerCount = 0;
		int32 DefenderCount = 0;
		for (const AController* Other : Participants)
		{
			const AKiraversePlayerState* OtherPS = Other->GetPlayerState<AKiraversePlayerState>();
			if (!OtherPS || OtherPS == PS)
			{
				continue;
			}
			if (OtherPS->GetTeam() == ETeam::Attackers) { ++AttackerCount; }
			else if (OtherPS->GetTeam() == ETeam::Defenders) { ++DefenderCount; }
		}
		PS->SetTeam(AttackerCount <= DefenderCount ? ETeam::Attackers : ETeam::Defenders);
	}
}

void AKiraverseGameMode::GetAllParticipants(TArray<AController*>& OutControllers) const
{
	OutControllers.Reset();

	// Humans first, then bots, so AssignTeams alternation spreads humans across both sides.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			OutControllers.Add(PC);
		}
	}

	for (TActorIterator<AKiraverseAIController> It(GetWorld()); It; ++It)
	{
		OutControllers.Add(*It);
	}
}

void AKiraverseGameMode::SpawnBots()
{
	if (!HasAuthority() || NumBotsPerTeam <= 0 || !GetWorld())
	{
		return;
	}

	if (!BotControllerClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: NumBotsPerTeam > 0 but BotControllerClass is not set; no bots spawned."));
		return;
	}

	const int32 TotalBots = NumBotsPerTeam * 2;
	for (int32 Index = 0; Index < TotalBots; ++Index)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AKiraverseAIController* BotController = GetWorld()->SpawnActor<AKiraverseAIController>(BotControllerClass, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!BotController)
		{
			continue;
		}

		// SpawnActor doesn't go through the login path that normally creates a PlayerState, so make one explicitly.
		if (!BotController->PlayerState)
		{
			BotController->InitPlayerState();
		}
		if (BotController->PlayerState)
		{
			BotController->PlayerState->SetPlayerName(FString::Printf(TEXT("Bot_%d"), Index + 1));
		}

		// A bot whose PlayerState isn't AKiraversePlayerState never gets a team and idles forever, so surface it.
		if (!BotController->GetPlayerState<AKiraversePlayerState>())
		{
			UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: bot %s has no AKiraversePlayerState; set the GameMode's PlayerStateClass."), *GetNameSafe(BotController));
		}
	}
}

void AKiraverseGameMode::AssignTeams()
{
	// One-time even split at match start; sides are never swapped between rounds, and humans are listed first so they spread across both sides.
	TArray<AController*> Participants;
	GetAllParticipants(Participants);

	for (int32 Index = 0; Index < Participants.Num(); ++Index)
	{
		if (AKiraversePlayerState* PS = Participants[Index]->GetPlayerState<AKiraversePlayerState>())
		{
			PS->SetTeam((Index % 2 == 0) ? ETeam::Attackers : ETeam::Defenders);
		}
	}
}

void AKiraverseGameMode::ReapplyTeamTagForPlayer(AController* Controller) const
{
	AKiraversePlayerState* PS = Controller ? Controller->GetPlayerState<AKiraversePlayerState>() : nullptr;
	if (!PS)
	{
		return;
	}

	// SetTeam(SameValue) is a no-op by design (see KiraversePlayerState.cpp), so re-assigning the
	// team the player already has does nothing here. Force a re-apply by round-tripping through
	// None, closing the RestartPlayer-before-SetTeam ordering gap noted in KiraverseGameMode.h.
	const ETeam CurrentTeam = PS->GetTeam();
	PS->SetTeam(ETeam::None);
	PS->SetTeam(CurrentTeam);
}

void AKiraverseGameMode::StartRound()
{
	if (!HasAuthority() || !CachedGameState)
	{
		return;
	}

	// Remove last round's bomb, unbinding first so its fuse can never end a later round.
	if (ActiveBomb)
	{
		ActiveBomb->OnPlanted.RemoveAll(this);
		ActiveBomb->OnExploded.RemoveAll(this);
		ActiveBomb->OnDefused.RemoveAll(this);
		ActiveBomb->Destroy();
		ActiveBomb = nullptr;
	}

	TArray<AController*> Participants;
	GetAllParticipants(Participants);

	for (AController* Controller : Participants)
	{
		// Drop last round's pawn so RestartPlayer always spawns a fresh one with reset attributes.
		if (APawn* OldPawn = Controller->GetPawn())
		{
			Controller->UnPossess();
			OldPawn->Destroy();
		}

		RestartPlayer(Controller);
		ReapplyTeamTagForPlayer(Controller);

		// The new pawn exists now, so the camera can be sent straight to it instead of lingering on a teammate.
		if (AKiraversePlayerController* HumanController = Cast<AKiraversePlayerController>(Controller))
		{
			HumanController->EndKillCam(HumanController->GetPawn());
		}

		if (AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(Controller->GetPawn()))
		{
			// Characters are new every round, so the binding is always fresh; AddUniqueDynamic guards a stray double call.
			Character->OnCharacterDied.AddUniqueDynamic(this, &AKiraverseGameMode::HandleCharacterDied);

			const AKiraversePlayerState* PS = Controller->GetPlayerState<AKiraversePlayerState>();
			if (PS)
			{
				GrantTeamBombAbility(Character, PS->GetTeam());
			}
		}
		else
		{
			// RestartPlayer runs PossessedBy synchronously, so a missing pawn means a bad DefaultPawnClass or no PlayerStart.
			UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: %s has no AKiraverseCharacter pawn after RestartPlayer; bomb ability not granted this round."),
				*GetNameSafe(Controller));
		}
	}

	GiveRandomAttackerTheBomb();

	CachedGameState->SetRoundState(ERoundState::InProgress);
	CachedGameState->SetPhaseEndTime(GetWorld()->GetTimeSeconds() + RoundTimeLimit);
	GetWorldTimerManager().SetTimer(RoundPhaseTimerHandle, this,
		&AKiraverseGameMode::OnRoundTimerExpired, RoundTimeLimit, false);
}

void AKiraverseGameMode::GrantTeamBombAbility(AKiraverseCharacter* Character, ETeam Team) const
{
	UAbilitySystemComponent* ASC = Character ? Character->GetAbilitySystemComponent() : nullptr;
	if (!ASC || !HasAuthority())
	{
		return;
	}

	TSubclassOf<UGameplayAbility> AbilityClass;
	if (Team == ETeam::Attackers && BombPlantAbilityClass)
	{
		AbilityClass = BombPlantAbilityClass;
	}
	else if (Team == ETeam::Defenders && BombDefuseAbilityClass)
	{
		AbilityClass = BombDefuseAbilityClass;
	}

	if (!AbilityClass)
	{
		return;
	}

	// Guard against double-granting across repeated StartRound calls to the same still-connected
	// ASC — see header comment on this function for why this check exists rather than assuming
	// a fresh grant is always safe.
	if (ASC->FindAbilitySpecFromClass(AbilityClass))
	{
		return;
	}

	ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1));
}

void AKiraverseGameMode::GiveRandomAttackerTheBomb()
{
	if (!BombClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: BombClass not set, round starting with no bomb."));
		return;
	}

	TArray<AController*> Participants;
	GetAllParticipants(Participants);

	TArray<AKiraverseCharacter*> Attackers;
	for (const AController* Controller : Participants)
	{
		const AKiraversePlayerState* PS = Controller->GetPlayerState<AKiraversePlayerState>();
		AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(Controller->GetPawn());
		if (PS && Character && PS->GetTeam() == ETeam::Attackers)
		{
			Attackers.Add(Character);
		}
	}

	if (Attackers.Num() == 0)
	{
		return;
	}

	AKiraverseCharacter* Carrier = Attackers[FMath::RandRange(0, Attackers.Num() - 1)];
	UKiraverseBombComponent* BombComponent = Carrier->FindComponentByClass<UKiraverseBombComponent>();
	if (!BombComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: chosen bomb carrier has no KiraverseBombComponent."));
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Carrier;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ActiveBomb = GetWorld()->SpawnActor<AKiraverseBomb>(BombClass, Carrier->GetActorTransform(), SpawnParams);
	if (ActiveBomb)
	{
		ActiveBomb->OnPlanted.AddDynamic(this, &AKiraverseGameMode::HandleBombPlanted);
		ActiveBomb->OnExploded.AddDynamic(this, &AKiraverseGameMode::HandleBombExploded);
		ActiveBomb->OnDefused.AddDynamic(this, &AKiraverseGameMode::HandleBombDefused);
		BombComponent->AttachBombToCarrier(ActiveBomb);
	}
}

void AKiraverseGameMode::OnRoundTimerExpired()
{
	// Only reachable while still InProgress: HandleBombPlanted (below) cancels
	// RoundPhaseTimerHandle the moment a plant succeeds, so this timer firing here always means
	// "round time ran out with the bomb never planted."
	EndRound(ETeam::Defenders);
}

void AKiraverseGameMode::HandleBombPlanted()
{
	// The bomb is now driving its own fuse timer (AKiraverseBomb::PlantAtSite already started it,
	// calling AKiraverseBomb::Detonate directly on expiry — no GameMode-side timer for this).
	// GameMode's only job here is: stop the round-timeout clock, since "ran out of time to plant"
	// no longer applies, and reflect the new phase for UI/HUD via GameState.
	GetWorldTimerManager().ClearTimer(RoundPhaseTimerHandle);

	if (CachedGameState && ActiveBomb)
	{
		CachedGameState->SetRoundState(ERoundState::BombPlanted);
		// Bomb actor is the single source of truth for the fuse deadline; mirror its absolute
		// FuseEndTime onto GameState's shared RoundPhaseEndTime so UI reading GameState (rather
		// than reaching into the bomb actor directly) still shows a correct countdown during this
		// phase. Both fields hold the same kind of value (an absolute GetWorld()->GetTimeSeconds()
		// timestamp, not a duration), so this is a direct copy rather than any recomputation.
		CachedGameState->SetPhaseEndTime(ActiveBomb->GetFuseEndTime());
	}
}

void AKiraverseGameMode::HandleBombExploded()
{
	EndRound(ETeam::Attackers);
}

void AKiraverseGameMode::HandleBombDefused()
{
	EndRound(ETeam::Defenders);
}

int32 AKiraverseGameMode::CountLivingOnTeam(ETeam Team) const
{
	TArray<AController*> Participants;
	GetAllParticipants(Participants);

	int32 Living = 0;
	for (const AController* Controller : Participants)
	{
		const AKiraversePlayerState* PS = Controller->GetPlayerState<AKiraversePlayerState>();
		const AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(Controller->GetPawn());
		if (PS && PS->GetTeam() == Team && Character && !Character->IsDead())
		{
			++Living;
		}
	}
	return Living;
}

void AKiraverseGameMode::CheckWipeOut()
{
	if (!CachedGameState)
	{
		return;
	}

	const ERoundState RoundState = CachedGameState->GetRoundState();
	if (RoundState != ERoundState::InProgress && RoundState != ERoundState::BombPlanted)
	{
		return;
	}

	// Defenders wiped out always hands attackers the round, planted or not.
	if (CountLivingOnTeam(ETeam::Defenders) == 0)
	{
		EndRound(ETeam::Attackers);
		return;
	}

	// Attackers wiped out only loses the round if the bomb is not planted; once planted, the fuse decides.
	if (RoundState == ERoundState::InProgress && CountLivingOnTeam(ETeam::Attackers) == 0)
	{
		EndRound(ETeam::Defenders);
	}
}

void AKiraverseGameMode::HandleCharacterDied(AKiraverseCharacter* DeadCharacter, AKiraverseCharacter* Killer)
{
	if (!HasAuthority() || !DeadCharacter)
	{
		return;
	}

	TArray<AController*> Participants;
	GetAllParticipants(Participants);

	// Every human either starts watching a teammate (if this was their own pawn) or re-targets (if they were watching the victim).
	for (AController* Controller : Participants)
	{
		AKiraversePlayerController* HumanController = Cast<AKiraversePlayerController>(Controller);
		if (!HumanController)
		{
			continue;
		}

		if (HumanController->GetPawn() == DeadCharacter)
		{
			HumanController->BeginKillCam();
		}
		else
		{
			HumanController->OnWatchedCharacterDied(DeadCharacter);
		}
	}

	CheckWipeOut();
}

void AKiraverseGameMode::EndRound(ETeam WinningTeam)
{
	// Timeout, explosion, defuse and wipe-out can race in one frame; only the first result counts.
	if (!CachedGameState || CachedGameState->GetRoundState() == ERoundState::RoundEnded)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(RoundPhaseTimerHandle);
	CachedGameState->SetRoundState(ERoundState::RoundEnded);
	CachedGameState->SetPhaseEndTime(0.f);
	CachedGameState->AddScore(WinningTeam, 1);

	CheckMatchEnd();

	GetWorldTimerManager().SetTimer(RoundRestartTimerHandle, this,
		&AKiraverseGameMode::StartRound, RoundEndDelay, false);
}

void AKiraverseGameMode::CheckMatchEnd()
{
	if (!CachedGameState)
	{
		return;
	}

	const int32 Target = CachedGameState->GetScoreToWinMatch();
	if (CachedGameState->GetAttackerScore() >= Target || CachedGameState->GetDefenderScore() >= Target)
	{
		// Match-end presentation (results screen, travel to lobby, etc.) is deliberately out of
		// scope here — no lobby/menu flow exists elsewhere in this codebase to hook into. Stopping
		// the round loop by clearing the restart timer is the safe minimum; wire up whatever
		// match-end flow the rest of the project uses at this point.
		GetWorldTimerManager().ClearTimer(RoundRestartTimerHandle);
	}
}
