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
		const double SafeSpacingDouble = static_cast<double>(SafeSpacing);
		const int32 SafeMaxProbeCount = FMath::Max(0, MaxProbeCount);
		if (SafeMaxProbeCount == 0) return;

		const FVector CenterWorld(
			static_cast<double>(CenterCell.X) * SafeSpacing,
			static_cast<double>(CenterCell.Y) * SafeSpacing,
			static_cast<double>(CenterCell.Z) * SafeSpacing);
		const float TransitionWidth = FMath::Max(SafeSpacing * 2.0f, SafeRadius * 0.05f);
		int64 LevelCandidateCounts[4] = { 0, 0, 0, 0 };
		int32 FirstX[4], LastX[4], FirstY[4], LastY[4], FirstZ[4], LastZ[4];
		int32 LevelSteps[4] = { 1, 2, 4, 8 };
		int64 MinDistanceSquared[4] = { 0, 0, 0, 0 };
		int64 MaxDistanceSquared[4] = { 0, 0, 0, 0 };

		// Count lattice points exactly, but analytically along X. For each
		// (Y,Z) row, the spherical shell intersects at most two X intervals.
		for (int32 Level = 0; Level < 4; ++Level)
		{
			const int32 LevelStep = LevelSteps[Level];
			const float BoundaryBefore = SafeRadius * (static_cast<float>(Level) / 4.0f);
			const float BoundaryAfter = SafeRadius * (static_cast<float>(Level + 1) / 4.0f);
			const float InnerRadius = Level == 0 ? 0.0f :
				FMath::Max(0.0f, BoundaryBefore - TransitionWidth);
			const float OuterRadius = Level == 3 ? SafeRadius :
				FMath::Min(SafeRadius, BoundaryAfter + TransitionWidth);
			const double SpacingSquared = SafeSpacingDouble * SafeSpacingDouble;
			MinDistanceSquared[Level] = static_cast<int64>(
				std::ceil((InnerRadius * InnerRadius) / SpacingSquared));
			MaxDistanceSquared[Level] = static_cast<int64>(
				std::floor((OuterRadius * OuterRadius) / SpacingSquared));

			const int32 MinCellX = FMath::FloorToInt((CenterWorld.X - OuterRadius) / SafeSpacing);
			const int32 MaxCellX = FMath::CeilToInt((CenterWorld.X + OuterRadius) / SafeSpacing);
			const int32 MinCellY = FMath::FloorToInt((CenterWorld.Y - OuterRadius) / SafeSpacing);
			const int32 MaxCellY = FMath::CeilToInt((CenterWorld.Y + OuterRadius) / SafeSpacing);
			const int32 MinCellZ = FMath::FloorToInt((CenterWorld.Z - OuterRadius) / SafeSpacing);
			const int32 MaxCellZ = FMath::CeilToInt((CenterWorld.Z + OuterRadius) / SafeSpacing);
			FirstX[Level] = FMath::CeilToInt(static_cast<float>(MinCellX) / LevelStep) * LevelStep;
			LastX[Level] = FMath::FloorToInt(static_cast<float>(MaxCellX) / LevelStep) * LevelStep;
			FirstY[Level] = FMath::CeilToInt(static_cast<float>(MinCellY) / LevelStep) * LevelStep;
			LastY[Level] = FMath::FloorToInt(static_cast<float>(MaxCellY) / LevelStep) * LevelStep;
			FirstZ[Level] = FMath::CeilToInt(static_cast<float>(MinCellZ) / LevelStep) * LevelStep;
			LastZ[Level] = FMath::FloorToInt(static_cast<float>(MaxCellZ) / LevelStep) * LevelStep;

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

			for (int64 DZ = FirstDeltaZ; DZ <= LastDeltaZ; DZ += LevelStep)
			{
				const int64 ZSquared = DZ * DZ;
				if (ZSquared > MaxQ) continue;
				for (int64 DY = FirstDeltaY; DY <= LastDeltaY; DY += LevelStep)
				{
					const int64 YZSquared = DY * DY + ZSquared;
					if (YZSquared > MaxQ) continue;
					const int64 MaxXAbs = DDGIFloorSqrt(MaxQ - YZSquared);
					const int64 MinXAbs = MinQ > YZSquared ?
						DDGICeilSqrt(MinQ - YZSquared) : 0;
					LevelCandidateCounts[Level] += DDGICountLatticeValuesByAbsoluteValue(
						FirstDeltaX, LastDeltaX, LevelStep, MinXAbs, MaxXAbs);
				}
			}
		}

		int64 TotalCandidateCount = 0;
		int32 NonEmptyLevelCount = 0;
		for (int32 Level = 0; Level < 4; ++Level)
		{
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

			// Find the exact integer squared-distance of the quota-th point.
			int64 LowQ = MinQ;
			int64 HighQ = MaxQ;
			while (LowQ < HighQ)
			{
				const int64 MidQ = LowQ + (HighQ - LowQ) / 2;
				int64 CountAtOrBelow = 0;
				for (int64 DZ = FirstDeltaZ; DZ <= LastDeltaZ; DZ += LevelStep)
				{
					const int64 ZSquared = DZ * DZ;
					if (ZSquared > MidQ) continue;
					for (int64 DY = FirstDeltaY; DY <= LastDeltaY; DY += LevelStep)
					{
						const int64 YZSquared = DY * DY + ZSquared;
						if (YZSquared > MidQ) continue;
						const int64 MaxXAbs = DDGIFloorSqrt(FMath::Min(MaxQ, MidQ) - YZSquared);
					const int64 MinXAbs = MinQ > YZSquared ?
						DDGICeilSqrt(MinQ - YZSquared) : 0;
						CountAtOrBelow += DDGICountLatticeValuesByAbsoluteValue(
							FirstDeltaX, LastDeltaX, LevelStep, MinXAbs, MaxXAbs);
					}
				}
				if (CountAtOrBelow >= Quota) HighQ = MidQ;
				else LowQ = MidQ + 1;
			}
			const int64 ThresholdQ = LowQ;
			TArray<FDDGISelectedProbeCandidate> SelectedCandidates;
			SelectedCandidates.Reserve(Quota);
			auto AddSelectedProbe = [&](int64 DX, int64 DY, int64 DZ)
			{
				FDDGISelectedProbeCandidate Candidate;
				Candidate.CellX = static_cast<int32>(static_cast<int64>(CenterCell.X) + DX);
				Candidate.CellY = static_cast<int32>(static_cast<int64>(CenterCell.Y) + DY);
				Candidate.CellZ = static_cast<int32>(static_cast<int64>(CenterCell.Z) + DZ);
				Candidate.Position = FVector4(
					static_cast<float>(Candidate.CellX) * SafeSpacing,
					static_cast<float>(Candidate.CellY) * SafeSpacing,
					static_cast<float>(Candidate.CellZ) * SafeSpacing,
					static_cast<float>(Level));
				Candidate.DistanceSquared = DX * DX + DY * DY + DZ * DZ;
				SelectedCandidates.Add(Candidate);
			};

			// Generate all candidates strictly nearer than the threshold. The
			// count is guaranteed to be smaller than Quota.
			for (int64 DZ = FirstDeltaZ; DZ <= LastDeltaZ; DZ += LevelStep)
			{
				const int64 ZSquared = DZ * DZ;
				if (ZSquared >= ThresholdQ) continue;
				for (int64 DY = FirstDeltaY; DY <= LastDeltaY; DY += LevelStep)
				{
					const int64 YZSquared = DY * DY + ZSquared;
					if (YZSquared >= ThresholdQ) continue;
					const int64 MaxXAbs = DDGIFloorSqrt(FMath::Min(MaxQ, ThresholdQ - 1) - YZSquared);
					const int64 MinXAbs = MinQ > YZSquared ?
						DDGICeilSqrt(MinQ - YZSquared) : 0;
					auto AddXRange = [&](int64 MinX, int64 MaxX)
					{
						const int64 ClampedMin = FMath::Max(FirstDeltaX, MinX);
						const int64 ClampedMax = FMath::Min(LastDeltaX, MaxX);
						if (ClampedMin > ClampedMax) return;
						const int64 FirstIndex = DDGICeilDiv(ClampedMin - FirstDeltaX, LevelStep);
						const int64 LastIndex = DDGIFloorDiv(ClampedMax - FirstDeltaX, LevelStep);
						for (int64 Index = FirstIndex; Index <= LastIndex; ++Index)
						{
							const int64 DX = FirstDeltaX + Index * LevelStep;
							AddSelectedProbe(DX, DY, DZ);
						}
					};
					if (MinXAbs == 0)
					{
						AddXRange(-MaxXAbs, -1);
						AddXRange(0, MaxXAbs);
					}
					else
					{
						AddXRange(-MaxXAbs, -MinXAbs);
						AddXRange(MinXAbs, MaxXAbs);
					}
				}
			}

			// At the threshold, X can only be +/-sqrt(ThresholdQ - YZ).
			// Z/Y/X traversal gives the deterministic tie-break order.
			for (int64 DZ = FirstDeltaZ; DZ <= LastDeltaZ && SelectedCandidates.Num() < Quota; DZ += LevelStep)
			{
				const int64 ZSquared = DZ * DZ;
				if (ZSquared > ThresholdQ) continue;
				for (int64 DY = FirstDeltaY; DY <= LastDeltaY && SelectedCandidates.Num() < Quota; DY += LevelStep)
				{
					const int64 Remainder = ThresholdQ - (DY * DY + ZSquared);
					if (Remainder < 0) continue;
					const int64 Root = DDGIFloorSqrt(Remainder);
					if (Root * Root != Remainder) continue;
					const int64 CandidateDX[2] = { -Root, Root };
					for (int32 SignIndex = 0; SignIndex < (Root == 0 ? 1 : 2) &&
						SelectedCandidates.Num() < Quota; ++SignIndex)
					{
						const int64 DX = CandidateDX[SignIndex];
						if (DX < FirstDeltaX || DX > LastDeltaX ||
							(DX - FirstDeltaX) % LevelStep != 0) continue;
						AddSelectedProbe(DX, DY, DZ);
					}
				}
			}

			SelectedCandidates.Sort(DDGISelectedProbeCandidateLess);
			for (const FDDGISelectedProbeCandidate& Candidate : SelectedCandidates)
			{
				OutPositions.Add(Candidate.Position);
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
