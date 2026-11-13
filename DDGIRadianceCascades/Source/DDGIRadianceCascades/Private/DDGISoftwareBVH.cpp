#include "DDGISoftwareBVH.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "StaticMeshResources.h"
#include "Materials/MaterialInterface.h"

#include <algorithm>

namespace
{
	struct FDDGISoftwareTriangle
	{
		FVector A;
		FVector B;
		FVector C;
		FVector Centroid;
		int32 OriginalIndex = INDEX_NONE;
		FVector BoundsMin;
		FVector BoundsMax;
		FVector EmissiveRadiance = FVector::ZeroVector;
		FVector BaseColor = FVector(0.18f, 0.18f, 0.18f);
	};

	struct FDDGISoftwareBVHNode
	{
		FVector BoundsMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector BoundsMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		int32 LeftChild = INDEX_NONE;
		int32 RightChild = INDEX_NONE;
		int32 FirstTriangle = 0;
		int32 TriangleCount = 0;
	};

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	void IncludePoint(FVector& BoundsMin, FVector& BoundsMax, const FVector& Point)
	{
		BoundsMin.X = FMath::Min(BoundsMin.X, Point.X);
		BoundsMin.Y = FMath::Min(BoundsMin.Y, Point.Y);
		BoundsMin.Z = FMath::Min(BoundsMin.Z, Point.Z);
		BoundsMax.X = FMath::Max(BoundsMax.X, Point.X);
		BoundsMax.Y = FMath::Max(BoundsMax.Y, Point.Y);
		BoundsMax.Z = FMath::Max(BoundsMax.Z, Point.Z);
	}

