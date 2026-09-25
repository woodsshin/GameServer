#include "Player/KiraversePlayerController.h"
#include "Character/KiraverseCharacter.h"
#include "Game/KiraversePlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "Engine/World.h"

void AKiraversePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (CycleNextAction)
		{
			EnhancedInput->BindAction(CycleNextAction, ETriggerEvent::Started, this, &AKiraversePlayerController::OnCycleNextPressed);
		}
		if (CyclePreviousAction)
		{
			EnhancedInput->BindAction(CyclePreviousAction, ETriggerEvent::Started, this, &AKiraversePlayerController::OnCyclePreviousPressed);
		}
	}
}

void AKiraversePlayerController::GatherLivingTeammates(TArray<AKiraverseCharacter*>& OutTeammates) const
{
	OutTeammates.Reset();

	const AKiraversePlayerState* OwnState = GetPlayerState<AKiraversePlayerState>();
	if (!OwnState || OwnState->GetTeam() == ETeam::None || !GetWorld())
	{
		return;
	}

	for (TActorIterator<AKiraverseCharacter> It(GetWorld()); It; ++It)
	{
		AKiraverseCharacter* Candidate = *It;
		if (Candidate == GetPawn() || Candidate->IsDead())
		{
			continue;
		}

		const AKiraversePlayerState* CandidateState = Candidate->GetPlayerState<AKiraversePlayerState>();
		if (CandidateState && CandidateState->GetTeam() == OwnState->GetTeam())
		{
			OutTeammates.Add(Candidate);
		}
	}

	// Name order is stable across calls, so "next" and "previous" always mean the same thing.
	OutTeammates.Sort([](const AKiraverseCharacter& A, const AKiraverseCharacter& B)
	{
		return A.GetName() < B.GetName();
	});
}

void AKiraversePlayerController::BeginKillCam()
{
	if (!HasAuthority())
	{
		return;
	}

	TArray<AKiraverseCharacter*> Teammates;
	GatherLivingTeammates(Teammates);
	if (Teammates.Num() == 0)
	{
		// No one left to watch: the round is over or about to be, so the camera stays where it is.
		bKillCamActive = false;
		WatchedCharacter = nullptr;
		return;
	}

	bKillCamActive = true;
	WatchedCharacter = Teammates[0];
	ClientSetKillCamTarget(WatchedCharacter);
}

void AKiraversePlayerController::EndKillCam(AActor* ViewTarget)
{
	if (!HasAuthority())
	{
		return;
	}

	bKillCamActive = false;
	WatchedCharacter = nullptr;
	ClientEndKillCam(ViewTarget);
}

void AKiraversePlayerController::CycleKillCamTarget(int32 Direction)
{
	if (!HasAuthority() || !bKillCamActive)
	{
		return;
	}

	TArray<AKiraverseCharacter*> Teammates;
	GatherLivingTeammates(Teammates);
	if (Teammates.Num() == 0)
	{
		return;
	}

	const int32 CurrentIndex = Teammates.IndexOfByKey(WatchedCharacter);
	// If the watched character is no longer a candidate, IndexOfByKey is INDEX_NONE and we restart from the first.
	const int32 NextIndex = (CurrentIndex == INDEX_NONE)
		? 0
		: (CurrentIndex + Direction + Teammates.Num()) % Teammates.Num();

	WatchedCharacter = Teammates[NextIndex];
	ClientSetKillCamTarget(WatchedCharacter);
}

void AKiraversePlayerController::OnWatchedCharacterDied(AKiraverseCharacter* DeadCharacter)
{
	if (!HasAuthority() || !bKillCamActive || DeadCharacter != WatchedCharacter)
	{
		return;
	}

	TArray<AKiraverseCharacter*> Teammates;
	GatherLivingTeammates(Teammates);
	if (Teammates.Num() == 0)
	{
		bKillCamActive = false;
		WatchedCharacter = nullptr;
		return;
	}

	WatchedCharacter = Teammates[0];
	ClientSetKillCamTarget(WatchedCharacter);
}

void AKiraversePlayerController::ClientSetKillCamTarget_Implementation(AActor* NewTarget)
{
	if (!NewTarget)
	{
		return;
	}

	if (const ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (KillCamMappingContext)
			{
				// AddMappingContext on an already-added context just updates priority, so repeated targets are harmless.
				Subsystem->AddMappingContext(KillCamMappingContext, 1);
			}
		}
	}

	SetViewTargetWithBlend(NewTarget, ViewBlendTime);
}

void AKiraversePlayerController::ClientEndKillCam_Implementation(AActor* ViewTarget)
{
	if (const ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (KillCamMappingContext)
			{
				Subsystem->RemoveMappingContext(KillCamMappingContext);
			}
		}
	}

	// Set explicitly rather than relying on possess-time camera management, which a previous kill cam target can override.
	if (ViewTarget)
	{
		SetViewTargetWithBlend(ViewTarget, 0.f);
	}
}

void AKiraversePlayerController::ServerCycleKillCamTarget_Implementation(int32 Direction)
{
	CycleKillCamTarget(Direction > 0 ? 1 : -1);
}

void AKiraversePlayerController::OnCycleNextPressed()
{
	ServerCycleKillCamTarget(1);
}

void AKiraversePlayerController::OnCyclePreviousPressed()
{
	ServerCycleKillCamTarget(-1);
}
