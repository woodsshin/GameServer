#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Game/KiraverseTypes.h"
#include "KiraverseGameState.generated.h"

// Replicated round/match state. The round timer is NOT a replicated countdown float: it's a
// single replicated server world-time timestamp (RoundPhaseEndTime) that every client subtracts
// GetWorld()->GetTimeSeconds() from locally each frame. This avoids the classic "countdown float
// drifts/jitters under packet loss" problem — one timestamp replicates once per phase change,
// not once per tick, and every client computes an identical remaining-time value from it.
UCLASS()
class KIRAVERSE_API AKiraverseGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AKiraverseGameState();

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Round")
	ERoundState GetRoundState() const { return RoundState; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Round")
	int32 GetAttackerScore() const { return AttackerScore; }

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Round")
	int32 GetDefenderScore() const { return DefenderScore; }

	// Seconds remaining in the current phase (round timer or bomb fuse), clamped to >= 0.
	// Meaningless outside InProgress/BombPlanted; callers should check GetRoundState() first.
	UFUNCTION(BlueprintPure, Category = "Kiraverse|Round")
	float GetPhaseTimeRemaining() const;

	//~ Authority-only mutators; called by AKiraverseGameMode. Not UFUNCTION-exposed to Blueprint
	//~ so GameMode stays the single writer.
	void SetRoundState(ERoundState NewState);
	void SetPhaseEndTime(float WorldTimeSeconds);
	void AddScore(ETeam ScoringTeam, int32 Amount = 1);
	void ResetScores();

protected:
	//~ Begin AActor interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Kiraverse|Round")
	ERoundState RoundState = ERoundState::WaitingToStart;

	// Absolute GetWorld()->GetTimeSeconds() value at which the current phase ends. 0 = no active timer.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Kiraverse|Round")
	float RoundPhaseEndTime = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Kiraverse|Round")
	int32 AttackerScore = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Kiraverse|Round")
	int32 DefenderScore = 0;

	// First team to reach this score wins the match. Matches the "5점 선취" requirement.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|Round")
	int32 ScoreToWinMatch = 5;

public:
	int32 GetScoreToWinMatch() const { return ScoreToWinMatch; }
};
