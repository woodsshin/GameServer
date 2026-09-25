#include "AI/BehaviorTree/BTTask_GuardBomb.h"
#include "AI/KiraverseAIController.h"
#include "AI/KiraverseAIQueries.h"
#include "Character/KiraverseCharacter.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

UBTTask_GuardBomb::UBTTask_GuardBomb()
{
	NodeName = TEXT("Guard Bomb");
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

FString UBTTask_GuardBomb::GetStaticDescription() const
{
	return FString::Printf(TEXT("Hold position and sweep view around %s"), *BombKey.SelectedKeyName.ToString());
}

EBTNodeResult::Type UBTTask_GuardBomb::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FGuardBombMemory* Memory = reinterpret_cast<FGuardBombMemory*>(NodeMemory);
	*Memory = FGuardBombMemory();

	AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Controller || !Blackboard || !Blackboard->GetValueAsObject(BombKey.SelectedKeyName))
	{
		return EBTNodeResult::Failed;
	}

	// Start each guard at a different point of the sweep so a group does not all stare the same way at once.
	Memory->ScanPhase = FMath::FRandRange(0.f, ScanPeriodSeconds);

	Controller->StopMovement();
	return EBTNodeResult::InProgress;
}

void UBTTask_GuardBomb::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FGuardBombMemory* Memory = reinterpret_cast<FGuardBombMemory*>(NodeMemory);
	AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	const AActor* Bomb = Blackboard ? Cast<AActor>(Blackboard->GetValueAsObject(BombKey.SelectedKeyName)) : nullptr;

	// Bomb gone (defused or exploded) or bot dead: the guard duty is over.
	if (!Self || KiraverseAIQueries::IsDead(Self) || !Bomb)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	Memory->ScanPhase = FMath::Fmod(Memory->ScanPhase + DeltaSeconds, ScanPeriodSeconds);

	// Sine sweep centered on the bomb direction: it dwells at the edges and moves fastest across the middle.
	const FVector ToBomb = (Bomb->GetActorLocation() - Self->GetActorLocation()).GetSafeNormal2D();
	const float SweepOffsetDegrees = ScanHalfAngleDegrees * FMath::Sin(2.f * PI * Memory->ScanPhase / ScanPeriodSeconds);
	const FVector ScanDirection = ToBomb.RotateAngleAxis(SweepOffsetDegrees, FVector::UpVector);

	// Look level with the eyes; a slightly downward point would make the bot stare at the floor.
	const FVector LookPoint = Self->GetPawnViewLocation() + ScanDirection * LookPointDistance;
	Controller->SetLookLocation(LookPoint);
}

EBTNodeResult::Type UBTTask_GuardBomb::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner()))
	{
		Controller->ClearLookLocation();
	}
	return EBTNodeResult::Aborted;
}

void UBTTask_GuardBomb::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	if (AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner()))
	{
		Controller->ClearLookLocation();
	}
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}
