#include "DDGISoftwareBVH.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "Model.h"
#include "Components/ModelComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "StaticMeshResources.h"
#include "Materials/MaterialInterface.h"

#include <algorithm>

namespace
{
	static const int32 GDDGISoftwareBVHHardTriangleLimit = 250000;

	struct FDDGISoftwareMaterialData
	{
		FVector EmissiveRadiance = FVector::ZeroVector;
		FVector BaseColor = FVector(0.18f, 0.18f, 0.18f);
		bool bUsesDefaultBaseColor = true;
		bool bUsesVertexColor = false;
		bool bBlocksDDGI = true;
		bool bIsMasked = false;
	};

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

	bool HasNegativeTransformDeterminant(const FTransform& Transform)
	{
		const FVector Scale = Transform.GetScale3D();
		// Rotation has a positive determinant. The sign of an FTransform's
		// linear determinant is therefore the parity of its negative scale axes.
		return ((Scale.X < 0.0f) != (Scale.Y < 0.0f)) != (Scale.Z < 0.0f);
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

bool DDGISoftwareBVH::BuildStaticWorld(
	UWorld* World,
	int32 MaxTriangleCount,
	TResourceArray<FVector4>& OutPackedNodes,
	TResourceArray<FVector4>& OutPackedTriangles,
	FVector& OutWorldOrigin,
	FDDGISoftwareBVHBuildStats& OutStats)
{
	// This builder reads UWorld, component, mesh render-data, and material UObjects.
	// Keep the entire snapshot operation on the Game Thread; do not add worker-thread calls.
	check(IsInGameThread());

	OutPackedNodes.Reset();
	OutPackedTriangles.Reset();
	OutWorldOrigin = FVector::ZeroVector;
	OutStats = FDDGISoftwareBVHBuildStats();

	if (!World || MaxTriangleCount <= 0)
	{
		return false;
	}

	// Keep CPU build memory and GPU structured-buffer sizes bounded even if
	// an external caller supplies an extreme Blueprint/config value.
	MaxTriangleCount = FMath::Min(MaxTriangleCount, GDDGISoftwareBVHHardTriangleLimit);

	// Reserve up to 25% of the global triangle budget for eligible BSP geometry.
	// Scale the reservation to the actual opaque BSP polygon count, so a small
	// BSP world does not unnecessarily reduce the static-mesh budget.
	int64 BSPCandidateTriangleCount = 0;
	for (ULevel* Level : World->GetLevels())
	{
		if (!IsValid(Level)) continue;
		for (UModelComponent* ModelComponent : Level->ModelComponents)
		{
			if (!IsValid(ModelComponent) || !ModelComponent->IsVisible() || ModelComponent->bHiddenInGame) continue;
			UModel* Model = ModelComponent->GetModel();
			if (!Model) continue;
			for (const FModelElement& Element : ModelComponent->GetElements())
			{
				if (!Element.Material || Element.Material->GetBlendMode() != BLEND_Opaque) continue;
				for (uint16 NodeIndex : Element.Nodes)
				{
					if (!Model->Nodes.IsValidIndex(NodeIndex)) continue;
					const FBspNode& Node = Model->Nodes[NodeIndex];
					if (Node.NumVertices >= 3 && Model->Surfs.IsValidIndex(Node.iSurf) &&
						(Model->Surfs[Node.iSurf].PolyFlags & PF_Invisible) == 0)
					{
						BSPCandidateTriangleCount = FMath::Min<int64>(
							static_cast<int64>(MaxTriangleCount),
							BSPCandidateTriangleCount + static_cast<int64>(Node.NumVertices - 2));
					}
				}
			}
		}
	}
	const int32 BSPTriangleReservation = static_cast<int32>(FMath::Min<int64>(
		static_cast<int64>(MaxTriangleCount / 4), BSPCandidateTriangleCount));
	const int32 StaticMeshTriangleBudget = MaxTriangleCount - BSPTriangleReservation;
	bool bStaticMeshBudgetReached = false;
	TArray<FDDGISoftwareTriangle> Triangles;
	Triangles.Reserve(FMath::Min(MaxTriangleCount, 65536));
	// Material interfaces are often shared by many static-mesh components.
	// Cache parameter evaluation for this build only; no UObject pointer survives it.
	TMap<const UMaterialInterface*, FDDGISoftwareMaterialData> MaterialDataCache;
	bool bHasWorldOrigin = false;
	bool bTriangleLimitReached = false;

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor) || Actor->IsHidden())
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

