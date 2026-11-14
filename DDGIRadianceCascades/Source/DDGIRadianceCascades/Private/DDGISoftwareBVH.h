#pragma once

#include "CoreMinimal.h"
#include "Containers/ResourceArray.h"

class UWorld;

struct FDDGISoftwareBVHBuildStats
{
	int32 IncludedComponents = 0;
	int32 SkippedNoCPUAccess = 0;
	int32 SkippedMovable = 0;
	int32 SkippedInstanced = 0;
	int32 SkippedInvalidTriangles = 0;
	int32 MaterialsUsingDefaultBaseColor = 0;
	int32 TriangleCount = 0;
	int32 NodeCount = 0;
	int32 MaxDepth = 0;
	int32 LeafCount = 0;
	int32 MinLeafTriangleCount = 0;
	int32 MaxLeafTriangleCount = 0;
	float AverageLeafTriangleCount = 0.0f;
	float EstimatedSAHCost = 0.0f;
};

namespace DDGISoftwareBVH
{
	void BuildStaticWorld(
		UWorld* World,
		int32 MaxTriangleCount,
		TResourceArray<FVector4>& OutPackedNodes,
		TResourceArray<FVector4>& OutPackedTriangles,
		FVector& OutWorldOrigin,
		FDDGISoftwareBVHBuildStats& OutStats);
}
