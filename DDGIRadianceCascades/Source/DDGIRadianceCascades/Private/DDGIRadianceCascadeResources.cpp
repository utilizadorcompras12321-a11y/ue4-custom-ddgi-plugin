#include "DDGIRadianceCascadeResources.h"
#include "DDGIMasterSubsystem.h"

#include "RHICommandList.h"
#include "RenderTargetPool.h"
#include "RenderGraph.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneInterface.h"
#include "Containers/ResourceArray.h"

namespace
{
	FORCEINLINE uint32 DDGIHashProbeCell(const FIntVector& Cell)
	{
		uint32 Hash = static_cast<uint32>(Cell.X) * 0x8DA6B343u;
		Hash ^= static_cast<uint32>(Cell.Y) * 0xD8163841u;
		Hash ^= static_cast<uint32>(Cell.Z) * 0xCB1AB31Fu;
		Hash ^= Hash >> 16;
		Hash *= 0x7FEB352Du;
		Hash ^= Hash >> 15;
		Hash *= 0x846CA68Bu;
		Hash ^= Hash >> 16;
		return Hash;
	}
}

namespace
{
	static const int32 GDDGIRadianceCascadeTextureWidth = 4096;
	static const int32 GDDGIRadianceCascadeMaxTextureDimension = 16384;

	struct FDDGIRadianceCascadeGPUStorage
	{
		TRefCountPtr<IPooledRenderTarget> Directions;
		TRefCountPtr<IPooledRenderTarget> Radiance;
		TRefCountPtr<IPooledRenderTarget> RadianceScratch;
		TRefCountPtr<IPooledRenderTarget> HitDistance;
		TRefCountPtr<IPooledRenderTarget> RayOrigins;
		TRefCountPtr<IPooledRenderTarget> ProbePositions;
		FBufferRHIRef ProbeSpatialHash;
		FShaderResourceViewRHIRef ProbeSpatialHashSRV;
		uint32 ProbeSpatialHashCapacity = 0;
		uint32 ProbeSpatialHashProbeCount = 0;
		float ProbeSpatialHashSpacing = 0.0f;
		FBufferRHIRef BVHNodes;
		FShaderResourceViewRHIRef BVHNodesSRV;
		FBufferRHIRef BVHTriangles;
		FShaderResourceViewRHIRef BVHTrianglesSRV;
		const FDDGISoftwareBVHData* BVHDataIdentity = nullptr;
		uint32 BVHNodeCount = 0;
		uint32 BVHTriangleCount = 0;
		bool bNeedsInitialization = true;
		FIntPoint Dimensions = FIntPoint::ZeroValue;
		int32 TotalRaysPerProbe = 0;
		int32 ProbeCapacity = 0;
		FIntPoint ProbePositionDimensions = FIntPoint::ZeroValue;
		int32 UploadedProbeCount = 0;

		void Release()
		{
			Directions.SafeRelease();
			Radiance.SafeRelease();
			RadianceScratch.SafeRelease();
			HitDistance.SafeRelease();
			RayOrigins.SafeRelease();
			ProbePositions.SafeRelease();
			ProbeSpatialHash.SafeRelease();
			ProbeSpatialHashSRV.SafeRelease();
			ProbeSpatialHashCapacity = 0;
			ProbeSpatialHashProbeCount = 0;
			ProbeSpatialHashSpacing = 0.0f;
			BVHNodes.SafeRelease();
			BVHNodesSRV.SafeRelease();
			BVHTriangles.SafeRelease();
			BVHTrianglesSRV.SafeRelease();
			BVHDataIdentity = nullptr;
			BVHNodeCount = 0;
			BVHTriangleCount = 0;
			Dimensions = FIntPoint::ZeroValue;
			ProbePositionDimensions = FIntPoint::ZeroValue;
			UploadedProbeCount = 0;
			TotalRaysPerProbe = 0;
			ProbeCapacity = 0;
			bNeedsInitialization = true;
		}
	};

	/** Accessed only on the render thread. */
	TMap<const FSceneInterface*, FDDGIRadianceCascadeGPUStorage> GDDGIRadianceCascadeStorage_RenderThread;
}

