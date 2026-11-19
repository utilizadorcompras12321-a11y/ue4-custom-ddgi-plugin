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
					return CameraLocation;
				}

				// Defensive fallback for a controller whose camera manager is not
				// initialized yet. This still asks the controller for its active view.
				FVector ViewLocation = FVector::ZeroVector;
				FRotator ViewRotation = FRotator::ZeroRotator;
				PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
				return ViewLocation;
			}
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
		const FVector& CameraLocation,
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
							// Keep the coarse lattice samples exactly on the inner
							// boundary. They are the one-cell overlap needed by trilinear
							// interpolation at the fine/coarse clipmap transition.
							const bool bInsidePreviousClipmap =
								CellX > InnerMinX && CellX < InnerMaxX &&
								CellY > InnerMinY && CellY < InnerMaxY &&
								CellZ > InnerMinZ && CellZ < InnerMaxZ;
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

		// Logical probes are traced sequentially; order them by distance to the
		// actual camera so each incremental trace batch updates the nearest first.
		TArray<int32> SortedIndices;
		SortedIndices.SetNumUninitialized(OutPositions.Num());
		for (int32 Index = 0; Index < SortedIndices.Num(); ++Index)
		{
			SortedIndices[Index] = Index;
		}
		SortedIndices.Sort([&](int32 A, int32 B)
		{
			const FVector4& PA = OutPositions[A];
			const FVector4& PB = OutPositions[B];
			const double AX = static_cast<double>(PA.X) - CameraLocation.X;
			const double AY = static_cast<double>(PA.Y) - CameraLocation.Y;
			const double AZ = static_cast<double>(PA.Z) - CameraLocation.Z;
			const double BX = static_cast<double>(PB.X) - CameraLocation.X;
			const double BY = static_cast<double>(PB.Y) - CameraLocation.Y;
			const double BZ = static_cast<double>(PB.Z) - CameraLocation.Z;
			const double DA = AX * AX + AY * AY + AZ * AZ;
			const double DB = BX * BX + BY * BY + BZ * BZ;
			if (DA != DB) return DA < DB;
			const FIntVector& CA = OutCellCoordinates[A];
			const FIntVector& CB = OutCellCoordinates[B];
			if (CA.Z != CB.Z) return CA.Z < CB.Z;
			if (CA.Y != CB.Y) return CA.Y < CB.Y;
			if (CA.X != CB.X) return CA.X < CB.X;
			return OutLevels[A] < OutLevels[B];
		});

		TArray<FVector4> NearestPositions;
		TArray<FIntVector> NearestCells;
		TArray<int32> NearestLevels;
		NearestPositions.Reserve(OutPositions.Num());
		NearestCells.Reserve(OutCellCoordinates.Num());
		NearestLevels.Reserve(OutLevels.Num());
		for (int32 Index : SortedIndices)
		{
			NearestPositions.Add(OutPositions[Index]);
			NearestCells.Add(OutCellCoordinates[Index]);
			NearestLevels.Add(OutLevels[Index]);
		}
		OutPositions = MoveTemp(NearestPositions);
		OutCellCoordinates = MoveTemp(NearestCells);
		OutLevels = MoveTemp(NearestLevels);
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
	const bool bRefreshDirectionalDiscovery =
		!bHasSearchedDirectionalLights ||
		GFrameCounter - LastDirectionalLightDiscoveryFrame >= 120u;
	if (bRefreshDirectionalDiscovery || bCachedLightBecameUnavailable || bCachedLightWasDestroyed)
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
				const float CandidateIndirectIntensity = IsValid(Candidate)
					? Candidate->Intensity * FMath::Max(0.0f, Candidate->IndirectLightingIntensity)
					: 0.0f;
				// DDGI owns this indirect-lighting path. Do not gate it on
				// bAffectDynamicIndirectLighting: in UE4.27 this flag is not
				// consistently exposed in the Directional Light details panel.
				if (IsValid(Candidate) && Candidate->IsVisible() &&
					CandidateIndirectIntensity > StrongestIntensity)
				{
					StrongestIntensity = CandidateIndirectIntensity;
					Light = Candidate;
				}
			}
		}
		ActiveDirectionalLight = Light;
		bHasSearchedDirectionalLights = true;
		LastDirectionalLightDiscoveryFrame = GFrameCounter;
	}

	const bool bHadLight = Config.bHasDirectionalLight;
	const bool bOldDirectionalLightCastsShadows = Config.bDirectionalLightCastsShadows;
	const FVector OldDirection = Config.DirectionalLightDirection;
	const FLinearColor OldColor = Config.DirectionalLightColor;
	const float OldIntensity = Config.DirectionalLightIntensity;

	if (IsValid(Light) && Light->IsVisible() && Light->Intensity > 0.0f)
	{
		Config.DirectionalLightDirection = -Light->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
		Config.DirectionalLightColor = Light->LightColor.ReinterpretAsLinear();
		// UE4 directional-light intensity is illuminance in lux. Preserve that value
		// for DDGI; the radiance shader applies the Lambertian 1/PI factor.
		// The optional actor scale is artistic only and defaults to 1.0 (no calibration).
		const float DirectionalIlluminanceLux =
			Light->Intensity * FMath::Max(0.0f, Light->IndirectLightingIntensity);
		const float ScaledDirectionalIlluminanceLux =
			DirectionalIlluminanceLux * FMath::Max(0.0f, InActor->DirectionalLightGIIntensityScale);
		Config.DirectionalLightIntensity =
			FMath::IsFinite(ScaledDirectionalIlluminanceLux)
				? ScaledDirectionalIlluminanceLux
				: 0.0f;
		Config.bHasDirectionalLight = true;
		Config.bDirectionalLightCastsShadows = Light->CastShadows != 0;
	}
	else
	{
		Config.bHasDirectionalLight = false;
		Config.bDirectionalLightCastsShadows = false;
		Config.DirectionalLightIntensity = 0.0f;
	}

	return bHadLight != Config.bHasDirectionalLight ||
		bOldDirectionalLightCastsShadows != Config.bDirectionalLightCastsShadows ||
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
	// A bounded 120-frame discovery interval also catches lights spawned at runtime.
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

		// Keep the highest-contribution lights first. The shader deliberately
		// shadows only a small prefix per hit to bound software-BVH traversal cost.
		CachedLocalLights.Sort([](
			const TWeakObjectPtr<UPointLightComponent>& A,
			const TWeakObjectPtr<UPointLightComponent>& B)
		{
			const UPointLightComponent* LightA = A.Get();
			const UPointLightComponent* LightB = B.Get();
			const float ScoreA = IsValid(LightA)
				? LightA->Intensity * FMath::Max(0.0f, LightA->IndirectLightingIntensity) *
					FMath::Max(1.0f, LightA->AttenuationRadius)
				: -1.0f;
			const float ScoreB = IsValid(LightB)
				? LightB->Intensity * FMath::Max(0.0f, LightB->IndirectLightingIntensity) *
					FMath::Max(1.0f, LightB->AttenuationRadius)
				: -1.0f;
			if (ScoreA != ScoreB)
			{
				return ScoreA > ScoreB;
			}
			if (!IsValid(LightA)) return false;
			if (!IsValid(LightB)) return true;
			return LightA->GetUniqueID() < LightB->GetUniqueID();
		});

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
			Light->Intensity <= 0.0f || Light->IndirectLightingIntensity <= 0.0f ||
			Light->AttenuationRadius <= 1.0f)
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
		// UE4 inverse-square point lights expose lumens. Convert to approximate
		// candela using 4PI sr for points or the spot cone's solid angle, then
		// convert the distance denominator from cm^2 to m^2 in the shader.
		const float LightSolidAngle = CosOuter > -1.0f
			? FMath::Max(0.01f, 2.0f * PI * (1.0f - CosOuter))
			: (4.0f * PI);
		const float ScaledIntensity = bInverseSquared
			? Light->Intensity * FMath::Max(0.0f, Light->IndirectLightingIntensity) *
				(10000.0f / LightSolidAngle) * IntensityScale
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
			Light->CastShadows != 0 ? 1.0f : 0.0f);
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
	// the small lighting payload instead of copying the full master config
	// (which can contain tens of thousands of probe positions).
	const bool bHasDirectionalLight = Config.bHasDirectionalLight;
	const bool bDirectionalLightCastsShadows = Config.bDirectionalLightCastsShadows;
	const FVector DirectionalLightDirection = Config.DirectionalLightDirection;
	const FLinearColor DirectionalLightColor = Config.DirectionalLightColor;
	const float DirectionalLightIntensity = Config.DirectionalLightIntensity;
	const TArray<FVector4> LocalLightPositionAndRadius = Config.LocalLightPositionAndRadius;
	const TArray<FVector4> LocalLightDirectionAndCosOuter = Config.LocalLightDirectionAndCosOuter;
	const TArray<FVector4> LocalLightColorAndIntensity = Config.LocalLightColorAndIntensity;
	const TArray<FVector4> LocalLightParameters = Config.LocalLightParameters;
	const uint32 LocalLightCount = Config.LocalLightCount;

	ENQUEUE_RENDER_COMMAND(DDGIUpdateMasterConfigLighting)(
		[Scene, bHasDirectionalLight, bDirectionalLightCastsShadows, DirectionalLightDirection, DirectionalLightColor, DirectionalLightIntensity,
		 LocalLightPositionAndRadius, LocalLightDirectionAndCosOuter, LocalLightColorAndIntensity,
		 LocalLightParameters, LocalLightCount](FRHICommandListImmediate& RHICmdList)
		{
			if (FDDGIMasterConfig* ExistingConfig = GDDGIMasterConfigs_RenderThread.Find(Scene))
			{
				ExistingConfig->bHasDirectionalLight = bHasDirectionalLight;
				ExistingConfig->bDirectionalLightCastsShadows = bDirectionalLightCastsShadows;
				ExistingConfig->DirectionalLightDirection = DirectionalLightDirection;
				ExistingConfig->DirectionalLightColor = DirectionalLightColor;
				ExistingConfig->DirectionalLightIntensity = DirectionalLightIntensity;
				ExistingConfig->LocalLightPositionAndRadius = LocalLightPositionAndRadius;
				ExistingConfig->LocalLightDirectionAndCosOuter = LocalLightDirectionAndCosOuter;
				ExistingConfig->LocalLightColorAndIntensity = LocalLightColorAndIntensity;
				ExistingConfig->LocalLightParameters = LocalLightParameters;
				ExistingConfig->LocalLightCount = LocalLightCount;
				// The scene geometry is static, but a changed light alters every
				// captured probe. Invalidate once so the next DDGI update refreshes
				// the complete active lattice instead of leaving stale lighting.
				DDGIInvalidateRadianceCascadeHistory_RenderThread(Scene);
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
	Config.LocalLightCount = 0;
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
		Config.bDirectionalLightCastsShadows = false;
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
		Config.ActiveProbeWindowCenterCell = FIntVector::ZeroValue;
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
		: 100.0f;
	const float RequestedProbeGridDistance = InActor->ProbeGridDistance;
	const float MaxProbeGridDistance = Config.ProbeSpacing * 56.0f;
	Config.ProbeGridDistance = std::isfinite(static_cast<double>(RequestedProbeGridDistance))
		? FMath::Clamp(RequestedProbeGridDistance, 1.0f, MaxProbeGridDistance)
		: FMath::Min(4000.0f, MaxProbeGridDistance);
	if (std::isfinite(static_cast<double>(RequestedProbeGridDistance)) &&
		RequestedProbeGridDistance > MaxProbeGridDistance)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DDGI: ProbeGridDistance %.1f cm exceeds the CPU lattice-search four-level clipmap coverage limit for spacing %.1f cm; clamped to %.1f cm (56 spacing units)."),
			RequestedProbeGridDistance, Config.ProbeSpacing, Config.ProbeGridDistance);
	}
	Config.RadianceCascades.CascadeCount = FMath::Clamp(InActor->CascadeCount, 1, 8);
	Config.RadianceCascades.BaseRayCount = FMath::Clamp(InActor->BaseRayCount, 1, 64);
	Config.RadianceCascades.BaseSegmentLength = FMath::Max(1.0f, InActor->BaseSegmentLength);
	Config.RadianceCascades.CascadeScale = FMath::Max(1.01f, InActor->CascadeScale);
	Config.RadianceCascades.MaxTraceDistance = FMath::Max(1.0f, InActor->DynamicGIDistance);
	Config.RadianceCascades.BuildCascadeSlices(Config.RadianceCascadeSlices);
	Config.TotalRaysPerProbe = Config.RadianceCascades.GetTotalRayCount();
	// Keep probe count within the texture/hash layout supported by the renderer.
	Config.MaxActiveProbes = FMath::Clamp(InActor->MaxActiveProbes, 1, 32768);
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

		if (BuildStats.SkippedInstanced > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI: Skipped %d movable ISM/HISM components containing %d instances. Static instances are included in the software BVH; movable instance transforms require a BVH refresh and are currently excluded."),
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
	Config.ActiveProbeWindowCenterCell = ActiveWindowCenterCell;
	bHasActiveWindowCenterCell = true;
	// Keep the trace/background-transition center continuous with the camera.
	// Apply-pass clipmap boundaries use ActiveProbeWindowCenterCell instead,
	// matching the independently snapped centers generated for each CPU level.
	Config.ActiveProbeWindowCenter = InitialCameraLocation;
	DDGIBuildActiveProbePositions(
		Config.ProbeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		InitialCameraLocation, Config.MaxActiveProbes, Config.ProbePositions,
		Config.ProbeCellCoordinates, Config.ProbeLevels);
	Config.ProbeCount = Config.ProbePositions.Num();
	// Estimate the actual generated lattice, not the hard safety cap.
	Config.EstimatedRadianceStorageBytes = Config.RadianceCascades.GetEstimatedStorageBytes(Config.ProbeCount);
	Config.ProbeLayoutHash = DDGIComputeProbeLayoutHash(
		Config.ProbeCellCoordinates, Config.ProbeLevels);


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

	// Keep the trace/background-transition center continuous every tick.
	// The apply pass receives the snapped lattice cell separately, and updates
	// that center only when the CPU probe window is actually rebuilt.
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
	Config.ActiveProbeWindowCenterCell = ActiveWindowCenterCell;
	bHasActiveWindowCenterCell = true;
	DDGIBuildActiveProbePositions(
		SafeSpacing, Config.ProbeGridDistance, ActiveWindowCenterCell,
		CameraLocation, Config.MaxActiveProbes, Config.ProbePositions,
		Config.ProbeCellCoordinates, Config.ProbeLevels);
	Config.ProbeCount = Config.ProbePositions.Num();
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
