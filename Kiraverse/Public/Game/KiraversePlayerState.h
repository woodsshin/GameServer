#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Game/KiraverseTypes.h"
#include "KiraversePlayerState.generated.h"

// Holds this player's team for the current round. GameMode is the only writer; the setter
// also grants/clears the matching Team.* loose tag on the possessed pawn's ASC, if any.
UCLASS()
class KIRAVERSE_API AKiraversePlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AKiraversePlayerState();

	UFUNCTION(BlueprintPure, Category = "Kiraverse|Team")
	ETeam GetTeam() const { return Team; }

	// Authority only. Clears any previous Team.* tag from the current pawn's ASC (if possessed)
	// and grants the new one. Safe to call again on the same value (no-ops the tag churn).
	void SetTeam(ETeam NewTeam);

protected:
	UFUNCTION()
	void OnRep_Team(ETeam OldTeam);

	//~ Begin AActor interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_Team, Category = "Kiraverse|Team")
	ETeam Team = ETeam::None;

private:
	// Shared by SetTeam (authority) and OnRep_Team (clients) so both paths update the same ASC tags.
	void ApplyTeamTagToPawnASC(ETeam OldTeam, ETeam NewTeam) const;
};
