#include "DDGIMasterActor.h"
#include "DDGIMasterSubsystem.h"

ADDGIMasterActor::ADDGIMasterActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
}

void ADDGIMasterActor::BeginPlay()
{
	Super::BeginPlay();
	LastPublishedLocation = GetActorLocation();
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
	const FVector CurrentLocation = GetActorLocation();
	if (!CurrentLocation.Equals(LastPublishedLocation, 0.5f))
	{
		LastPublishedLocation = CurrentLocation;
		RefreshWorldConfig();
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

void ADDGIMasterActor::RefreshWorldConfig()
{
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			Subsystem->RegisterMasterActor(this);
			Subsystem->RefreshConfig(this);
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
