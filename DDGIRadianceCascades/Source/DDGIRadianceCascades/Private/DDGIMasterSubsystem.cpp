#include "DDGIMasterSubsystem.h"
#include "DDGIMasterActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DDGIRadianceCascadeResources.h"
#include "DDGISoftwareBVH.h"
#include "Components/DirectionalLightComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
	struct FDDGIProbeCandidate
	{
		FVector4 Position;
		double DistanceSquared;
	};

	FVector DDGIGetCameraWorldLocation(const ADDGIMasterActor* InActor)
	{
		if (!InActor) return FVector::ZeroVector;
		UWorld* World = InActor->GetWorld();
		if (World)
		{
			if (APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				FVector CameraLocation = FVector::ZeroVector;
				FRotator CameraRotation = FRotator::ZeroRotator;
				PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);
				return CameraLocation;
			}
		}
		return InActor->GetActorLocation();
	}

	FIntVector DDGIQuantizeWindowCenter(const FVector& WorldLocation, float Spacing)
	{
		const float SafeSpacing = FMath::Max(1.0f, Spacing);
		return FIntVector(
			FMath::RoundToInt(WorldLocation.X / SafeSpacing),
			FMath::RoundToInt(WorldLocation.Y / SafeSpacing),
			FMath::RoundToInt(WorldLocation.Z / SafeSpacing));
	}

	void DDGIBuildActiveProbePositions(
		float Spacing,
		float Radius,
		const FIntVector& CenterCell,
		int32 MaxProbeCount,
		TArray<FVector4>& OutPositions)
	{
		OutPositions.Reset();
		const float SafeSpacing = FMath::Max(1.0f, Spacing);
		const float SafeRadius = FMath::Max(0.0f, Radius);
		const int32 SafeMaxProbeCount = FMath::Max(0, MaxProbeCount);
		if (SafeMaxProbeCount == 0) return;

		const FVector CenterWorld(
			static_cast<double>(CenterCell.X) * SafeSpacing,
			static_cast<double>(CenterCell.Y) * SafeSpacing,
			static_cast<double>(CenterCell.Z) * SafeSpacing);
		const float TransitionWidth = FMath::Max(SafeSpacing * 2.0f, SafeRadius * 0.05f);
		TArray<FDDGIProbeCandidate> LevelCandidates[4];
		int32 TotalCandidateCount = 0;

		// All coordinates remain on the immutable world-origin lattice. The
		// camera only selects which globally aligned cells are active.
		for (int32 Level = 0; Level < 4; ++Level)
		{
			const int32 LevelStep = 1 << Level;
			const float BoundaryBefore = SafeRadius * (static_cast<float>(Level) / 4.0f);
			const float BoundaryAfter = SafeRadius * (static_cast<float>(Level + 1) / 4.0f);
			const float InnerRadius = Level == 0 ? 0.0f :
				FMath::Max(0.0f, BoundaryBefore - TransitionWidth);
			const float OuterRadius = Level == 3 ? SafeRadius :
				FMath::Min(SafeRadius, BoundaryAfter + TransitionWidth);
			const double InnerRadiusSquared = static_cast<double>(InnerRadius) * InnerRadius;
			const double OuterRadiusSquared = static_cast<double>(OuterRadius) * OuterRadius;

			const int32 MinCellX = FMath::FloorToInt((CenterWorld.X - OuterRadius) / SafeSpacing);
			const int32 MaxCellX = FMath::CeilToInt((CenterWorld.X + OuterRadius) / SafeSpacing);
			const int32 MinCellY = FMath::FloorToInt((CenterWorld.Y - OuterRadius) / SafeSpacing);
			const int32 MaxCellY = FMath::CeilToInt((CenterWorld.Y + OuterRadius) / SafeSpacing);
			const int32 MinCellZ = FMath::FloorToInt((CenterWorld.Z - OuterRadius) / SafeSpacing);
			const int32 MaxCellZ = FMath::CeilToInt((CenterWorld.Z + OuterRadius) / SafeSpacing);
			const int32 FirstX = FMath::CeilToInt(static_cast<float>(MinCellX) / LevelStep) * LevelStep;
			const int32 LastX = FMath::FloorToInt(static_cast<float>(MaxCellX) / LevelStep) * LevelStep;
			const int32 FirstY = FMath::CeilToInt(static_cast<float>(MinCellY) / LevelStep) * LevelStep;
			const int32 LastY = FMath::FloorToInt(static_cast<float>(MaxCellY) / LevelStep) * LevelStep;
			const int32 FirstZ = FMath::CeilToInt(static_cast<float>(MinCellZ) / LevelStep) * LevelStep;
			const int32 LastZ = FMath::FloorToInt(static_cast<float>(MaxCellZ) / LevelStep) * LevelStep;

			for (int32 CellZ = FirstZ; CellZ <= LastZ; CellZ += LevelStep)
			{
				const double DZ = (static_cast<double>(CellZ) - CenterCell.Z) * SafeSpacing;
				const double ZSquared = DZ * DZ;
				if (ZSquared > OuterRadiusSquared) continue;
				for (int32 CellY = FirstY; CellY <= LastY; CellY += LevelStep)
				{
					const double DY = (static_cast<double>(CellY) - CenterCell.Y) * SafeSpacing;
					const double YZSquared = DY * DY + ZSquared;
					if (YZSquared > OuterRadiusSquared) continue;
					for (int32 CellX = FirstX; CellX <= LastX; CellX += LevelStep)
					{
						const double DX = (static_cast<double>(CellX) - CenterCell.X) * SafeSpacing;
						const double DistanceSquared = DX * DX + YZSquared;
						if (DistanceSquared < InnerRadiusSquared || DistanceSquared > OuterRadiusSquared) continue;
						FDDGIProbeCandidate& Candidate = LevelCandidates[Level].AddDefaulted_GetRef();
						Candidate.Position = FVector4(
							static_cast<float>(CellX) * SafeSpacing,
							static_cast<float>(CellY) * SafeSpacing,
							static_cast<float>(CellZ) * SafeSpacing,
							static_cast<float>(Level));
						Candidate.DistanceSquared = DistanceSquared;
					}
				}
			}
			TotalCandidateCount += LevelCandidates[Level].Num();
		}

		if (TotalCandidateCount == 0) return;

		// Reserve one slot per non-empty level when the cap permits it. Allocate
		// the remaining budget proportionally to each level's candidate density,
		// preventing the dense inner lattice from consuming every active slot.
		int32 Quotas[4] = { 0, 0, 0, 0 };
		int32 NonEmptyLevelCount = 0;
		for (int32 Level = 0; Level < 4; ++Level)
		{
			if (LevelCandidates[Level].Num() > 0) ++NonEmptyLevelCount;
		}
		int32 RemainingBudget = SafeMaxProbeCount;
		if (SafeMaxProbeCount >= NonEmptyLevelCount)
		{
			for (int32 Level = 0; Level < 4; ++Level)
			{
				if (LevelCandidates[Level].Num() > 0)
				{
					Quotas[Level] = 1;
					--RemainingBudget;
				}
			}
		}

		while (RemainingBudget > 0)
		{
			int32 BestLevel = INDEX_NONE;
			double BestScore = -1.0;
			for (int32 Level = 0; Level < 4; ++Level)
			{
				const int32 CandidateCount = LevelCandidates[Level].Num();
				if (Quotas[Level] >= CandidateCount) continue;
				const double Score = static_cast<double>(CandidateCount) /
					static_cast<double>(Quotas[Level] + 1);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestLevel = Level;
				}
			}
			if (BestLevel == INDEX_NONE) break;
			++Quotas[BestLevel];
			--RemainingBudget;
		}

		const auto CandidateLess = [](const FDDGIProbeCandidate& A, const FDDGIProbeCandidate& B)
		{
			if (A.DistanceSquared != B.DistanceSquared)
			{
				return A.DistanceSquared < B.DistanceSquared;
			}
			if (A.Position.Z != B.Position.Z) return A.Position.Z < B.Position.Z;
			if (A.Position.Y != B.Position.Y) return A.Position.Y < B.Position.Y;
			return A.Position.X < B.Position.X;
		};

		OutPositions.Reserve(FMath::Min(SafeMaxProbeCount, TotalCandidateCount));
		for (int32 Level = 0; Level < 4; ++Level)
		{
			if (Quotas[Level] <= 0) continue;
			LevelCandidates[Level].Sort(CandidateLess);
			const int32 SelectedCount = FMath::Min(Quotas[Level], LevelCandidates[Level].Num());
			for (int32 Index = 0; Index < SelectedCount; ++Index)
			{
				OutPositions.Add(LevelCandidates[Level][Index].Position);
			}
		}
	}
	uint32 DDGIComputeProbeLayoutHash(
		const TArray<FVector4>& ProbePositions,
		float ProbeSpacing)
	{
		const float SafeProbeSpacing = FMath::Max(1.0f, ProbeSpacing);
		uint32 Hash = 2166136261u;
		for (const FVector4& Position : ProbePositions)
		{
			const FVector Relative(Position.X, Position.Y, Position.Z);
			const FIntVector Cell(
				FMath::RoundToInt(Relative.X / SafeProbeSpacing),
				FMath::RoundToInt(Relative.Y / SafeProbeSpacing),
				FMath::RoundToInt(Relative.Z / SafeProbeSpacing));
			Hash = (Hash ^ static_cast<uint32>(Cell.X)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(Cell.Y)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(Cell.Z)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(FMath::RoundToInt(Position.W))) * 16777619u;
		}
		return Hash;
	}

	/** Accessed only on the render thread. */
	TMap<const FSceneInterface*, FDDGIMasterConfig> GDDGIMasterConfigs_RenderThread;
}

