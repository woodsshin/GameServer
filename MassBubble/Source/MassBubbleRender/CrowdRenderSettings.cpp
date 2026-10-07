#include "CrowdRenderSettings.h"

UCrowdRenderSettings::UCrowdRenderSettings()
{
	CategoryName = TEXT("Game");
	AgentMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
}
