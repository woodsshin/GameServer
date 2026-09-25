#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_PickUpBomb.generated.h"

// Fires the bomb interact ability once; succeeds only if the bot ends up carrying the bomb.
UCLASS()
class KIRAVERSE_API UBTTask_PickUpBomb : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_PickUpBomb();

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	virtual FString GetStaticDescription() const override;
};
