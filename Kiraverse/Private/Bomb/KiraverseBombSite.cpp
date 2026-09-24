#include "Bomb/KiraverseBombSite.h"
#include "Character/KiraverseCharacter.h"
#include "Components/BoxComponent.h"

AKiraverseBombSite::AKiraverseBombSite()
{
	PrimaryActorTick.bCanEverTick = false;

	PlantZone = CreateDefaultSubobject<UBoxComponent>(TEXT("PlantZone"));
	PlantZone->InitBoxExtent(FVector(200.f, 200.f, 100.f));
	PlantZone->SetCollisionProfileName(TEXT("Trigger"));
	SetRootComponent(PlantZone);

	PlantSocket = CreateDefaultSubobject<USceneComponent>(TEXT("PlantSocket"));
	PlantSocket->SetupAttachment(PlantZone);
}

bool AKiraverseBombSite::IsCharacterInPlantZone(const AKiraverseCharacter* Character) const
{
	if (!Character || !PlantZone)
	{
		return false;
	}

	return PlantZone->IsOverlappingActor(Character);
}