void DDGIUpdateRadianceCascadeResources_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    const FSceneInterface* Scene,
    const FDDGIMasterConfig& Config)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (!Scene)
    {
        return;
    }

    const int64 TotalTexels = static_cast<int64>(FMath::Max(0, Config.MaxActiveProbes)) *
        static_cast<int64>(FMath::Max(0, Config.TotalRaysPerProbe));
    const int64 Height64 = (TotalTexels + GDDGIRadianceCascadeTextureWidth - 1) /
        GDDGIRadianceCascadeTextureWidth;

    if (TotalTexels <= 0 || Height64 <= 0 || Height64 > GDDGIRadianceCascadeMaxTextureDimension)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: requested GPU storage exceeds the supported 2D texture capacity (%lld ray records). Reduce MaxActiveProbes or cascade ray counts."), TotalTexels);
        return;
    }

    const FIntPoint RequiredDimensions(GDDGIRadianceCascadeTextureWidth, static_cast<int32>(Height64));
    FDDGIRadianceCascadeGPUStorage& Storage = GDDGIRadianceCascadeStorage_RenderThread.FindOrAdd(Scene);
    const bool bRayStorageMatches =
        Storage.Dimensions == RequiredDimensions &&
        Storage.TotalRaysPerProbe == Config.TotalRaysPerProbe &&
        Storage.ProbeCapacity == Config.MaxActiveProbes &&
        Storage.Directions.IsValid() &&
        Storage.Radiance.IsValid() &&
        Storage.RadianceScratch.IsValid() &&
        Storage.HitDistance.IsValid() &&
        Storage.RayOrigins.IsValid();

    if (!bRayStorageMatches)
    {
        Storage.Release();

        // Half-float RGBA texels store direction and radiance; R32F stores hit distance.
        const FPooledRenderTargetDesc DirectionDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        const FPooledRenderTargetDesc RadianceDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        const FPooledRenderTargetDesc DistanceDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);

        GRenderTargetPool.FindFreeElement(RHICmdList, DirectionDesc, Storage.Directions,
            TEXT("DDGIRadianceCascade_Directions"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, RadianceDesc, Storage.Radiance,
            TEXT("DDGIRadianceCascade_Radiance"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, RadianceDesc, Storage.RadianceScratch,
            TEXT("DDGIRadianceCascade_RadianceScratch"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, DistanceDesc, Storage.HitDistance,
            TEXT("DDGIRadianceCascade_HitDistance"), ERenderTargetTransience::NonTransient);
        const FPooledRenderTargetDesc OriginDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        GRenderTargetPool.FindFreeElement(RHICmdList, OriginDesc, Storage.RayOrigins,
            TEXT("DDGIRadianceCascade_RayOrigins"), ERenderTargetTransience::NonTransient);

        if (!Storage.Directions.IsValid() || !Storage.Radiance.IsValid() || !Storage.RadianceScratch.IsValid() || !Storage.HitDistance.IsValid() || !Storage.RayOrigins.IsValid())
        {
            Storage.Release();
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate persistent GPU textures."));
            return;
        }

        Storage.Dimensions = RequiredDimensions;
        Storage.TotalRaysPerProbe = Config.TotalRaysPerProbe;
        Storage.ProbeCapacity = Config.MaxActiveProbes;
    }

    // Keep probe origins in a separate float4 texture. Refresh its contents on
    // every Master Actor publication, including actor movement with unchanged capacity.
    const int32 ProbeTextureWidth = 4096;
    const int32 ProbeTextureHeight = FMath::Max(1, FMath::DivideAndRoundUp(Config.MaxActiveProbes, ProbeTextureWidth));
    const FIntPoint ProbePositionDimensions(ProbeTextureWidth, ProbeTextureHeight);
    if (Storage.ProbePositionDimensions != ProbePositionDimensions || !Storage.ProbePositions.IsValid())
    {
        Storage.ProbePositions.SafeRelease();
        const FPooledRenderTargetDesc ProbePositionDesc = FPooledRenderTargetDesc::Create2DDesc(
            ProbePositionDimensions, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource, false);
        GRenderTargetPool.FindFreeElement(RHICmdList, ProbePositionDesc, Storage.ProbePositions,
            TEXT("DDGIRadianceCascade_ProbePositions"), ERenderTargetTransience::NonTransient);
        if (!Storage.ProbePositions.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate sparse probe-position texture."));
            return;
        }
        Storage.ProbePositionDimensions = ProbePositionDimensions;
    }

    const int32 ProbeTextureTexelCount = ProbePositionDimensions.X * ProbePositionDimensions.Y;
    TArray<FVector4> UploadData;
    UploadData.SetNumZeroed(ProbeTextureTexelCount);
    const int32 ProbeCount = FMath::Min(Config.ProbePositions.Num(), Config.MaxActiveProbes);
    for (int32 ProbeIndex = 0; ProbeIndex < ProbeCount; ++ProbeIndex)
    {
        UploadData[ProbeIndex] = Config.ProbePositions[ProbeIndex];
    }

    FTexture2DRHIRef ProbePositionRHI =
        Storage.ProbePositions->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D();
    if (ProbePositionRHI.IsValid())
    {
        Storage.bNeedsInitialization = true;
        const FUpdateTextureRegion2D UpdateRegion(
            0, 0, 0, 0, ProbePositionDimensions.X, ProbePositionDimensions.Y);
        RHICmdList.UpdateTexture2D(
            ProbePositionRHI,
            0,
            UpdateRegion,
            static_cast<uint32>(ProbePositionDimensions.X * sizeof(FVector4)),
            reinterpret_cast<const uint8*>(UploadData.GetData()));
        Storage.UploadedProbeCount = ProbeCount;
    }
    else
    {
        Storage.UploadedProbeCount = 0;
        UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: sparse probe-position texture has no 2D RHI resource."));
    }


    // Build a deterministic open-addressed spatial hash for sparse probe cells.
    // Probe positions are generated on a regular lattice around ProbeGridOrigin,
    // so integer cell coordinates remain stable when the Master Actor translates.
    const uint32 ProbeCountForHash = static_cast<uint32>(FMath::Min(Config.ProbePositions.Num(), Config.MaxActiveProbes));
    const float SafeProbeSpacing = FMath::Max(1.0f, Config.ProbeSpacing);
    uint32 RequiredHashCapacity = 2u;
    while (RequiredHashCapacity < FMath::Max(2u, ProbeCountForHash * 2u) && RequiredHashCapacity < (1u << 30))
    {
        RequiredHashCapacity <<= 1u;
    }
    const bool bProbeHashMatches =
        Storage.ProbeSpatialHash.IsValid() &&
        Storage.ProbeSpatialHashSRV.IsValid() &&
        Storage.ProbeSpatialHashCapacity == RequiredHashCapacity &&
        Storage.ProbeSpatialHashProbeCount == ProbeCountForHash &&
        FMath::IsNearlyEqual(Storage.ProbeSpatialHashSpacing, SafeProbeSpacing, 1.0e-4f);
    if (!bProbeHashMatches)
    {
        Storage.ProbeSpatialHash.SafeRelease();
        Storage.ProbeSpatialHashSRV.SafeRelease();

        TResourceArray<FVector4> HashEntries;
        HashEntries.SetNumUninitialized(static_cast<int32>(RequiredHashCapacity));
        for (FVector4& Entry : HashEntries)
        {
            Entry = FVector4(0.0f, 0.0f, 0.0f, -1.0f);
        }

        bool bHashBuildSucceeded = true;
        for (uint32 ProbeIndex = 0u; ProbeIndex < ProbeCountForHash; ++ProbeIndex)
        {
            const FVector4& Position = Config.ProbePositions[static_cast<int32>(ProbeIndex)];
            const FVector Relative = FVector(Position.X, Position.Y, Position.Z) - Config.ProbeGridOrigin;
            const FIntVector Cell(
                FMath::RoundToInt(Relative.X / SafeProbeSpacing),
                FMath::RoundToInt(Relative.Y / SafeProbeSpacing),
                FMath::RoundToInt(Relative.Z / SafeProbeSpacing));
            uint32 Slot = DDGIHashProbeCell(Cell) & (RequiredHashCapacity - 1u);
            bool bInserted = false;
            for (uint32 ProbeAttempt = 0u; ProbeAttempt < RequiredHashCapacity; ++ProbeAttempt)
            {
                FVector4& Entry = HashEntries[static_cast<int32>(Slot)];
                if (Entry.W < 0.0f)
                {
                    Entry = FVector4(
                        static_cast<float>(Cell.X),
                        static_cast<float>(Cell.Y),
                        static_cast<float>(Cell.Z),
                        static_cast<float>(ProbeIndex));
                    bInserted = true;
                    break;
                }
                if (FMath::RoundToInt(Entry.X) == Cell.X &&
                    FMath::RoundToInt(Entry.Y) == Cell.Y &&
                    FMath::RoundToInt(Entry.Z) == Cell.Z)
                {
                    bHashBuildSucceeded = false;
                    break;
                }
                Slot = (Slot + 1u) & (RequiredHashCapacity - 1u);
            }
            if (!bInserted)
            {
                bHashBuildSucceeded = false;
                break;
            }
        }
        if (bHashBuildSucceeded)
        {
            FRHIResourceCreateInfo HashCreateInfo(TEXT("DDGIRadianceCascade_ProbeSpatialHash"));
            HashCreateInfo.ResourceArray = &HashEntries;
            Storage.ProbeSpatialHash = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(HashEntries.GetResourceDataSize()),
                BUF_ShaderResource,
                HashCreateInfo);
            if (Storage.ProbeSpatialHash.IsValid())
            {
                Storage.ProbeSpatialHashSRV = RHICreateShaderResourceView(Storage.ProbeSpatialHash);
            }
        }
        if (!Storage.ProbeSpatialHash.IsValid() || !Storage.ProbeSpatialHashSRV.IsValid())
        {
            Storage.ProbeSpatialHash.SafeRelease();
            Storage.ProbeSpatialHashSRV.SafeRelease();
            Storage.ProbeSpatialHashCapacity = 0;
            Storage.ProbeSpatialHashProbeCount = 0;
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to build sparse probe spatial hash."));
        }
        else
        {
            Storage.ProbeSpatialHashCapacity = RequiredHashCapacity;
            Storage.ProbeSpatialHashProbeCount = ProbeCountForHash;
            Storage.ProbeSpatialHashSpacing = SafeProbeSpacing;
        }
    }

    // Upload the immutable CPU BVH once per snapshot. Master Actor movement
    // republishes probe origins but keeps the same shared BVH pointer.
    const FDDGISoftwareBVHData* CurrentBVH = Config.SoftwareBVH.Get();
    if (Storage.BVHDataIdentity != CurrentBVH || !Storage.BVHNodesSRV.IsValid() || !Storage.BVHTrianglesSRV.IsValid())
    {
        Storage.BVHNodes.SafeRelease();
        Storage.BVHNodesSRV.SafeRelease();
        Storage.BVHTriangles.SafeRelease();
        Storage.BVHTrianglesSRV.SafeRelease();
        Storage.BVHNodeCount = 0;
        Storage.BVHTriangleCount = 0;
        Storage.BVHDataIdentity = CurrentBVH;

        if (CurrentBVH && CurrentBVH->NodeCount > 0 && CurrentBVH->TriangleCount > 0 &&
            CurrentBVH->PackedNodes.Num() >= CurrentBVH->NodeCount * 3 &&
            CurrentBVH->PackedTriangles.Num() >= CurrentBVH->TriangleCount * 4)
        {
            TResourceArray<FVector4> NodeData;
            NodeData.Append(CurrentBVH->PackedNodes);
            FRHIResourceCreateInfo NodeCreateInfo(TEXT("DDGIRadianceCascade_SoftwareBVHNodes"));
            NodeCreateInfo.ResourceArray = &NodeData;
            Storage.BVHNodes = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(NodeData.GetResourceDataSize()),
                BUF_ShaderResource,
                NodeCreateInfo);

            TResourceArray<FVector4> TriangleData;
            TriangleData.Append(CurrentBVH->PackedTriangles);
            FRHIResourceCreateInfo TriangleCreateInfo(TEXT("DDGIRadianceCascade_SoftwareBVHTriangles"));
            TriangleCreateInfo.ResourceArray = &TriangleData;
            Storage.BVHTriangles = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(TriangleData.GetResourceDataSize()),
                BUF_ShaderResource,
                TriangleCreateInfo);

            if (Storage.BVHNodes.IsValid() && Storage.BVHTriangles.IsValid())
            {
                Storage.BVHNodesSRV = RHICreateShaderResourceView(Storage.BVHNodes);
                Storage.BVHTrianglesSRV = RHICreateShaderResourceView(Storage.BVHTriangles);
                if (Storage.BVHNodesSRV.IsValid() && Storage.BVHTrianglesSRV.IsValid())
                {
                    Storage.BVHNodeCount = static_cast<uint32>(CurrentBVH->NodeCount);
                    Storage.BVHTriangleCount = static_cast<uint32>(CurrentBVH->TriangleCount);
                }
                else
                {
                    Storage.BVHNodes.SafeRelease();
                    Storage.BVHNodesSRV.SafeRelease();
                    Storage.BVHTriangles.SafeRelease();
                    Storage.BVHTrianglesSRV.SafeRelease();
                    UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create BVH structured-buffer SRVs."));
                }
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate BVH structured buffers."));
            }
        }
        else
        {
            // Keep valid, zero-count SRVs so the trace shader can still run its
            // background-only path when the world contains no supported triangles.
            TResourceArray<FVector4> EmptyNodeData;
            EmptyNodeData.AddZeroed(3);
            FRHIResourceCreateInfo EmptyNodeCreateInfo(TEXT("DDGIRadianceCascade_EmptyBVHNodes"));
            EmptyNodeCreateInfo.ResourceArray = &EmptyNodeData;
            Storage.BVHNodes = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(EmptyNodeData.GetResourceDataSize()),
                BUF_ShaderResource,
                EmptyNodeCreateInfo);

            TResourceArray<FVector4> EmptyTriangleData;
            EmptyTriangleData.AddZeroed(4);
            FRHIResourceCreateInfo EmptyTriangleCreateInfo(TEXT("DDGIRadianceCascade_EmptyBVHTriangles"));
            EmptyTriangleCreateInfo.ResourceArray = &EmptyTriangleData;
            Storage.BVHTriangles = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(EmptyTriangleData.GetResourceDataSize()),
                BUF_ShaderResource,
                EmptyTriangleCreateInfo);

            if (Storage.BVHNodes.IsValid() && Storage.BVHTriangles.IsValid())
            {
                Storage.BVHNodesSRV = RHICreateShaderResourceView(Storage.BVHNodes);
                Storage.BVHTrianglesSRV = RHICreateShaderResourceView(Storage.BVHTriangles);
            }
            Storage.BVHNodeCount = 0;
            Storage.BVHTriangleCount = 0;
        }
    }
}

