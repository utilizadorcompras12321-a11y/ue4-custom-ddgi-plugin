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
		bool bNeedsInitialization = true;
		FIntPoint Dimensions = FIntPoint::ZeroValue;
		int32 TotalRaysPerProbe = 0;
		int32 ProbeCapacity = 0;

		void Release()
		{
			Directions.SafeRelease();
			Radiance.SafeRelease();
			HitDistance.SafeRelease();
			Dimensions = FIntPoint::ZeroValue;
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

	if (Storage.Dimensions == RequiredDimensions &&
		Storage.TotalRaysPerProbe == Config.TotalRaysPerProbe &&
		Storage.ProbeCapacity == Config.MaxActiveProbes &&
		Storage.Directions.IsValid() &&
		Storage.Radiance.IsValid() &&
		Storage.HitDistance.IsValid())
	{
		return;
	}

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