	int32 BuildNode(
		TArray<FDDGISoftwareTriangle>& Triangles,
		int32 FirstTriangle,
		int32 TriangleCount,
		TArray<FDDGISoftwareBVHNode>& Nodes,
		int32 Depth = 0)
	{
		const int32 NodeIndex = Nodes.AddDefaulted();
		FVector BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		FVector CentroidMin(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector CentroidMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

		for (int32 Index = FirstTriangle; Index < FirstTriangle + TriangleCount; ++Index)
		{
			const FDDGISoftwareTriangle& Triangle = Triangles[Index];
			IncludePoint(BoundsMin, BoundsMax, Triangle.BoundsMin);
			IncludePoint(BoundsMin, BoundsMax, Triangle.BoundsMax);
			IncludePoint(CentroidMin, CentroidMax, Triangle.Centroid);
		}

		// Keep the worst-case traversal depth below the shader's 64-entry stack.
		// At the limit, emit a larger leaf rather than risking a dropped child.
		if (TriangleCount <= 4 || Depth >= 48)
		{
			FDDGISoftwareBVHNode& Leaf = Nodes[NodeIndex];
			Leaf.BoundsMin = BoundsMin;
			Leaf.BoundsMax = BoundsMax;
			Leaf.FirstTriangle = FirstTriangle;
			Leaf.TriangleCount = TriangleCount;
			return NodeIndex;
		}

		const FVector Extent = CentroidMax - CentroidMin;
		const float AxisExtent[3] = { Extent.X, Extent.Y, Extent.Z };
		const float CentroidMinAxis[3] = { CentroidMin.X, CentroidMin.Y, CentroidMin.Z };
		const int32 BinCount = 12;
		struct FBin
		{
			FVector BoundsMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector BoundsMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			int32 Count = 0;
		};
		int32 BestAxis = INDEX_NONE;
		int32 BestSplitBin = INDEX_NONE;
		float BestCost = FLT_MAX;

		auto BoundsArea = [](const FVector& Min, const FVector& Max) -> float
		{
			const FVector Size = Max - Min;
			return 2.0f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
		};

		// Deterministic 12-bin SAH: choose the split that minimizes the
		// estimated number of triangle tests in the resulting child nodes.
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisExtent[AxisIndex] <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			FBin Bins[BinCount];
			for (int32 TriangleIndex = FirstTriangle;
				TriangleIndex < FirstTriangle + TriangleCount; ++TriangleIndex)
			{
				const FVector& Center = Triangles[TriangleIndex].Centroid;
				const float Coordinate = AxisIndex == 0 ? Center.X : (AxisIndex == 1 ? Center.Y : Center.Z);
				const float Normalized = (Coordinate - CentroidMinAxis[AxisIndex]) / AxisExtent[AxisIndex];
				const int32 BinIndex = FMath::Clamp(FMath::FloorToInt(Normalized * BinCount), 0, BinCount - 1);
				FBin& Bin = Bins[BinIndex];
				++Bin.Count;
				IncludePoint(Bin.BoundsMin, Bin.BoundsMax, Triangles[TriangleIndex].BoundsMin);
				IncludePoint(Bin.BoundsMin, Bin.BoundsMax, Triangles[TriangleIndex].BoundsMax);
			}

			FVector LeftMin[BinCount];
			FVector LeftMax[BinCount];
			FVector RightMin[BinCount];
			FVector RightMax[BinCount];
			int32 LeftCount[BinCount] = {};
			int32 RightCount[BinCount] = {};
			FVector RunningMin(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector RunningMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			int32 RunningCount = 0;
			for (int32 BinIndex = 0; BinIndex < BinCount; ++BinIndex)
			{
				if (Bins[BinIndex].Count > 0)
				{
					IncludePoint(RunningMin, RunningMax, Bins[BinIndex].BoundsMin);
					IncludePoint(RunningMin, RunningMax, Bins[BinIndex].BoundsMax);
				}
				RunningCount += Bins[BinIndex].Count;
				LeftMin[BinIndex] = RunningMin;
				LeftMax[BinIndex] = RunningMax;
				LeftCount[BinIndex] = RunningCount;
			}
			RunningMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
			RunningMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			RunningCount = 0;
			for (int32 BinIndex = BinCount - 1; BinIndex >= 0; --BinIndex)
			{
				if (Bins[BinIndex].Count > 0)
				{
					IncludePoint(RunningMin, RunningMax, Bins[BinIndex].BoundsMin);
					IncludePoint(RunningMin, RunningMax, Bins[BinIndex].BoundsMax);
				}
				RunningCount += Bins[BinIndex].Count;
				RightMin[BinIndex] = RunningMin;
				RightMax[BinIndex] = RunningMax;
				RightCount[BinIndex] = RunningCount;
			}

			for (int32 SplitBin = 0; SplitBin < BinCount - 1; ++SplitBin)
			{
				const int32 LCount = LeftCount[SplitBin];
				const int32 RCount = RightCount[SplitBin + 1];
				if (LCount == 0 || RCount == 0)
				{
					continue;
				}
				// Prevent pathological, highly unbalanced recursion.
				if (LCount * 4 < TriangleCount || RCount * 4 < TriangleCount)
				{
					continue;
				}
				const float Cost =
					BoundsArea(LeftMin[SplitBin], LeftMax[SplitBin]) * LCount +
					BoundsArea(RightMin[SplitBin + 1], RightMax[SplitBin + 1]) * RCount;
				if (Cost < BestCost)
				{
					BestCost = Cost;
					BestAxis = AxisIndex;
					BestSplitBin = SplitBin;
				}
			}
		}

		int32 Middle = FirstTriangle;
		if (BestAxis != INDEX_NONE)
		{
			const float AxisMinimum = CentroidMinAxis[BestAxis];
			const float AxisSize = AxisExtent[BestAxis];
			FDDGISoftwareTriangle* Data = Triangles.GetData();
			const auto PartitionEnd = std::stable_partition(
				Data + FirstTriangle, Data + FirstTriangle + TriangleCount,
				[BestAxis, AxisMinimum, AxisSize, BestSplitBin, BinCount](const FDDGISoftwareTriangle& Triangle)
				{
					const float Coordinate = BestAxis == 0 ? Triangle.Centroid.X :
						(BestAxis == 1 ? Triangle.Centroid.Y : Triangle.Centroid.Z);
					const float Normalized = (Coordinate - AxisMinimum) / AxisSize;
					const int32 BinIndex = FMath::Clamp(FMath::FloorToInt(Normalized * BinCount), 0, BinCount - 1);
					return BinIndex <= BestSplitBin;
				});
			Middle = static_cast<int32>(PartitionEnd - Data);
		}

		// Degenerate centroid distributions or an empty SAH partition fall
		// back to a balanced median split on the widest centroid axis.
		if (Middle <= FirstTriangle || Middle >= FirstTriangle + TriangleCount)
		{
			int32 Axis = 0;
			if (Extent.Y > Extent.X && Extent.Y >= Extent.Z) Axis = 1;
			else if (Extent.Z > Extent.X && Extent.Z > Extent.Y) Axis = 2;
			Middle = FirstTriangle + TriangleCount / 2;
			FDDGISoftwareTriangle* Data = Triangles.GetData();
			std::sort(
				Data + FirstTriangle, Data + FirstTriangle + TriangleCount,
				[Axis](const FDDGISoftwareTriangle& A, const FDDGISoftwareTriangle& B)
				{
					const float AValue = Axis == 0 ? A.Centroid.X : (Axis == 1 ? A.Centroid.Y : A.Centroid.Z);
					const float BValue = Axis == 0 ? B.Centroid.X : (Axis == 1 ? B.Centroid.Y : B.Centroid.Z);
					return AValue == BValue ? A.OriginalIndex < B.OriginalIndex : AValue < BValue;
				});
		}

		const int32 LeftCount = Middle - FirstTriangle;
		const int32 LeftChild = BuildNode(Triangles, FirstTriangle, LeftCount, Nodes, Depth + 1);
		const int32 RightChild = BuildNode(Triangles, Middle, TriangleCount - LeftCount, Nodes, Depth + 1);

		FDDGISoftwareBVHNode& Node = Nodes[NodeIndex];
		Node.BoundsMin = BoundsMin;
		Node.BoundsMax = BoundsMax;
		Node.LeftChild = LeftChild;
		Node.RightChild = RightChild;
		return NodeIndex;
	}

	void GatherBuildStatistics(
		const TArray<FDDGISoftwareBVHNode>& Nodes,
		int32 NodeIndex,
		int32 Depth,
		int32& OutMaxDepth,
		int32& OutLeafCount,
		int32& OutMinLeafTriangles,
		int32& OutMaxLeafTriangles,
		int64& OutLeafTriangleTotal,
		float& OutWeightedArea)
	{
		if (!Nodes.IsValidIndex(NodeIndex)) return;
		const FDDGISoftwareBVHNode& Node = Nodes[NodeIndex];
		OutMaxDepth = FMath::Max(OutMaxDepth, Depth);
		if (Node.TriangleCount > 0)
		{
			++OutLeafCount;
			OutMinLeafTriangles = FMath::Min(OutMinLeafTriangles, Node.TriangleCount);
			OutMaxLeafTriangles = FMath::Max(OutMaxLeafTriangles, Node.TriangleCount);
			OutLeafTriangleTotal += Node.TriangleCount;
			const FVector Size = Node.BoundsMax - Node.BoundsMin;
			const float Area = 2.0f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
			OutWeightedArea += Area * Node.TriangleCount;
			return;
		}
		GatherBuildStatistics(Nodes, Node.LeftChild, Depth + 1, OutMaxDepth, OutLeafCount,
			OutMinLeafTriangles, OutMaxLeafTriangles, OutLeafTriangleTotal, OutWeightedArea);
		GatherBuildStatistics(Nodes, Node.RightChild, Depth + 1, OutMaxDepth, OutLeafCount,
			OutMinLeafTriangles, OutMaxLeafTriangles, OutLeafTriangleTotal, OutWeightedArea);
	}


	bool ValidateBVH(
		const TArray<FDDGISoftwareBVHNode>& Nodes,
		int32 TriangleCount)
	{
		if (Nodes.Num() == 0 || TriangleCount <= 0)
		{
			return false;
		}

		TArray<uint8> VisitedNodes;
		VisitedNodes.Init(0, Nodes.Num());
		TArray<uint8> CoveredTriangles;
		CoveredTriangles.Init(0, TriangleCount);
		TArray<int32> PendingNodes;
		TArray<int32> PendingDepths;
		PendingNodes.Add(0);
		PendingDepths.Add(0);
		int32 VisitedCount = 0;
		int32 CoveredCount = 0;

		while (PendingNodes.Num() > 0)
		{
			const int32 LastIndex = PendingNodes.Num() - 1;
			const int32 NodeIndex = PendingNodes[LastIndex];
			const int32 Depth = PendingDepths[LastIndex];
			PendingNodes.RemoveAt(LastIndex, 1, false);
			PendingDepths.RemoveAt(LastIndex, 1, false);

			if (!Nodes.IsValidIndex(NodeIndex) || Depth > 48 || VisitedNodes[NodeIndex] != 0)
			{
				return false;
			}

			VisitedNodes[NodeIndex] = 1;
			++VisitedCount;
			const FDDGISoftwareBVHNode& Node = Nodes[NodeIndex];
			if (!IsFiniteVector(Node.BoundsMin) || !IsFiniteVector(Node.BoundsMax) ||
				Node.BoundsMin.X > Node.BoundsMax.X ||
				Node.BoundsMin.Y > Node.BoundsMax.Y ||
				Node.BoundsMin.Z > Node.BoundsMax.Z)
			{
				return false;
			}

			if (Node.TriangleCount > 0)
			{
				if (Node.LeftChild != INDEX_NONE || Node.RightChild != INDEX_NONE ||
					Node.TriangleCount > TriangleCount ||
					Node.FirstTriangle < 0 || Node.FirstTriangle > TriangleCount - Node.TriangleCount)
				{
					return false;
				}

				for (int32 TriangleIndex = Node.FirstTriangle;
					TriangleIndex < Node.FirstTriangle + Node.TriangleCount; ++TriangleIndex)
				{
					if (CoveredTriangles[TriangleIndex] != 0)
					{
						return false;
					}
					CoveredTriangles[TriangleIndex] = 1;
					++CoveredCount;
				}
			}
			else
			{
				if (Node.TriangleCount != 0 ||
					!Nodes.IsValidIndex(Node.LeftChild) || !Nodes.IsValidIndex(Node.RightChild) ||
					Node.LeftChild == Node.RightChild || Node.FirstTriangle != 0)
				{
					return false;
				}
				PendingNodes.Add(Node.RightChild);
				PendingDepths.Add(Depth + 1);
				PendingNodes.Add(Node.LeftChild);
				PendingDepths.Add(Depth + 1);
			}
		}

		return VisitedCount == Nodes.Num() && CoveredCount == TriangleCount;
	}


	bool IsValidTriangleForPacking(const FDDGISoftwareTriangle& Triangle)
	{
		return IsFiniteVector(Triangle.A) &&
			IsFiniteVector(Triangle.B) &&
			IsFiniteVector(Triangle.C) &&
			IsFiniteVector(Triangle.Centroid) &&
			IsFiniteVector(Triangle.BoundsMin) &&
			IsFiniteVector(Triangle.BoundsMax) &&
			IsFiniteVector(Triangle.EmissiveRadiance) &&
			IsFiniteVector(Triangle.BaseColor) &&
			Triangle.BoundsMin.X <= Triangle.BoundsMax.X &&
			Triangle.BoundsMin.Y <= Triangle.BoundsMax.Y &&
			Triangle.BoundsMin.Z <= Triangle.BoundsMax.Z &&
			Triangle.EmissiveRadiance.X >= 0.0f &&
			Triangle.EmissiveRadiance.Y >= 0.0f &&
			Triangle.EmissiveRadiance.Z >= 0.0f &&
			Triangle.BaseColor.X >= 0.0f &&
			Triangle.BaseColor.Y >= 0.0f &&
			Triangle.BaseColor.Z >= 0.0f;
	}

	FVector4 PackVector(const FVector& Value)
	{
		return FVector4(Value.X, Value.Y, Value.Z, 0.0f);
	}
}

void DDGISoftwareBVH::BuildStaticWorld(
	UWorld* World,
	int32 MaxTriangleCount,
	TArray<FVector4>& OutPackedNodes,
	TArray<FVector4>& OutPackedTriangles,
	FDDGISoftwareBVHBuildStats& OutStats)
{
	OutPackedNodes.Reset();
	OutPackedTriangles.Reset();
	OutStats = FDDGISoftwareBVHBuildStats();

	if (!World || MaxTriangleCount <= 0)
	{
		return;
	}

	TArray<FDDGISoftwareTriangle> Triangles;
	Triangles.Reserve(FMath::Min(MaxTriangleCount, 65536));

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor))
		{
			continue;
		}

		TArray<UStaticMeshComponent*> Components;
		Actor->GetComponents<UStaticMeshComponent>(Components);

		for (UStaticMeshComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->GetStaticMesh())
			{
				continue;
			}

			if (Cast<UInstancedStaticMeshComponent>(Component))
			{
				++OutStats.SkippedInstanced;
				continue;
			}

			if (Component->Mobility != EComponentMobility::Static)
			{
				++OutStats.SkippedMovable;
				continue;
			}

			const UStaticMesh* StaticMesh = Component->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				continue;
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
			if (!PositionBuffer.GetAllowCPUAccess() || !LOD.IndexBuffer.GetAllowCPUAccess())
			{
				++OutStats.SkippedNoCPUAccess;
				continue;
			}

			const uint32 VertexCount = PositionBuffer.GetNumVertices();
			const uint32 IndexCount = LOD.IndexBuffer.GetNumIndices();
			if (VertexCount == 0 || IndexCount < 3)
			{
				continue;
			}

			const FTransform ComponentTransform = Component->GetComponentTransform();
			const int32 ComponentTriangleCount = static_cast<int32>(IndexCount / 3);
			bool bAddedAnyTriangle = false;

			// Resolve each triangle's material from the LOD section ranges.
			// Only the explicit DDGI emissive parameter is sampled here; the
			// software BVH does not evaluate arbitrary material graphs.
			TArray<int32> TriangleMaterialIndices;
			TriangleMaterialIndices.Init(INDEX_NONE, ComponentTriangleCount);
			for (const FStaticMeshSection& Section : LOD.Sections)
			{
				const int32 FirstSectionTriangle = static_cast<int32>(Section.FirstIndex / 3u);
				const int32 EndSectionTriangle = FMath::Min(
					FirstSectionTriangle + static_cast<int32>(Section.NumTriangles),
					ComponentTriangleCount);
				for (int32 SectionTriangle = FirstSectionTriangle;
					SectionTriangle < EndSectionTriangle; ++SectionTriangle)
				{
					TriangleMaterialIndices[SectionTriangle] = static_cast<int32>(Section.MaterialIndex);
				}
			}

			TArray<FVector> MaterialEmissiveRadiance;
			MaterialEmissiveRadiance.Init(FVector::ZeroVector, Component->GetNumMaterials());

			TArray<FVector> MaterialBaseColor;
			// Materials without an explicit DDGI_BaseColor parameter use a neutral 18% diffuse fallback.
			// Arbitrary material graphs and texture samples are not evaluated by this CPU BVH.
			MaterialBaseColor.Init(FVector(0.18f, 0.18f, 0.18f), Component->GetNumMaterials());
			const FMaterialParameterInfo BaseColorParameterInfo(FName(TEXT("DDGI_BaseColor")));
			const FMaterialParameterInfo EmissiveParameterInfo(FName(TEXT("DDGI_EmissiveRadiance")));
			for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
			{
				UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
				if (!Material)
				{
					continue;
				}

				FLinearColor EmissiveColor = FLinearColor::Black;
				if (Material->GetVectorParameterValue(EmissiveParameterInfo, EmissiveColor))
				{
					MaterialEmissiveRadiance[MaterialIndex] = FVector(
						FMath::Max(0.0f, EmissiveColor.R),
						FMath::Max(0.0f, EmissiveColor.G),
						FMath::Max(0.0f, EmissiveColor.B));
				}
				FLinearColor BaseColorValue = FLinearColor::White;
				if (Material->GetVectorParameterValue(BaseColorParameterInfo, BaseColorValue))
				{
					MaterialBaseColor[MaterialIndex] = FVector(
						FMath::Max(0.0f, BaseColorValue.R),
						FMath::Max(0.0f, BaseColorValue.G),
						FMath::Max(0.0f, BaseColorValue.B));
				}
				else
				{
					++OutStats.MaterialsUsingDefaultBaseColor;
				}
			}

			for (int32 TriangleIndex = 0; TriangleIndex < ComponentTriangleCount; ++TriangleIndex)
			{
				if (Triangles.Num() >= MaxTriangleCount)
				{
					break;
				}

				const uint32 I0 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 0);
				const uint32 I1 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 1);
				const uint32 I2 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 2);
				if (I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
				{
					continue;
				}

				const FVector A = ComponentTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I0)));
				const FVector B = ComponentTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I1)));
				const FVector C = ComponentTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I2)));
				if (!IsFiniteVector(A) || !IsFiniteVector(B) || !IsFiniteVector(C))
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				// Reject zero-area and near-degenerate triangles before they enter
				// the BVH. The threshold is expressed in squared world-space area.
				const FVector Cross = FVector::CrossProduct(B - A, C - A);
				if (!IsFiniteVector(Cross) || Cross.SizeSquared() <= 1.0e-12f)
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				FDDGISoftwareTriangle& Triangle = Triangles.AddDefaulted_GetRef();
				Triangle.OriginalIndex = Triangles.Num() - 1;
				Triangle.A = A;
				Triangle.B = B;
				Triangle.C = C;
				// Divide before adding so the centroid does not overflow merely because
				// the intermediate sum of three finite world positions exceeds float range.
				Triangle.Centroid = A / 3.0f + B / 3.0f + C / 3.0f;
				const int32 MaterialIndex = TriangleMaterialIndices.IsValidIndex(TriangleIndex)
					? TriangleMaterialIndices[TriangleIndex]
					: INDEX_NONE;
				if (MaterialEmissiveRadiance.IsValidIndex(MaterialIndex))
				{
					Triangle.EmissiveRadiance = MaterialEmissiveRadiance[MaterialIndex];
				}

				if (MaterialBaseColor.IsValidIndex(MaterialIndex))
				{
					Triangle.BaseColor = MaterialBaseColor[MaterialIndex];
				}
				Triangle.BoundsMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
				Triangle.BoundsMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, A);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, B);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, C);
				bAddedAnyTriangle = true;
			}

			if (bAddedAnyTriangle)
			{
				++OutStats.IncludedComponents;
			}

			if (Triangles.Num() >= MaxTriangleCount)
			{
				break;
			}
		}

		if (Triangles.Num() >= MaxTriangleCount)
		{
			break;
		}
	}

	OutStats.TriangleCount = Triangles.Num();
	if (Triangles.Num() == 0)
	{
		return;
	}

	TArray<FDDGISoftwareBVHNode> Nodes;
	Nodes.Reserve(Triangles.Num() / 2);
	// Node metadata is stored in float4, so integer indices must remain exactly
	// representable by IEEE-754 single precision (24 significant bits).
	const int32 MaxExactFloatInteger = 1 << 24;
	if (Triangles.Num() >= MaxExactFloatInteger)
	{
		UE_LOG(LogTemp, Error, TEXT("DDGI: Software BVH triangle count exceeds exact float metadata range."));
		OutStats.TriangleCount = 0;
		return;
	}
	for (const FDDGISoftwareTriangle& Triangle : Triangles)
	{
		if (!IsValidTriangleForPacking(Triangle))
		{
			UE_LOG(LogTemp, Error, TEXT("DDGI: Software BVH contains non-finite or invalid triangle attributes; geometry upload was skipped."));
			OutStats.TriangleCount = 0;
			return;
		}
	}

	BuildNode(Triangles, 0, Triangles.Num(), Nodes);
	if (Nodes.Num() >= MaxExactFloatInteger || !ValidateBVH(Nodes, Triangles.Num()))
	{
		UE_LOG(LogTemp, Error, TEXT("DDGI: Software BVH validation failed; geometry upload was skipped."));
		OutStats.TriangleCount = 0;
		return;
	}

	// Each triangle occupies four float4 entries in the shader buffer.
	OutPackedTriangles.Reserve(Triangles.Num() * 4);
	for (const FDDGISoftwareTriangle& Triangle : Triangles)
	{
		// XYZ stores positions; W stores the corresponding emissive channel.
		OutPackedTriangles.Add(FVector4(Triangle.A.X, Triangle.A.Y, Triangle.A.Z, Triangle.EmissiveRadiance.X));
		OutPackedTriangles.Add(FVector4(Triangle.B.X, Triangle.B.Y, Triangle.B.Z, Triangle.EmissiveRadiance.Y));
		OutPackedTriangles.Add(FVector4(Triangle.C.X, Triangle.C.Y, Triangle.C.Z, Triangle.EmissiveRadiance.Z));
		OutPackedTriangles.Add(FVector4(Triangle.BaseColor.X, Triangle.BaseColor.Y, Triangle.BaseColor.Z, 0.0f));
	}

	OutPackedNodes.Reserve(Nodes.Num() * 3);
	for (const FDDGISoftwareBVHNode& Node : Nodes)
	{
		OutPackedNodes.Add(PackVector(Node.BoundsMin));
		OutPackedNodes.Add(PackVector(Node.BoundsMax));
		OutPackedNodes.Add(FVector4(
			static_cast<float>(Node.LeftChild),
			static_cast<float>(Node.RightChild),
			static_cast<float>(Node.FirstTriangle),
			static_cast<float>(Node.TriangleCount)));
	}

	OutStats.NodeCount = Nodes.Num();
	int64 LeafTriangleTotal = 0;
	float WeightedLeafArea = 0.0f;
	OutStats.MinLeafTriangleCount = MAX_int32;
	GatherBuildStatistics(Nodes, 0, 0, OutStats.MaxDepth, OutStats.LeafCount,
		OutStats.MinLeafTriangleCount, OutStats.MaxLeafTriangleCount, LeafTriangleTotal, WeightedLeafArea);
	if (OutStats.LeafCount > 0)
	{
		OutStats.AverageLeafTriangleCount = static_cast<float>(LeafTriangleTotal) / OutStats.LeafCount;
		const FVector RootSize = Nodes[0].BoundsMax - Nodes[0].BoundsMin;
		const float RootArea = 2.0f * (RootSize.X * RootSize.Y + RootSize.Y * RootSize.Z + RootSize.Z * RootSize.X);
		OutStats.EstimatedSAHCost = RootArea > SMALL_NUMBER ? WeightedLeafArea / RootArea : 0.0f;
	}
	else
	{
		OutStats.MinLeafTriangleCount = 0;
	}
}