void DDGIReleaseRadianceCascadeResources_RenderThread(const FSceneInterface* Scene)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
	{
		Storage->Release();
		GDDGIRadianceCascadeStorage_RenderThread.Remove(Scene);
	}
}

bool DDGIGetRadianceCascadeInitializationTextures_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTexture*& OutDirections,
    FRDGTexture*& OutRadiance,
    FRDGTexture*& OutHitDistance,
    FRDGTexture*& OutRayOrigins)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutDirections = nullptr;
    OutRadiance = nullptr;
    OutHitDistance = nullptr;
    OutRayOrigins = nullptr;

    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->bNeedsInitialization ||
        !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() || !Storage->HitDistance.IsValid() || !Storage->RayOrigins.IsValid())
    {
        return false;
    }

    OutDirections = GraphBuilder.RegisterExternalTexture(Storage->Directions);
    OutRadiance = GraphBuilder.RegisterExternalTexture(Storage->Radiance);
    OutHitDistance = GraphBuilder.RegisterExternalTexture(Storage->HitDistance);
    OutRayOrigins = GraphBuilder.RegisterExternalTexture(Storage->RayOrigins);
    return OutDirections && OutRadiance && OutHitDistance && OutRayOrigins;
}



bool DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
	const FSceneInterface* Scene,
	FTexture2DRHIRef& OutDirections,
	FTexture2DRHIRef& OutRadiance,
	FTexture2DRHIRef& OutHitDistance,
	FTexture2DRHIRef& OutRayOrigins,
	FTexture2DRHIRef& OutProbePositions,
	uint32& OutProbeCount)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	OutDirections.SafeRelease();
	OutRadiance.SafeRelease();
	OutHitDistance.SafeRelease();
	OutRayOrigins.SafeRelease();
	OutProbePositions.SafeRelease();
	OutProbeCount = 0;

	FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
	if (!Storage || !Storage->bNeedsInitialization ||
		!Storage->Directions.IsValid() || !Storage->Radiance.IsValid() ||
		!Storage->HitDistance.IsValid() || !Storage->RayOrigins.IsValid() ||
		!Storage->ProbePositions.IsValid() || Storage->UploadedProbeCount <= 0)
	{
		return false;
	}

	const FSceneRenderTargetItem& DirectionsItem = Storage->Directions->GetRenderTargetItem();
	const FSceneRenderTargetItem& RadianceItem = Storage->Radiance->GetRenderTargetItem();
	const FSceneRenderTargetItem& HitDistanceItem = Storage->HitDistance->GetRenderTargetItem();
	const FSceneRenderTargetItem& RayOriginsItem = Storage->RayOrigins->GetRenderTargetItem();
	const FSceneRenderTargetItem& ProbePositionsItem = Storage->ProbePositions->GetRenderTargetItem();

	if (!DirectionsItem.ShaderResourceTexture.IsValid() ||
		!RadianceItem.ShaderResourceTexture.IsValid() ||
		!HitDistanceItem.ShaderResourceTexture.IsValid() ||
		!RayOriginsItem.ShaderResourceTexture.IsValid() ||
		!ProbePositionsItem.ShaderResourceTexture.IsValid())
	{
		return false;
	}

	OutDirections = DirectionsItem.ShaderResourceTexture->GetTexture2D();
	OutRadiance = RadianceItem.ShaderResourceTexture->GetTexture2D();
	OutHitDistance = HitDistanceItem.ShaderResourceTexture->GetTexture2D();
	OutRayOrigins = RayOriginsItem.ShaderResourceTexture->GetTexture2D();
	OutProbePositions = ProbePositionsItem.ShaderResourceTexture->GetTexture2D();
	OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);

	return OutDirections.IsValid() && OutRadiance.IsValid() &&
		OutHitDistance.IsValid() && OutRayOrigins.IsValid() && OutProbePositions.IsValid();
}



