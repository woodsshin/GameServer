#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTDecorator.h"
#include "BTDecorator_RoundActive.generated.h"

// Last condition result seen by this decorator, kept per bot so a change triggers exactly one re-evaluation.
struct FRoundActiveMemory
{
	bool bLastResult = false;
};

// Passes only while the round is in progress or the bomb is planted, and while the bot itself is alive.
UCLASS()
class KIRAVERSE_API UBTDecorator_RoundActive : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTDecorator_RoundActive();

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FRoundActiveMemory); }

protected:
	virtual void OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

	virtual FString GetStaticDescription() const override;
};
