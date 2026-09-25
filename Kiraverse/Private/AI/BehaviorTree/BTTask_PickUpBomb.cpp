#include "AI/BehaviorTree/BTTask_PickUpBomb.h"
#include "AI/KiraverseAIController.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBombComponent.h"
#include "Character/KiraverseCharacter.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

UBTTask_PickUpBomb::UBTTask_PickUpBomb()
{
	NodeName = TEXT("Pick Up Bomb");
}

FString UBTTask_PickUpBomb::GetStaticDescription() const
{
	return TEXT("Activate Bomb.PickUp; succeeds if the bot is now carrying the bomb.");
}

EBTNodeResult::Type UBTTask_PickUpBomb::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	const UKiraverseBombComponent* BombComponent = Self ? Self->GetBombComponent() : nullptr;
	if (!Controller || !BombComponent)
	{
		return EBTNodeResult::Failed;
	}

	// GA_Bomb_Interact toggles between pick-up and drop; activating it while carrying would drop the bomb.
	if (BombComponent->HasBomb())
	{
		return EBTNodeResult::Succeeded;
	}

	Controller->TryActivateAbilityByTag(KiraverseGameplayTags::Ability_Bomb_PickUp.GetTag());

	// The ability's own pickup-radius check decides success; the carry state afterwards is the real answer.
	return BombComponent->HasBomb() ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
}
