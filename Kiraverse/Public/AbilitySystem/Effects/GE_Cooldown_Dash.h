#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Cooldown_Dash.generated.h"

// Duration effect; while active it grants Cooldown.Dash, which blocks GA_Dash re-activation.
UCLASS()
class KIRAVERSE_API UGE_Cooldown_Dash : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_Cooldown_Dash();
};
