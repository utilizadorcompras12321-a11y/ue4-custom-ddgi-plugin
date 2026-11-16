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

void ADDGIMasterActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->WorldType == EWorldType::Editor)
		{
			RefreshWorldConfig();
		}
	}
#endif
}

void ADDGIMasterActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (UWorld* World = GetWorld())
	{
		if (UDDGIMasterSubsystem* Subsystem = World->GetSubsystem<UDDGIMasterSubsystem>())
		{
			// Blueprint/runtime changes to the GI enable switch must reach
			// the render thread too; editor property callbacks do not run here.
			const FDDGIMasterConfig& CurrentConfig = Subsystem->GetConfig();
			if (CurrentConfig.bEnableDynamicGI != bEnableDynamicGI ||
				CurrentConfig.bEnableStaticBackground != bEnableStaticBackground ||
				CurrentConfig.ProbeUpdateBudget != FMath::Max(0, ProbeUpdateBudget))
			{
				Subsystem->RefreshConfig(this);
			}

			if (bEnableDynamicGI)
			{
				Subsystem->RefreshDirectionalLighting(this);
				Subsystem->UpdateActiveProbeWindow(this);
			}
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
