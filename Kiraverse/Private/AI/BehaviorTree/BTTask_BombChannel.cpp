#include "AI/BehaviorTree/BTTask_BombChannel.h"
#include "AI/KiraverseAIController.h"
#include "AI/KiraverseAIQueries.h"
#include "AbilitySystem/KiraverseGameplayTags.h"
#include "Bomb/KiraverseBomb.h"
#include "Character/KiraverseCharacter.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

UBTTask_BombChannel::UBTTask_BombChannel()
{
	NodeName = TEXT("Bomb Channel (Plant/Defuse)");
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

FString UBTTask_BombChannel::GetStaticDescription() const
{
	return TEXT("Plant as attacker, defuse as defender; holds still until the channel ends.");
}

FGameplayTag UBTTask_BombChannel::GetChannelAbilityTag(const AKiraverseAIController* Controller) const
{
	switch (Controller ? Controller->GetOwnTeam() : ETeam::None)
	{
	case ETeam::Attackers: return KiraverseGameplayTags::Ability_Bomb_Plant.GetTag();
	case ETeam::Defenders: return KiraverseGameplayTags::Ability_Bomb_Defuse.GetTag();
	default: return FGameplayTag();
	}
}

bool UBTTask_BombChannel::DidChannelSucceed(ETeam Team, const AKiraverseBomb* Bomb) const
{
	// Judge by the world, not by the ability's internals: an interrupted channel leaves the bomb's state untouched.
	if (!Bomb)
	{
		// The bomb actor is gone: it was destroyed after the round ended, which only a completed defuse or an explosion causes.
		// Defusing a bomb that then vanished is a success; a vanished bomb on the plant side never happens mid-channel.
		return Team == ETeam::Defenders;
	}

	switch (Team)
	{
	case ETeam::Attackers: return Bomb->GetBombState() == EBombState::Planted;
	case ETeam::Defenders: return Bomb->GetBombState() == EBombState::Defused;
	default: return false;
	}
}

EBTNodeResult::Type UBTTask_BombChannel::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FBombChannelMemory* Memory = reinterpret_cast<FBombChannelMemory*>(NodeMemory);
	*Memory = FBombChannelMemory();

	AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;
	const FGameplayTag AbilityTag = GetChannelAbilityTag(Controller);
	if (!Self || !AbilityTag.IsValid())
	{
		return EBTNodeResult::Failed;
	}

	// Remember which bomb this channel acts on before anything can destroy it, so the outcome is judged against the right actor.
	Memory->TargetBomb = KiraverseAIQueries::FindLiveBomb(Self->GetWorld());

	// The channel must be still: any drift past ChannelInterruptMoveRadius cancels it, so stop path-following first.
	Controller->StopMovement();

	// The ability's own CanStartChannel decides range validity; a false here means the bot is not in a valid spot.
	if (!Controller->TryActivateAbilityByTag(AbilityTag))
	{
		return EBTNodeResult::Failed;
	}

	// Activation is synchronous on the server: a live channel already shows its state tag, none means CanStartChannel refused it.
	Memory->bChannelObserved = KiraverseAIQueries::IsChanneling(Self);
	return Memory->bChannelObserved ? EBTNodeResult::InProgress : EBTNodeResult::Failed;
}

void UBTTask_BombChannel::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FBombChannelMemory* Memory = reinterpret_cast<FBombChannelMemory*>(NodeMemory);
	const AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner());
	const AKiraverseCharacter* Self = Controller ? Cast<AKiraverseCharacter>(Controller->GetPawn()) : nullptr;

	if (!Self || KiraverseAIQueries::IsDead(Self))
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	// The ability drops its state tag when the channel ends, whether it completed or was interrupted.
	if (KiraverseAIQueries::IsChanneling(Self))
	{
		return;
	}

	// The channel is over; only the bomb's resulting state says which way it ended.
	const bool bSucceeded = DidChannelSucceed(Controller->GetOwnTeam(), Memory->TargetBomb.Get());
	FinishLatentTask(OwnerComp, bSucceeded ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
}

EBTNodeResult::Type UBTTask_BombChannel::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// A tree that moves on (threat appeared, round ended) must not leave the plant/defuse ticking in the background.
	CancelChannel(OwnerComp);
	return EBTNodeResult::Aborted;
}

void UBTTask_BombChannel::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	// Only a task that did not run its course needs the ability stopped; a Succeeded channel already ended itself.
	if (TaskResult != EBTNodeResult::Succeeded)
	{
		CancelChannel(OwnerComp);
	}
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_BombChannel::CancelChannel(UBehaviorTreeComponent& OwnerComp) const
{
	if (AKiraverseAIController* Controller = Cast<AKiraverseAIController>(OwnerComp.GetAIOwner()))
	{
		Controller->CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Bomb_Plant.GetTag());
		Controller->CancelAbilitiesByTag(KiraverseGameplayTags::Ability_Bomb_Defuse.GetTag());
	}
}
