#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BTTask_GuardBomb.generated.h"

// Per-execution scratch data kept in node memory, so one node asset can serve many bots safely.
struct FGuardBombMemory
{
	float ScanPhase = 0.f;
};

// Holds position covering a planted bomb, sweeping the view across the bomb's approaches until aborted.
UCLASS()
class KIRAVERSE_API UBTTask_GuardBomb : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_GuardBomb();

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FGuardBombMemory); }

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

	virtual FString GetStaticDescription() const override;

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector BombKey;

	// Half-width in degrees of the sweep centered on the bomb direction.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot", meta = (ClampMin = "0", ClampMax = "180"))
	float ScanHalfAngleDegrees = 70.f;

	// Seconds for one full left-to-right-to-left sweep.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot", meta = (ClampMin = "0.5"))
	float ScanPeriodSeconds = 5.f;

	// How far ahead of the bot the look point is placed; only its direction matters.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float LookPointDistance = 1000.f;
};
