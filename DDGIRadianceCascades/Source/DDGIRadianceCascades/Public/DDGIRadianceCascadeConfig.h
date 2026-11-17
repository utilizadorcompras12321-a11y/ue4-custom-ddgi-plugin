#pragma once

#include "CoreMinimal.h"
#include "DDGIRadianceCascadeConfig.generated.h"

/**
 * World-level Radiance Cascades layout.
 *
 * Each cascade stores radiance for a non-overlapping distance interval.
 * Angular cells are hierarchically bisected in azimuth: each parent cell
 * maps to two equal-solid-angle child cells in the next cascade.
 * This describes the ray hierarchy and packed per-probe ray-data layout;
 * it does not perform scene tracing or allocate GPU resources.
 */
USTRUCT(BlueprintType)
struct FDDGIRadianceCascadeSlice
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radiance Cascades")
	int32 CascadeIndex = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radiance Cascades", meta = (Units = "cm"))
	float StartDistance = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radiance Cascades", meta = (Units = "cm"))
	float EndDistance = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radiance Cascades")
	int32 RayCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radiance Cascades")
	int32 StorageOffset = 0;
};

USTRUCT(BlueprintType)
struct FDDGIRadianceCascadeConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1", ClampMax = "8"))
	int32 CascadeCount = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1", ClampMax = "64"))
	int32 BaseRayCount = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.0"))
	float BaseSegmentLength = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.01"))
	float CascadeScale = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.0"))
	float MaxTraceDistance = 7750.0f;

	FORCEINLINE int32 GetConfiguredCascadeCount() const
	{
		return FMath::Clamp(CascadeCount, 1, 8);
	}

	// Trailing cascades that begin outside MaxTraceDistance have no valid
	// ray interval. Exclude them from the packed layout, not just from tracing,
	// so direction, radiance, hit and integration textures all shrink together.
	FORCEINLINE int32 GetSafeCascadeCount() const
	{
		const int32 ConfiguredCount = GetConfiguredCascadeCount();
		const float SafeScale = FMath::Max(1.01f, CascadeScale);
		const float SafeBaseLength = FMath::Max(1.0f, BaseSegmentLength);
		const float SafeMaxDistance = FMath::Max(1.0f, MaxTraceDistance);

		int32 ActiveCount = 0;
		int32 TotalRays = 0;
		const int32 SafeBaseRayCount = FMath::Clamp(BaseRayCount, 1, 64);
		const int32 MaxRaysPerProbe = 128;
		for (int32 Index = 0; Index < ConfiguredCount; ++Index)
		{
			const float CascadePower = FMath::Pow(SafeScale, static_cast<float>(Index));
			const float StartDistance =
				SafeBaseLength * (CascadePower - 1.0f) / (SafeScale - 1.0f);
			if (StartDistance >= SafeMaxDistance)
			{
				break;
			}
			const int32 CascadeRays = SafeBaseRayCount << Index;
			if (TotalRays + CascadeRays > MaxRaysPerProbe)
			{
				break;
			}
			TotalRays += CascadeRays;
			++ActiveCount;
		}
		// Cascade zero starts at distance zero and is always active for a
		// sanitized positive trace distance.
		return FMath::Max(1, ActiveCount);
	}

	FORCEINLINE int32 GetRayCount(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, 7);
		const int32 SafeBaseRayCount = FMath::Clamp(BaseRayCount, 1, 64);
		return SafeBaseRayCount << SafeIndex;
	}

	FORCEINLINE int32 GetRayStorageOffset(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount());
		int32 Offset = 0;

		for (int32 Index = 0; Index < SafeIndex; ++Index)
		{
			Offset += GetRayCount(Index);
		}
		return Offset;
	}

	FORCEINLINE int32 GetTotalRayCount() const
	{
		return GetRayStorageOffset(GetSafeCascadeCount());
	}

	FORCEINLINE int32 GetRayStorageIndex(int32 CascadeIndex, int32 RayIndex) const
	{
		const int32 SafeCascade = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		const int32 SafeRay = FMath::Clamp(RayIndex, 0, GetRayCount(SafeCascade) - 1);
		return GetRayStorageOffset(SafeCascade) + SafeRay;
	}

	FORCEINLINE float GetCascadeSegmentLength(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		return FMath::Max(1.0f, BaseSegmentLength) *
			FMath::Pow(FMath::Max(1.01f, CascadeScale), static_cast<float>(SafeIndex));
	}

	FORCEINLINE float GetCascadeStartDistance(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetConfiguredCascadeCount() - 1);
		const float SafeScale = FMath::Max(1.01f, CascadeScale);
		const float SafeBaseLength = FMath::Max(1.0f, BaseSegmentLength);
		const float CascadePower = FMath::Pow(SafeScale, static_cast<float>(SafeIndex));
		const float Distance = SafeBaseLength * (CascadePower - 1.0f) / (SafeScale - 1.0f);
		return FMath::Min(Distance, FMath::Max(1.0f, MaxTraceDistance));
	}

	FORCEINLINE float GetCascadeEndDistance(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetConfiguredCascadeCount() - 1);
		const float SafeMaxDistance = FMath::Max(1.0f, MaxTraceDistance);
		const float Start = GetCascadeStartDistance(SafeIndex);

		// Match the trace shader: the farthest active cascade owns all remaining
		// distance up to MaxTraceDistance, even when the configured geometric
		// series would otherwise stop short of the requested ray distance.
		if (SafeIndex == GetSafeCascadeCount() - 1)
		{
			return SafeMaxDistance;
		}

		return FMath::Min(
			Start + GetCascadeSegmentLength(SafeIndex),
			SafeMaxDistance);
	}

	// Builds the packed per-probe cascade table consumed later by GPU upload.
	FORCEINLINE void BuildCascadeSlices(TArray<FDDGIRadianceCascadeSlice>& OutSlices) const
	{
		OutSlices.Reset(GetSafeCascadeCount());

		for (int32 Index = 0; Index < GetSafeCascadeCount(); ++Index)
		{
			FDDGIRadianceCascadeSlice& Slice = OutSlices.AddDefaulted_GetRef();
			Slice.CascadeIndex = Index;
			Slice.StartDistance = GetCascadeStartDistance(Index);
			Slice.EndDistance = GetCascadeEndDistance(Index);
			Slice.RayCount = GetRayCount(Index);
			Slice.StorageOffset = GetRayStorageOffset(Index);
		}
	}

	// Hierarchical angular storage is capped at 128 rays per probe.
	// Current GPU layout: raw radiance (8) + gather scratch (8) + packed final radiance (4) +
	// hit distance (4) + hit triangle index (4) = 28 bytes per ray per probe.
	// Direction (8 bytes) is stored once per ray layout and shared by every probe.
	// Probe origins use a separate float4 (16 bytes per probe). Excludes BVH/hash buffers and padding.
	FORCEINLINE int64 GetEstimatedStorageBytes(int32 ProbeCount, int32 BytesPerRay = 28) const
	{
		const int64 SafeProbeCount = static_cast<int64>(FMath::Max(0, ProbeCount));
		const int64 SafeRayCount = static_cast<int64>(GetTotalRayCount());
		return SafeProbeCount *
			(SafeRayCount * static_cast<int64>(FMath::Max(1, BytesPerRay)) + 16ll) +
			SafeRayCount * 8ll;
	}
};
