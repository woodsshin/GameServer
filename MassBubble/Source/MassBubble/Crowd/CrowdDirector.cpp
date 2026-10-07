#include "Crowd/CrowdDirector.h"

#include "Crowd/CrowdSubsystem.h"
#include "Engine/World.h"

bool UCrowdDirector::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->GetNetMode() != NM_Client;
}

bool UCrowdDirector::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UCrowdDirector::Initialize(FSubsystemCollectionBase& Collection)
{
	// Make sure the crowd subsystem (and through it the Mass entity subsystem) exists before we tick.
	Crowd = Collection.InitializeDependency<UCrowdSubsystem>();
	Super::Initialize(Collection);
}

void UCrowdDirector::Tick(float DeltaTime)
{
	if (UCrowdSubsystem* Subsystem = Crowd.Get())
	{
		Subsystem->TickDirector(DeltaTime);
	}
}

TStatId UCrowdDirector::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdDirector, STATGROUP_Tickables);
}
