#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Game/KiraverseTypes.h"
#include "KiraverseGameMode.generated.h"

class AKiraverseBomb;
class AKiraverseCharacter;
class AKiraverseGameState;
class AKiraverseAIController;
class AKiraversePlayerController;
class UGA_Bomb_Plant;
class UGA_Bomb_Defuse;

// Owns the whole round loop for bomb-defusal: team assignment, respawns, random bomb carrier
// pick, round-end adjudication (timeout / explosion / defuse), and match win at ScoreToWinMatch.
// Server/listen-host only; every branch below assumes HasAuthority().
UCLASS()
class KIRAVERSE_API AKiraverseGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AKiraverseGameMode();

	virtual void StartPlay() override;

	// Bound to AKiraverseBomb::OnPlanted / OnExploded / OnDefused so the bomb doesn't need to know
	// about GameMode's round-state machine; it just fires events, GameMode reacts. The bomb OWNS
	// its own fuse timer (set in AKiraverseBomb::PlantAtSite, firing AKiraverseBomb::Detonate
	// directly) — GameMode does NOT run a second timer for the fuse. That keeps "when does the
	// bomb explode" single-sourced on the bomb actor; GameMode only reacts to the resulting event.
	UFUNCTION()
	void HandleBombPlanted();

	UFUNCTION()
	void HandleBombExploded();

	UFUNCTION()
	void HandleBombDefused();

	// Bound to every character's OnCharacterDied at round start; drives kill cam and wipe-out round ending.
	UFUNCTION()
	void HandleCharacterDied(AKiraverseCharacter* DeadCharacter, AKiraverseCharacter* Killer);

protected:
	// Spawns NumBotsPerTeam AI controllers per side at match start; each gets its own PlayerState.
	void SpawnBots();

	// Counts living characters on a team; dead ones and pawn-less controllers do not count.
	int32 CountLivingOnTeam(ETeam Team) const;

	// Ends the round if a death just wiped out a team, applying the plant-state rules.
	void CheckWipeOut();

	// Every controller that owns a Kiraverse PlayerState, human or bot, so round logic treats both alike.
	void GetAllParticipants(TArray<AController*>& OutControllers) const;

	// --- Round flow, in call order ---
	void AssignTeams();
	void StartRound();
	void GiveRandomAttackerTheBomb();

	// Grants BombPlantAbilityClass to attackers and BombDefuseAbilityClass to defenders. Called
	// from StartRound right after RestartPlayer/ReapplyTeamTagForPlayer, same phase of the loop
	// AKiraverseCharacter::GrantDefaultAbilities runs in for Jump/Dash/Zoom. Checks the character's
	// ASC for whether it already has the ability (via FindAbilitySpecFromClass) before granting
	// again, since StartRound runs every round and a respawned character's ASC is a fresh
	// InitAbilityActorInfo call but abilities granted to a still-alive-PlayerState ASC can persist
	// across a respawn depending on engine version/InstancingPolicy — guarding against a
	// double-grant here rather than assuming either way.
	void GrantTeamBombAbility(AKiraverseCharacter* Character, ETeam Team) const;
	void EndRound(ETeam WinningTeam);
	void CheckMatchEnd();

	// Timer entry point for the "round in progress, bomb never planted" case. The symmetric
	// "planted, fuse ran out" case has no GameMode-owned timer at all — see HandleBombExploded.
	void OnRoundTimerExpired();

	// Re-applies Team.* loose tags to a freshly (re)spawned pawn's ASC. Needed because PlayerState's
	// SetTeam can run before RestartPlayer, in which case GetPawn() was null at tag-apply time —
	// this closes that ordering gap by re-syncing right after respawn.
	void ReapplyTeamTagForPlayer(AController* Controller) const;

	//~ Begin AGameModeBase interface
	virtual void PostLogin(APlayerController* NewPlayer) override;
	//~ End AGameModeBase interface

	// Seconds attackers have to plant before defenders win by timeout.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Round")
	float RoundTimeLimit = 120.f;

	// Seconds between a successful plant and detonation if not defused.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Round")
	float BombFuseTime = 45.f;

	// Brief pause after a round ends before the next one starts, so clients can see the result.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Round")
	float RoundEndDelay = 5.f;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	TSubclassOf<AKiraverseBomb> BombClass;

	// Granted once per character, team-appropriate only — same idea as CurrentFireAbilityHandle
	// in KiraverseWeaponComponent granting only the equipped weapon's fire ability rather than
	// every GA_Fire_* subclass. ActivationBlockedTags on each ability is a secondary safety net,
	// not the primary gate; the primary gate is simply not granting the wrong one. GA_Bomb_Interact
	// is NOT team-gated (see its own header comment) so it belongs on DefaultAbilities on the
	// character instead, granted to everyone the same way Jump/Dash/Zoom are.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	TSubclassOf<UGA_Bomb_Plant> BombPlantAbilityClass;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bomb")
	TSubclassOf<UGA_Bomb_Defuse> BombDefuseAbilityClass;

	// Bots added to EACH team at match start (0 disables bots). Total bots = 2 * this value.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bot", meta = (ClampMin = "0"))
	int32 NumBotsPerTeam = 0;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Bot")
	TSubclassOf<AKiraverseAIController> BotControllerClass;

	UPROPERTY()
	TObjectPtr<AKiraverseGameState> CachedGameState;

private:
	FTimerHandle RoundPhaseTimerHandle;
	FTimerHandle RoundRestartTimerHandle;

	// Set when a bomb is spawned/given for the round; cleared on round end. GameMode holds this
	// (rather than reaching through a player's WeaponComponent-equivalent) because the bomb can
	// end up dropped/unowned mid-round, and GameMode still needs a handle to bind Exploded/Defused.
	UPROPERTY()
	TObjectPtr<AKiraverseBomb> ActiveBomb;
};
