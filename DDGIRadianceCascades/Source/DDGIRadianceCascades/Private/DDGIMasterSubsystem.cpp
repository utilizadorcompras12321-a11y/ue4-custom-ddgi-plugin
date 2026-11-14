#include "DDGIMasterSubsystem.h"
#include "DDGIMasterActor.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DDGIRadianceCascadeResources.h"
#include "DDGISoftwareBVH.h"
#include "Components/DirectionalLightComponent.h"

namespace
{
	/**
	 * Counts lattice points inside the spherical probe domain. Stops as soon as
	 * the budget is exceeded; this is used only to select a spacing, not to
	 * construct the final (complete) lattice.
	 */
	int32 DDGICountSparseProbePositions(
		float Spacing,
		float Radius,
		int32 StopAfter)
	{
		const float SafeSpacing = FMath::Max(1.0f, Spacing);
		const float SafeRadius = FMath::Max(0.0f, Radius);
		const int32 MaxShell = FMath::CeilToInt(SafeRadius / SafeSpacing);
		const double RadiusSquared = static_cast<double>(SafeRadius) * SafeRadius;
		int32 Count = 1; // Origin.
		for (int32 Shell = 1; Shell <= MaxShell; ++Shell)
		{
			for (int32 Z = -Shell; Z <= Shell; ++Z)
			{
				for (int32 Y = -Shell; Y <= Shell; ++Y)
				{
					for (int32 X = -Shell; X <= Shell; ++X)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != Shell)
						{
							continue;
						}
					const double DX = static_cast<double>(X) * SafeSpacing;
					const double DY = static_cast<double>(Y) * SafeSpacing;
					const double DZ = static_cast<double>(Z) * SafeSpacing;
					if (DX * DX + DY * DY + DZ * DZ > RadiusSquared)
					{
						continue;
					}
					if (++Count > StopAfter)
					{
						return Count;
					}
				}
				}
			}
		}
		return Count;
	}
	/**
	 * Builds a complete, deterministic 3D lattice inside the configured sphere.
	 * If the requested spacing exceeds the probe budget, spacing is increased
	 * uniformly until the whole spherical domain fits. No partial shell is kept.
	 */
	float DDGIBuildSparseProbePositions(
		const FVector& Origin,
		float RequestedSpacing,
		float Radius,
		int32 MaxProbeCount,
		TArray<FVector4>& OutPositions)
	{
		OutPositions.Reset();
		const int32 SafeMaxProbeCount = FMath::Max(0, MaxProbeCount);
		if (SafeMaxProbeCount == 0)
		{
			return FMath::Max(1.0f, RequestedSpacing);
		}

		const float SafeRadius = FMath::Max(0.0f, Radius);
		float EffectiveSpacing = FMath::Max(1.0f, RequestedSpacing);
		if (DDGICountSparseProbePositions(EffectiveSpacing, SafeRadius, SafeMaxProbeCount) > SafeMaxProbeCount)
		{
			float LowerSpacing = EffectiveSpacing;
			float UpperSpacing = FMath::Max(EffectiveSpacing, SafeRadius * 2.0f + EffectiveSpacing);
			for (int32 Iteration = 0; Iteration < 24; ++Iteration)
			{
				const float CandidateSpacing = LowerSpacing + (UpperSpacing - LowerSpacing) * 0.5f;
				if (DDGICountSparseProbePositions(CandidateSpacing, SafeRadius, SafeMaxProbeCount) > SafeMaxProbeCount)
				{
					LowerSpacing = CandidateSpacing;
				}
				else
				{
					UpperSpacing = CandidateSpacing;
				}
			}
			EffectiveSpacing = UpperSpacing;
		}

		const int32 MaxShell = FMath::CeilToInt(SafeRadius / EffectiveSpacing);
		const double RadiusSquared = static_cast<double>(SafeRadius) * SafeRadius;
		OutPositions.Reserve(SafeMaxProbeCount);
		OutPositions.Add(FVector4(Origin.X, Origin.Y, Origin.Z, 1.0f));

		for (int32 Z = -MaxShell; Z <= MaxShell; ++Z)
		{
			for (int32 Y = -MaxShell; Y <= MaxShell; ++Y)
			{
				for (int32 X = -MaxShell; X <= MaxShell; ++X)
				{
					if (X == 0 && Y == 0 && Z == 0)
					{
						continue;
					}
					const double DX = static_cast<double>(X) * EffectiveSpacing;
					const double DY = static_cast<double>(Y) * EffectiveSpacing;
					const double DZ = static_cast<double>(Z) * EffectiveSpacing;
					if (DX * DX + DY * DY + DZ * DZ > RadiusSquared)
					{
						continue;
					}
					const FVector Position = Origin + FVector(
						static_cast<float>(DX),
						static_cast<float>(DY),
						static_cast<float>(DZ));
					OutPositions.Add(FVector4(Position.X, Position.Y, Position.Z, 1.0f));
				}
			}
		}
		return EffectiveSpacing;
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

void UDDGIMasterSubsystem::Deinitialize()
{
	// Remove render-thread state while the world's scene is still available.
	// The queued command is ordered after any earlier config publications.
	UWorld* World = GetWorld();
	FSceneInterface* Scene = World ? World->Scene : nullptr;
	if (Scene)
	{
		ENQUEUE_RENDER_COMMAND(DDGIDeinitializeWorldSubsystem)(
			[Scene](FRHICommandListImmediate& RHICmdList)
			{
				GDDGIMasterConfigs_RenderThread.Remove(Scene);
				DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
			});
	}

	ActiveMasterActor.Reset();
	ActiveDirectionalLight.Reset();
	Config = FDDGIMasterConfig();
	CachedSoftwareBVHTriangleLimit = INDEX_NONE;
	bHasSearchedDirectionalLights = false;

	Super::Deinitialize();
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

void UDDGIMasterSubsystem::RefreshDirectionalLighting(ADDGIMasterActor* InActor)
{
	if (IsValid(InActor) && ActiveMasterActor.Get() == InActor && UpdateDirectionalLighting(InActor))
	{
		PublishConfigValuesOnly();
	}
}

bool UDDGIMasterSubsystem::UpdateDirectionalLighting(const ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor))
	{
		return false;
	}

	UDirectionalLightComponent* Light = ActiveDirectionalLight.Get();
	const bool bCachedLightBecameUnavailable = ActiveDirectionalLight.IsValid() &&
		(!Light->IsVisible() || Light->Intensity <= 0.0f);
	const bool bCachedLightWasDestroyed = !ActiveDirectionalLight.IsValid() && Config.bHasDirectionalLight;
	if (!bHasSearchedDirectionalLights || bCachedLightBecameUnavailable || bCachedLightWasDestroyed)
	{
		ActiveDirectionalLight.Reset();
		Light = nullptr;
		float StrongestIntensity = 0.0f;
		for (TActorIterator<AActor> ActorIt(InActor->GetWorld()); ActorIt; ++ActorIt)
		{
			AActor* LightActor = *ActorIt;
			if (!IsValid(LightActor))
			{
				continue;
			}

			TArray<UDirectionalLightComponent*> DirectionalLights;
			LightActor->GetComponents<UDirectionalLightComponent>(DirectionalLights);
			for (UDirectionalLightComponent* Candidate : DirectionalLights)
			{
				if (IsValid(Candidate) && Candidate->IsVisible() && Candidate->Intensity > StrongestIntensity)
				{
					StrongestIntensity = Candidate->Intensity;
					Light = Candidate;
				}
			}
		}
		ActiveDirectionalLight = Light;
		bHasSearchedDirectionalLights = true;
	}

	const bool bHadLight = Config.bHasDirectionalLight;
	const FVector OldDirection = Config.DirectionalLightDirection;
	const FLinearColor OldColor = Config.DirectionalLightColor;
	const float OldIntensity = Config.DirectionalLightIntensity;

	if (IsValid(Light) && Light->IsVisible() && Light->Intensity > 0.0f)
	{
		Config.DirectionalLightDirection = -Light->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
		Config.DirectionalLightColor = Light->LightColor.ReinterpretAsLinear();
		Config.DirectionalLightIntensity = FMath::Min(Light->Intensity * 1.0e-5f, 100.0f);
		Config.bHasDirectionalLight = true;
	}
	else
	{
		Config.bHasDirectionalLight = false;
		Config.DirectionalLightIntensity = 0.0f;
	}

	return bHadLight != Config.bHasDirectionalLight ||
		!OldDirection.Equals(Config.DirectionalLightDirection, 1.0e-4f) ||
		!OldColor.Equals(Config.DirectionalLightColor, 1.0e-4f) ||
		!FMath::IsNearlyEqual(OldIntensity, Config.DirectionalLightIntensity, 1.0e-4f);
}

