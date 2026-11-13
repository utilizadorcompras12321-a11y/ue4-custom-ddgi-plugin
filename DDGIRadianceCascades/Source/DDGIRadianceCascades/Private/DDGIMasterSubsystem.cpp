#include "DDGIMasterSubsystem.h"
#include "DDGIMasterActor.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DDGIRadianceCascadeResources.h"

namespace
{
	/** Accessed only on the render thread. */
	TMap<const FSceneInterface*, FDDGIMasterConfig> GDDGIMasterConfigs_RenderThread;
}

bool DDGIGetMasterConfig_RenderThread(const FSceneInterface* Scene, FDDGIMasterConfig& OutConfig)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());

	if (const FDDGIMasterConfig* FoundConfig = GDDGIMasterConfigs_RenderThread.Find(Scene))
	{
		OutConfig = *FoundConfig;
		return true;
	}
	return false;
}

void UDDGIMasterSubsystem::RegisterMasterActor(ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor))
	{
		return;
	}

	if (ActiveMasterActor.IsValid() && ActiveMasterActor.Get() != InActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("DDGI: World '%s' already has a DDGI Master Actor. The additional actor '%s' will be ignored."),
			*GetNameSafe(GetWorld()), *GetNameSafe(InActor));
		return;
	}

	ActiveMasterActor = InActor;
	CopyConfigFromActor(InActor);
}

void UDDGIMasterSubsystem::UnregisterMasterActor(ADDGIMasterActor* InActor)
{
	if (ActiveMasterActor.Get() != InActor)
	{
		return;
	}

	ActiveMasterActor.Reset();
	Config = FDDGIMasterConfig();
	PublishConfig(false);
}

void UDDGIMasterSubsystem::RefreshConfig(ADDGIMasterActor* InActor)
{
	if (IsValid(InActor) && ActiveMasterActor.Get() == InActor)
	{
		CopyConfigFromActor(InActor);
	}
}

void UDDGIMasterSubsystem::CopyConfigFromActor(const ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor))
	{
		return;
	}

	Config.bEnableDynamicGI = InActor->bEnableDynamicGI;
	Config.GIIntensity = FMath::Max(0.0f, InActor->GIIntensity);
	Config.DynamicGIDistance = FMath::Max(0.0f, InActor->DynamicGIDistance);
	Config.RadianceCascades.CascadeCount = FMath::Clamp(InActor->CascadeCount, 1, 8);
	Config.RadianceCascades.BaseRayCount = FMath::Clamp(InActor->BaseRayCount, 1, 64);
	Config.RadianceCascades.RaySplitFactor = FMath::Clamp(InActor->RaySplitFactor, 1, 4);
	Config.RadianceCascades.BaseSegmentLength = FMath::Max(1.0f, InActor->BaseSegmentLength);
	Config.RadianceCascades.CascadeScale = FMath::Max(1.01f, InActor->CascadeScale);
	Config.RadianceCascades.MaxTraceDistance = FMath::Max(1.0f, InActor->DynamicGIDistance);
	Config.RadianceCascades.BuildCascadeSlices(Config.RadianceCascadeSlices);
	Config.TotalRaysPerProbe = Config.RadianceCascades.GetTotalRayCount();
	Config.ProbeSpacing = FMath::Max(1.0f, InActor->ProbeSpacing);
	Config.MaxActiveProbes = FMath::Max(1, InActor->MaxActiveProbes);
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.MaxActiveProbes);
	Config.ProbeUpdateBudget = FMath::Max(0, InActor->ProbeUpdateBudget);
	Config.bEnableStaticBackground = InActor->bEnableStaticBackground;
	Config.StaticBackgroundColor = InActor->StaticBackgroundColor;
	Config.StaticBackgroundIntensity = FMath::Max(0.0f, InActor->StaticBackgroundIntensity);
	Config.BackgroundTransitionStart = FMath::Max(0.0f, InActor->BackgroundTransitionStart);
	Config.BackgroundTransitionEnd = FMath::Max(Config.BackgroundTransitionStart, InActor->BackgroundTransitionEnd);
	PublishConfig(true);
}

void UDDGIMasterSubsystem::PublishConfig(bool bHasConfig)
{
	UWorld* World = GetWorld();
	FSceneInterface* Scene = World ? World->Scene : nullptr;
	if (!Scene)
	{
		return;
	}

	const FDDGIMasterConfig ConfigCopy = Config;
	ENQUEUE_RENDER_COMMAND(DDGIPublishMasterConfig)(
		[Scene, bHasConfig, ConfigCopy](FRHICommandListImmediate& RHICmdList)
		{
			if (bHasConfig)
			{
				GDDGIMasterConfigs_RenderThread.Add(Scene, ConfigCopy);
				DDGIUpdateRadianceCascadeResources_RenderThread(RHICmdList, Scene, ConfigCopy);
			}
			else
			{
				GDDGIMasterConfigs_RenderThread.Remove(Scene);
				DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
			}
		});
}
