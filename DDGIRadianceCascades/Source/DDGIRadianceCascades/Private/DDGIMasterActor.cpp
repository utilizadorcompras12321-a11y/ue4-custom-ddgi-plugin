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
			const float SafeProbeSpacing = FMath::Clamp(ProbeSpacing, 1.0f, 100000.0f);
			const float SafeProbeGridDistance = FMath::Clamp(
				ProbeGridDistance, 1.0f, SafeProbeSpacing * 56.0f);
			const bool bRuntimeConfigChanged =
				CurrentConfig.bEnableDynamicGI != bEnableDynamicGI ||
				CurrentConfig.bEnableStaticBackground != bEnableStaticBackground ||
				!FMath::IsNearlyEqual(CurrentConfig.GIIntensity, FMath::Max(0.0f, GIIntensity), 1.0e-4f) ||
				!FMath::IsNearlyEqual(CurrentConfig.LocalLightGIIntensityScale, FMath::Max(0.0f, LocalLightGIIntensityScale), 1.0e-4f) ||
				!FMath::IsNearlyEqual(CurrentConfig.DynamicGIDistance, FMath::Max(1.0f, DynamicGIDistance), 1.0e-3f) ||
				!FMath::IsNearlyEqual(CurrentConfig.ProbeGridDistance, SafeProbeGridDistance, 1.0e-3f) ||
				!FMath::IsNearlyEqual(CurrentConfig.ProbeSpacing, SafeProbeSpacing, 1.0e-4f) ||
				CurrentConfig.RadianceCascades.CascadeCount != FMath::Clamp(CascadeCount, 1, 8) ||
				CurrentConfig.RadianceCascades.BaseRayCount != FMath::Clamp(BaseRayCount, 1, 64) ||
				CurrentConfig.RadianceCascades.RaySplitFactor != FMath::Clamp(RaySplitFactor, 1, 4) ||
				!FMath::IsNearlyEqual(CurrentConfig.RadianceCascades.BaseSegmentLength, FMath::Max(1.0f, BaseSegmentLength), 1.0e-4f) ||
				!FMath::IsNearlyEqual(CurrentConfig.RadianceCascades.CascadeScale, FMath::Max(1.01f, CascadeScale), 1.0e-4f) ||
				CurrentConfig.MaxActiveProbes != FMath::Clamp(MaxActiveProbes, 1, 32768) ||
				CurrentConfig.MaxSoftwareTriangles != FMath::Clamp(MaxSoftwareTriangles, 1, 250000) ||
				CurrentConfig.ProbeUpdateBudget != FMath::Max(0, ProbeUpdateBudget) ||
				!CurrentConfig.StaticBackgroundColor.Equals(StaticBackgroundColor, 1.0e-4f) ||
				!FMath::IsNearlyEqual(CurrentConfig.StaticBackgroundIntensity, FMath::Max(0.0f, StaticBackgroundIntensity), 1.0e-4f) ||
				!FMath::IsNearlyEqual(CurrentConfig.BackgroundTransitionStart, FMath::Max(0.0f, BackgroundTransitionStart), 1.0e-3f) ||
				!FMath::IsNearlyEqual(CurrentConfig.BackgroundTransitionEnd,
					FMath::Max(FMath::Max(0.0f, BackgroundTransitionStart), BackgroundTransitionEnd), 1.0e-3f);
			if (bRuntimeConfigChanged)
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
