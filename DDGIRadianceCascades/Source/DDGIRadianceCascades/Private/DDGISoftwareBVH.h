#pragma once

#include "CoreMinimal.h"

class UWorld;

struct FDDGISoftwareBVHBuildStats
{
	int32 IncludedComponents = 0;
	int32 SkippedNoCPUAccess = 0;
	int32 SkippedMovable = 0;
	int32 SkippedInstanced = 0;
	int32 TriangleCount = 0;
	int32 NodeCount = 0;
};

namespace DDGISoftwareBVH
{
	void BuildStaticWorld(
		UWorld* World,
		int32 MaxTriangleCount,
		TArray<FVector4>& OutPackedNodes,
		TArray<FVector4>& OutPackedTriangles,
		FDDGISoftwareBVHBuildStats& OutStats);
}