const FDDGIMasterConfig* DDGIGetMasterConfig_RenderThread(const FSceneInterface* Scene)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	return GDDGIMasterConfigs_RenderThread.Find(Scene);
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
	ActiveWindowCenterCell = FIntVector::ZeroValue;
	bHasActiveWindowCenterCell = false;
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
	ActiveWindowCenterCell = FIntVector::ZeroValue;
	bHasActiveWindowCenterCell = false;
	PublishConfig(false);
}

void UDDGIMasterSubsystem::RefreshConfig(ADDGIMasterActor* InActor)
{
	if (IsValid(InActor) && ActiveMasterActor.Get() == InActor)
	{
		CopyConfigFromActor(InActor);
	}
}

void UDDGIMasterSubsystem::RebuildSoftwareBVH(ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor) || ActiveMasterActor.Get() != InActor)
	{
		return;
	}

	// Build a replacement without discarding the last valid CPU/GPU snapshot.
	CopyConfigFromActor(InActor, true);
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
		// Keep the scene light untouched; apply a separate scale only to DDGI radiance.
		Config.DirectionalLightIntensity = FMath::Min(
			Light->Intensity * 1.0e-5f * FMath::Max(0.0f, InActor->DirectionalLightGIIntensityScale),
			100.0f);
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

	// Lighting changes do not alter probe layout or BVH data. Capture only
	// the four lighting values instead of copying the full master config
	// (which can contain tens of thousands of probe positions).
	const bool bHasDirectionalLight = Config.bHasDirectionalLight;
	const FVector DirectionalLightDirection = Config.DirectionalLightDirection;
	const FLinearColor DirectionalLightColor = Config.DirectionalLightColor;
	const float DirectionalLightIntensity = Config.DirectionalLightIntensity;

	ENQUEUE_RENDER_COMMAND(DDGIUpdateMasterConfigLighting)(
		[Scene, bHasDirectionalLight, DirectionalLightDirection, DirectionalLightColor, DirectionalLightIntensity](FRHICommandListImmediate& RHICmdList)
		{
			if (FDDGIMasterConfig* ExistingConfig = GDDGIMasterConfigs_RenderThread.Find(Scene))
			{
				ExistingConfig->bHasDirectionalLight = bHasDirectionalLight;
				ExistingConfig->DirectionalLightDirection = DirectionalLightDirection;
				ExistingConfig->DirectionalLightColor = DirectionalLightColor;
				ExistingConfig->DirectionalLightIntensity = DirectionalLightIntensity;
			}
		});
}