			// The BVH represents geometry that can contribute to runtime rendering.
			// Exclude editor-hidden/invisible components and components hidden in game.
			if (!Component->IsVisible() || Component->bHiddenInGame)
			{
				continue;
			}

			UInstancedStaticMeshComponent* InstancedComponent =
				Cast<UInstancedStaticMeshComponent>(Component);
			if (Component->Mobility == EComponentMobility::Movable)
			{
				++OutStats.SkippedMovable;
				if (InstancedComponent)
				{
					++OutStats.SkippedInstanced;
					OutStats.SkippedInstancedInstances += InstancedComponent->GetInstanceCount();
				}
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
			if (!StaticMesh->bAllowCPUAccess)
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

			TArray<FTransform> GeometryTransforms;
			if (InstancedComponent)
			{
				const int32 InstanceCount = InstancedComponent->GetInstanceCount();
				GeometryTransforms.Reserve(InstanceCount);
				for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
				{
					FTransform InstanceTransform;
					if (InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
					{
						GeometryTransforms.Add(InstanceTransform);
					}
					else
					{
						++OutStats.SkippedInvalidTriangles;
					}
				}
			}
			else
			{
				GeometryTransforms.Add(Component->GetComponentTransform());
			}
			if (GeometryTransforms.Num() == 0)
			{
				continue;
			}

			// Keep one immutable coordinate frame for this BVH. Choosing the first
			// CPU-readable static mesh instance avoids adding large absolute world
			// coordinates to local vertex offsets before packing them as float.
			if (!bHasWorldOrigin)
			{
				OutWorldOrigin = GeometryTransforms[0].GetLocation();
				bHasWorldOrigin = true;
			}
			const int32 ComponentTriangleCount = static_cast<int32>(IndexCount / 3u);
			bool bAddedAnyTriangle = false;

			TArray<FVector> MaterialEmissiveRadiance;
			MaterialEmissiveRadiance.Init(FVector::ZeroVector, Component->GetNumMaterials());

			TArray<FVector> MaterialBaseColor;
			// Materials without an explicit DDGI_BaseColor parameter use a neutral 18% diffuse fallback.
			// Optional metallic and vertex-color overrides are applied below; arbitrary material graphs and textures are not evaluated.
			MaterialBaseColor.Init(FVector(0.18f, 0.18f, 0.18f), Component->GetNumMaterials());
			// The software BVH has no transmittance representation. Only opaque and
			// masked materials may conservatively occlude; transparent blend modes must
			// not become fully opaque blockers.
			TArray<uint8> MaterialBlocksDDGI;
			MaterialBlocksDDGI.Init(1, Component->GetNumMaterials());
			TArray<uint8> MaterialIsMasked;
			MaterialIsMasked.Init(0, Component->GetNumMaterials());
			TArray<uint8> MaterialUsesVertexColor;
			MaterialUsesVertexColor.Init(0, Component->GetNumMaterials());
			const FColorVertexBuffer& ColorVertexBuffer = LOD.VertexBuffers.ColorVertexBuffer;
			const bool bHasCPUVertexColors = ColorVertexBuffer.GetVertexData() != nullptr &&
				ColorVertexBuffer.GetNumVertices() >= VertexCount;
			const FMaterialParameterInfo BaseColorParameterInfo(FName(TEXT("DDGI_BaseColor")));
			const FMaterialParameterInfo EmissiveParameterInfo(FName(TEXT("DDGI_EmissiveRadiance")));
			for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
			{
				UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
				if (!Material)
				{
					continue;
				}

				FDDGISoftwareMaterialData* CachedData = MaterialDataCache.Find(Material);
				if (!CachedData)
				{
					FDDGISoftwareMaterialData NewData;
					const EBlendMode BlendMode = Material->GetBlendMode();
					NewData.bBlocksDDGI = BlendMode == BLEND_Opaque;
					NewData.bIsMasked = BlendMode == BLEND_Masked;
					FLinearColor EmissiveColor = FLinearColor::Black;
					if (Material->GetVectorParameterValue(EmissiveParameterInfo, EmissiveColor))
					{
						NewData.EmissiveRadiance = FVector(
							FMath::Max(0.0f, EmissiveColor.R),
							FMath::Max(0.0f, EmissiveColor.G),
							FMath::Max(0.0f, EmissiveColor.B));
					}

					FLinearColor BaseColorValue = FLinearColor::White;
					NewData.bUsesDefaultBaseColor = !Material->GetVectorParameterValue(BaseColorParameterInfo, BaseColorValue);
					float MetallicValue = 0.0f;
					Material->GetScalarParameterValue(FMaterialParameterInfo(FName(TEXT("DDGI_Metallic"))), MetallicValue);
					const float DiffuseEnergy = 1.0f - FMath::Clamp(MetallicValue, 0.0f, 1.0f);
					float UseVertexColorValue = 0.0f;
					Material->GetScalarParameterValue(FMaterialParameterInfo(FName(TEXT("DDGI_UseVertexColor"))), UseVertexColorValue);
					NewData.bUsesVertexColor = UseVertexColorValue > 0.5f;
					if (!NewData.bUsesDefaultBaseColor)
					{
						NewData.BaseColor = FVector(
							FMath::Max(0.0f, BaseColorValue.R),
							FMath::Max(0.0f, BaseColorValue.G),
							FMath::Max(0.0f, BaseColorValue.B));
					}
					NewData.BaseColor *= DiffuseEnergy;
					CachedData = &MaterialDataCache.Add(Material, NewData);
				}

				MaterialEmissiveRadiance[MaterialIndex] = CachedData->EmissiveRadiance;
				MaterialBaseColor[MaterialIndex] = CachedData->BaseColor;
				MaterialBlocksDDGI[MaterialIndex] = CachedData->bBlocksDDGI ? 1 : 0;
				MaterialIsMasked[MaterialIndex] = CachedData->bIsMasked ? 1 : 0;
				MaterialUsesVertexColor[MaterialIndex] = CachedData->bUsesVertexColor ? 1 : 0;
				if (CachedData->bUsesDefaultBaseColor)
				{
					// Count fallback material slots, not unique material objects.
					++OutStats.MaterialsUsingDefaultBaseColor;
				}
			}

			for (const FTransform& ComponentTransform : GeometryTransforms)
			{
				const FVector ComponentRelativeLocation =
					ComponentTransform.GetLocation() - OutWorldOrigin;
				int32 MaterialSectionIndex = 0;
				int64 MaterialSectionFirstTriangle = LOD.Sections.Num() > 0
					? static_cast<int64>(LOD.Sections[0].FirstIndex / 3u) : MAX_int64;
				int64 MaterialSectionEndTriangle = LOD.Sections.Num() > 0
					? MaterialSectionFirstTriangle + static_cast<int64>(LOD.Sections[0].NumTriangles) : MAX_int64;
				for (int32 TriangleIndex = 0; TriangleIndex < ComponentTriangleCount; ++TriangleIndex)
				{
				while (MaterialSectionIndex < LOD.Sections.Num() &&
					static_cast<int64>(TriangleIndex) >= MaterialSectionEndTriangle)
				{
					++MaterialSectionIndex;
					if (MaterialSectionIndex < LOD.Sections.Num())
					{
						MaterialSectionFirstTriangle = static_cast<int64>(LOD.Sections[MaterialSectionIndex].FirstIndex / 3u);
						MaterialSectionEndTriangle = MaterialSectionFirstTriangle +
							static_cast<int64>(LOD.Sections[MaterialSectionIndex].NumTriangles);
					}
				}
				const int32 MaterialIndex =
					MaterialSectionIndex < LOD.Sections.Num() &&
					static_cast<int64>(TriangleIndex) >= MaterialSectionFirstTriangle &&
					static_cast<int64>(TriangleIndex) < MaterialSectionEndTriangle
					? static_cast<int32>(LOD.Sections[MaterialSectionIndex].MaterialIndex)
					: INDEX_NONE;
				// Masked materials need their opacity mask evaluated per UV/texel.
				// This CPU BVH stores no UVs or material textures, so treating them
				// as opaque creates false blockers (notably foliage and fences).
				// Exclude them rather than inventing a solid surface.
				if (MaterialIsMasked.IsValidIndex(MaterialIndex) &&
					MaterialIsMasked[MaterialIndex] != 0)
				{
					++OutStats.SkippedMaskedTriangles;
					continue;
				}
				if (MaterialBlocksDDGI.IsValidIndex(MaterialIndex) &&
					MaterialBlocksDDGI[MaterialIndex] == 0)
				{
					continue;
				}

				const uint32 I0 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 0);
				const uint32 I1 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 1);
				const uint32 I2 = LOD.IndexBuffer.GetIndex(TriangleIndex * 3 + 2);
				if (I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				// Transform only the local offset, then add the component translation
				// relative to the BVH origin. Do not form large absolute world positions.
				const FVector A = ComponentTransform.TransformVector(FVector(PositionBuffer.VertexPosition(I0))) + ComponentRelativeLocation;
				FVector B = ComponentTransform.TransformVector(FVector(PositionBuffer.VertexPosition(I1))) + ComponentRelativeLocation;
				FVector C = ComponentTransform.TransformVector(FVector(PositionBuffer.VertexPosition(I2))) + ComponentRelativeLocation;
				// A negative scale reverses triangle winding. Restore the source mesh's
				// front-face orientation so the shade pass's back-face test remains valid.
				if (HasNegativeTransformDeterminant(ComponentTransform))
				{
					Swap(B, C);
				}
				if (!IsFiniteVector(A) || !IsFiniteVector(B) || !IsFiniteVector(C))
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				// Reject zero-area and near-collinear triangles using a scale-
				// invariant test. Normalize edge vectors before the cross product
				// so the degeneracy check does not depend on mesh size or overflow
				// merely because the triangle is very large.
				const FVector EdgeAB = B - A;
				const FVector EdgeAC = C - A;
				const FVector EdgeBC = C - B;
				const float EdgeScale = FMath::Max(
					FMath::Max(
						FMath::Max(FMath::Abs(EdgeAB.X), FMath::Abs(EdgeAB.Y)),
						FMath::Abs(EdgeAB.Z)),
					FMath::Max(
						FMath::Max(
							FMath::Max(FMath::Abs(EdgeAC.X), FMath::Abs(EdgeAC.Y)),
							FMath::Abs(EdgeAC.Z)),
						FMath::Max(
							FMath::Max(FMath::Abs(EdgeBC.X), FMath::Abs(EdgeBC.Y)),
							FMath::Abs(EdgeBC.Z))));
				if (!FMath::IsFinite(EdgeScale) || EdgeScale <= 0.0f)
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				const FVector ScaledCross = FVector::CrossProduct(
					EdgeAB / EdgeScale,
					EdgeAC / EdgeScale);
				if (!IsFiniteVector(ScaledCross) || ScaledCross.SizeSquared() <= 1.0e-12f)
				{
					++OutStats.SkippedInvalidTriangles;
					continue;
				}

				// Only flag truncation after finding one more eligible triangle.
				if (Triangles.Num() >= StaticMeshTriangleBudget)
				{
					bStaticMeshBudgetReached = true;
					break;
				}

				FDDGISoftwareTriangle& Triangle = Triangles.AddDefaulted_GetRef();
				Triangle.OriginalIndex = Triangles.Num() - 1;
				Triangle.A = A;
				Triangle.B = B;
				Triangle.C = C;
				// Divide before adding so the centroid does not overflow merely because
				// the intermediate sum of three finite world positions exceeds float range.
				Triangle.Centroid = A / 3.0f + B / 3.0f + C / 3.0f;
				if (MaterialEmissiveRadiance.IsValidIndex(MaterialIndex))
				{
					Triangle.EmissiveRadiance = MaterialEmissiveRadiance[MaterialIndex];
				}

				if (MaterialBaseColor.IsValidIndex(MaterialIndex))
				{
					Triangle.BaseColor = MaterialBaseColor[MaterialIndex];
				}
				// The software BVH stores one diffuse value per triangle. When explicitly
				// requested by DDGI_UseVertexColor, approximate the interpolated vertex
				// color at the triangle centroid. This avoids changing the GPU BVH stride.
				if (MaterialUsesVertexColor.IsValidIndex(MaterialIndex) &&
					MaterialUsesVertexColor[MaterialIndex] != 0 && bHasCPUVertexColors)
				{
					const FLinearColor ColorA(ColorVertexBuffer.VertexColor(I0));
					const FLinearColor ColorB(ColorVertexBuffer.VertexColor(I1));
					const FLinearColor ColorC(ColorVertexBuffer.VertexColor(I2));
					const FVector AverageVertexColor(
						FMath::Max(0.0f, (ColorA.R + ColorB.R + ColorC.R) / 3.0f),
						FMath::Max(0.0f, (ColorA.G + ColorB.G + ColorC.G) / 3.0f),
						FMath::Max(0.0f, (ColorA.B + ColorB.B + ColorC.B) / 3.0f));
					Triangle.BaseColor *= AverageVertexColor;
				}
				Triangle.BoundsMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
				Triangle.BoundsMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, A);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, B);
				IncludePoint(Triangle.BoundsMin, Triangle.BoundsMax, C);
				bAddedAnyTriangle = true;
				}
				if (bStaticMeshBudgetReached)
				{
					break;
				}
			}

			if (bAddedAnyTriangle)
			{
				++OutStats.IncludedComponents;
			}

			if (bStaticMeshBudgetReached)
			{
				break;
			}
		}

		if (bStaticMeshBudgetReached)
		{
			break;
		}
	}

	// BSP polygons are convex; triangulate each visible opaque polygon as a fan.
	// The CPU BVH has no opacity-mask/transmittance representation, so masked and
	// blended materials are excluded just like their static-mesh equivalents.
	for (ULevel* Level : World->GetLevels())
	{
		if (!IsValid(Level)) continue;
		for (UModelComponent* ModelComponent : Level->ModelComponents)
		{
			if (!IsValid(ModelComponent) || !ModelComponent->IsVisible() || ModelComponent->bHiddenInGame) continue;
			UModel* Model = ModelComponent->GetModel();
			if (!Model) continue;
			const FTransform ComponentTransform = ModelComponent->GetComponentTransform();
			if (!bHasWorldOrigin)
			{
				OutWorldOrigin = ComponentTransform.GetLocation();
				bHasWorldOrigin = true;
			}
			const FVector ComponentRelativeLocation = ComponentTransform.GetLocation() - OutWorldOrigin;
			bool bAddedBSPTriangle = false;
			const TIndirectArray<FModelElement>& Elements = ModelComponent->GetElements();
			for (const FModelElement& Element : Elements)
			{
				UMaterialInterface* Material = Element.Material;
				if (!Material) continue;
				FDDGISoftwareMaterialData* CachedData = MaterialDataCache.Find(Material);
				if (!CachedData)
				{
					FDDGISoftwareMaterialData NewData;
					const EBlendMode BlendMode = Material->GetBlendMode();
					NewData.bBlocksDDGI = BlendMode == BLEND_Opaque;
					NewData.bIsMasked = BlendMode == BLEND_Masked;
					FLinearColor EmissiveColor = FLinearColor::Black;
					Material->GetVectorParameterValue(FMaterialParameterInfo(FName(TEXT("DDGI_EmissiveRadiance"))), EmissiveColor);
					NewData.EmissiveRadiance = FVector(FMath::Max(0.0f,EmissiveColor.R),FMath::Max(0.0f,EmissiveColor.G),FMath::Max(0.0f,EmissiveColor.B));
					FLinearColor BaseColorValue = FLinearColor::White;
					NewData.bUsesDefaultBaseColor = !Material->GetVectorParameterValue(FMaterialParameterInfo(FName(TEXT("DDGI_BaseColor"))), BaseColorValue);
					float MetallicValue = 0.0f;
					Material->GetScalarParameterValue(FMaterialParameterInfo(FName(TEXT("DDGI_Metallic"))), MetallicValue);
					const float DiffuseEnergy = 1.0f - FMath::Clamp(MetallicValue, 0.0f, 1.0f);
					if (!NewData.bUsesDefaultBaseColor) NewData.BaseColor = FVector(FMath::Max(0.0f,BaseColorValue.R),FMath::Max(0.0f,BaseColorValue.G),FMath::Max(0.0f,BaseColorValue.B));
					NewData.BaseColor *= DiffuseEnergy;
					CachedData = &MaterialDataCache.Add(Material, NewData);
				}
				if (!CachedData->bBlocksDDGI)
				{
					if (CachedData->bIsMasked)
					{
						for (uint16 NodeIndex : Element.Nodes)
							if (Model->Nodes.IsValidIndex(NodeIndex) && Model->Nodes[NodeIndex].NumVertices >= 3)
								OutStats.SkippedMaskedTriangles += Model->Nodes[NodeIndex].NumVertices - 2;
					}
					continue;
				}
				if (CachedData->bUsesDefaultBaseColor) ++OutStats.MaterialsUsingDefaultBaseColor;
				for (uint16 NodeIndex : Element.Nodes)
				{
					if (!Model->Nodes.IsValidIndex(NodeIndex)) { ++OutStats.SkippedInvalidTriangles; continue; }
					const FBspNode& Node = Model->Nodes[NodeIndex];
					if (Node.NumVertices < 3 || !Model->Surfs.IsValidIndex(Node.iSurf) || (Model->Surfs[Node.iSurf].PolyFlags & PF_Invisible) != 0) continue;
					for (int32 FanIndex = 1; FanIndex < Node.NumVertices - 1; ++FanIndex)
					{
						const int32 CornerIndices[3] = { 0, FanIndex, FanIndex + 1 };
						FVector P[3];
						bool bValid = true;
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							const int64 VertIndex = static_cast<int64>(Node.iVertPool) + CornerIndices[Corner];
							if (VertIndex < 0 || VertIndex >= Model->Verts.Num()) { bValid = false; break; }
							const int32 PointIndex = Model->Verts[static_cast<int32>(VertIndex)].pVertex;
							if (!Model->Points.IsValidIndex(PointIndex)) { bValid = false; break; }
							P[Corner] = ComponentTransform.TransformVector(Model->Points[PointIndex]) + ComponentRelativeLocation;
							if (!IsFiniteVector(P[Corner])) { bValid = false; break; }
						}
						if (!bValid) { ++OutStats.SkippedInvalidTriangles; continue; }
						// BSP components can also carry negative scale; keep their
						// transformed winding consistent with the static-mesh path.
						if (HasNegativeTransformDeterminant(ComponentTransform))
						{
							Swap(P[1], P[2]);
						}
						const FVector E0 = P[1] - P[0];
						const FVector E1 = P[2] - P[0];
						const float Scale = FMath::Max(FMath::Max(FMath::Max(FMath::Abs(E0.X),FMath::Abs(E0.Y)),FMath::Abs(E0.Z)),FMath::Max(FMath::Max(FMath::Abs(E1.X),FMath::Abs(E1.Y)),FMath::Abs(E1.Z)));
						if (!FMath::IsFinite(Scale) || Scale <= 0.0f || FVector::CrossProduct(E0 / Scale, E1 / Scale).SizeSquared() <= 1.0e-12f) { ++OutStats.SkippedInvalidTriangles; continue; }
						// Match the static-mesh path: only report truncation after finding
						// one additional finite, non-degenerate, blocking triangle.
						if (Triangles.Num() >= MaxTriangleCount) { bTriangleLimitReached = true; break; }
						FDDGISoftwareTriangle& Triangle = Triangles.AddDefaulted_GetRef();
						Triangle.OriginalIndex = Triangles.Num() - 1;
						Triangle.A = P[0]; Triangle.B = P[1]; Triangle.C = P[2];
						Triangle.Centroid = P[0] / 3.0f + P[1] / 3.0f + P[2] / 3.0f;
						Triangle.EmissiveRadiance = CachedData->EmissiveRadiance;
						Triangle.BaseColor = CachedData->BaseColor;
						Triangle.BoundsMin = FVector(FLT_MAX,FLT_MAX,FLT_MAX);
						Triangle.BoundsMax = FVector(-FLT_MAX,-FLT_MAX,-FLT_MAX);
						IncludePoint(Triangle.BoundsMin,Triangle.BoundsMax,P[0]); IncludePoint(Triangle.BoundsMin,Triangle.BoundsMax,P[1]); IncludePoint(Triangle.BoundsMin,Triangle.BoundsMax,P[2]);
						bAddedBSPTriangle = true;
					}
					if (bTriangleLimitReached) break;
				}
				if (bTriangleLimitReached) break;
			}
			if (bAddedBSPTriangle) ++OutStats.IncludedBSPComponents;
			if (bTriangleLimitReached) break;
		}
		if (bTriangleLimitReached) break;
	}

	// The cap is only considered reached when an additional valid, blocking
	// triangle exists beyond the accepted-triangle capacity.
	// The static-mesh share can be exhausted before the global cap when BSP has a protected reservation.
	// Report that truncation too, even if invalid BSP polygons leave part of the reservation unused.
	OutStats.bTriangleLimitReached = bTriangleLimitReached || bStaticMeshBudgetReached;
	OutStats.TriangleCount = Triangles.Num();
	if (Triangles.Num() == 0)
	{
		// A valid empty snapshot is different from a failed build.
		return true;
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
		return false;
	}
	for (const FDDGISoftwareTriangle& Triangle : Triangles)
	{
		if (!IsValidTriangleForPacking(Triangle))
		{
			UE_LOG(LogTemp, Error, TEXT("DDGI: Software BVH contains non-finite or invalid triangle attributes; geometry upload was skipped."));
			OutStats.TriangleCount = 0;
			return false;
		}
	}

	BuildNode(Triangles, 0, Triangles.Num(), Nodes);
	if (Nodes.Num() >= MaxExactFloatInteger || !ValidateBVH(Nodes, Triangles.Num()))
	{
		UE_LOG(LogTemp, Error, TEXT("DDGI: Software BVH validation failed; geometry upload was skipped."));
		OutStats.TriangleCount = 0;
		return false;
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

	return true;
}
