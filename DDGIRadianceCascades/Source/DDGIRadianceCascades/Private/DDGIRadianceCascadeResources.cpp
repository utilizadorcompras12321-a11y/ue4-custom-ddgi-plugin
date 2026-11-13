#include "DDGIRadianceCascadeResources.h"
#include "DDGIMasterSubsystem.h"

#include "RHICommandList.h"
#include "RenderTargetPool.h"
#include "RenderGraph.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneInterface.h"

namespace
{
	static const int32 GDDGIRadianceCascadeTextureWidth = 4096;
	static const int32 GDDGIRadianceCascadeMaxTextureDimension = 16384;

	struct FDDGIRadianceCascadeGPUStorage
	{
		TRefCountPtr<IPooledRenderTarget> Directions;
		TRefCountPtr<IPooledRenderTarget> Radiance;
		TRefCountPtr<IPooledRenderTarget> HitDistance;
		TRefCountPtr<IPooledRenderTarget> ProbePositions;
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
			HitDistance.SafeRelease();
			ProbePositions.SafeRelease();
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
        Storage.HitDistance.IsValid();

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
        GRenderTargetPool.FindFreeElement(RHICmdList, DistanceDesc, Storage.HitDistance,
            TEXT("DDGIRadianceCascade_HitDistance"), ERenderTargetTransience::NonTransient);

        if (!Storage.Directions.IsValid() || !Storage.Radiance.IsValid() || !Storage.HitDistance.IsValid())
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
    FRDGTexture*& OutHitDistance)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutDirections = nullptr;
    OutRadiance = nullptr;
    OutHitDistance = nullptr;

    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->bNeedsInitialization ||
        !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() || !Storage->HitDistance.IsValid())
    {
        return false;
    }

    OutDirections = GraphBuilder.RegisterExternalTexture(Storage->Directions);
    OutRadiance = GraphBuilder.RegisterExternalTexture(Storage->Radiance);
    OutHitDistance = GraphBuilder.RegisterExternalTexture(Storage->HitDistance);
    Storage->bNeedsInitialization = false;
    return OutDirections && OutRadiance && OutHitDistance;
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