void UDDGIMasterSubsystem::CopyConfigFromActor(const ADDGIMasterActor* InActor, bool bForceSoftwareBVHRebuild)
{
	if (!IsValid(InActor))
	{
		return;
	}

	Config.bEnableDynamicGI = InActor->bEnableDynamicGI;
	Config.GIIntensity = FMath::Max(0.0f, InActor->GIIntensity);

	UpdateDirectionalLighting(InActor);
	Config.DynamicGIDistance = FMath::Max(1.0f, InActor->DynamicGIDistance);
	Config.ProbeGridDistance = FMath::Max(1.0f, InActor->ProbeGridDistance);
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
	Config.ProbeUpdateBudget = FMath::Max(0, InActor->ProbeUpdateBudget);
	Config.MaxSoftwareTriangles = FMath::Clamp(InActor->MaxSoftwareTriangles, 1, 250000);

	// Build the immutable static-scene BVH once. Moving the Master Actor only
	// moves probe origins; it must not rebuild the world geometry every tick.
	if (bForceSoftwareBVHRebuild || !Config.SoftwareBVH.IsValid() ||
		CachedSoftwareBVHTriangleLimit != Config.MaxSoftwareTriangles)
	{
		TSharedPtr<FDDGISoftwareBVHData, ESPMode::ThreadSafe> NewBVH =
			MakeShareable(new FDDGISoftwareBVHData());

		FDDGISoftwareBVHBuildStats BuildStats;
		const bool bBuildSucceeded = DDGISoftwareBVH::BuildStaticWorld(
			InActor->GetWorld(),
			Config.MaxSoftwareTriangles,
			NewBVH->PackedNodes,
			NewBVH->PackedTriangles,
			NewBVH->WorldOrigin,
			BuildStats);

		if (!bBuildSucceeded)
		{
			UE_LOG(LogTemp, Error,
				TEXT("DDGI: Software BVH build failed. Keeping the previous valid geometry snapshot; the new scene geometry was not published."));
			if (!Config.SoftwareBVH.IsValid())
			{
				return;
			}
		}
		else
		{
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
		if (BuildStats.SkippedInstanced > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: Skipped %d ISM/HISM components containing %d instances. Their geometry is not currently included in the software BVH."),
				BuildStats.SkippedInstanced, BuildStats.SkippedInstancedInstances);
		}
		if (BuildStats.bTriangleLimitReached)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: Software BVH reached its %d-triangle limit. Components encountered later in world iteration may be omitted; increase MaxSoftwareTriangles or reduce static-mesh geometry if coverage is incomplete."), Config.MaxSoftwareTriangles);
		}
		if (BuildStats.MaterialsUsingDefaultBaseColor > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: %d material slots do not expose the DDGI_BaseColor vector parameter; their GI albedo is approximated as neutral 0.18 gray. Material textures and arbitrary material graphs are not evaluated."), BuildStats.MaterialsUsingDefaultBaseColor);
		}
		}
	}

	// The lattice remains anchored at world origin. The snapped camera cell
	// selects an active subset; it never changes the lattice spacing.
	Config.ProbeGridOrigin = -Config.SoftwareBVH->WorldOrigin;
	const float RequestedProbeSpacing = Config.ProbeSpacing;
	const FVector InitialCameraLocation = DDGIGetCameraWorldLocation(InActor);
	ActiveWindowCenterCell = DDGIQuantizeWindowCenter(InitialCameraLocation, Config.ProbeSpacing);
	bHasActiveWindowCenterCell = true;
	DDGIBuildActiveProbePositions(
		Config.ProbeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		Config.MaxActiveProbes, Config.ProbePositions);
	Config.ProbeCount = Config.ProbePositions.Num();
	// Estimate the actual generated lattice, not the hard safety cap.
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.ProbeCount);
	Config.ProbeLayoutHash = DDGIComputeProbeLayoutHash(
		Config.ProbePositions, Config.ProbeSpacing);

	UE_LOG(LogTemp, Log,
		TEXT("DDGI probe grid: radius %.1f cm, requested spacing %.1f cm, effective spacing %.1f cm, probes %d / %d; ray trace distance %.1f cm; cascades %d / %d active; rays per probe %d."),
		Config.ProbeGridDistance, RequestedProbeSpacing, Config.ProbeSpacing,
		Config.ProbeCount, Config.MaxActiveProbes, Config.DynamicGIDistance,
		Config.RadianceCascades.GetSafeCascadeCount(),
		Config.RadianceCascades.GetConfiguredCascadeCount(),
		Config.TotalRaysPerProbe);

	Config.bEnableStaticBackground = InActor->bEnableStaticBackground;
	Config.StaticBackgroundColor = InActor->StaticBackgroundColor;
	Config.StaticBackgroundIntensity = FMath::Max(0.0f, InActor->StaticBackgroundIntensity);
	Config.BackgroundTransitionStart = FMath::Max(0.0f, InActor->BackgroundTransitionStart);
	Config.BackgroundTransitionEnd = FMath::Max(Config.BackgroundTransitionStart, InActor->BackgroundTransitionEnd);
	PublishConfig(true);
}

