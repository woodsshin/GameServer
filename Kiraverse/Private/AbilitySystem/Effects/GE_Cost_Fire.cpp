#include "AbilitySystem/Effects/GE_Cost_Fire.h"
#include "AbilitySystem/KiraverseAttributeSet.h"
#include "AbilitySystem/KiraverseGameplayTags.h"

UGE_Cost_Fire::UGE_Cost_Fire()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo AmmoModifier;
	AmmoModifier.Attribute = UKiraverseAttributeSet::GetAmmoAttribute();
	AmmoModifier.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat AmmoMagnitude;
	AmmoMagnitude.DataTag = KiraverseGameplayTags::Data_Cost_Ammo;
	AmmoModifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(AmmoMagnitude);
	Modifiers.Add(AmmoModifier);

	FGameplayModifierInfo StaminaModifier;
	StaminaModifier.Attribute = UKiraverseAttributeSet::GetStaminaAttribute();
	StaminaModifier.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat StaminaMagnitude;
	StaminaMagnitude.DataTag = KiraverseGameplayTags::Data_Cost_Stamina;
	StaminaModifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(StaminaMagnitude);
	Modifiers.Add(StaminaModifier);
}
