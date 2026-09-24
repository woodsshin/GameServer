#include "Game/KiraversePlayerState.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// Team.* tags are mutually exclusive and not part of any transient ability's ActivationOwnedTags,
	// so both the grant and the clear have to be explicit here.
	FGameplayTag TagForTeam(ETeam InTeam)
	{
		switch (InTeam)
		{
		case ETeam::Attackers: return KiraverseGameplayTags::Team_Attackers;
		case ETeam::Defenders: return KiraverseGameplayTags::Team_Defenders;
		default: return FGameplayTag::EmptyTag;
		}
	}
}

AKiraversePlayerState::AKiraversePlayerState()
{
}

void AKiraversePlayerState::SetTeam(ETeam NewTeam)
{
	if (!HasAuthority() || Team == NewTeam)
	{
		return;
	}

	const ETeam OldTeam = Team;
	Team = NewTeam;
	ApplyTeamTagToPawnASC(OldTeam, NewTeam);

	// PlayerState doesn't ReplicatedUsing-fire on the server that set it, so OnRep won't run
	// locally here; ApplyTeamTagToPawnASC above already covers the server's own ASC update.
}

void AKiraversePlayerState::OnRep_Team(ETeam OldTeam)
{
	ApplyTeamTagToPawnASC(OldTeam, Team);
}

void AKiraversePlayerState::ApplyTeamTagToPawnASC(ETeam OldTeam, ETeam NewTeam) const
{
	// Only the possessing pawn's own ASC needs this locally: ability activation/prediction
	// for Plant/Defuse only ever runs on the owning client and the server, never simulated proxies.
	const APawn* Pawn = GetPawn();
	const IAbilitySystemInterface* ASI = Pawn ? Cast<IAbilitySystemInterface>(Pawn) : nullptr;
	UAbilitySystemComponent* ASC = ASI ? ASI->GetAbilitySystemComponent() : nullptr;
	if (!ASC)
	{
		return;
	}

	const FGameplayTag OldTag = TagForTeam(OldTeam);
	if (OldTag.IsValid())
	{
		ASC->RemoveLooseGameplayTag(OldTag);
	}

	const FGameplayTag NewTag = TagForTeam(NewTeam);
	if (NewTag.IsValid())
	{
		ASC->AddLooseGameplayTag(NewTag);
	}
}

void AKiraversePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKiraversePlayerState, Team);
}
