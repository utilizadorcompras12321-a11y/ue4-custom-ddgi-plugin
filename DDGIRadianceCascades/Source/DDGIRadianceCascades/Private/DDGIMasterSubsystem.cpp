#include "DDGIMasterSubsystem.h"
#include "DDGIMasterActor.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DDGIRadianceCascadeResources.h"
#include "DDGISoftwareBVH.h"

namespace
{
	/**
	 * Builds a deterministic, center-out 3D lattice. Each shell is emitted before
	 * the next one, so a finite probe budget always prioritizes the Master Actor
	 * origin and its nearest neighbors.
	 */
	void DDGIBuildSparseProbePositions(
		const FVector& Origin,
		float Spacing,
		float Radius,
		int32 MaxProbeCount,
		TArray<FVector4>& OutPositions)
	{
		OutPositions.Reset();
		const int32 SafeMaxProbeCount = FMath::Max(0, MaxProbeCount);
		if (SafeMaxProbeCount == 0)
		{
			return;
		}

		const float SafeSpacing = FMath::Max(1.0f, Spacing);
		const float SafeRadius = FMath::Max(0.0f, Radius);
		const int32 MaxShell = FMath::CeilToInt(SafeRadius / SafeSpacing);
		const float RadiusSquared = SafeRadius * SafeRadius;
		OutPositions.Reserve(SafeMaxProbeCount);
		OutPositions.Add(FVector4(Origin.X, Origin.Y, Origin.Z, 1.0f));

		for (int32 Shell = 1; Shell <= MaxShell && OutPositions.Num() < SafeMaxProbeCount; ++Shell)
		{
			for (int32 Z = -Shell; Z <= Shell && OutPositions.Num() < SafeMaxProbeCount; ++Z)
			{
				for (int32 Y = -Shell; Y <= Shell && OutPositions.Num() < SafeMaxProbeCount; ++Y)
				{
					for (int32 X = -Shell; X <= Shell && OutPositions.Num() < SafeMaxProbeCount; ++X)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != Shell)
						{
							continue;
						}

						const FVector Offset(
							static_cast<float>(X) * SafeSpacing,
							static_cast<float>(Y) * SafeSpacing,
							static_cast<float>(Z) * SafeSpacing);
						if (Offset.SizeSquared() > RadiusSquared)
						{
							continue;
						}

						const FVector Position = Origin + Offset;
						OutPositions.Add(FVector4(Position.X, Position.Y, Position.Z, 1.0f));
					}
				}
			}
		}
	}
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
	CachedSoftwareBVHTriangleLimit = INDEX_NONE;
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
	Config.ProbeGridOrigin = InActor->GetActorLocation();
	Config.MaxActiveProbes = FMath::Max(1, InActor->MaxActiveProbes);
	DDGIBuildSparseProbePositions(Config.ProbeGridOrigin, Config.ProbeSpacing, Config.DynamicGIDistance, Config.MaxActiveProbes, Config.ProbePositions);
	Config.ProbeCount = Config.ProbePositions.Num();
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.MaxActiveProbes);
	Config.ProbeUpdateBudget = FMath::Max(0, InActor->ProbeUpdateBudget);
	Config.MaxSoftwareTriangles = FMath::Max(1, InActor->MaxSoftwareTriangles);

	// Build the immutable static-scene BVH once. Moving the Master Actor only
	// moves probe origins; it must not rebuild the world geometry every tick.
	if (!Config.SoftwareBVH.IsValid() || CachedSoftwareBVHTriangleLimit != Config.MaxSoftwareTriangles)
	{
		TSharedPtr<FDDGISoftwareBVHData, ESPMode::ThreadSafe> NewBVH =
			MakeShareable(new FDDGISoftwareBVHData());

		FDDGISoftwareBVHBuildStats BuildStats;
		DDGISoftwareBVH::BuildStaticWorld(
			InActor->GetWorld(),
			Config.MaxSoftwareTriangles,
			NewBVH->PackedNodes,
			NewBVH->PackedTriangles,
			BuildStats);

		NewBVH->IncludedComponents = BuildStats.IncludedComponents;
		NewBVH->SkippedNoCPUAccess = BuildStats.SkippedNoCPUAccess;
		NewBVH->SkippedMovable = BuildStats.SkippedMovable;
		NewBVH->SkippedInstanced = BuildStats.SkippedInstanced;
		NewBVH->TriangleCount = BuildStats.TriangleCount;
		NewBVH->NodeCount = BuildStats.NodeCount;
		Config.SoftwareBVH = NewBVH;
		CachedSoftwareBVHTriangleLimit = Config.MaxSoftwareTriangles;

		UE_LOG(LogTemp, Log,
			TEXT("Radiance Cascades software BVH snapshot: %d triangles, %d nodes, %d static components. Skipped: %d without CPU access, %d movable, %d instanced."),
			NewBVH->TriangleCount, NewBVH->NodeCount, NewBVH->IncludedComponents,
			NewBVH->SkippedNoCPUAccess, NewBVH->SkippedMovable, NewBVH->SkippedInstanced);
	}
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
