#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Cost_Fire.generated.h"

// Instant effect; Ammo/Stamina deltas are injected per-shot via SetByCaller.
UCLASS()
class KIRAVERSE_API UGE_Cost_Fire : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_Cost_Fire();
};
