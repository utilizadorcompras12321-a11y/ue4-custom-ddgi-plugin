#include "DDGIMasterActor.h"
#include "DDGIMasterSubsystem.h"
#include "Components/SceneComponent.h"

ADDGIMasterActor::ADDGIMasterActor()
{
	SceneRootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRootComponent);
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
}

void ADDGIMasterActor::BeginPlay()
{
	Super::BeginPlay();
	RefreshWorldConfig();
}

void ADDGIMasterActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			Subsystem->RefreshDirectionalLighting(this);
		}
	}
}

void ADDGIMasterActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			Subsystem->UnregisterMasterActor(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void ADDGIMasterActor::RebuildStaticGeometryBVH()
{
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			Subsystem->RebuildSoftwareBVH(this);
		}
	}
}

void ADDGIMasterActor::RefreshWorldConfig()
{
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			Subsystem->RegisterMasterActor(this);
		}
	}
}

#if WITH_EDITOR
void ADDGIMasterActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (BackgroundTransitionEnd < BackgroundTransitionStart)
	{
		BackgroundTransitionEnd = BackgroundTransitionStart;
	}
	RefreshWorldConfig();
}
#endif
