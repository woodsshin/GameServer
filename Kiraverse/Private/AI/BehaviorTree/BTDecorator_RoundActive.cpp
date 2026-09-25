#include "AI/BehaviorTree/BTDecorator_RoundActive.h"
#include "AI/KiraverseAIController.h"
#include "AI/KiraverseAIQueries.h"
#include "Character/KiraverseCharacter.h"
#include "Game/KiraverseGameState.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Engine/World.h"

UBTDecorator_RoundActive::UBTDecorator_RoundActive()
{
	NodeName = TEXT("Round Active");

	// Aborts the running subtree the moment the round ends or the bot dies, without waiting for a task to finish.
	bNotifyTick = true;
	bNotifyBecomeRelevant = true;
	FlowAbortMode = EBTFlowAbortMode::Self;
}

FString UBTDecorator_RoundActive::GetStaticDescription() const
{
	return TEXT("Round is in progress (or bomb planted) and this bot is alive");
}

bool UBTDecorator_RoundActive::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	const UWorld* World = Self ? Self->GetWorld() : nullptr;
	const AKiraverseGameState* GameState = World ? World->GetGameState<AKiraverseGameState>() : nullptr;
	if (!Self || !GameState || KiraverseAIQueries::IsDead(Self))
	{
		return false;
	}

	const ERoundState RoundState = GameState->GetRoundState();
	return RoundState == ERoundState::InProgress || RoundState == ERoundState::BombPlanted;
}

void UBTDecorator_RoundActive::OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// Record the value the tree just used to enter this subtree, so only a later change requests a re-evaluation.
	reinterpret_cast<FRoundActiveMemory*>(NodeMemory)->bLastResult = CalculateRawConditionValue(OwnerComp, NodeMemory);
}

void UBTDecorator_RoundActive::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	// This decorator watches world state rather than a blackboard key, so it must poll; it only wakes the tree on a change.
	FRoundActiveMemory* Memory = reinterpret_cast<FRoundActiveMemory*>(NodeMemory);
	const bool bNow = CalculateRawConditionValue(OwnerComp, NodeMemory);
	if (bNow != Memory->bLastResult)
	{
		Memory->bLastResult = bNow;
		OwnerComp.RequestExecution(this);
	}
}
