#include "DDGIMasterSubsystem.h"
#include "DDGIMasterActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DDGIRadianceCascadeResources.h"
#include "DDGISoftwareBVH.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#endif
#include <cmath>

namespace
{
	struct FDDGISelectedProbeCandidate
	{
		FVector4 Position;
		int64 DistanceSquared;
		int32 CellX;
		int32 CellY;
		int32 CellZ;
	};

	bool DDGISelectedProbeCandidateLess(
		const FDDGISelectedProbeCandidate& A, const FDDGISelectedProbeCandidate& B)
	{
		if (A.DistanceSquared != B.DistanceSquared)
		{
			return A.DistanceSquared < B.DistanceSquared;
		}
		if (A.CellZ != B.CellZ) return A.CellZ < B.CellZ;
		if (A.CellY != B.CellY) return A.CellY < B.CellY;
		return A.CellX < B.CellX;
	}

	int64 DDGISafeFloorCell(double Value)
	{
		if (!std::isfinite(Value) || Value <= static_cast<double>(MIN_int32))
		{
			return Value < 0.0 ? MIN_int32 : MAX_int32;
		}
		if (Value >= static_cast<double>(MAX_int32)) return MAX_int32;
		return static_cast<int64>(std::floor(Value));
	}

	int64 DDGISafeRoundCell(double Value)
	{
		if (!std::isfinite(Value))
		{
			return Value < 0.0 ? MIN_int32 : MAX_int32;
		}
		if (Value <= static_cast<double>(MIN_int32)) return MIN_int32;
		if (Value >= static_cast<double>(MAX_int32)) return MAX_int32;
		return static_cast<int64>(std::round(Value));
	}

	int64 DDGISafeCeilCell(double Value)
	{
		if (!std::isfinite(Value) || Value <= static_cast<double>(MIN_int32))
		{
			return Value < 0.0 ? MIN_int32 : MAX_int32;
		}
		if (Value >= static_cast<double>(MAX_int32)) return MAX_int32;
		return static_cast<int64>(std::ceil(Value));
	}

	int64 DDGIFloorSqrt(int64 Value)
	{
		if (Value <= 0) return 0;
		int64 Root = static_cast<int64>(std::sqrt(static_cast<double>(Value)));
		while (Root + 1 > 0 && Root + 1 <= Value / (Root + 1)) ++Root;
		while (Root > 0 && Root > Value / Root) --Root;
		return Root;
	}

	int64 DDGICeilSqrt(int64 Value)
	{
		const int64 Root = DDGIFloorSqrt(Value);
		return Root * Root < Value ? Root + 1 : Root;
	}

	int64 DDGIFloorDiv(int64 Numerator, int64 Denominator)
	{
		int64 Quotient = Numerator / Denominator;
		const int64 Remainder = Numerator % Denominator;
		if (Remainder < 0) --Quotient;
		return Quotient;
	}

	int64 DDGICeilDiv(int64 Numerator, int64 Denominator)
	{
		return -DDGIFloorDiv(-Numerator, Denominator);
	}

	bool DDGIGetLatticeRangeWithinAbs(
		int64 FirstValue, int64 LastValue, int64 Step, int64 MaxAbs,
		int64& OutFirst, int64& OutLast)
	{
		if (MaxAbs < 0) return false;
		const int64 ClampedMin = FMath::Max(FirstValue, -MaxAbs);
		const int64 ClampedMax = FMath::Min(LastValue, MaxAbs);
		if (ClampedMin > ClampedMax) return false;
		const int64 FirstIndex = DDGICeilDiv(ClampedMin - FirstValue, Step);
		const int64 LastIndex = DDGIFloorDiv(ClampedMax - FirstValue, Step);
		if (LastIndex < FirstIndex) return false;
		OutFirst = FirstValue + FirstIndex * Step;
		OutLast = FirstValue + LastIndex * Step;
		return true;
	}

	int64 DDGICountLatticeValuesInRange(
		int64 FirstValue, int64 LastValue, int64 Step, int64 MinValue, int64 MaxValue)
	{
		const int64 ClampedMin = FMath::Max(FirstValue, MinValue);
		const int64 ClampedMax = FMath::Min(LastValue, MaxValue);
		if (ClampedMin > ClampedMax) return 0;
		const int64 FirstIndex = DDGICeilDiv(ClampedMin - FirstValue, Step);
		const int64 LastIndex = DDGIFloorDiv(ClampedMax - FirstValue, Step);
		return LastIndex >= FirstIndex ? LastIndex - FirstIndex + 1 : 0;
	}

