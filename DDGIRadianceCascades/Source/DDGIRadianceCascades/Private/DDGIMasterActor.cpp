#include "DDGIMasterActor.h"
#include "DDGIMasterSubsystem.h"

ADDGIMasterActor::ADDGIMasterActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
}

void ADDGIMasterActor::BeginPlay()
{
	Super::BeginPlay();
	RefreshWorldConfig();
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
