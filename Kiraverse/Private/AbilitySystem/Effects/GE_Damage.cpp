#include "AbilitySystem/Effects/GE_Damage.h"
#include "AbilitySystem/Calculations/DamageExecCalculation.h"

UGE_Damage::UGE_Damage()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayEffectExecutionDefinition ExecutionDefinition;
	ExecutionDefinition.CalculationClass = UDamageExecCalculation::StaticClass();
	Executions.Add(ExecutionDefinition);
}