	int64 DDGICountLatticeValuesByAbsoluteValue(
		int64 FirstValue, int64 LastValue, int64 Step, int64 MinAbs, int64 MaxAbs)
	{
		if (MinAbs > MaxAbs) return 0;
		if (MinAbs == 0)
		{
			return DDGICountLatticeValuesInRange(FirstValue, LastValue, Step, -MaxAbs, 0) +
				DDGICountLatticeValuesInRange(FirstValue, LastValue, Step, 1, MaxAbs);
		}
		return DDGICountLatticeValuesInRange(FirstValue, LastValue, Step, -MaxAbs, -MinAbs) +
			DDGICountLatticeValuesInRange(FirstValue, LastValue, Step, MinAbs, MaxAbs);
	}
	bool DDGIGetLatticeRange(
		int64 FirstValue, int64 LastValue, int64 Step, int64 MinValue, int64 MaxValue,
		int64& OutFirst, int64& OutLast)
	{
		const int64 ClampedMin = FMath::Max(FirstValue, MinValue);
		const int64 ClampedMax = FMath::Min(LastValue, MaxValue);
		if (ClampedMin > ClampedMax) return false;
		const int64 FirstIndex = DDGICeilDiv(ClampedMin - FirstValue, Step);
		const int64 LastIndex = DDGIFloorDiv(ClampedMax - FirstValue, Step);
		if (LastIndex < FirstIndex) return false;
		OutFirst = FirstValue + FirstIndex * Step;
		OutLast = FirstValue + LastIndex * Step;
		return true;
	}

	void DDGISiftSelectedProbeHeapUp(
		TArray<FDDGISelectedProbeCandidate>& Heap, int32 Index)
	{
		while (Index > 0)
		{
			const int32 Parent = (Index - 1) / 2;
			if (!DDGISelectedProbeCandidateLess(Heap[Parent], Heap[Index])) break;
			Heap.Swap(Parent, Index);
			Index = Parent;
		}
	}

	void DDGISiftSelectedProbeHeapDown(
		TArray<FDDGISelectedProbeCandidate>& Heap, int32 Index)
	{
		for (;;)
		{
			const int32 Left = Index * 2 + 1;
			const int32 Right = Left + 1;
			int32 Worst = Index;
			if (Left < Heap.Num() &&
				DDGISelectedProbeCandidateLess(Heap[Worst], Heap[Left]))
			{
				Worst = Left;
			}
			if (Right < Heap.Num() &&
				DDGISelectedProbeCandidateLess(Heap[Worst], Heap[Right]))
			{
				Worst = Right;
			}
			if (Worst == Index) break;
			Heap.Swap(Index, Worst);
			Index = Worst;
		}
	}

	void DDGIInsertBoundedSelectedProbe(
		TArray<FDDGISelectedProbeCandidate>& Heap,
		const FDDGISelectedProbeCandidate& Candidate,
		int32 Capacity)
	{
		if (Capacity <= 0) return;
		if (Heap.Num() < Capacity)
		{
			const int32 NewIndex = Heap.Add(Candidate);
			DDGISiftSelectedProbeHeapUp(Heap, NewIndex);
			return;
		}
		if (DDGISelectedProbeCandidateLess(Candidate, Heap[0]))
		{
			Heap[0] = Candidate;
			DDGISiftSelectedProbeHeapDown(Heap, 0);
		}
	}

