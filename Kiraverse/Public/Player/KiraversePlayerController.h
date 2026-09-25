#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "KiraversePlayerController.generated.h"

class AKiraverseCharacter;
class UInputMappingContext;
class UInputAction;

// Human controller: drives the kill cam by spectating living teammates after the possessed pawn dies.
UCLASS()
class KIRAVERSE_API AKiraversePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// Authority only. Starts spectating the first living teammate, or does nothing if none is alive.
	void BeginKillCam();

	// Authority only. Ends the kill cam and returns the camera to ViewTarget (the newly spawned pawn at round start).
	void EndKillCam(AActor* ViewTarget);

	// Authority only. Cycles to the next/previous living teammate; Direction is +1 or -1.
	void CycleKillCamTarget(int32 Direction);

	// Authority only. Re-targets the kill cam if the currently watched teammate just died.
	void OnWatchedCharacterDied(AKiraverseCharacter* DeadCharacter);

	bool IsKillCamActive() const { return bKillCamActive; }

protected:
	virtual void SetupInputComponent() override;

	// Server -> owning client: the server decides who to watch, the client applies the camera change.
	UFUNCTION(Client, Reliable)
	void ClientSetKillCamTarget(AActor* NewTarget);

	// Removes the kill cam mapping context and returns the camera to ViewTarget on the owning client.
	UFUNCTION(Client, Reliable)
	void ClientEndKillCam(AActor* ViewTarget);

	// Client -> server: player asked to cycle spectate target.
	UFUNCTION(Server, Reliable)
	void ServerCycleKillCamTarget(int32 Direction);

	void OnCycleNextPressed();
	void OnCyclePreviousPressed();

	// Mapping context is added only while the kill cam is active so cycle keys never fire during play.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|KillCam")
	TObjectPtr<UInputMappingContext> KillCamMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|KillCam")
	TObjectPtr<UInputAction> CycleNextAction;

	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|KillCam")
	TObjectPtr<UInputAction> CyclePreviousAction;

	// Seconds the camera takes to blend to the new target.
	UPROPERTY(EditDefaultsOnly, Category = "Kiraverse|KillCam")
	float ViewBlendTime = 0.5f;

private:
	// Living, same-team characters other than this controller's own, in stable world order.
	void GatherLivingTeammates(TArray<AKiraverseCharacter*>& OutTeammates) const;

	UPROPERTY()
	TObjectPtr<AKiraverseCharacter> WatchedCharacter;

	bool bKillCamActive = false;
};
