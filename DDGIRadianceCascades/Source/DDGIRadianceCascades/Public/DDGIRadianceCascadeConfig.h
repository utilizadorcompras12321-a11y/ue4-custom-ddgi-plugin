#pragma once

#include "CoreMinimal.h"
#include "DDGIRadianceCascadeConfig.generated.h"

/**
 * World-level Radiance Cascades layout.
 *
 * Each cascade stores radiance for a non-overlapping distance interval.
 * Direction counts grow by RaySplitFactor between adjacent cascades.
 * This is layout/configuration only; it does not perform scene tracing.
 */
struct FDDGIRadianceCascadeConfig
{
	GENERATED_BODY()

	UPROPERTY() int32 CascadeCount = 5;
	UPROPERTY() int32 BaseRayCount = 6;
	UPROPERTY() int32 RaySplitFactor = 4;
	UPROPERTY() float BaseSegmentLength = 250.0f;
	UPROPERTY() float CascadeScale = 2.0f;
	UPROPERTY() float MaxTraceDistance = 7750.0f;

	FORCEINLINE int32 GetSafeCascadeCount() const
	{
		return FMath::Clamp(CascadeCount, 1, 8);
	}

	FORCEINLINE int32 GetRayCount(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		int64 Count = FMath::Max(1, BaseRayCount);
		const int32 Split = FMath::Clamp(RaySplitFactor, 1, 4);
		for (int32 Index = 0; Index < SafeIndex; ++Index)
		{
			Count *= Split;
			if (Count > 65536)
			{
				return 65536;
			}
		}
		return static_cast<int32>(Count);
	}

	FORCEINLINE float GetCascadeStartDistance(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		const float SafeBaseLength = FMath::Max(1.0f, BaseSegmentLength);
		const float SafeScale = FMath::Max(1.01f, CascadeScale);
		float Distance = 0.0f;
		float SegmentLength = SafeBaseLength;
		for (int32 Index = 0; Index < SafeIndex; ++Index)
		{
			Distance += SegmentLength;
			SegmentLength *= SafeScale;
		}
		return FMath::Min(Distance, FMath::Max(1.0f, MaxTraceDistance));
	}

	FORCEINLINE float GetCascadeEndDistance(int32 CascadeIndex) const
	{
		const int32 SafeIndex = FMath::Clamp(CascadeIndex, 0, GetSafeCascadeCount() - 1);
		const float Start = GetCascadeStartDistance(SafeIndex);
		const float Length = FMath::Max(1.0f, BaseSegmentLength) *
			FMath::Pow(FMath::Max(1.01f, CascadeScale), static_cast<float>(SafeIndex));
		return FMath::Min(Start + Length, FMath::Max(1.0f, MaxTraceDistance));
	}
};