	FVector DDGIGetCameraWorldLocation(const ADDGIMasterActor* InActor)
	{
		if (!InActor) return FVector::ZeroVector;

		UWorld* World = InActor->GetWorld();
#if WITH_EDITOR
		// Editor worlds do not have a PlayerController. Follow the active level
		// editor viewport instead of silently using the Master Actor root.
		if (World && World->WorldType == EWorldType::Editor && GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				if (FEditorViewportClient* ViewportClient =
					static_cast<FEditorViewportClient*>(ActiveViewport->GetClient()))
				{
					const FVector CameraLocation = ViewportClient->GetViewLocation();
					static bool bLoggedEditorViewport = false;
					if (!bLoggedEditorViewport)
					{
						UE_LOG(LogTemp, Warning,
							TEXT("DDGI DIAG: probe window camera source=EditorViewport, camera=(%.1f, %.1f, %.1f), master-root=(%.1f, %.1f, %.1f)."),
							CameraLocation.X, CameraLocation.Y, CameraLocation.Z,
							InActor->GetActorLocation().X, InActor->GetActorLocation().Y, InActor->GetActorLocation().Z);
						bLoggedEditorViewport = true;
					}
					return CameraLocation;
				}
			}
		}
#endif
		if (World)
		{
			if (APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				// PlayerCameraManager tracks the active view, including ViewTarget
				// changes and camera blends used by gameplay and cutscenes.
				if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
				{
					const FVector CameraLocation = CameraManager->GetCameraLocation();
					static bool bLoggedCameraManager = false;
					if (!bLoggedCameraManager)
					{
						UE_LOG(LogTemp, Warning,
							TEXT("DDGI DIAG: probe window camera source=PlayerCameraManager, camera=(%.1f, %.1f, %.1f), master-root=(%.1f, %.1f, %.1f), world=%s."),
							CameraLocation.X, CameraLocation.Y, CameraLocation.Z,
							InActor->GetActorLocation().X, InActor->GetActorLocation().Y, InActor->GetActorLocation().Z,
							*GetNameSafe(World));
						bLoggedCameraManager = true;
					}
					return CameraLocation;
				}

				// Defensive fallback for a controller whose camera manager is not
				// initialized yet. This still asks the controller for its active view.
				FVector ViewLocation = FVector::ZeroVector;
				FRotator ViewRotation = FRotator::ZeroRotator;
				PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
				static bool bLoggedControllerFallback = false;
				if (!bLoggedControllerFallback)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("DDGI DIAG: PlayerCameraManager unavailable; using PlayerController view location=(%.1f, %.1f, %.1f)."),
						ViewLocation.X, ViewLocation.Y, ViewLocation.Z);
					bLoggedControllerFallback = true;
				}
				return ViewLocation;
			}
		}

		static bool bLoggedRootFallback = false;
		if (!bLoggedRootFallback)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("DDGI DIAG: NO PlayerController/PlayerCameraManager; probe window is falling back to master ROOT=(%.1f, %.1f, %.1f), world=%s, world-type=%d."),
				InActor->GetActorLocation().X, InActor->GetActorLocation().Y, InActor->GetActorLocation().Z,
				*GetNameSafe(World), World ? static_cast<int32>(World->WorldType) : -1);
			bLoggedRootFallback = true;
		}
		return InActor->GetActorLocation();
	}
	FIntVector DDGIQuantizeWindowCenter(const FVector& WorldLocation, float Spacing)
	{
		if (!std::isfinite(static_cast<double>(Spacing)) ||
			!std::isfinite(static_cast<double>(WorldLocation.X)) ||
			!std::isfinite(static_cast<double>(WorldLocation.Y)) ||
			!std::isfinite(static_cast<double>(WorldLocation.Z)))
		{
			return FIntVector::ZeroValue;
		}
		const double SafeSpacing = FMath::Max(1.0, static_cast<double>(Spacing));
		return FIntVector(
			static_cast<int32>(DDGISafeRoundCell(WorldLocation.X / SafeSpacing)),
			static_cast<int32>(DDGISafeRoundCell(WorldLocation.Y / SafeSpacing)),
			static_cast<int32>(DDGISafeRoundCell(WorldLocation.Z / SafeSpacing)));
	}

	void DDGIBuildActiveProbePositions(
		float Spacing,
		float Radius,
		const FIntVector& CenterCell,
		int32 MaxProbeCount,
		TArray<FVector4>& OutPositions,
		TArray<FIntVector>& OutCellCoordinates,
		TArray<int32>& OutLevels)
	{
		OutPositions.Reset();
		OutCellCoordinates.Reset();
		OutLevels.Reset();

		if (!std::isfinite(static_cast<double>(Spacing)) ||
			!std::isfinite(static_cast<double>(Radius)))
		{
			return;
		}

		const double SafeSpacing = FMath::Clamp(static_cast<double>(Spacing), 1.0, 100000.0);
		const double SafeRadius = FMath::Max(0.0, static_cast<double>(Radius));
		const int32 SafeMaxProbeCount = FMath::Clamp(MaxProbeCount, 0, 32768);
		if (SafeRadius <= 0.0 || SafeMaxProbeCount == 0)
		{
			return;
		}

		// Four nested 16^3 clipmaps. Every level is aligned to the immutable
		// world lattice; level spacing doubles and each coarser level stores
		// the outer ring plus one coarse-cell overlap for seamless interpolation.
		static const int32 ClipmapResolution = 16;
		static const int32 ClipmapMinOffset = -8;
		static const int32 ClipmapMaxOffset = 7;
		static const int32 ClipmapLevelCount = 4;

		TArray<FVector4> LevelPositions[ClipmapLevelCount];
		TArray<FIntVector> LevelCells[ClipmapLevelCount];
		int32 ActiveLevelCount = 0;
		int64 PreviousCenterX = 0;
		int64 PreviousCenterY = 0;
		int64 PreviousCenterZ = 0;
		int64 PreviousStep = 1;

		for (int32 Level = 0; Level < ClipmapLevelCount; ++Level)
		{
			const int64 Step = 1ll << Level;
			const double HalfExtent = SafeSpacing * 6.0 * static_cast<double>(Step);
			const int64 CenterX = DDGISafeRoundCell(static_cast<double>(CenterCell.X) / static_cast<double>(Step)) * Step;
			const int64 CenterY = DDGISafeRoundCell(static_cast<double>(CenterCell.Y) / static_cast<double>(Step)) * Step;
			const int64 CenterZ = DDGISafeRoundCell(static_cast<double>(CenterCell.Z) / static_cast<double>(Step)) * Step;

			LevelPositions[Level].Reserve(ClipmapResolution * ClipmapResolution * ClipmapResolution);
			LevelCells[Level].Reserve(ClipmapResolution * ClipmapResolution * ClipmapResolution);

			for (int32 Z = ClipmapMinOffset; Z <= ClipmapMaxOffset; ++Z)
			{
				const int64 CellZ = CenterZ + static_cast<int64>(Z) * Step;
				for (int32 Y = ClipmapMinOffset; Y <= ClipmapMaxOffset; ++Y)
				{
					const int64 CellY = CenterY + static_cast<int64>(Y) * Step;
					for (int32 X = ClipmapMinOffset; X <= ClipmapMaxOffset; ++X)
					{
						const int64 CellX = CenterX + static_cast<int64>(X) * Step;

						// Coarser clipmaps are rings, not overlapping full volumes.
						// This preserves a single owner for every world-space region.
						if (Level > 0)
						{
							// Keep one coarse cell of overlap inside the previous
							// clipmap so trilinear sampling has all eight corners
							// while blending across the clipmap boundary.
							const int64 Overlap = Step;
							const int64 InnerMinX = PreviousCenterX + ClipmapMinOffset * PreviousStep + Overlap;
							const int64 InnerMaxX = PreviousCenterX + ClipmapMaxOffset * PreviousStep - Overlap;
							const int64 InnerMinY = PreviousCenterY + ClipmapMinOffset * PreviousStep + Overlap;
							const int64 InnerMaxY = PreviousCenterY + ClipmapMaxOffset * PreviousStep - Overlap;
							const int64 InnerMinZ = PreviousCenterZ + ClipmapMinOffset * PreviousStep + Overlap;
							const int64 InnerMaxZ = PreviousCenterZ + ClipmapMaxOffset * PreviousStep - Overlap;
							const bool bInsidePreviousClipmap =
								CellX >= InnerMinX && CellX <= InnerMaxX &&
								CellY >= InnerMinY && CellY <= InnerMaxY &&
								CellZ >= InnerMinZ && CellZ <= InnerMaxZ;
							if (bInsidePreviousClipmap)
							{
								continue;
							}
						}

						// Retain one current-level cell beyond the requested
						// radius so trilinear reconstruction has its outer corners.
						const double DX = static_cast<double>(CellX - CenterCell.X) * SafeSpacing;
						const double DY = static_cast<double>(CellY - CenterCell.Y) * SafeSpacing;
						const double DZ = static_cast<double>(CellZ - CenterCell.Z) * SafeSpacing;
						const double EffectiveHalfExtent = FMath::Min(HalfExtent, SafeRadius);
						if (FMath::Max(FMath::Abs(DX), FMath::Max(FMath::Abs(DY), FMath::Abs(DZ))) >
							EffectiveHalfExtent + SafeSpacing * static_cast<double>(Step) ||
							CellX < MIN_int32 || CellX > MAX_int32 ||
							CellY < MIN_int32 || CellY > MAX_int32 ||
							CellZ < MIN_int32 || CellZ > MAX_int32)
						{
							continue;
						}

						const FIntVector Cell(
							static_cast<int32>(CellX),
							static_cast<int32>(CellY),
							static_cast<int32>(CellZ));
						LevelCells[Level].Add(Cell);
						LevelPositions[Level].Add(FVector4(
							static_cast<float>(static_cast<double>(Cell.X) * SafeSpacing),
							static_cast<float>(static_cast<double>(Cell.Y) * SafeSpacing),
							static_cast<float>(static_cast<double>(Cell.Z) * SafeSpacing),
							static_cast<float>(Level)));
					}
				}
			}

			if (LevelPositions[Level].Num() > 0)
			{
				++ActiveLevelCount;
			}

			PreviousCenterX = CenterX;
			PreviousCenterY = CenterY;
			PreviousCenterZ = CenterZ;
			PreviousStep = Step;

			if (HalfExtent >= SafeRadius)
			{
				break;
			}
		}

		if (ActiveLevelCount == 0)
		{
			return;
		}

		int32 TotalCandidates = 0;
		for (int32 Level = 0; Level < ActiveLevelCount; ++Level)
		{
			TotalCandidates += LevelPositions[Level].Num();
		}
		const int32 TargetCount = FMath::Min(SafeMaxProbeCount, TotalCandidates);
		int32 Quotas[ClipmapLevelCount] = { 0, 0, 0, 0 };
		int32 Remaining = TargetCount;

		// Reserve an equal initial share for every populated level, then
		// distribute the remainder in stable level order. Default capacity
		// exceeds the complete four-level clipmap, so this is only a safety cap.
		for (int32 Level = 0; Level < ActiveLevelCount && Remaining > 0; ++Level)
		{
			if (LevelPositions[Level].Num() > 0)
			{
				Quotas[Level] = 1;
				--Remaining;
			}
		}
		while (Remaining > 0)
		{
			bool bAssigned = false;
			for (int32 Level = 0; Level < ActiveLevelCount && Remaining > 0; ++Level)
			{
				if (Quotas[Level] < LevelPositions[Level].Num())
				{
					++Quotas[Level];
					--Remaining;
					bAssigned = true;
				}
			}
			if (!bAssigned)
			{
				break;
			}
		}

		OutPositions.Reserve(TargetCount);
		OutCellCoordinates.Reserve(TargetCount);
		OutLevels.Reserve(TargetCount);
		for (int32 Level = 0; Level < ActiveLevelCount; ++Level)
		{
			const int32 CandidateCount = LevelPositions[Level].Num();
			const int32 Quota = Quotas[Level];
			if (CandidateCount <= 0 || Quota <= 0)
			{
				continue;
			}

			// Evenly decimate in stable Z/Y/X order if a deliberately small
			// capacity is configured. No frame-dependent or random selection.
			for (int32 Selected = 0; Selected < Quota; ++Selected)
			{
				const int32 CandidateIndex = static_cast<int32>(
					(static_cast<int64>(Selected) * CandidateCount) / Quota);
				OutPositions.Add(LevelPositions[Level][CandidateIndex]);
				OutCellCoordinates.Add(LevelCells[Level][CandidateIndex]);
				OutLevels.Add(Level);
			}
		}
	}
	void DDGICountSpatialProbeLevels(
		const TArray<int32>& ProbeLevels,
		int32& OutLevel0,
		int32& OutLevel1,
		int32& OutLevel2,
		int32& OutLevel3)
	{
		OutLevel0 = 0;
		OutLevel1 = 0;
		OutLevel2 = 0;
		OutLevel3 = 0;
		for (int32 Level : ProbeLevels)
		{
			if (Level == 0) ++OutLevel0;
			else if (Level == 1) ++OutLevel1;
			else if (Level == 2) ++OutLevel2;
			else if (Level == 3) ++OutLevel3;
		}
	}

	uint32 DDGIComputeProbeLayoutHash(
		const TArray<FIntVector>& CellCoordinates,
		const TArray<int32>& ProbeLevels)
	{
		uint32 Hash = 2166136261u;
		const int32 Count = FMath::Min(CellCoordinates.Num(), ProbeLevels.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FIntVector& Cell = CellCoordinates[Index];
			Hash = (Hash ^ static_cast<uint32>(Cell.X)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(Cell.Y)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(Cell.Z)) * 16777619u;
			Hash = (Hash ^ static_cast<uint32>(ProbeLevels[Index])) * 16777619u;
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
	CachedLocalLights.Reset();
	bHasSearchedLocalLights = false;
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
	CachedLocalLights.Reset();
	bHasSearchedLocalLights = false;
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
	if (!IsValid(InActor) || ActiveMasterActor.Get() != InActor || !InActor->bEnableDynamicGI)
	{
		return;
	}

	// Build a replacement without discarding the last valid CPU/GPU snapshot.
	CopyConfigFromActor(InActor, true);
}

void UDDGIMasterSubsystem::RefreshDirectionalLighting(ADDGIMasterActor* InActor)
{
	if (IsValid(InActor) && ActiveMasterActor.Get() == InActor &&
		InActor->bEnableDynamicGI && Config.bEnableDynamicGI)
	{
		const bool bDirectionalChanged = UpdateDirectionalLighting(InActor);
		const bool bLocalLightsChanged = UpdateLocalLighting(InActor);
		if (bDirectionalChanged || bLocalLightsChanged)
		{
			PublishConfigValuesOnly();
		}
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
				if (IsValid(Candidate) && Candidate->IsVisible() && Candidate->bAffectDynamicIndirectLighting && Candidate->Intensity > StrongestIntensity)
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

	if (IsValid(Light) && Light->IsVisible() && Light->bAffectDynamicIndirectLighting && Light->Intensity > 0.0f)
	{
		Config.DirectionalLightDirection = -Light->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
		Config.DirectionalLightColor = Light->LightColor.ReinterpretAsLinear();
		// Keep the scene light untouched; apply a separate scale only to DDGI radiance.
		Config.DirectionalLightIntensity = FMath::Min(
			Light->Intensity * FMath::Max(0.0f, Light->IndirectLightingIntensity) *
			1.0e-5f * FMath::Max(0.0f, InActor->DirectionalLightGIIntensityScale),
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


bool UDDGIMasterSubsystem::UpdateLocalLighting(const ADDGIMasterActor* InActor)
{
	if (!IsValid(InActor) || !InActor->GetWorld())
	{
		return false;
	}

	// Discover components occasionally, but read cached transforms/intensities
	// every frame so moving point/spot lights update without an actor scan.
	// A 2-second discovery interval also catches lights spawned at runtime.
	if (!bHasSearchedLocalLights || GFrameCounter - LastLocalLightDiscoveryFrame >= 120u)
	{
		CachedLocalLights.Reset();
		for (TActorIterator<AActor> ActorIt(InActor->GetWorld()); ActorIt; ++ActorIt)
		{
			AActor* LightActor = *ActorIt;
			if (!IsValid(LightActor))
			{
				continue;
			}
			TArray<UPointLightComponent*> Components;
			LightActor->GetComponents<UPointLightComponent>(Components);
			for (UPointLightComponent* Component : Components)
			{
				if (IsValid(Component))
				{
					CachedLocalLights.Add(Component);
				}
			}
		}
		LastLocalLightDiscoveryFrame = GFrameCounter;
		bHasSearchedLocalLights = true;
	}

	static const uint32 MaxLocalLights = 32u;
	TArray<FVector4> NewPositionAndRadius;
	TArray<FVector4> NewDirectionAndCosOuter;
	TArray<FVector4> NewColorAndIntensity;
	TArray<FVector4> NewParameters;
	NewPositionAndRadius.SetNumZeroed(MaxLocalLights);
	NewDirectionAndCosOuter.SetNumZeroed(MaxLocalLights);
	NewColorAndIntensity.SetNumZeroed(MaxLocalLights);
	NewParameters.SetNumZeroed(MaxLocalLights);

	const FVector BVHOrigin = Config.SoftwareBVH.IsValid()
		? Config.SoftwareBVH->WorldOrigin
		: FVector::ZeroVector;
	const float IntensityScale = FMath::Max(0.0f, InActor->LocalLightGIIntensityScale);
	uint32 NewLightCount = 0u;
	for (const TWeakObjectPtr<UPointLightComponent>& WeakLight : CachedLocalLights)
	{
		UPointLightComponent* Light = WeakLight.Get();
		if (!IsValid(Light) || !Light->IsVisible() || !Light->bAffectDynamicIndirectLighting ||
			Light->Intensity <= 0.0f || Light->AttenuationRadius <= 1.0f)
		{
			continue;
		}
		if (NewLightCount >= MaxLocalLights)
		{
			break;
		}

		const FVector LocalPosition = Light->GetComponentLocation() - BVHOrigin;
		FVector Forward = FVector::ForwardVector;
		float CosOuter = -2.0f; // Negative sentinel identifies a point light.
		float CosInner = -2.0f;
		if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
		{
			Forward = Spot->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
			CosOuter = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Spot->OuterConeAngle, 0.0f, 89.0f)));
			CosInner = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Spot->InnerConeAngle, 0.0f, 89.0f)));
		}

		const bool bInverseSquared = Light->bUseInverseSquaredFalloff != 0;
		// UE4 inverse-square point lights expose lumens. Convert to an
		// approximate candela scale (lumens / 4PI), then cm^-2 to m^-2.
		const float ScaledIntensity = bInverseSquared
			? Light->Intensity * FMath::Max(0.0f, Light->IndirectLightingIntensity) *
				(10000.0f / (4.0f * PI)) * IntensityScale
			: Light->Intensity * FMath::Max(0.0f, Light->IndirectLightingIntensity) * IntensityScale;
		const FLinearColor Color = Light->LightColor.ReinterpretAsLinear();

		NewPositionAndRadius[NewLightCount] = FVector4(
			LocalPosition.X, LocalPosition.Y, LocalPosition.Z,
			FMath::Max(1.0f, Light->AttenuationRadius));
		NewDirectionAndCosOuter[NewLightCount] = FVector4(
			Forward.X, Forward.Y, Forward.Z, CosOuter);
		NewColorAndIntensity[NewLightCount] = FVector4(
			Color.R, Color.G, Color.B, ScaledIntensity);
		NewParameters[NewLightCount] = FVector4(
			FMath::Max(0.1f, Light->LightFalloffExponent),
			bInverseSquared ? 1.0f : 0.0f,
			CosInner,
			0.0f);
		++NewLightCount;
	}

	bool bChanged = Config.LocalLightCount != NewLightCount ||
		Config.LocalLightPositionAndRadius.Num() != static_cast<int32>(MaxLocalLights) ||
		Config.LocalLightDirectionAndCosOuter.Num() != static_cast<int32>(MaxLocalLights) ||
		Config.LocalLightColorAndIntensity.Num() != static_cast<int32>(MaxLocalLights) ||
		Config.LocalLightParameters.Num() != static_cast<int32>(MaxLocalLights);
	if (!bChanged)
	{
		for (uint32 Index = 0u; Index < MaxLocalLights; ++Index)
		{
			if (!Config.LocalLightPositionAndRadius[Index].Equals(NewPositionAndRadius[Index], 0.1f) ||
				!Config.LocalLightDirectionAndCosOuter[Index].Equals(NewDirectionAndCosOuter[Index], 1.0e-4f) ||
				!Config.LocalLightColorAndIntensity[Index].Equals(NewColorAndIntensity[Index], 1.0e-3f) ||
				!Config.LocalLightParameters[Index].Equals(NewParameters[Index], 1.0e-4f))
			{
				bChanged = true;
				break;
			}
		}
	}
	if (bChanged)
	{
		Config.LocalLightPositionAndRadius = MoveTemp(NewPositionAndRadius);
		Config.LocalLightDirectionAndCosOuter = MoveTemp(NewDirectionAndCosOuter);
		Config.LocalLightColorAndIntensity = MoveTemp(NewColorAndIntensity);
		Config.LocalLightParameters = MoveTemp(NewParameters);
		Config.LocalLightCount = NewLightCount;
	}
	return bChanged;
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
	const TArray<FVector4> LocalLightPositionAndRadius = Config.LocalLightPositionAndRadius;
	const TArray<FVector4> LocalLightDirectionAndCosOuter = Config.LocalLightDirectionAndCosOuter;
	const TArray<FVector4> LocalLightColorAndIntensity = Config.LocalLightColorAndIntensity;
	const TArray<FVector4> LocalLightParameters = Config.LocalLightParameters;
	const uint32 LocalLightCount = Config.LocalLightCount;

	ENQUEUE_RENDER_COMMAND(DDGIUpdateMasterConfigLighting)(
		[Scene, bHasDirectionalLight, DirectionalLightDirection, DirectionalLightColor, DirectionalLightIntensity,
		 LocalLightPositionAndRadius, LocalLightDirectionAndCosOuter, LocalLightColorAndIntensity,
		 LocalLightParameters, LocalLightCount](FRHICommandListImmediate& RHICmdList)
		{
			if (FDDGIMasterConfig* ExistingConfig = GDDGIMasterConfigs_RenderThread.Find(Scene))
			{
				ExistingConfig->bHasDirectionalLight = bHasDirectionalLight;
				ExistingConfig->DirectionalLightDirection = DirectionalLightDirection;
				ExistingConfig->DirectionalLightColor = DirectionalLightColor;
				ExistingConfig->DirectionalLightIntensity = DirectionalLightIntensity;
				ExistingConfig->LocalLightPositionAndRadius = LocalLightPositionAndRadius;
				ExistingConfig->LocalLightDirectionAndCosOuter = LocalLightDirectionAndCosOuter;
				ExistingConfig->LocalLightColorAndIntensity = LocalLightColorAndIntensity;
				ExistingConfig->LocalLightParameters = LocalLightParameters;
				ExistingConfig->LocalLightCount = LocalLightCount;
			}
		});
}


