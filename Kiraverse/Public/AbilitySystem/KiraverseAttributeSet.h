#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "KiraverseAttributeSet.generated.h"

// Wraps the four GAMEPLAYATTRIBUTE_* macros the engine provides for one attribute.
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

// Health and stamina pool shared by every Kiraverse character.
UCLASS()
class KIRAVERSE_API UKiraverseAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UKiraverseAttributeSet();

	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Health", ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Health", ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, MaxHealth)

	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Stamina", ReplicatedUsing = OnRep_Stamina)
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, Stamina)

	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Stamina", ReplicatedUsing = OnRep_MaxStamina)
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, MaxStamina)

	// Meta attribute: incoming damage lands here first, then gets folded into Health.
	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Meta")
	FGameplayAttributeData Damage;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, Damage)

	// Spent per shot by GE_Cost_Fire. Not a meta attribute: clients read it in CommitFireCost, so it must replicate.
	UPROPERTY(BlueprintReadOnly, Category = "Kiraverse|Ammo", ReplicatedUsing = OnRep_Ammo)
	FGameplayAttributeData Ammo;
	ATTRIBUTE_ACCESSORS(UKiraverseAttributeSet, Ammo)

	//~ Begin UAttributeSet interface
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End UAttributeSet interface

protected:
	UFUNCTION()
	virtual void OnRep_Health(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	virtual void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	virtual void OnRep_Stamina(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	virtual void OnRep_MaxStamina(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	virtual void OnRep_Ammo(const FGameplayAttributeData& OldValue);

private:
	// Keeps Health/Stamina inside [0, Max] whenever an effect tries to change them.
	void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const;
};
