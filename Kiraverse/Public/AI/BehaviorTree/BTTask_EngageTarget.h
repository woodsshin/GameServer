#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BTTask_EngageTarget.generated.h"

// Per-execution scratch data kept in node memory, so one node asset can serve many bots safely.
struct FEngageTargetMemory
{
	float TimeSinceAimReroll = 0.f;
	bool bIsFiring = false;
};

// Aims at and shoots TargetEnemy until it is lost or dies; stops firing whenever the task ends or is aborted.
UCLASS()
class KIRAVERSE_API UBTTask_EngageTarget : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EngageTarget();

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FEngageTargetMemory); }

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

	virtual FString GetStaticDescription() const override;

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

	// Beyond this distance the bot stops firing (it keeps aiming) so shots are not wasted at extreme range.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float MaxFireDistance = 2500.f;

	// Seconds between fresh aim-error rolls, so aim wobbles at a human cadence rather than every frame.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float AimErrorRerollInterval = 0.3f;

private:
	// Cancels fire and clears the aim target; safe to call when nothing is active.
	void StopEngaging(UBehaviorTreeComponent& OwnerComp, FEngageTargetMemory* Memory);
};
