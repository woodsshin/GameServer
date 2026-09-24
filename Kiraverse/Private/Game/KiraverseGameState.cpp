#include "Game/KiraverseGameState.h"
#include "Net/UnrealNetwork.h"

AKiraverseGameState::AKiraverseGameState()
{
}

float AKiraverseGameState::GetPhaseTimeRemaining() const
{
	if (RoundPhaseEndTime <= 0.f || !GetWorld())
	{
		return 0.f;
	}

	return FMath::Max(0.f, RoundPhaseEndTime - GetWorld()->GetTimeSeconds());
}

void AKiraverseGameState::SetRoundState(ERoundState NewState)
{
	if (HasAuthority())
	{
		RoundState = NewState;
	}
}

void AKiraverseGameState::SetPhaseEndTime(float WorldTimeSeconds)
{
	if (HasAuthority())
	{
		RoundPhaseEndTime = WorldTimeSeconds;
	}
}

void AKiraverseGameState::AddScore(ETeam ScoringTeam, int32 Amount)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ScoringTeam == ETeam::Attackers)
	{
		AttackerScore += Amount;
	}
	else if (ScoringTeam == ETeam::Defenders)
	{
		DefenderScore += Amount;
	}
}

void AKiraverseGameState::ResetScores()
{
	if (HasAuthority())
	{
		AttackerScore = 0;
		DefenderScore = 0;
	}
}

void AKiraverseGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKiraverseGameState, RoundState);
	DOREPLIFETIME(AKiraverseGameState, RoundPhaseEndTime);
	DOREPLIFETIME(AKiraverseGameState, AttackerScore);
	DOREPLIFETIME(AKiraverseGameState, DefenderScore);
}
