#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTService.h"
#include "BTService_UpdateWorldState.generated.h"

// Periodically inspects the world and writes every fact the tree branches on into the blackboard.
UCLASS()
class KIRAVERSE_API UBTService_UpdateWorldState : public UBTService
{
	GENERATED_BODY()

public:
	UBTService_UpdateWorldState();

protected:
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

	virtual FString GetStaticServiceDescription() const override;

	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float EnemySightRange = 3000.f;

	// While channeling, a visible enemy closer than this sets ThreatClose so the tree can abort the channel.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float ChannelAbortRange = 600.f;

	// How far from the planted bomb a guard stands, along the line toward the nearest approach.
	UPROPERTY(EditAnywhere, Category = "Kiraverse|Bot")
	float GuardStandOffDistance = 450.f;
};