bool DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
	const FSceneInterface* Scene,
	FTexture2DRHIRef& OutDirections,
	FTexture2DRHIRef& OutRadiance,
	FTexture2DRHIRef& OutHitDistance,
	FTexture2DRHIRef& OutRayOrigins,
	uint32& OutProbeCount,
	uint32& OutTotalRaysPerProbe)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	OutDirections.SafeRelease();
	OutRadiance.SafeRelease();
	OutHitDistance.SafeRelease();
	OutRayOrigins.SafeRelease();
	OutProbeCount = 0;
	OutTotalRaysPerProbe = 0;

	const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
	if (!Storage || !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() ||
		!Storage->HitDistance.IsValid() || !Storage->RayOrigins.IsValid() ||
		Storage->UploadedProbeCount <= 0 || Storage->TotalRaysPerProbe <= 0)
	{
		return false;
	}

	const FSceneRenderTargetItem& DirectionsItem = Storage->Directions->GetRenderTargetItem();
	const FSceneRenderTargetItem& RadianceItem = Storage->Radiance->GetRenderTargetItem();
	const FSceneRenderTargetItem& HitDistanceItem = Storage->HitDistance->GetRenderTargetItem();
	const FSceneRenderTargetItem& RayOriginsItem = Storage->RayOrigins->GetRenderTargetItem();
	if (!DirectionsItem.ShaderResourceTexture.IsValid() ||
		!RadianceItem.ShaderResourceTexture.IsValid() ||
		!HitDistanceItem.ShaderResourceTexture.IsValid() ||
		!RayOriginsItem.ShaderResourceTexture.IsValid())
	{
		return false;
	}

	OutDirections = DirectionsItem.ShaderResourceTexture->GetTexture2D();
	OutRadiance = RadianceItem.ShaderResourceTexture->GetTexture2D();
	OutHitDistance = HitDistanceItem.ShaderResourceTexture->GetTexture2D();
	OutRayOrigins = RayOriginsItem.ShaderResourceTexture->GetTexture2D();
	OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
	OutTotalRaysPerProbe = static_cast<uint32>(Storage->TotalRaysPerProbe);
	return OutDirections.IsValid() && OutRadiance.IsValid() &&
		OutHitDistance.IsValid() && OutRayOrigins.IsValid();
}

