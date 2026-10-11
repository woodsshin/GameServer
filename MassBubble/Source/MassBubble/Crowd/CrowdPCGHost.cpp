#include "Crowd/CrowdPCGHost.h"

#include "Components/BoxComponent.h"
#include "Misc/EngineVersionComparison.h"

#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"

// The point data classes changed in UE 5.6 (struct-of-arrays instead of TArray<FPCGPoint>). Only CollectPoints() touches them.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Data/PCGPointData.h"
#else
#include "Data/PCGBasePointData.h"
#endif

ACrowdPCGHost::ACrowdPCGHost()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	RegionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("RegionBox"));
	RegionBox->InitBoxExtent(FVector(100.0, 100.0, 100.0));
	RegionBox->SetMobility(EComponentMobility::Movable);
	// PCG takes the generation bounds from the primitive components of the actor. The box must not collide with anything,
	// but it keeps a query body that ignores every channel: a bounds helper that only looks at colliding components
	// still sees it, and nothing can ever hit it.
	RegionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	RegionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	RegionBox->SetGenerateOverlapEvents(false);
	RegionBox->SetCanEverAffectNavigation(false);
	SetRootComponent(RegionBox);

	PCGRunner = CreateDefaultSubobject<UPCGComponent>(TEXT("PCGRunner"));
	// Never generate by itself (on load, on begin play): the planner decides when, with the bounds and seed of one region.
	PCGRunner->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
}

void ACrowdPCGHost::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if (PCGRunner != nullptr)
	{
		GeneratedHandle = PCGRunner->OnPCGGraphGeneratedDelegate.AddUObject(this, &ACrowdPCGHost::HandleGenerated);
		CancelledHandle = PCGRunner->OnPCGGraphCancelledDelegate.AddUObject(this, &ACrowdPCGHost::HandleCancelled);
	}
}

void ACrowdPCGHost::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PCGRunner != nullptr)
	{
		PCGRunner->OnPCGGraphGeneratedDelegate.Remove(GeneratedHandle);
		PCGRunner->OnPCGGraphCancelledDelegate.Remove(CancelledHandle);
	}
	GeneratedHandle.Reset();
	CancelledHandle.Reset();

	Super::EndPlay(EndPlayReason);
}

void ACrowdPCGHost::SetGenerationBounds(double SizeCm, double HalfHeightCm)
{
	if (RegionBox != nullptr)
	{
		const double Half = 0.5 * SizeCm;
		RegionBox->InitBoxExtent(FVector(Half, Half, FMath::Max(HalfHeightCm, 1.0)));
	}
}

bool ACrowdPCGHost::StartGeneration(UPCGGraphInterface* Graph, int32 Seed)
{
	if (PCGRunner == nullptr || Graph == nullptr)
	{
		return false;
	}

	bFinished = false;
	bSucceeded = false;

	PCGRunner->Seed = Seed;
	PCGRunner->SetGraph(Graph);

	// Local and forced: this component lives on the server only, nothing of it is replicated. The result arrives through
	// OnPCGGraphGeneratedDelegate (some frames later: PCG schedules the run on its own executor).
	PCGRunner->GenerateLocal(/*bForce=*/true);
	return true;
}

void ACrowdPCGHost::HandleGenerated(UPCGComponent* Component)
{
	bFinished = true;
	bSucceeded = true;
}

void ACrowdPCGHost::HandleCancelled(UPCGComponent* Component)
{
	bFinished = true;
	bSucceeded = false;
}

int32 ACrowdPCGHost::CollectPoints(TArray<FCrowdPlanPoint>& OutPoints) const
{
	if (PCGRunner == nullptr)
	{
		return 0;
	}

	const int32 Before = OutPoints.Num();
	const FPCGDataCollection& Output = PCGRunner->GetGeneratedGraphOutput();

	// Every point data item on the output node counts: a graph may deliver several (one per branch).
	for (const FPCGTaggedData& Tagged : Output.TaggedData)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		const UPCGPointData* PointData = Cast<UPCGPointData>(Tagged.Data);
		if (PointData == nullptr)
		{
			continue;
		}

		OutPoints.Reserve(OutPoints.Num() + PointData->GetPoints().Num());
		for (const FPCGPoint& Point : PointData->GetPoints())
		{
			FCrowdPlanPoint& Out = OutPoints.AddDefaulted_GetRef();
			const FVector Location = Point.Transform.GetLocation();
			Out.Location = FVector2D(Location.X, Location.Y);
			Out.YawRad = FMath::DegreesToRadians(static_cast<float>(Point.Transform.Rotator().Yaw));
			Out.Seed = static_cast<uint32>(Point.Seed);
			Out.Density = Point.Density;
		}
#else
		const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Tagged.Data);
		if (PointData == nullptr)
		{
			continue;
		}

		const int32 Num = PointData->GetNumPoints();
		const auto Transforms = PointData->GetConstTransformValueRange();
		const auto Seeds = PointData->GetConstSeedValueRange();
		const auto Densities = PointData->GetConstDensityValueRange();

		OutPoints.Reserve(OutPoints.Num() + Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FTransform& Transform = Transforms[Index];

			FCrowdPlanPoint& Out = OutPoints.AddDefaulted_GetRef();
			const FVector Location = Transform.GetLocation();
			Out.Location = FVector2D(Location.X, Location.Y);
			Out.YawRad = FMath::DegreesToRadians(static_cast<float>(Transform.Rotator().Yaw));
			Out.Seed = static_cast<uint32>(Seeds[Index]);
			Out.Density = Densities[Index];
		}
#endif
	}

	return OutPoints.Num() - Before;
}
