#include "AbilitySystem/Abilities/GA_Fire_Base.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "AbilitySystem/KiraverseAttributeSet.h"
#include "AbilitySystem/Effects/GE_Cost_Fire.h"
#include "Character/KiraverseCharacter.h"
#include "Weapon/KiraverseWeaponComponent.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "AbilitySystemComponent.h"

UGA_Fire_Base::UGA_Fire_Base()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Fire);
}

AKiraverseCharacter* UGA_Fire_Base::GetKiraverseCharacter(const FGameplayAbilityActorInfo* ActorInfo) const
{
	return ActorInfo ? Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
}

AKiraverseWeaponBase* UGA_Fire_Base::GetEquippedWeapon(const FGameplayAbilityActorInfo* ActorInfo) const
{
	if (const AKiraverseCharacter* Character = GetKiraverseCharacter(ActorInfo))
	{
		if (UKiraverseWeaponComponent* WeaponComponent = Character->GetWeaponComponent())
		{
			return WeaponComponent->GetCurrentWeapon();
		}
	}
	return nullptr;
}

bool UGA_Fire_Base::CommitFireCost(const FGameplayAbilityActorInfo* ActorInfo) const
{
	AKiraverseWeaponBase* Weapon = GetEquippedWeapon(ActorInfo);
	UAbilitySystemComponent* ASC = ActorInfo ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
	const UKiraverseAttributeSet* AttributeSet = ASC ? ASC->GetSet<UKiraverseAttributeSet>() : nullptr;
	if (!Weapon || !ASC || !AttributeSet)
	{
		return false;
	}

	if (AttributeSet->GetAmmo() < Weapon->GetAmmoCost() || AttributeSet->GetStamina() < Weapon->GetStaminaCost())
	{
		return false;
	}

	FGameplayEffectContextHandle EffectContext = ASC->MakeEffectContext();
	EffectContext.AddSourceObject(this);
	const FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(UGE_Cost_Fire::StaticClass(), GetAbilityLevel(), EffectContext);
	if (!SpecHandle.IsValid())
	{
		return false;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(KiraverseGameplayTags::Data_Cost_Ammo, -Weapon->GetAmmoCost());
	SpecHandle.Data->SetSetByCallerMagnitude(KiraverseGameplayTags::Data_Cost_Stamina, -Weapon->GetStaminaCost());
	ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
	return true;
}
