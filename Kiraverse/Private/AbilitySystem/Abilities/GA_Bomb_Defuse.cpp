#include "AbilitySystem/Abilities/GA_Bomb_Defuse.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBomb.h"
#include "Character/KiraverseCharacter.h"
#include "Kismet/GameplayStatics.h"

UGA_Bomb_Defuse::UGA_Bomb_Defuse()
{
	AbilityTags.AddTag(KiraverseGameplayTags::Ability_Bomb_Defuse);
	ActivationOwnedTags.AddTag(KiraverseGameplayTags::State_Defusing);

	// Defenders-only — mirror of GA_Bomb_Plant's Team.Defenders block (see that class's
	// constructor comment re: Team.None activators not being reachable in practice).
	ActivationBlockedTags.AddTag(KiraverseGameplayTags::Team_Attackers);
}

bool UGA_Bomb_Defuse::CanStartChannel(const AKiraverseCharacter* Character) const
{
	return FindPlantedBombInRange(Character) != nullptr;
}

bool UGA_Bomb_Defuse::OnChannelCompleted(AKiraverseCharacter* Character)
{
	// Re-resolve rather than caching from CanStartChannel — same reasoning as
	// UGA_Bomb_Plant::OnChannelCompleted: confirm the bomb is still there and still Planted
	// (not, e.g., already exploded in the instant between the last interrupt check and this call)
	// right before acting on it.
	AKiraverseBomb* Bomb = FindPlantedBombInRange(Character);
	return Bomb && Bomb->Defuse();
}

AKiraverseBomb* UGA_Bomb_Defuse::FindPlantedBombInRange(const AKiraverseCharacter* Character) const
{
	if (!Character || !GetWorld())
	{
		return nullptr;
	}

	TArray<AActor*> Bombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AKiraverseBomb::StaticClass(), Bombs);

	const FVector OwnerLocation = Character->GetActorLocation();
	for (AActor* BombActor : Bombs)
	{
		AKiraverseBomb* Bomb = Cast<AKiraverseBomb>(BombActor);
		if (Bomb && Bomb->GetBombState() == EBombState::Planted
			&& FVector::DistSquared(OwnerLocation, Bomb->GetActorLocation()) <= FMath::Square(DefuseRange))
		{
			return Bomb;
		}
	}

	return nullptr;
}
