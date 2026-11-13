#pragma once

#include "CoreMinimal.h"
#include "DDGIRadianceCascadeConfig.generated.h"

/**
 * World-level Radiance Cascades layout.
 *
 * Each cascade stores radiance for a non-overlapping distance interval.
 * Direction counts grow by RaySplitFactor between adjacent cascades.
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1", ClampMax = "4"))
	int32 RaySplitFactor = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.0"))
	float BaseSegmentLength = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.01"))
	float CascadeScale = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiance Cascades", meta = (ClampMin = "1.0"))
	float MaxTraceDistance = 7750.0f;

	FORCEINLINE int32 GetSafeCascadeCount() const
	{
		return FMath::Clamp(CascadeCount, 1, 8);
	}

	FORCEINLINE int32 GetRayCount(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		int64 Count = FMath::Clamp(BaseRayCount, 1, 64);
		const int32 Split = FMath::Clamp(RaySplitFactor, 1, 4);

		for (int32 Index = 0; Index < SafeIndex; ++Index)
		{
			Count = FMath::Min<int64>(Count * Split, 65536);
		}
		return static_cast<int32>(Count);
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
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		float Distance = 0.0f;

		for (int32 Index = 0; Index < SafeIndex; ++Index)
		{
			Distance += GetCascadeSegmentLength(Index);
		}
		return FMath::Min(Distance, FMath::Max(1.0f, MaxTraceDistance));
	}

	FORCEINLINE float GetCascadeEndDistance(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		const float Start = GetCascadeStartDistance(SafeIndex);
		return FMath::Min(
			Start + GetCascadeSegmentLength(SafeIndex),
			FMath::Max(1.0f, MaxTraceDistance));
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

	// GPU format uses half-float RGBA radiance plus one R32F hit-distance texel.
	FORCEINLINE int64 GetEstimatedStorageBytes(int32 ProbeCount, int32 BytesPerRay = 20) const
	{
		return static_cast<int64>(FMath::Max(0, ProbeCount)) *
			static_cast<int64>(GetTotalRayCount()) *
			static_cast<int64>(FMath::Max(1, BytesPerRay));
	}
};