bool DDGIGetRadianceCascadeScratchTexture_RenderThread(
	const FSceneInterface* Scene,
	FTexture2DRHIRef& OutRadianceScratch)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	OutRadianceScratch.SafeRelease();
	const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
	if (!Storage || !Storage->RadianceScratch.IsValid())
	{
		return false;
	}
	const FSceneRenderTargetItem& ScratchItem = Storage->RadianceScratch->GetRenderTargetItem();
	if (!ScratchItem.ShaderResourceTexture.IsValid())
	{
		return false;
	}
	OutRadianceScratch = ScratchItem.ShaderResourceTexture->GetTexture2D();
	return OutRadianceScratch.IsValid();
}

void DDGISwapRadianceCascadeRadianceBuffers_RenderThread(const FSceneInterface* Scene)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
	{
		Swap(Storage->Radiance, Storage->RadianceScratch);
	}
}

bool DDGIGetSoftwareBVHResources_RenderThread(
	const FSceneInterface* Scene,
	FShaderResourceViewRHIRef& OutNodesSRV,
	FShaderResourceViewRHIRef& OutTrianglesSRV,
	uint32& OutNodeCount,
	uint32& OutTriangleCount)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	OutNodesSRV.SafeRelease();
	OutTrianglesSRV.SafeRelease();
	OutNodeCount = 0;
	OutTriangleCount = 0;

	const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
	if (!Storage || !Storage->BVHNodesSRV.IsValid() || !Storage->BVHTrianglesSRV.IsValid())
	{
		return false;
	}

	OutNodesSRV = Storage->BVHNodesSRV;
	OutTrianglesSRV = Storage->BVHTrianglesSRV;
	OutNodeCount = Storage->BVHNodeCount;
	OutTriangleCount = Storage->BVHTriangleCount;
	return true;
}

bool DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutSpatialHashSRV,
    uint32& OutHashCapacity,
    float& OutProbeSpacing)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutSpatialHashSRV.SafeRelease();
    OutHashCapacity = 0;
    OutProbeSpacing = 0.0f;

    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->ProbeSpatialHashSRV.IsValid() ||
        Storage->ProbeSpatialHashCapacity == 0 || Storage->UploadedProbeCount <= 0)
    {
        return false;
    }

    OutSpatialHashSRV = Storage->ProbeSpatialHashSRV;
    OutHashCapacity = Storage->ProbeSpatialHashCapacity;
    OutProbeSpacing = Storage->ProbeSpatialHashSpacing;
    return true;
}

bool DDGIGetRadianceCascadeProbePositions_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTextureRef& OutProbePositions,
    uint32& OutProbeCount)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutProbePositions = nullptr;
    OutProbeCount = 0;

    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->ProbePositions.IsValid() || Storage->UploadedProbeCount <= 0)
    {
        return false;
    }

    OutProbePositions = GraphBuilder.RegisterExternalTexture(Storage->ProbePositions);
    OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
    return OutProbePositions != nullptr;
}

void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
    {
        Storage->bNeedsInitialization = false;
    }
}