void UDDGIMasterSubsystem::PublishConfigActiveWindowCenterOnly()
{
	UWorld* World = GetWorld();
	FSceneInterface* Scene = World ? World->Scene : nullptr;
	if (!Scene)
	{
		return;
	}

	const FVector ActiveProbeWindowCenter = Config.ActiveProbeWindowCenter;
	ENQUEUE_RENDER_COMMAND(DDGIUpdateActiveProbeWindowCenter)(
		[Scene, ActiveProbeWindowCenter](FRHICommandListImmediate& RHICmdList)
		{
			if (FDDGIMasterConfig* ExistingConfig = GDDGIMasterConfigs_RenderThread.Find(Scene))
			{
				ExistingConfig->ActiveProbeWindowCenter = ActiveProbeWindowCenter;
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
	Config.LocalLightGIIntensityScale = FMath::Max(0.0f, InActor->LocalLightGIIntensityScale);
	Config.LocalLightPositionAndRadius.SetNumZeroed(32);
	Config.LocalLightDirectionAndCosOuter.SetNumZeroed(32);
	Config.LocalLightColorAndIntensity.SetNumZeroed(32);
	Config.LocalLightParameters.SetNumZeroed(32);
	Config.bEnableStaticBackground = InActor->bEnableStaticBackground;

	if (!Config.bEnableDynamicGI)
	{
		// A disabled master must stop CPU scene extraction as well as GPU work.
		// Drop the CPU BVH and lattice so re-enabling forces a clean rebuild.
		Config.bHasDirectionalLight = false;
		Config.DirectionalLightIntensity = 0.0f;
		ActiveDirectionalLight.Reset();
		bHasSearchedDirectionalLights = false;
		Config.LocalLightCount = 0;
		Config.LocalLightPositionAndRadius.SetNumZeroed(32);
		Config.LocalLightDirectionAndCosOuter.SetNumZeroed(32);
		Config.LocalLightColorAndIntensity.SetNumZeroed(32);
		Config.LocalLightParameters.SetNumZeroed(32);
		CachedLocalLights.Reset();
		bHasSearchedLocalLights = false;
		Config.SoftwareBVH.Reset();
		Config.ProbePositions.Reset();
		Config.ProbeCellCoordinates.Reset();
		Config.ProbeLevels.Reset();
		Config.ProbeCount = 0;
		Config.EstimatedRadianceStorageBytes = 0;
		Config.ProbeLayoutHash = 0;
		Config.ProbeGridOrigin = FVector::ZeroVector;
		Config.ActiveProbeWindowCenter = FVector::ZeroVector;
		CachedSoftwareBVHTriangleLimit = INDEX_NONE;
		ActiveWindowCenterCell = FIntVector::ZeroValue;
		bHasActiveWindowCenterCell = false;
		PublishConfig(true);
		return;
	}

	UpdateDirectionalLighting(InActor);
	Config.DynamicGIDistance = FMath::Max(1.0f, InActor->DynamicGIDistance);
	const float RequestedProbeSpacingFromActor = InActor->ProbeSpacing;
	Config.ProbeSpacing = std::isfinite(static_cast<double>(RequestedProbeSpacingFromActor))
		? FMath::Clamp(RequestedProbeSpacingFromActor, 1.0f, 100000.0f)
		: 200.0f;
	const float RequestedProbeGridDistance = InActor->ProbeGridDistance;
	const float MaxProbeGridDistance = Config.ProbeSpacing * 120.0f;
	Config.ProbeGridDistance = std::isfinite(static_cast<double>(RequestedProbeGridDistance))
		? FMath::Clamp(RequestedProbeGridDistance, 1.0f, MaxProbeGridDistance)
		: FMath::Min(4000.0f, MaxProbeGridDistance);
	if (std::isfinite(static_cast<double>(RequestedProbeGridDistance)) &&
		RequestedProbeGridDistance > MaxProbeGridDistance)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DDGI: ProbeGridDistance %.1f cm exceeds the CPU lattice-search safety limit for spacing %.1f cm; clamped to %.1f cm (120 spacing units)."),
			RequestedProbeGridDistance, Config.ProbeSpacing, Config.ProbeGridDistance);
	}
	Config.RadianceCascades.CascadeCount = FMath::Clamp(InActor->CascadeCount, 1, 8);
	Config.RadianceCascades.BaseRayCount = FMath::Clamp(InActor->BaseRayCount, 1, 64);
	Config.RadianceCascades.RaySplitFactor = FMath::Clamp(InActor->RaySplitFactor, 1, 4);
	Config.RadianceCascades.BaseSegmentLength = FMath::Max(1.0f, InActor->BaseSegmentLength);
	Config.RadianceCascades.CascadeScale = FMath::Max(1.01f, InActor->CascadeScale);
	Config.RadianceCascades.MaxTraceDistance = FMath::Max(1.0f, InActor->DynamicGIDistance);
	Config.RadianceCascades.BuildCascadeSlices(Config.RadianceCascadeSlices);
	Config.TotalRaysPerProbe = Config.RadianceCascades.GetTotalRayCount();
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
			TEXT("Radiance Cascades software BVH: %d triangles, %d nodes, depth %d, %d leaves (triangles/leaf min %d avg %.2f max %d), normalized leaf cost %.3f, %d static components, %d BSP components. Skipped: %d no CPU access, %d movable, %d instanced, %d invalid, %d masked. %d material slots use default DDGI albedo."),
			NewBVH->TriangleCount, NewBVH->NodeCount, BuildStats.MaxDepth, BuildStats.LeafCount,
			BuildStats.MinLeafTriangleCount, BuildStats.AverageLeafTriangleCount,
			BuildStats.MaxLeafTriangleCount, BuildStats.EstimatedSAHCost,
			NewBVH->IncludedComponents, BuildStats.IncludedBSPComponents, NewBVH->SkippedNoCPUAccess, NewBVH->SkippedMovable,
			NewBVH->SkippedInstanced, NewBVH->SkippedInvalidTriangles, BuildStats.SkippedMaskedTriangles, BuildStats.MaterialsUsingDefaultBaseColor);
		if (BuildStats.SkippedInstanced > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: Skipped %d ISM/HISM components containing %d instances. Their geometry is not currently included in the software BVH."),
				BuildStats.SkippedInstanced, BuildStats.SkippedInstancedInstances);
		}
		if (BuildStats.bTriangleLimitReached)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: Software BVH reached its %d-triangle global or per-category budget. Eligible static-mesh or BSP geometry may be omitted; increase MaxSoftwareTriangles or reduce scene geometry if coverage is incomplete."), Config.MaxSoftwareTriangles);
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
	// Keep the blend origin continuous with the camera. The snapped cell
	// remains the deterministic lattice-selection anchor, but must not drive
	// shader blend distance or the cascade boundaries will jump by ProbeSpacing.
	Config.ActiveProbeWindowCenter = InitialCameraLocation;
	DDGIBuildActiveProbePositions(
		Config.ProbeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		Config.MaxActiveProbes, Config.ProbePositions, Config.ProbeCellCoordinates, Config.ProbeLevels);
	Config.ProbeCount = Config.ProbePositions.Num();
	int32 InitialLevel0 = 0, InitialLevel1 = 0, InitialLevel2 = 0, InitialLevel3 = 0;
	DDGICountSpatialProbeLevels(Config.ProbeLevels,
		InitialLevel0, InitialLevel1, InitialLevel2, InitialLevel3);
	UE_LOG(LogTemp, Log,
		TEXT("DDGI spatial probe levels: L0=%d L1=%d L2=%d L3=%d (total=%d)."),
		InitialLevel0, InitialLevel1, InitialLevel2, InitialLevel3, Config.ProbeCount);
	// Estimate the actual generated lattice, not the hard safety cap.
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.ProbeCount);
	Config.ProbeLayoutHash = DDGIComputeProbeLayoutHash(
		Config.ProbeCellCoordinates, Config.ProbeLevels);

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
	if (!IsValid(InActor) || ActiveMasterActor.Get() != InActor ||
		!Config.bEnableDynamicGI || !Config.SoftwareBVH.IsValid())
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

	// The blend origin is continuous and must follow the camera every tick.
	// Previously it was only assigned after a probe-window rebuild, so the
	// shader's cascade boundaries remained frozen and then jumped at recenter.
	const bool bBlendCenterChanged =
		!Config.ActiveProbeWindowCenter.Equals(CameraLocation, 0.01f);
	if (bBlendCenterChanged)
	{
		Config.ActiveProbeWindowCenter = CameraLocation;
	}

	const bool bNeedsRecenter = !bHasActiveWindowCenterCell ||
		FMath::Abs(Delta.X) > RecenterThreshold ||
		FMath::Abs(Delta.Y) > RecenterThreshold ||
		FMath::Abs(Delta.Z) > RecenterThreshold;
	if (!bNeedsRecenter)
	{
		if (bBlendCenterChanged)
		{
			PublishConfigActiveWindowCenterOnly();
		}
		return;
	}

	ActiveWindowCenterCell = DDGIQuantizeWindowCenter(CameraLocation, SafeSpacing);
	bHasActiveWindowCenterCell = true;
	DDGIBuildActiveProbePositions(
		SafeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		Config.MaxActiveProbes, Config.ProbePositions,
		Config.ProbeCellCoordinates, Config.ProbeLevels);
	Config.ProbeCount = Config.ProbePositions.Num();
	int32 UpdatedLevel0 = 0, UpdatedLevel1 = 0, UpdatedLevel2 = 0, UpdatedLevel3 = 0;
	DDGICountSpatialProbeLevels(Config.ProbeLevels,
		UpdatedLevel0, UpdatedLevel1, UpdatedLevel2, UpdatedLevel3);
	UE_LOG(LogTemp, Log,
		TEXT("DDGI spatial probe window recentered: L0=%d L1=%d L2=%d L3=%d (total=%d)."),
		UpdatedLevel0, UpdatedLevel1, UpdatedLevel2, UpdatedLevel3, Config.ProbeCount);
	Config.EstimatedRadianceStorageBytes =
		Config.RadianceCascades.GetEstimatedStorageBytes(Config.ProbeCount);
	Config.ProbeLayoutHash = DDGIComputeProbeLayoutHash(
		Config.ProbeCellCoordinates, Config.ProbeLevels);

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
