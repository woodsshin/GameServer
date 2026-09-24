#include "AbilitySystem/Effects/GE_Cooldown_Dash.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_Cooldown_Dash::UGE_Cooldown_Dash()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(1.5f));

	// GE Components (5.3+) replace the old InheritableOwnedTagsContainer for granting tags.
	UTargetTagsGameplayEffectComponent& TagsComponent = AddComponent<UTargetTagsGameplayEffectComponent>();
	FInheritedTagContainer TagChanges;
	TagChanges.Added.AddTag(KiraverseGameplayTags::Cooldown_Dash);
	TagsComponent.SetAndApplyTargetTagChanges(TagChanges);
}
