#include "AI/BehaviorTree/BTTask_EngageTarget.h"
#include "AI/KiraverseAIController.h"
#include "AI/KiraverseAIQueries.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystemComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

UBTTask_EngageTarget::UBTTask_EngageTarget()
{
	NodeName = TEXT("Engage Target");
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

FString UBTTask_EngageTarget::GetStaticDescription() const
{
	return FString::Printf(TEXT("Aim and fire at %s"), *TargetKey.SelectedKeyName.ToString());
}

EBTNodeResult::Type UBTTask_EngageTarget::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FEngageTargetMemory* Memory = reinterpret_cast<FEngageTargetMemory*>(NodeMemory);
	*Memory = FEngageTargetMemory();

	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	const AActor* Target = Blackboard ? Cast<AActor>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName)) : nullptr;
	if (!Target)
	{
		return EBTNodeResult::Failed;
	}

	return EBTNodeResult::InProgress;
}

void UBTTask_EngageTarget::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FEngageTargetMemory* Memory = reinterpret_cast<FEngageTargetMemory*>(NodeMemory);
	AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	const AKiraverseCharacter* Target = Blackboard ? Cast<AKiraverseCharacter>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName)) : nullptr;

	// Lost sight, target died, or self died: the fight is over, so hand control back to the tree.
	if (!Controller || !Self || KiraverseAIQueries::IsDead(Self) || !Target || KiraverseAIQueries::IsDead(Target))
	{
		StopEngaging(OwnerComp, Memory);
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	Controller->SetAimTarget(Target);

	// Re-roll aim error on a slow cadence; SetAimTarget above already rolled once on the first frame.
	Memory->TimeSinceAimReroll += DeltaSeconds;
	if (Memory->TimeSinceAimReroll >= AimErrorRerollInterval)
	{
		Memory->TimeSinceAimReroll = 0.f;
		Controller->RerollAimError();
	}

	const float DistSq = FVector::DistSquared(Self->GetActorLocation(), Target->GetActorLocation());
	if (DistSq > FMath::Square(MaxFireDistance))
	{
		// Keep aiming but hold fire while the target is out of effective range.
		if (Memory->bIsFiring)
		{
			Controller->CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Fire.GetTag());
			Memory->bIsFiring = false;
		}
		return;
	}

	// Auto weapons keep firing on their own timer once running; single-shot and projectile weapons need a new activation per shot.
	const UAbilitySystemComponent* ASC = Self->GetAbilitySystemComponent();
	const bool bAutoFireRunning = ASC && ASC->HasMatchingGameplayTag(KiraverseGameplayTags::State_Firing);
	if (!bAutoFireRunning)
	{
		Memory->bIsFiring = Controller->TryActivateAbilityByTag(KiraverseGameplayTags::Ability_Fire.GetTag()) || Memory->bIsFiring;
	}
}

EBTNodeResult::Type UBTTask_EngageTarget::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// Without this an aborted engage would leave GA_Fire_HitscanAuto running while the bot walks off elsewhere.
	StopEngaging(OwnerComp, reinterpret_cast<FEngageTargetMemory*>(NodeMemory));
	return EBTNodeResult::Aborted;
}

void UBTTask_EngageTarget::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	StopEngaging(OwnerComp, reinterpret_cast<FEngageTargetMemory*>(NodeMemory));
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EngageTarget::StopEngaging(UBehaviorTreeComponent& OwnerComp, FEngageTargetMemory* Memory)
{
	if (AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner()))
	{
		Controller->SetAimTarget(nullptr);

		// Cancel unconditionally: a re-entered task starts with fresh memory, so a stale bIsFiring must never gate this.
		Controller->CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Fire.GetTag());
	}

	if (Memory)
	{
		Memory->bIsFiring = false;
	}
}
