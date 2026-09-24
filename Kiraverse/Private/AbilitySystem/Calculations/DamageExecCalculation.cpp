#include "AbilitySystem/Calculations/DamageExecCalculation.h"
#include "AbilitySystem/KiraverseAttributeSet.h"
#include "AbilitySystem/KiraverseGameplayTags.h"

void UDamageExecCalculation::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

	// Magnitude was injected by the firing weapon via SetSetByCallerMagnitude.
	const float RawDamage = Spec.GetSetByCallerMagnitude(KiraverseGameplayTags::Data_Damage, false, 0.f);
	if (RawDamage > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UKiraverseAttributeSet::GetDamageAttribute(), EGameplayModOp::Additive, RawDamage));
	}
}
