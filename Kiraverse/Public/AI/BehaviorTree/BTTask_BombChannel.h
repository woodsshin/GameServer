#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameplayTagContainer.h"
#include "Game/KiraverseTypes.h"
#include "BTTask_BombChannel.generated.h"

class AKiraverseBomb;

// Per-execution scratch data kept in node memory, so one node asset can serve many bots safely.
struct FBombChannelMemory
{
	// The bomb this channel acts on, captured at start so the outcome can be judged even after the round ends.
	TWeakObjectPtr<AKiraverseBomb> TargetBomb;

	// Set true once IsChanneling has been observed; distinguishes "not started yet" from "already finished".
	bool bChannelObserved = false;
};

// Runs the team's plant or defuse channel to completion, cancelling the ability if the tree aborts the task.
UCLASS()
class KIRAVERSE_API UBTTask_BombChannel : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_BombChannel();

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FBombChannelMemory); }

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

	virtual FString GetStaticDescription() const override;

private:
	// Plant for attackers, Defuse for defenders; None if the bot has no team yet.
	FGameplayTag GetChannelAbilityTag(const class AKiraverseAIController* Controller) const;

	// Whether the world now shows the result this team's channel is supposed to produce.
	bool DidChannelSucceed(ETeam Team, const AKiraverseBomb* Bomb) const;

	void CancelChannel(UBehaviorTreeComponent& OwnerComp) const;
};
