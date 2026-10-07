#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdDirector.generated.h"

class UCrowdSubsystem;

/**
 * Game-thread driver for UCrowdSubsystem.
 *
 * Why a separate class: Mass processors that query entities are pruned when no matching archetype exists yet,
 * so the very first spawn can never be triggered from inside a processor. A tickable world subsystem bootstraps
 * the population, then Mass takes over the per-entity work.
 */
UCLASS()
class MASSBUBBLE_API UCrowdDirector : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	TWeakObjectPtr<UCrowdSubsystem> Crowd;
};
