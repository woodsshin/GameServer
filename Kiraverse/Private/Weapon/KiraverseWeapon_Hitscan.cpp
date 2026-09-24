#include "Weapon/KiraverseWeapon_Hitscan.h"
#include "Character/KiraverseCharacter.h"
#include "AbilitySystem/Effects/GE_Damage.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"

void AKiraverseWeapon_Hitscan::Fire(AKiraverseCharacter* Shooter)
{
	if (!Shooter || !GetWorld())
	{
		return;
	}

	// Server-authoritative hit detection; NetExecutionPolicy::LocalPredicted also calls
	// this on the firing client, which returns here without tracing.
	if (!Shooter->HasAuthority())
	{
		return;
	}

	const FVector TraceStart = Shooter->GetPawnViewLocation();
	FVector TraceDirection = Shooter->GetControlRotation().Vector();
	if (SpreadAngle > 0.f)
	{
		TraceDirection = FMath::VRandCone(TraceDirection, FMath::DegreesToRadians(SpreadAngle));
	}
	const FVector TraceEnd = TraceStart + TraceDirection * TraceRange;

	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Shooter);
	QueryParams.AddIgnoredActor(this);

	FHitResult HitResult;
	if (!GetWorld()->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, TraceChannel, QueryParams))
	{
		return;
	}

	UAbilitySystemComponent* SourceASC = Shooter->GetAbilitySystemComponent();
	IAbilitySystemInterface* TargetASI = Cast<IAbilitySystemInterface>(HitResult.GetActor());
	UAbilitySystemComponent* TargetASC = TargetASI ? TargetASI->GetAbilitySystemComponent() : nullptr;
	if (!SourceASC || !TargetASC)
	{
		return;
	}

	FGameplayEffectContextHandle EffectContext = SourceASC->MakeEffectContext();
	EffectContext.AddInstigator(Shooter, this);
	EffectContext.AddHitResult(HitResult);

	const FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(UGE_Damage::StaticClass(), 1.f, EffectContext);
	if (SpecHandle.IsValid())
	{
		// Magnitude travels through the effect as a SetByCaller value, read back in the exec calc.
		SpecHandle.Data->SetSetByCallerMagnitude(KiraverseGameplayTags::Data_Damage, GetBaseDamage());
		SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
	}
}
