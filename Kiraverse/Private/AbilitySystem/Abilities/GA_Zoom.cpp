#include "AbilitySystem/Abilities/GA_Zoom.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Abilities/Tasks/AbilityTask_WaitInputRelease.h"
#include "Character/KiraverseCharacter.h"
#include "Weapon/KiraverseWeaponComponent.h"
#include "Weapon/KiraverseWeaponBase.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

UGA_Zoom::UGA_Zoom()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Zoom);
	ActivationOwnedTags.AddTag(KiraverseGameplayTags::State_Zooming);
}

void UGA_Zoom::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	CachedHandle = Handle;
	CachedActorInfo = *ActorInfo;
	CachedActivationInfo = ActivationInfo;

	UAbilityTask_WaitInputRelease* WaitRelease = UAbilityTask_WaitInputRelease::WaitInputRelease(this, false);
	WaitRelease->OnRelease.AddDynamic(this, &UGA_Zoom::OnZoomInputReleased);
	WaitRelease->ReadyForActivation();

	// FOV is a local camera effect; nothing to do off the owning client.
	if (!ActorInfo->IsLocallyControlled())
	{
		return;
	}

	const APlayerController* PlayerController = Cast<APlayerController>(ActorInfo->PlayerController.Get());
	APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr;
	const AKiraverseCharacter* Character = Cast<AKiraverseCharacter>(ActorInfo->AvatarActor.Get());
	const AKiraverseWeaponBase* Weapon = Character && Character->GetWeaponComponent()
		? Character->GetWeaponComponent()->GetCurrentWeapon()
		: nullptr;

	if (CameraManager && Weapon)
	{
		CameraManager->SetFOV(Weapon->GetZoomFOV());
	}
}

void UGA_Zoom::OnZoomInputReleased(float TimeHeld)
{
	EndAbility(CachedHandle, &CachedActorInfo, CachedActivationInfo, true, false);
}

void UGA_Zoom::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (ActorInfo && ActorInfo->IsLocallyControlled())
	{
		const APlayerController* PlayerController = Cast<APlayerController>(ActorInfo->PlayerController.Get());
		if (APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr)
		{
			CameraManager->UnlockFOV();
		}
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