void UDDGIMasterSubsystem::UpdateActiveProbeWindow(const ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor) || ActiveMasterActor.Get() != InActor || !Config.SoftwareBVH.IsValid())
	{
		return;
	}

	const FVector CameraLocation = DDGIGetCameraWorldLocation(InActor);
	const float SafeSpacing = FMath::Max(1.0f, Config.ProbeSpacing);
	const FVector CurrentCenter(
		static_cast<double>(ActiveWindowCenterCell.X) * SafeSpacing,
		static_cast<double>(ActiveWindowCenterCell.Y) * SafeSpacing,
		static_cast<double>(ActiveWindowCenterCell.Z) * SafeSpacing);
	const float RecenterThreshold = SafeSpacing * 0.75f;
	const FVector Delta = CameraLocation - CurrentCenter;

	if (bHasActiveWindowCenterCell &&
		FMath::Abs(Delta.X) <= RecenterThreshold &&
		FMath::Abs(Delta.Y) <= RecenterThreshold &&
		FMath::Abs(Delta.Z) <= RecenterThreshold)
	{
		return;
	}

	ActiveWindowCenterCell = DDGIQuantizeWindowCenter(CameraLocation, SafeSpacing);
	bHasActiveWindowCenterCell = true;
	DDGIBuildActiveProbePositions(
		SafeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		Config.MaxActiveProbes, Config.ProbePositions);
	Config.ProbeCount = Config.ProbePositions.Num();
	Config.EstimatedRadianceStorageBytes =
		Config.RadianceCascades.GetEstimatedStorageBytes(Config.ProbeCount);
	Config.ProbeLayoutHash = DDGIComputeProbeLayoutHash(
		Config.ProbePositions, Config.ProbeSpacing);

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
