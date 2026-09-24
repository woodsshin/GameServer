#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Damage.generated.h"

// Instant effect; the actual magnitude comes from UDamageExecCalculation.
UCLASS()
class KIRAVERSE_API UGE_Damage : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_Damage();
};
