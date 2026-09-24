#include "Weapon/KiraverseWeaponComponent.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "AbilitySystem/Abilities/GA_Fire_Base.h"
#include "Character/KiraverseCharacter.h"
#include "Net/UnrealNetwork.h"

UKiraverseWeaponComponent::UKiraverseWeaponComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UKiraverseWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UKiraverseWeaponComponent, CurrentWeapon);
}

void UKiraverseWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	if (DefaultWeaponClasses.Num() > 0)
	{
		EquipWeaponAtIndex(0);
	}
}

void UKiraverseWeaponComponent::EquipWeaponAtIndex(int32 Index)
{
	if (DefaultWeaponClasses.IsValidIndex(Index))
	{
		EquipWeapon(DefaultWeaponClasses[Index]);
	}
}

void UKiraverseWeaponComponent::EquipWeapon(TSubclassOf<AKiraverseWeaponBase> WeaponClass)
{
	AKiraverseCharacter* OwningCharacter = Cast<AKiraverseCharacter>(GetOwner());
	if (!OwningCharacter || !WeaponClass || !OwningCharacter->HasAuthority() || !GetWorld())
	{
		return;
	}

	UnequipCurrentWeapon();

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = OwningCharacter;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	CurrentWeapon = GetWorld()->SpawnActor<AKiraverseWeaponBase>(WeaponClass, FTransform::Identity, SpawnParams);
	if (!CurrentWeapon)
	{
		return;
	}

	CurrentWeapon->AttachToComponent(OwningCharacter->GetMesh(),
		FAttachmentTransformRules(EAttachmentRule::SnapToTarget, true), WeaponSocketName);

	// Granting this weapon's fire ability is what makes Ability.Fire input resolve to it.
	if (UAbilitySystemComponent* ASC = OwningCharacter->GetAbilitySystemComponent())
	{
		if (TSubclassOf<UGA_Fire_Base> FireAbilityClass = CurrentWeapon->GetFireAbilityClass())
		{
			CurrentFireAbilityHandle = ASC->GiveAbility(FGameplayAbilitySpec(FireAbilityClass, 1));
		}
	}
}

void UKiraverseWeaponComponent::UnequipCurrentWeapon()
{
	if (!CurrentWeapon)
	{
		return;
	}

	if (const AKiraverseCharacter* OwningCharacter = Cast<AKiraverseCharacter>(GetOwner()))
	{
		if (UAbilitySystemComponent* ASC = OwningCharacter->GetAbilitySystemComponent())
		{
			if (CurrentFireAbilityHandle.IsValid())
			{
				ASC->ClearAbility(CurrentFireAbilityHandle);
			}
		}
	}

	CurrentWeapon->Destroy();
	CurrentWeapon = nullptr;
}

void UKiraverseWeaponComponent::OnRep_CurrentWeapon(AKiraverseWeaponBase* OldWeapon)
{
	// Client-side hook for reacting to a weapon swap (HUD, zoom FOV, equip FX).
}
