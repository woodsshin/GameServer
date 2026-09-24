#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "KiraverseGameplayAbility.generated.h"

// Shared base for all Kiraverse abilities; centralizes activation policy defaults.
UCLASS()
class KIRAVERSE_API UKiraverseGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UKiraverseGameplayAbility();
};
