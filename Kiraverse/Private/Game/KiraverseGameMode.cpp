#include "Game/KiraverseGameMode.h"
#include "Game/KiraverseGameState.h"
#include "Game/KiraversePlayerState.h"
#include "Bomb/KiraverseBomb.h"
#include "Bomb/KiraverseBombComponent.h"
#include "AbilitySystem/Abilities/GA_Bomb_Plant.h"
#include "AbilitySystem/Abilities/GA_Bomb_Defuse.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"

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

	AssignTeams();
	StartRound();
}

void AKiraverseGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// A player joining mid-match still needs a team. NOTE: this assigns them to whichever side
	// is currently smaller, but does NOT put them into the round already in progress as a bomb
	// carrier candidate — GiveRandomAttackerTheBomb only runs at round start, so a joiner arrives
	// as a normal attacker/defender and is eligible starting next round. This is a deliberate
	// choice to avoid mid-round team-size churn affecting bomb-carrier odds; flagging in case a
	// different mid-match join behavior is wanted.
	if (AKiraversePlayerState* PS = NewPlayer->GetPlayerState<AKiraversePlayerState>())
	{
		int32 AttackerCount = 0;
		int32 DefenderCount = 0;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			if (const AKiraversePlayerState* OtherPS = It->Get() ? It->Get()->GetPlayerState<AKiraversePlayerState>() : nullptr)
			{
				if (OtherPS == PS)
				{
					continue;
				}
				if (OtherPS->GetTeam() == ETeam::Attackers) { ++AttackerCount; }
				else if (OtherPS->GetTeam() == ETeam::Defenders) { ++DefenderCount; }
			}
		}
		PS->SetTeam(AttackerCount <= DefenderCount ? ETeam::Attackers : ETeam::Defenders);
	}
}

void AKiraverseGameMode::AssignTeams()
{
	// One-time even split at match start. Sides are NOT swapped between rounds by this GameMode —
	// the task summary specifies team assignment and round win/loss but not attacker/defender
	// side-swap, so that's left out rather than assumed. Easy to add in EndRound if wanted: swap
	// each AKiraversePlayerState's team right before the next StartRound() call.
	TArray<APlayerController*> Controllers;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			Controllers.Add(PC);
		}
	}

	for (int32 Index = 0; Index < Controllers.Num(); ++Index)
	{
		if (AKiraversePlayerState* PS = Controllers[Index]->GetPlayerState<AKiraversePlayerState>())
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

	ActiveBomb = nullptr;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}
		RestartPlayer(PC);
		ReapplyTeamTagForPlayer(PC);

		if (AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(PC->GetPawn()))
		{
			const AKiraversePlayerState* PS = PC->GetPlayerState<AKiraversePlayerState>();
			if (PS)
			{
				GrantTeamBombAbility(Character, PS->GetTeam());
			}
		}
		else
		{
			// RestartPlayer should synchronously produce an AKiraverseCharacter pawn (PossessedBy
			// runs inside Possess(), called inside RestartPlayer, before it returns — see
			// AKiraverseCharacter::PossessedBy). Landing here means either DefaultPawnClass isn't
			// an AKiraverseCharacter subclass, or RestartPlayer failed to spawn/possess at all.
			// Either way this player gets no Plant/Defuse ability for the round; logging it since
			// it would otherwise fail completely silently.
			UE_LOG(LogTemp, Warning, TEXT("KiraverseGameMode: %s has no AKiraverseCharacter pawn after RestartPlayer; bomb ability not granted this round."),
				*GetNameSafe(PC));
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

	TArray<AKiraverseCharacter*> Attackers;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		const AKiraversePlayerState* PS = PC ? PC->GetPlayerState<AKiraversePlayerState>() : nullptr;
		AKiraverseCharacter* Character = PC ? Cast<AKiraverseCharacter>(PC->GetPawn()) : nullptr;
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

void AKiraverseGameMode::EndRound(ETeam WinningTeam)
{
	if (!CachedGameState)
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