void UDDGIMasterSubsystem::PublishConfigValuesOnly()
{
	UWorld* World = GetWorld();
	FSceneInterface* Scene = World ? World->Scene : nullptr;
	if (!Scene)
	{
		return;
	}

	const FDDGIMasterConfig ConfigCopy = Config;
	ENQUEUE_RENDER_COMMAND(DDGIUpdateMasterConfigValues)(
		[Scene, ConfigCopy](FRHICommandListImmediate& RHICmdList)
		{
			GDDGIMasterConfigs_RenderThread.Add(Scene, ConfigCopy);
		});
}

void UDDGIMasterSubsystem::CopyConfigFromActor(const ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor))
	{
		return;
	}

	Config.bEnableDynamicGI = InActor->bEnableDynamicGI;
	Config.GIIntensity = FMath::Max(0.0f, InActor->GIIntensity);

	UpdateDirectionalLighting(InActor);
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
	// Keep probe count within the texture/hash layout supported by the renderer.
	Config.MaxActiveProbes = FMath::Clamp(InActor->MaxActiveProbes, 1, 32768);
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.MaxActiveProbes);
	Config.ProbeUpdateBudget = FMath::Max(0, InActor->ProbeUpdateBudget);
	Config.MaxSoftwareTriangles = FMath::Clamp(InActor->MaxSoftwareTriangles, 1, 250000);

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
			NewBVH->WorldOrigin,
			BuildStats);

		NewBVH->IncludedComponents = BuildStats.IncludedComponents;
		NewBVH->SkippedNoCPUAccess = BuildStats.SkippedNoCPUAccess;
		NewBVH->SkippedMovable = BuildStats.SkippedMovable;
		NewBVH->SkippedInstanced = BuildStats.SkippedInstanced;
		NewBVH->SkippedInvalidTriangles = BuildStats.SkippedInvalidTriangles;
		NewBVH->MaterialsUsingDefaultBaseColor = BuildStats.MaterialsUsingDefaultBaseColor;
		NewBVH->TriangleCount = BuildStats.TriangleCount;
		NewBVH->NodeCount = BuildStats.NodeCount;
		Config.SoftwareBVH = NewBVH;
		CachedSoftwareBVHTriangleLimit = Config.MaxSoftwareTriangles;

		UE_LOG(LogTemp, Log,
			TEXT("Radiance Cascades software BVH: %d triangles, %d nodes, depth %d, %d leaves (triangles/leaf min %d avg %.2f max %d), normalized leaf cost %.3f, %d static components. Skipped: %d no CPU access, %d movable, %d instanced, %d invalid. %d material slots use default DDGI albedo."),
			NewBVH->TriangleCount, NewBVH->NodeCount, BuildStats.MaxDepth, BuildStats.LeafCount,
			BuildStats.MinLeafTriangleCount, BuildStats.AverageLeafTriangleCount,
			BuildStats.MaxLeafTriangleCount, BuildStats.EstimatedSAHCost,
			NewBVH->IncludedComponents, NewBVH->SkippedNoCPUAccess, NewBVH->SkippedMovable,
			NewBVH->SkippedInstanced, NewBVH->SkippedInvalidTriangles, BuildStats.MaterialsUsingDefaultBaseColor);
		if (BuildStats.MaterialsUsingDefaultBaseColor > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: %d material slots do not expose the DDGI_BaseColor vector parameter; their GI albedo is approximated as neutral 0.18 gray. Material textures and arbitrary material graphs are not evaluated."), BuildStats.MaterialsUsingDefaultBaseColor);
		}
	}

	// The BVH origin remains fixed until the BVH is rebuilt. The Master Actor
	// may move independently; only its probe lattice moves within this frame.
	Config.ProbeGridOrigin = InActor->GetActorLocation() - Config.SoftwareBVH->WorldOrigin;
	Config.ProbeSpacing = DDGIBuildSparseProbePositions(
		Config.ProbeGridOrigin,
		Config.ProbeSpacing,
		Config.DynamicGIDistance,
		Config.MaxActiveProbes,
		Config.ProbePositions);
	Config.ProbeCount = Config.ProbePositions.Num();

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
