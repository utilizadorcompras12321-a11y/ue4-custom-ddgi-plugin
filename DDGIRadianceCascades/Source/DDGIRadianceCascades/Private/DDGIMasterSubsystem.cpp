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
		// Bound the lattice search independently of MaxActiveProbes. The
		// candidate-count and selection passes both visit the Y/Z lattice rows;
		// an unbounded radius/spacing ratio can otherwise stall the game thread
		// even though only a small fixed number of probes is retained.
		const float SafeSpacing = FMath::Clamp(Spacing, 1.0f, 100000.0f);
		const double SafeSpacingDouble = static_cast<double>(SafeSpacing);
		const double MaxRadius = SafeSpacingDouble * 120.0;
		const double RequestedRadius = FMath::Max(0.0, static_cast<double>(Radius));
		const float SafeRadius = static_cast<float>(FMath::Min(RequestedRadius, MaxRadius));
		const int32 SafeMaxProbeCount = FMath::Max(0, MaxProbeCount);
		if (SafeMaxProbeCount == 0) return;

		const double TransitionWidth = FMath::Max(
			SafeSpacingDouble * 2.0, static_cast<double>(SafeRadius) * 0.05);
		int64 LevelCandidateCounts[4] = { 0, 0, 0, 0 };
		int64 FirstX[4], LastX[4], FirstY[4], LastY[4], FirstZ[4], LastZ[4];
		// Explicit non-dyadic spatial steps retain substantially more detail in distant LODs.
		int32 LevelSteps[4] = { 1, 2, 3, 4 };
		int64 MinDistanceSquared[4] = { 0, 0, 0, 0 };
		int64 MaxDistanceSquared[4] = { 0, 0, 0, 0 };

		// Count lattice points exactly, but analytically along X. For each
		// (Y,Z) row, the spherical shell intersects at most two X intervals.
		for (int32 Level = 0; Level < 4; ++Level)
		{
			const int32 LevelStep = LevelSteps[Level];
			const double BoundaryBefore = static_cast<double>(SafeRadius) *
				(static_cast<double>(Level) / 4.0);
			const double BoundaryAfter = static_cast<double>(SafeRadius) *
				(static_cast<double>(Level + 1) / 4.0);
			// Trilinear sampling needs all eight lattice corners around every
			// query point. At most one full level-spacing per axis separates a
			// query point from a corner, so sqrt(3) * LevelSpacing is a
			// conservative radial support margin. Expand both sides of each
			// level's blend region; otherwise the shell boundary loses corners
			// and the interpolated cascade collapses to zero near its edge.
			const double LevelSpacing = SafeSpacingDouble * static_cast<double>(LevelStep);
			const double SupportMargin = std::sqrt(3.0) * LevelSpacing;
			const double InnerRadius = Level == 0 ? 0.0 :
				FMath::Max(0.0, BoundaryBefore - TransitionWidth - SupportMargin);
			const double OuterRadius = (Level == 3 ? static_cast<double>(SafeRadius) :
				FMath::Min(static_cast<double>(SafeRadius), BoundaryAfter + TransitionWidth)) +
				SupportMargin;
			const double RadiusInCells = OuterRadius / SafeSpacingDouble;
			const double MaxSafeRadiusInCells =
				std::sqrt(static_cast<double>(MAX_int64) / 3.0);
			if (!std::isfinite(RadiusInCells) || RadiusInCells > MaxSafeRadiusInCells)
			{
				// Squared 3D distances must remain representable in signed int64.
				OutPositions.Reset();
				return;
			}
			const double SpacingSquared = SafeSpacingDouble * SafeSpacingDouble;
			MinDistanceSquared[Level] = static_cast<int64>(
				std::ceil((InnerRadius * InnerRadius) / SpacingSquared));
			MaxDistanceSquared[Level] = static_cast<int64>(
				std::floor((OuterRadius * OuterRadius) / SpacingSquared));

			const int64 MinCellX = DDGISafeFloorCell(
				static_cast<double>(CenterCell.X) - RadiusInCells);
			const int64 MaxCellX = DDGISafeCeilCell(
				static_cast<double>(CenterCell.X) + RadiusInCells);
			const int64 MinCellY = DDGISafeFloorCell(
				static_cast<double>(CenterCell.Y) - RadiusInCells);
			const int64 MaxCellY = DDGISafeCeilCell(
				static_cast<double>(CenterCell.Y) + RadiusInCells);
			const int64 MinCellZ = DDGISafeFloorCell(
				static_cast<double>(CenterCell.Z) - RadiusInCells);
			const int64 MaxCellZ = DDGISafeCeilCell(
				static_cast<double>(CenterCell.Z) + RadiusInCells);
			FirstX[Level] = DDGICeilDiv(MinCellX, LevelStep) * LevelStep;
			LastX[Level] = DDGIFloorDiv(MaxCellX, LevelStep) * LevelStep;
			FirstY[Level] = DDGICeilDiv(MinCellY, LevelStep) * LevelStep;
			LastY[Level] = DDGIFloorDiv(MaxCellY, LevelStep) * LevelStep;
			FirstZ[Level] = DDGICeilDiv(MinCellZ, LevelStep) * LevelStep;
			LastZ[Level] = DDGIFloorDiv(MaxCellZ, LevelStep) * LevelStep;

			if (MinDistanceSquared[Level] > MaxDistanceSquared[Level] ||
				FirstX[Level] > LastX[Level] ||
				FirstY[Level] > LastY[Level] ||
				FirstZ[Level] > LastZ[Level])
			{
				continue;
			}

			const int64 FirstDeltaX = static_cast<int64>(FirstX[Level]) - CenterCell.X;
			const int64 LastDeltaX = static_cast<int64>(LastX[Level]) - CenterCell.X;
			const int64 FirstDeltaY = static_cast<int64>(FirstY[Level]) - CenterCell.Y;
			const int64 LastDeltaY = static_cast<int64>(LastY[Level]) - CenterCell.Y;
			const int64 FirstDeltaZ = static_cast<int64>(FirstZ[Level]) - CenterCell.Z;
			const int64 LastDeltaZ = static_cast<int64>(LastZ[Level]) - CenterCell.Z;
			const int64 MinQ = MinDistanceSquared[Level];
			const int64 MaxQ = MaxDistanceSquared[Level];

			int64 ZStart = 0, ZEnd = -1;
			if (DDGIGetLatticeRangeWithinAbs(
				FirstDeltaZ, LastDeltaZ, LevelStep, DDGIFloorSqrt(MaxQ), ZStart, ZEnd))
			{
				for (int64 DZ = ZStart; DZ <= ZEnd; DZ += LevelStep)
				{
					const int64 ZSquared = DZ * DZ;
					int64 YStart = 0, YEnd = -1;
					if (!DDGIGetLatticeRangeWithinAbs(
						FirstDeltaY, LastDeltaY, LevelStep,
						DDGIFloorSqrt(MaxQ - ZSquared), YStart, YEnd)) continue;
					for (int64 DY = YStart; DY <= YEnd; DY += LevelStep)
					{
						const int64 YZSquared = DY * DY + ZSquared;
						const int64 MaxXAbs = DDGIFloorSqrt(MaxQ - YZSquared);
						const int64 MinXAbs = MinQ > YZSquared ?
							DDGICeilSqrt(MinQ - YZSquared) : 0;
						const int64 RowCandidateCount = DDGICountLatticeValuesByAbsoluteValue(
							FirstDeltaX, LastDeltaX, LevelStep, MinXAbs, MaxXAbs);
						if (RowCandidateCount > MAX_int64 - LevelCandidateCounts[Level])
						{
							OutPositions.Reset();
							return;
						}
						LevelCandidateCounts[Level] += RowCandidateCount;
					}
				}
			}
		}

		int64 TotalCandidateCount = 0;
		int32 NonEmptyLevelCount = 0;
		for (int32 Level = 0; Level < 4; ++Level)
		{
			if (LevelCandidateCounts[Level] > MAX_int64 - TotalCandidateCount)
			{
				OutPositions.Reset();
				return;
			}
			TotalCandidateCount += LevelCandidateCounts[Level];
			if (LevelCandidateCounts[Level] > 0) ++NonEmptyLevelCount;
		}
		if (TotalCandidateCount == 0) return;

		// Preserve the existing proportional per-level quotas exactly.
		int32 Quotas[4] = { 0, 0, 0, 0 };
		int32 RemainingBudget = SafeMaxProbeCount;
		if (SafeMaxProbeCount >= NonEmptyLevelCount)
		{
			for (int32 Level = 0; Level < 4; ++Level)
			{
				if (LevelCandidateCounts[Level] > 0)
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
				const int64 CandidateCount = LevelCandidateCounts[Level];
				if (static_cast<int64>(Quotas[Level]) >= CandidateCount) continue;
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

		OutPositions.Reserve(static_cast<int32>(FMath::Min<int64>(SafeMaxProbeCount, TotalCandidateCount)));
		OutCellCoordinates.Reserve(static_cast<int32>(FMath::Min<int64>(SafeMaxProbeCount, TotalCandidateCount)));
		OutLevels.Reserve(static_cast<int32>(FMath::Min<int64>(SafeMaxProbeCount, TotalCandidateCount)));
		for (int32 Level = 0; Level < 4; ++Level)
		{
			const int32 Quota = Quotas[Level];
			if (Quota <= 0 || LevelCandidateCounts[Level] <= 0) continue;

			const int32 LevelStep = LevelSteps[Level];
			const int64 FirstDeltaX = static_cast<int64>(FirstX[Level]) - CenterCell.X;
			const int64 LastDeltaX = static_cast<int64>(LastX[Level]) - CenterCell.X;
			const int64 FirstDeltaY = static_cast<int64>(FirstY[Level]) - CenterCell.Y;
			const int64 LastDeltaY = static_cast<int64>(LastY[Level]) - CenterCell.Y;
			const int64 FirstDeltaZ = static_cast<int64>(FirstZ[Level]) - CenterCell.Z;
			const int64 LastDeltaZ = static_cast<int64>(LastZ[Level]) - CenterCell.Z;
			const int64 MinQ = MinDistanceSquared[Level];
			const int64 MaxQ = MaxDistanceSquared[Level];

			// One Y/Z pass per level. Keep only the best Quota candidates in
			// a bounded max-heap. X values in each row are visited by increasing
			// absolute distance, so a candidate that cannot beat the heap root
			// proves that no later X in that row can enter the result.
			TArray<FDDGISelectedProbeCandidate> SelectedCandidates;
			SelectedCandidates.Reserve(Quota);

			int64 ZStart = 0, ZEnd = -1;
			if (DDGIGetLatticeRangeWithinAbs(
				FirstDeltaZ, LastDeltaZ, LevelStep, DDGIFloorSqrt(MaxQ), ZStart, ZEnd))
			{
				for (int64 DZ = ZStart; DZ <= ZEnd; DZ += LevelStep)
				{
					const int64 ZSquared = DZ * DZ;
					int64 YStart = 0, YEnd = -1;
					if (!DDGIGetLatticeRangeWithinAbs(
						FirstDeltaY, LastDeltaY, LevelStep,
						DDGIFloorSqrt(MaxQ - ZSquared), YStart, YEnd)) continue;

					for (int64 DY = YStart; DY <= YEnd; DY += LevelStep)
					{
						const int64 YZSquared = DY * DY + ZSquared;
						const int64 MaxXAbs = DDGIFloorSqrt(MaxQ - YZSquared);
						const int64 MinXAbs = MinQ > YZSquared ?
							DDGICeilSqrt(MinQ - YZSquared) : 0;

						int64 NegativeFirst = 0, NegativeLast = -1;
						int64 PositiveFirst = 0, PositiveLast = -1;
						const bool bHasNegative = DDGIGetLatticeRange(
							FirstDeltaX, LastDeltaX, LevelStep,
							-MaxXAbs, MinXAbs == 0 ? -1 : -MinXAbs,
							NegativeFirst, NegativeLast);
						const bool bHasPositive = DDGIGetLatticeRange(
							FirstDeltaX, LastDeltaX, LevelStep,
							MinXAbs, MaxXAbs, PositiveFirst, PositiveLast);

						int64 NegativeX = bHasNegative ? NegativeLast : 0;
						int64 PositiveX = bHasPositive ? PositiveFirst : 0;
						bool bNegativeValid = bHasNegative;
						bool bPositiveValid = bHasPositive;

						while (bNegativeValid || bPositiveValid)
						{
							const bool bTakeNegative = bNegativeValid &&
								(!bPositiveValid || -NegativeX <= PositiveX);
							const int64 DX = bTakeNegative ? NegativeX : PositiveX;

							FDDGISelectedProbeCandidate Candidate;
							Candidate.CellX = static_cast<int32>(
								static_cast<int64>(CenterCell.X) + DX);
							Candidate.CellY = static_cast<int32>(
								static_cast<int64>(CenterCell.Y) + DY);
							Candidate.CellZ = static_cast<int32>(
								static_cast<int64>(CenterCell.Z) + DZ);
							Candidate.Position = FVector4(
								static_cast<float>(Candidate.CellX) * SafeSpacing,
								static_cast<float>(Candidate.CellY) * SafeSpacing,
								static_cast<float>(Candidate.CellZ) * SafeSpacing,
								static_cast<float>(Level));
							Candidate.DistanceSquared = DX * DX + YZSquared;

							const bool bHeapHasRoom = SelectedCandidates.Num() < Quota;
							if (!bHeapHasRoom &&
								!DDGISelectedProbeCandidateLess(Candidate, SelectedCandidates[0]))
							{
								break;
							}
							DDGIInsertBoundedSelectedProbe(
								SelectedCandidates, Candidate, Quota);

							if (bTakeNegative)
							{
								NegativeX -= LevelStep;
								bNegativeValid = NegativeX >= NegativeFirst;
							}
							else
							{
								PositiveX += LevelStep;
								bPositiveValid = PositiveX <= PositiveLast;
							}
						}
					}
				}
			}

			SelectedCandidates.Sort(DDGISelectedProbeCandidateLess);
			for (const FDDGISelectedProbeCandidate& Candidate : SelectedCandidates)
			{
				OutPositions.Add(Candidate.Position);
				OutCellCoordinates.Add(FIntVector(Candidate.CellX, Candidate.CellY, Candidate.CellZ));
				OutLevels.Add(FMath::RoundToInt(Candidate.Position.W));
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
		InActor->bEnableDynamicGI && Config.bEnableDynamicGI && UpdateDirectionalLighting(InActor))
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
	Config.bEnableStaticBackground = InActor->bEnableStaticBackground;

	if (!Config.bEnableDynamicGI)
	{
		// A disabled master must stop CPU scene extraction as well as GPU work.
		// Drop the CPU BVH and lattice so re-enabling forces a clean rebuild.
		Config.bHasDirectionalLight = false;
		Config.DirectionalLightIntensity = 0.0f;
		ActiveDirectionalLight.Reset();
		bHasSearchedDirectionalLights = false;
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
