#include "DDGIRadianceCascadeResources.h"
#include "DDGIMasterSubsystem.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderTargetPool.h"
#include "RenderGraph.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneInterface.h"
#include "Containers/ResourceArray.h"

namespace
{
	FORCEINLINE int32 DDGIGetSpatialLevelStep(int32 Level)
	{
		static const int32 Steps[4] = { 1, 2, 4, 8 };
		return Level >= 0 && Level < UE_ARRAY_COUNT(Steps) ? Steps[Level] : 0;
	}

	FORCEINLINE uint32 DDGIHashProbeCell(const FIntVector& Cell, uint32 Level)
	{
		uint32 Hash = static_cast<uint32>(Cell.X) * 0x8DA6B343u;
		Hash ^= static_cast<uint32>(Cell.Y) * 0xD8163841u;
		Hash ^= static_cast<uint32>(Cell.Z) * 0xCB1AB31Fu;
		Hash ^= (Level + 1u) * 0x9E3779B9u;
		Hash ^= Hash >> 16;
		Hash *= 0x7FEB352Du;
		Hash ^= Hash >> 15;
		Hash *= 0x846CA68Bu;
		Hash ^= Hash >> 16;
		return Hash;
	}

	struct FDDGIProbeSpatialHashEntry
	{
		int32 X;
		int32 Y;
		int32 Z;
		int32 EncodedProbe;
	};
	static_assert(sizeof(FDDGIProbeSpatialHashEntry) == 4 * sizeof(int32), "Probe spatial hash entries must match the HLSL int4 stride.");

	struct FDDGIProbeCellKey
	{
		FIntVector Cell;
		uint32 Level;
		bool operator==(const FDDGIProbeCellKey& Other) const
		{
			return Cell == Other.Cell && Level == Other.Level;
		}
		friend uint32 GetTypeHash(const FDDGIProbeCellKey& Key)
		{
			return DDGIHashProbeCell(Key.Cell, Key.Level);
		}
	};

	struct FDDGIRetainedPhysicalProbeSlot
	{
		uint32 PhysicalSlot = MAX_uint32;
		uint32 LastSeenUpdate = 0u;
	};

	// Keep recently invisible lattice probes alive long enough for the camera
	// window to move back without discarding their accumulated ray history.
	static const uint32 GDDGIPhysicalProbeRetentionFrames = 30u;
}

namespace
{
	static const int32 GDDGIRadianceCascadeTextureWidth = 4096;
	static const int32 GDDGIRadianceCascadeMaxTextureDimension = 16384;
	// Five per-probe ray textures consume 32 bytes per ray record.
	// Directions are stored once per ray layout (8 bytes per direction), shared
	// by all probes. Bound this subsystem independently of RHI texture limits.
	static const int64 GDDGIRadianceCascadeMaxRayStorageBytes = 512ll * 1024ll * 1024ll;
	static const int64 GDDGIRadianceCascadeBytesPerProbeRay = 32ll;
	static const int64 GDDGIRadianceCascadeBytesPerSharedDirection = 8ll;

	struct FDDGIRadianceCascadeGPUStorage
	{
		TRefCountPtr<IPooledRenderTarget> Directions;
		TRefCountPtr<IPooledRenderTarget> Radiance;
		TRefCountPtr<IPooledRenderTarget> RadianceScratch;
		TRefCountPtr<IPooledRenderTarget> RadianceIntegrationScratch;
		TRefCountPtr<IPooledRenderTarget> HitDistance;
		TRefCountPtr<IPooledRenderTarget> HitTriangleIndex;
		TRefCountPtr<IPooledRenderTarget> ProbePositions;
		FStructuredBufferRHIRef LogicalToPhysicalSlots;
		FShaderResourceViewRHIRef LogicalToPhysicalSlotsSRV;
		FStructuredBufferRHIRef NewlyAllocatedPhysicalSlots;
		FShaderResourceViewRHIRef NewlyAllocatedPhysicalSlotsSRV;
		TMap<FDDGIProbeCellKey, FDDGIRetainedPhysicalProbeSlot> ProbeCellToPhysicalSlot;
		uint32 ProbeUpdateSerial = 0u;
		TArray<uint32> LogicalToPhysicalSlotCPU;
		TArray<uint32> NewlyAllocatedPhysicalSlotsCPU;
		TArray<uint32> FreePhysicalSlots;
		uint32 PhysicalSlotCapacity = 0;
		FStructuredBufferRHIRef ProbeSpatialHash;
		FShaderResourceViewRHIRef ProbeSpatialHashSRV;
		FStructuredBufferRHIRef ProbeNeighborIndices;
		FShaderResourceViewRHIRef ProbeNeighborIndicesSRV;
		uint32 ProbeNeighborProbeCount = 0;
		uint32 ProbeNeighborLayoutHash = 0;
		float ProbeNeighborSpacing = 0.0f;
		uint32 ProbeSpatialHashCapacity = 0;
		uint32 ProbeSpatialHashProbeCount = 0;
		// Resource updates run at most once per scene/frame. Retry failed lookup
		// allocations periodically instead of rebuilding them on every frame.
		uint32 ProbeSpatialHashRetryCountdown = 0;
		uint32 ProbeNeighborRetryCountdown = 0;
		uint32 ProbeSpatialHashLayoutHash = 0;
		float ProbeSpatialHashSpacing = 0.0f;
		FStructuredBufferRHIRef BVHNodes;
		FShaderResourceViewRHIRef BVHNodesSRV;
		FStructuredBufferRHIRef BVHTriangles;
		FShaderResourceViewRHIRef BVHTrianglesSRV;
		// Retain the immutable CPU snapshot while its GPU buffers are cached.
		// This also prevents allocator address reuse from making a new BVH look
		// identical to a previously uploaded snapshot.
		TSharedPtr<FDDGISoftwareBVHData, ESPMode::ThreadSafe> BVHDataReference;
		uint32 BVHRetryCountdown = 0;
		uint32 BVHNodeCount = 0;
		uint32 BVHTriangleCount = 0;
		uint32 LastRadianceUpdateFrameNumber = MAX_uint32;
		bool bNeedsInitialization = true;
		bool bNeedsDirectionInitialization = true;
		bool bInvalidateAllPhysicalSlots = false;
		bool bPendingPhysicalSlotsUpload = false;
		FIntPoint Dimensions = FIntPoint::ZeroValue;
		FIntPoint DirectionDimensions = FIntPoint::ZeroValue;
		int32 TotalRaysPerProbe = 0;
		int32 RayLayoutCascadeCount = 0;
		int32 RayLayoutBaseRayCount = 0;
		int32 RayLayoutSplitFactor = 0;
		int32 RayProbeCapacity = 0;
		FIntPoint ProbePositionDimensions = FIntPoint::ZeroValue;
		// Cache validated lattice identities across scene updates. Re-quantizing every
		// probe every frame is unnecessary while the published layout is unchanged.
		TArray<FDDGIProbeCellKey> ProbeCellKeys;
		// Persistent logical lookup reused by neighbor construction and the future
		// logical-to-physical slot allocator. Keys are global lattice identities.
		TMap<FDDGIProbeCellKey, uint32> ProbeCellToLogicalIndex;
		int32 UploadedProbeCount = 0;
		uint32 UploadedProbeLayoutHash = 0;
		float UploadedProbeSpacing = 0.0f;

		void Release()
		{
			Directions.SafeRelease();
			Radiance.SafeRelease();
			RadianceScratch.SafeRelease();
			RadianceIntegrationScratch.SafeRelease();
			HitDistance.SafeRelease();
			HitTriangleIndex.SafeRelease();
			ProbePositions.SafeRelease();
			LogicalToPhysicalSlots.SafeRelease();
			LogicalToPhysicalSlotsSRV.SafeRelease();
			NewlyAllocatedPhysicalSlots.SafeRelease();
			NewlyAllocatedPhysicalSlotsSRV.SafeRelease();
			ProbeCellToPhysicalSlot.Reset();
			ProbeUpdateSerial = 0u;
			LogicalToPhysicalSlotCPU.Reset();
			NewlyAllocatedPhysicalSlotsCPU.Reset();
			FreePhysicalSlots.Reset();
			PhysicalSlotCapacity = 0;
			ProbeSpatialHash.SafeRelease();
			ProbeSpatialHashSRV.SafeRelease();
			ProbeNeighborIndices.SafeRelease();
			ProbeNeighborIndicesSRV.SafeRelease();
			ProbeNeighborProbeCount = 0;
			ProbeNeighborLayoutHash = 0;
			ProbeNeighborSpacing = 0.0f;
			ProbeSpatialHashCapacity = 0;
			ProbeSpatialHashProbeCount = 0;
			ProbeSpatialHashRetryCountdown = 0;
			ProbeNeighborRetryCountdown = 0;
			ProbeSpatialHashLayoutHash = 0;
			ProbeSpatialHashSpacing = 0.0f;
			BVHNodes.SafeRelease();
			BVHNodesSRV.SafeRelease();
			BVHTriangles.SafeRelease();
			BVHTrianglesSRV.SafeRelease();
			BVHDataReference.Reset();
			BVHRetryCountdown = 0;
			BVHNodeCount = 0;
			BVHTriangleCount = 0;
			Dimensions = FIntPoint::ZeroValue;
			DirectionDimensions = FIntPoint::ZeroValue;
			RayProbeCapacity = 0;
			ProbePositionDimensions = FIntPoint::ZeroValue;
			ProbeCellKeys.Reset();
			ProbeCellToLogicalIndex.Reset();
			UploadedProbeCount = 0;
			UploadedProbeLayoutHash = 0;
			UploadedProbeSpacing = 0.0f;
			TotalRaysPerProbe = 0;
			RayLayoutCascadeCount = 0;
			RayLayoutBaseRayCount = 0;
			RayLayoutSplitFactor = 0;
			bNeedsInitialization = true;
			bNeedsDirectionInitialization = true;
			bInvalidateAllPhysicalSlots = false;
			bPendingPhysicalSlotsUpload = false;
			LastRadianceUpdateFrameNumber = MAX_uint32;
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

    // Disabling Dynamic GI releases every persistent GPU allocation, including
    // ray histories, lookup hashes and the software BVH.
    // Re-enabling will recreate them through the normal validated path.
    if (!Config.bEnableDynamicGI)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        return;
    }

    // MaxActiveProbes is a hard safety cap. GPU ray and probe-position textures
    // are sized from the actual active probe count to avoid reserving unused capacity.
    // Enforce the cap here as well as in the actor/subsystem for external configs.
    if (Config.MaxActiveProbes <= 0 || Config.MaxActiveProbes > 32768 ||
        Config.TotalRaysPerProbe <= 0 || Config.ProbePositions.Num() > Config.MaxActiveProbes)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: invalid probe configuration (probes %d, positions %d, rays per probe %d). Supported probe count is 1..32768."),
            Config.MaxActiveProbes, Config.ProbePositions.Num(), Config.TotalRaysPerProbe);
        return;
    }

    const int32 ActiveProbeCount = Config.ProbePositions.Num();
    if (ActiveProbeCount <= 0)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        return;
    }

    const int64 TotalTexels = static_cast<int64>(ActiveProbeCount) *
        static_cast<int64>(Config.TotalRaysPerProbe);
    const int64 MaxTextureTexels = static_cast<int64>(GDDGIRadianceCascadeTextureWidth) *
        static_cast<int64>(GDDGIRadianceCascadeMaxTextureDimension);
    const int64 Height64 = (TotalTexels + GDDGIRadianceCascadeTextureWidth - 1) /
        GDDGIRadianceCascadeTextureWidth;

    // Account for complete allocated rows, not only logically active ray records.
    // Per-probe history uses five textures (32 bytes/texel): radiance,
    // two radiance ping-pong targets, hit distance, and hit-triangle index.
    // The direction texture has its own row padding and is shared per ray layout.
    const int64 AllocatedRayTexels = Height64 * GDDGIRadianceCascadeTextureWidth;
    const int64 DirectionHeight64 =
        (static_cast<int64>(Config.TotalRaysPerProbe) + GDDGIRadianceCascadeTextureWidth - 1) /
        GDDGIRadianceCascadeTextureWidth;
    const int64 AllocatedDirectionTexels = DirectionHeight64 * GDDGIRadianceCascadeTextureWidth;
    const int64 EstimatedRayStorageBytes =
        AllocatedRayTexels * GDDGIRadianceCascadeBytesPerProbeRay +
        AllocatedDirectionTexels * GDDGIRadianceCascadeBytesPerSharedDirection;
    if (TotalTexels <= 0 || TotalTexels > MaxTextureTexels ||
        Height64 <= 0 || Height64 > GDDGIRadianceCascadeMaxTextureDimension ||
        DirectionHeight64 <= 0 || DirectionHeight64 > GDDGIRadianceCascadeMaxTextureDimension ||
        EstimatedRayStorageBytes > GDDGIRadianceCascadeMaxRayStorageBytes)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        UE_LOG(LogTemp, Error,
            TEXT("Radiance Cascades: requested ray storage exceeds supported limits (%lld records, %.1f MiB; limit %.1f MiB). Reduce active probes or cascade ray counts."),
            TotalTexels,
            static_cast<double>(EstimatedRayStorageBytes) / (1024.0 * 1024.0),
            static_cast<double>(GDDGIRadianceCascadeMaxRayStorageBytes) / (1024.0 * 1024.0));
        return;
    }

    FDDGIRadianceCascadeGPUStorage& Storage = GDDGIRadianceCascadeStorage_RenderThread.FindOrAdd(Scene);
    // Ray textures are a grow-only capacity pool. A smaller active window must
    // not shrink/recreate the textures: doing so discards every probe's history.
    // A ray-layout change still requires new storage, but must not evict the
    // scene BVH or spatial lookup resources.
    const int32 RequestedRayLayoutCascadeCount = Config.RadianceCascades.GetSafeCascadeCount();
    const int32 RequestedRayLayoutBaseRayCount = FMath::Clamp(Config.RadianceCascades.BaseRayCount, 1, 64);
    const int32 RequestedRayLayoutSplitFactor = FMath::Clamp(Config.RadianceCascades.RaySplitFactor, 1, 4);
    const bool bRayLayoutChanged =
        Storage.TotalRaysPerProbe != 0 &&
        (Storage.TotalRaysPerProbe != Config.TotalRaysPerProbe ||
            Storage.RayLayoutCascadeCount != RequestedRayLayoutCascadeCount ||
            Storage.RayLayoutBaseRayCount != RequestedRayLayoutBaseRayCount ||
            Storage.RayLayoutSplitFactor != RequestedRayLayoutSplitFactor);
    const int64 ExistingProbeCapacity =
        (!bRayLayoutChanged && Storage.RayProbeCapacity > 0)
        ? static_cast<int64>(Storage.RayProbeCapacity)
        : 0;
    // Keep modest growth headroom so small active-window count fluctuations
    // do not recreate ray-history textures and discard retained probe history.
    // This is bounded by MaxActiveProbes; it never reserves the full cap by default.
    const int64 ActiveProbeCapacity = FMath::Min<int64>(
        Config.MaxActiveProbes,
        static_cast<int64>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(ActiveProbeCount))));
    const int64 RequiredProbeCapacity = FMath::Max<int64>(ActiveProbeCapacity, ExistingProbeCapacity);
    const int64 RequiredCapacityTexels = RequiredProbeCapacity * static_cast<int64>(Config.TotalRaysPerProbe);
    const int64 CapacityHeight64 = (RequiredCapacityTexels + GDDGIRadianceCascadeTextureWidth - 1) /
        GDDGIRadianceCascadeTextureWidth;

    // Validate the actual grow-only allocation, including power-of-two headroom
    // and any retained high-water capacity. The active-count estimate above is
    // not sufficient to bound the textures that will really be allocated.
    const int64 AllocatedCapacityRayTexels =
        CapacityHeight64 * GDDGIRadianceCascadeTextureWidth;
    const int64 EstimatedCapacityStorageBytes =
        AllocatedCapacityRayTexels * GDDGIRadianceCascadeBytesPerProbeRay +
        AllocatedDirectionTexels * GDDGIRadianceCascadeBytesPerSharedDirection;
    if (RequiredCapacityTexels <= 0 ||
        RequiredCapacityTexels > MaxTextureTexels ||
        CapacityHeight64 <= 0 ||
        CapacityHeight64 > GDDGIRadianceCascadeMaxTextureDimension ||
        EstimatedCapacityStorageBytes > GDDGIRadianceCascadeMaxRayStorageBytes)
    {
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        UE_LOG(LogTemp, Error,
            TEXT("Radiance Cascades: allocated ray capacity exceeds supported limits (%lld probes, %lld records, %.1f MiB; limit %.1f MiB). Reduce active probes or cascade ray counts."),
            RequiredProbeCapacity,
            RequiredCapacityTexels,
            static_cast<double>(EstimatedCapacityStorageBytes) / (1024.0 * 1024.0),
            static_cast<double>(GDDGIRadianceCascadeMaxRayStorageBytes) / (1024.0 * 1024.0));
        return;
    }

    const FIntPoint RequiredDimensions(GDDGIRadianceCascadeTextureWidth, static_cast<int32>(CapacityHeight64));
    const FIntPoint RequiredDirectionDimensions(GDDGIRadianceCascadeTextureWidth, static_cast<int32>(DirectionHeight64));
    const bool bRayStorageMatches =
        Storage.Dimensions == RequiredDimensions &&
        Storage.DirectionDimensions == RequiredDirectionDimensions &&
        Storage.TotalRaysPerProbe == Config.TotalRaysPerProbe &&
        Storage.RayLayoutCascadeCount == RequestedRayLayoutCascadeCount &&
        Storage.RayLayoutBaseRayCount == RequestedRayLayoutBaseRayCount &&
        Storage.RayLayoutSplitFactor == RequestedRayLayoutSplitFactor &&
        Storage.Directions.IsValid() &&
        Storage.Radiance.IsValid() &&
        Storage.RadianceScratch.IsValid() &&
        Storage.RadianceIntegrationScratch.IsValid() &&
        Storage.HitDistance.IsValid() &&
        Storage.HitTriangleIndex.IsValid();

    if (!bRayStorageMatches)
    {
        Storage.Directions.SafeRelease();
        Storage.Radiance.SafeRelease();
        Storage.RadianceScratch.SafeRelease();
        Storage.RadianceIntegrationScratch.SafeRelease();
        Storage.HitDistance.SafeRelease();
        Storage.HitTriangleIndex.SafeRelease();
        Storage.Dimensions = FIntPoint::ZeroValue;
        Storage.DirectionDimensions = FIntPoint::ZeroValue;
        Storage.TotalRaysPerProbe = 0;
        Storage.RayLayoutCascadeCount = 0;
        Storage.RayLayoutBaseRayCount = 0;
        Storage.RayLayoutSplitFactor = 0;
        Storage.RayProbeCapacity = 0;
        Storage.bNeedsInitialization = true;
        Storage.bNeedsDirectionInitialization = true;
        Storage.bInvalidateAllPhysicalSlots = false;
        Storage.bPendingPhysicalSlotsUpload = false;
        // Ray texture recreation invalidates every physical slot and its history.
        Storage.LogicalToPhysicalSlots.SafeRelease();
        Storage.LogicalToPhysicalSlotsSRV.SafeRelease();
        Storage.NewlyAllocatedPhysicalSlots.SafeRelease();
        Storage.NewlyAllocatedPhysicalSlotsSRV.SafeRelease();
        Storage.ProbeCellToPhysicalSlot.Reset();
        Storage.LogicalToPhysicalSlotCPU.Reset();
        Storage.NewlyAllocatedPhysicalSlotsCPU.Reset();
        Storage.FreePhysicalSlots.Reset();
        Storage.PhysicalSlotCapacity = 0;

        // Half-float RGBA texels store direction and radiance; R32F stores hit distance.
        const FPooledRenderTargetDesc DirectionDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDirectionDimensions, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        const FPooledRenderTargetDesc RadianceDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        const FPooledRenderTargetDesc DistanceDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        const FPooledRenderTargetDesc HitTriangleIndexDesc = FPooledRenderTargetDesc::Create2DDesc(
            RequiredDimensions, PF_R32_UINT, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);

        GRenderTargetPool.FindFreeElement(RHICmdList, DirectionDesc, Storage.Directions,
            TEXT("DDGIRadianceCascade_Directions"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, RadianceDesc, Storage.Radiance,
            TEXT("DDGIRadianceCascade_Radiance"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, RadianceDesc, Storage.RadianceScratch,
            TEXT("DDGIRadianceCascade_RadianceScratch"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, RadianceDesc, Storage.RadianceIntegrationScratch,
            TEXT("DDGIRadianceCascade_RadianceIntegrationScratch"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, DistanceDesc, Storage.HitDistance,
            TEXT("DDGIRadianceCascade_HitDistance"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, HitTriangleIndexDesc, Storage.HitTriangleIndex,
            TEXT("DDGIRadianceCascade_HitTriangleIndex"), ERenderTargetTransience::NonTransient);
        if (!Storage.Directions.IsValid() || !Storage.Radiance.IsValid() || !Storage.RadianceScratch.IsValid() || !Storage.RadianceIntegrationScratch.IsValid() ||
            !Storage.HitDistance.IsValid() || !Storage.HitTriangleIndex.IsValid())
        {
            Storage.Release();
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate persistent GPU textures."));
            return;
        }

        Storage.Dimensions = RequiredDimensions;
        Storage.DirectionDimensions = RequiredDirectionDimensions;
        Storage.TotalRaysPerProbe = Config.TotalRaysPerProbe;
        Storage.RayLayoutCascadeCount = RequestedRayLayoutCascadeCount;
        Storage.RayLayoutBaseRayCount = RequestedRayLayoutBaseRayCount;
        Storage.RayLayoutSplitFactor = RequestedRayLayoutSplitFactor;
        // The final texture row is fully allocated, even when the active ray
        // count does not fill it. Count every complete probe-sized range that
        // physically fits in the allocated texture, not only active probes.
        const int64 AllocatedTextureTexels =
            static_cast<int64>(Storage.Dimensions.X) * Storage.Dimensions.Y;
        Storage.RayProbeCapacity = static_cast<int32>(
            AllocatedTextureTexels / Config.TotalRaysPerProbe);
        Storage.PhysicalSlotCapacity = FMath::Min<uint32>(
            static_cast<uint32>(Storage.RayProbeCapacity),
            static_cast<uint32>(Config.MaxActiveProbes));
        // Push in reverse order so the first new probe receives physical slot zero.
        for (uint32 Slot = Storage.PhysicalSlotCapacity; Slot > 0u; --Slot)
        {
            Storage.FreePhysicalSlots.Add(Slot - 1u);
        }
    }

    // Keep grid-relative probe offsets in a separate float4 texture. A pure
    // Master Actor translation changes only ProbeGridOrigin and does not require
    // rewriting this texture.
    const int32 ProbeTextureWidth = 4096;
    // Probe positions are metadata, not ray history. Grow this texture only
    // when the active window exceeds its previous high-water mark; never reserve
    // MaxActiveProbes up front and never shrink it as the camera moves.
    const int32 ExistingProbePositionCapacity = Storage.ProbePositions.IsValid()
        ? Storage.ProbePositionDimensions.X * Storage.ProbePositionDimensions.Y
        : 0;
    const int32 RequiredProbePositionCapacity =
        FMath::Max(ActiveProbeCount, ExistingProbePositionCapacity);
    const int32 ProbeTextureHeight = FMath::Max(
        1, FMath::DivideAndRoundUp(RequiredProbePositionCapacity, ProbeTextureWidth));
    const FIntPoint ProbePositionDimensions(ProbeTextureWidth, ProbeTextureHeight);
    if (Storage.ProbePositionDimensions != ProbePositionDimensions || !Storage.ProbePositions.IsValid())
    {
        Storage.ProbePositions.SafeRelease();
        const FPooledRenderTargetDesc ProbePositionDesc = FPooledRenderTargetDesc::Create2DDesc(
            ProbePositionDimensions, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_None,
            TexCreate_ShaderResource | TexCreate_UAV, false);
        GRenderTargetPool.FindFreeElement(RHICmdList, ProbePositionDesc, Storage.ProbePositions,
            TEXT("DDGIRadianceCascade_ProbePositions"), ERenderTargetTransience::NonTransient);
        if (!Storage.ProbePositions.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate sparse probe-position texture."));
            return;
        }
        Storage.ProbePositionDimensions = ProbePositionDimensions;
        Storage.UploadedProbeCount = 0;
        Storage.UploadedProbeLayoutHash = 0;
        Storage.UploadedProbeSpacing = 0.0f;
    }

    const int32 ProbeCount = ActiveProbeCount;
    const uint32 ProbeCountForHash = static_cast<uint32>(ActiveProbeCount);
    // Keep the open-addressed table at <= 50% load and make its capacity a
    // power of two because both CPU insertion and the shader use a bit mask.
    // ActiveProbeCount is capped at 32768 above, so doubling cannot overflow.
    const uint32 RequiredHashCapacity = FMath::RoundUpToPowerOfTwo(
        FMath::Max(2u, ProbeCountForHash * 2u));
    const float SafeProbeSpacing = FMath::Max(1.0f, Config.ProbeSpacing);

    // Probe identity is immutable for a published lattice. Check its snapshot
    // before doing per-probe validation/quantization so steady-state scene
    // updates reuse the cached integer keys instead of rebuilding them.
    const uint32 ProbeLayoutHash = Config.ProbeLayoutHash;
    bool bProbeIdentityMatches =
        Config.ProbeCellCoordinates.Num() == ActiveProbeCount &&
        Config.ProbeLevels.Num() == ActiveProbeCount &&
        Storage.ProbeCellKeys.Num() == ActiveProbeCount;
    if (bProbeIdentityMatches)
    {
        for (int32 ProbeIndex = 0; ProbeIndex < ActiveProbeCount; ++ProbeIndex)
        {
            const FDDGIProbeCellKey& CachedKey = Storage.ProbeCellKeys[ProbeIndex];
            if (CachedKey.Cell != Config.ProbeCellCoordinates[ProbeIndex] ||
                CachedKey.Level != static_cast<uint32>(Config.ProbeLevels[ProbeIndex]))
            {
                bProbeIdentityMatches = false;
                break;
            }
        }
    }
    const bool bProbePositionsMatch =
        Storage.ProbePositions.IsValid() &&
        Storage.UploadedProbeCount == ProbeCount &&
        Storage.UploadedProbeLayoutHash == ProbeLayoutHash &&
        FMath::IsNearlyEqual(Storage.UploadedProbeSpacing, SafeProbeSpacing, 1.0e-4f) &&
        bProbeIdentityMatches;
    const bool bPhysicalMappingMissing =
        Storage.PhysicalSlotCapacity == 0 ||
        Storage.LogicalToPhysicalSlotCPU.Num() != ActiveProbeCount ||
        !Storage.LogicalToPhysicalSlotsSRV.IsValid();
    const bool bProbeLayoutChanged = !bProbePositionsMatch || bPhysicalMappingMissing;

    if (bProbeLayoutChanged)
    {
        if (Config.ProbeCellCoordinates.Num() != ActiveProbeCount ||
            Config.ProbeLevels.Num() != ActiveProbeCount)
        {
            DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
            UE_LOG(LogTemp, Error,
                TEXT("Radiance Cascades: exact integer probe identity arrays do not match the active probe count."));
            return;
        }
        const float LatticeTolerance = FMath::Max(0.01f, SafeProbeSpacing * 1.0e-4f);
        Storage.ProbeCellKeys.SetNumUninitialized(ActiveProbeCount);
        Storage.ProbeCellToLogicalIndex.Reset();
        Storage.ProbeCellToLogicalIndex.Reserve(ActiveProbeCount);
        for (int32 ProbeIndex = 0; ProbeIndex < ActiveProbeCount; ++ProbeIndex)
        {
            const FVector4& Position = Config.ProbePositions[ProbeIndex];
            const int32 ProbeLevel = Config.ProbeLevels[ProbeIndex];
            const FIntVector Cell = Config.ProbeCellCoordinates[ProbeIndex];
            if (ProbeLevel < 0 || ProbeLevel > 3 ||
                !FMath::IsNearlyEqual(Position.W, static_cast<float>(ProbeLevel), 1.0e-3f))
            {
                DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
                UE_LOG(LogTemp, Error,
                    TEXT("Radiance Cascades: probe %d has invalid explicit spatial level %d (position level %.3f)."),
                    ProbeIndex, ProbeLevel, Position.W);
                return;
            }

            const int32 LevelCellStep = DDGIGetSpatialLevelStep(ProbeLevel);
            const FDDGIProbeCellKey ProbeKey{ Cell, static_cast<uint32>(ProbeLevel) };
            Storage.ProbeCellKeys[ProbeIndex] = ProbeKey;
            if (Storage.ProbeCellToLogicalIndex.Contains(ProbeKey))
            {
                DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
                UE_LOG(LogTemp, Error,
                    TEXT("Radiance Cascades: duplicate probe-cell identity at logical index %d (cell %d, %d, %d, level %d)."),
                    ProbeIndex, Cell.X, Cell.Y, Cell.Z, ProbeLevel);
                return;
            }
            Storage.ProbeCellToLogicalIndex.Add(ProbeKey, static_cast<uint32>(ProbeIndex));
            const bool bOnBaseLattice =
                FMath::IsNearlyEqual(Position.X, static_cast<float>(static_cast<double>(Cell.X) * SafeProbeSpacing), LatticeTolerance) &&
                FMath::IsNearlyEqual(Position.Y, static_cast<float>(static_cast<double>(Cell.Y) * SafeProbeSpacing), LatticeTolerance) &&
                FMath::IsNearlyEqual(Position.Z, static_cast<float>(static_cast<double>(Cell.Z) * SafeProbeSpacing), LatticeTolerance);
            const bool bAlignedToLevel =
                (Cell.X % LevelCellStep) == 0 &&
                (Cell.Y % LevelCellStep) == 0 &&
                (Cell.Z % LevelCellStep) == 0;
            if (!bOnBaseLattice || !bAlignedToLevel)
            {
                DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
                UE_LOG(LogTemp, Error,
                    TEXT("Radiance Cascades: probe %d at (%.2f, %.2f, %.2f) is not aligned to level %d (base spacing %.2f cm)."),
                    ProbeIndex, Position.X, Position.Y, Position.Z, ProbeLevel, SafeProbeSpacing);
                return;
            }
        }
    }
    else if (Storage.ProbeCellKeys.Num() != ActiveProbeCount ||
        Storage.ProbeCellToLogicalIndex.Num() != ActiveProbeCount)
    {
        // A matching uploaded snapshot without its CPU identity cache is an
        // inconsistent state; fail closed rather than building lookup data
        // from stale or incomplete keys.
        DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
        UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: cached probe-cell identity count does not match the uploaded layout."));
        return;
    }
    const TArray<FDDGIProbeCellKey>& ProbeCellKeys = Storage.ProbeCellKeys;

    // Preserve physical ray history by global cell identity while compact logical
    // indices are free to change with the active window.
    if (Storage.PhysicalSlotCapacity == 0 ||
        Storage.PhysicalSlotCapacity < static_cast<uint32>(ActiveProbeCount))
    {
        Storage.Release();
        UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: physical slot capacity is smaller than the active probe set."));
        return;
    }
    if (bProbeLayoutChanged)
    {
        Storage.bPendingPhysicalSlotsUpload = true;
        TSet<FDDGIProbeCellKey> ActiveKeys;
        ActiveKeys.Reserve(ActiveProbeCount);
        for (const FDDGIProbeCellKey& Key : ProbeCellKeys)
        {
            ActiveKeys.Add(Key);
        }

        // Refresh visible identities and expire only probes that have remained
        // outside the active window for the retention period. Their physical
        // slots and ray history remain untouched while they are retained.
        // Collect expired pending slots first, then filter the pending array once;
        // repeatedly calling TArray::Remove inside this loop is quadratic.
        TSet<uint32> ExpiredPendingSlots;
        for (auto It = Storage.ProbeCellToPhysicalSlot.CreateIterator(); It; ++It)
        {
            if (ActiveKeys.Contains(It.Key()))
            {
                It.Value().LastSeenUpdate = Storage.ProbeUpdateSerial;
                continue;
            }

            const uint32 FramesSinceVisible =
                Storage.ProbeUpdateSerial - It.Value().LastSeenUpdate;
            if (FramesSinceVisible >= GDDGIPhysicalProbeRetentionFrames)
            {
                const uint32 ExpiredSlot = It.Value().PhysicalSlot;
                // A retained identity owns exactly one physical slot; expired map entries
                // are removed immediately below, so this slot cannot already be free.
                Storage.FreePhysicalSlots.Add(ExpiredSlot);
                ExpiredPendingSlots.Add(ExpiredSlot);
                It.RemoveCurrent();
            }
        }
        if (ExpiredPendingSlots.Num() > 0)
        {
            const int32 PreviousPendingSlotCount = Storage.NewlyAllocatedPhysicalSlotsCPU.Num();
            Storage.NewlyAllocatedPhysicalSlotsCPU.RemoveAll(
                [&ExpiredPendingSlots](uint32 PendingSlot)
                {
                    return ExpiredPendingSlots.Contains(PendingSlot);
                });
            if (Storage.NewlyAllocatedPhysicalSlotsCPU.Num() != PreviousPendingSlotCount)
            {
                Storage.bPendingPhysicalSlotsUpload = true;
            }
        }

        int32 NewIdentityCount = 0;
        for (const FDDGIProbeCellKey& Key : ProbeCellKeys)
        {
            if (!Storage.ProbeCellToPhysicalSlot.Contains(Key))
            {
                ++NewIdentityCount;
            }
        }
        const int32 RequiredEvictionCount = FMath::Max(
            0, NewIdentityCount - Storage.FreePhysicalSlots.Num());

        struct FDDGIProbeEvictionCandidate
        {
            FDDGIProbeCellKey Key;
            uint32 LastSeenUpdate;
            uint32 PhysicalSlot;
        };
        TArray<FDDGIProbeEvictionCandidate> EvictionCandidates;
        if (RequiredEvictionCount > 0)
        {
            EvictionCandidates.Reserve(Storage.ProbeCellToPhysicalSlot.Num());
            for (const auto& Pair : Storage.ProbeCellToPhysicalSlot)
            {
                if (!ActiveKeys.Contains(Pair.Key))
                {
                    EvictionCandidates.Add(FDDGIProbeEvictionCandidate{
                        Pair.Key,
                        Pair.Value.LastSeenUpdate,
                        Pair.Value.PhysicalSlot });
                }
            }
            EvictionCandidates.Sort([](
                const FDDGIProbeEvictionCandidate& A,
                const FDDGIProbeEvictionCandidate& B)
            {
                return A.LastSeenUpdate == B.LastSeenUpdate
                    ? A.PhysicalSlot < B.PhysicalSlot
                    : A.LastSeenUpdate < B.LastSeenUpdate;
            });
        }
        int32 NextEvictionCandidate = 0;

        // Physical slots are unique by allocator invariant. Track pending initialization
        // membership in a hash set so adding thousands of probes is O(N), not O(N^2).
        TSet<uint32> NewlyAllocatedSlotSet;
        NewlyAllocatedSlotSet.Reserve(Storage.NewlyAllocatedPhysicalSlotsCPU.Num() + ActiveProbeCount);
        for (const uint32 PendingSlot : Storage.NewlyAllocatedPhysicalSlotsCPU)
        {
            NewlyAllocatedSlotSet.Add(PendingSlot);
        }

        Storage.LogicalToPhysicalSlotCPU.SetNumUninitialized(ActiveProbeCount);
        for (int32 LogicalIndex = 0; LogicalIndex < ActiveProbeCount; ++LogicalIndex)
        {
            const FDDGIProbeCellKey& Key = ProbeCellKeys[LogicalIndex];
            if (FDDGIRetainedPhysicalProbeSlot* ExistingSlot =
                Storage.ProbeCellToPhysicalSlot.Find(Key))
            {
                ExistingSlot->LastSeenUpdate = Storage.ProbeUpdateSerial;
                Storage.LogicalToPhysicalSlotCPU[LogicalIndex] = ExistingSlot->PhysicalSlot;
                continue;
            }

            uint32 NewPhysicalSlot = MAX_uint32;
            if (Storage.FreePhysicalSlots.Num() > 0)
            {
                NewPhysicalSlot = Storage.FreePhysicalSlots.Pop(false);
            }
            else
            {
                // Candidates were sorted once above. Consume them in O(1) per
                // eviction instead of rescanning the retained map for every
                // newly visible probe (which becomes quadratic at pool capacity).
                if (!EvictionCandidates.IsValidIndex(NextEvictionCandidate))
                {
                    Storage.Release();
                    UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: active probes exhausted the physical slot pool; no inactive probe can be evicted."));
                    return;
                }

                const FDDGIProbeEvictionCandidate& Candidate =
                    EvictionCandidates[NextEvictionCandidate++];
                NewPhysicalSlot = Candidate.PhysicalSlot;
                Storage.ProbeCellToPhysicalSlot.Remove(Candidate.Key);
            }

            FDDGIRetainedPhysicalProbeSlot NewSlot;
            NewSlot.PhysicalSlot = NewPhysicalSlot;
            NewSlot.LastSeenUpdate = Storage.ProbeUpdateSerial;
            Storage.ProbeCellToPhysicalSlot.Add(Key, NewSlot);
            Storage.LogicalToPhysicalSlotCPU[LogicalIndex] = NewPhysicalSlot;
            if (!NewlyAllocatedSlotSet.Contains(NewPhysicalSlot))
            {
                NewlyAllocatedSlotSet.Add(NewPhysicalSlot);
                Storage.NewlyAllocatedPhysicalSlotsCPU.Add(NewPhysicalSlot);
            }
            Storage.bPendingPhysicalSlotsUpload = true;
            Storage.bNeedsInitialization = true;
        }

        TResourceArray<uint32> LogicalToPhysicalData;
        LogicalToPhysicalData.Append(Storage.LogicalToPhysicalSlotCPU);
        FRHIResourceCreateInfo LogicalToPhysicalInfo(TEXT("DDGIRadianceCascade_LogicalToPhysicalSlots"));
        LogicalToPhysicalInfo.ResourceArray = &LogicalToPhysicalData;
        Storage.LogicalToPhysicalSlots.SafeRelease();
        Storage.LogicalToPhysicalSlotsSRV.SafeRelease();
        Storage.LogicalToPhysicalSlots = RHICreateStructuredBuffer(
            sizeof(uint32), static_cast<uint32>(LogicalToPhysicalData.GetResourceDataSize()),
            BUF_ShaderResource, LogicalToPhysicalInfo);
        if (Storage.LogicalToPhysicalSlots.IsValid())
        {
            Storage.LogicalToPhysicalSlotsSRV = RHICreateShaderResourceView(Storage.LogicalToPhysicalSlots);
        }

        if (!Storage.LogicalToPhysicalSlotsSRV.IsValid())
        {
            Storage.LogicalToPhysicalSlots.SafeRelease();
            Storage.LogicalToPhysicalSlotsSRV.SafeRelease();
            Storage.NewlyAllocatedPhysicalSlots.SafeRelease();
            Storage.NewlyAllocatedPhysicalSlotsSRV.SafeRelease();
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to upload physical probe-slot mapping buffers."));
            return;
        }
    }

    if (Storage.bInvalidateAllPhysicalSlots)
    {
        Storage.NewlyAllocatedPhysicalSlotsCPU.Reset();
        // Logical-to-physical mapping is one-to-one, so every slot appears once.
        Storage.NewlyAllocatedPhysicalSlotsCPU.Append(Storage.LogicalToPhysicalSlotCPU);
        Storage.bInvalidateAllPhysicalSlots = false;
        Storage.bPendingPhysicalSlotsUpload = true;
    }

    if (Storage.bPendingPhysicalSlotsUpload)
    {
        TResourceArray<uint32> NewPhysicalSlotData;
        if (Storage.NewlyAllocatedPhysicalSlotsCPU.Num() > 0)
        {
            NewPhysicalSlotData.Append(Storage.NewlyAllocatedPhysicalSlotsCPU);
        }
        else
        {
            NewPhysicalSlotData.Add(0u);
        }
        FRHIResourceCreateInfo NewPhysicalSlotsInfo(TEXT("DDGIRadianceCascade_PendingPhysicalSlots"));
        NewPhysicalSlotsInfo.ResourceArray = &NewPhysicalSlotData;
        Storage.NewlyAllocatedPhysicalSlots.SafeRelease();
        Storage.NewlyAllocatedPhysicalSlotsSRV.SafeRelease();
        Storage.NewlyAllocatedPhysicalSlots = RHICreateStructuredBuffer(
            sizeof(uint32), static_cast<uint32>(NewPhysicalSlotData.GetResourceDataSize()),
            BUF_ShaderResource, NewPhysicalSlotsInfo);
        if (Storage.NewlyAllocatedPhysicalSlots.IsValid())
        {
            Storage.NewlyAllocatedPhysicalSlotsSRV =
                RHICreateShaderResourceView(Storage.NewlyAllocatedPhysicalSlots);
        }
        if (!Storage.NewlyAllocatedPhysicalSlotsSRV.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to upload pending physical probe slots; initialization will retry."));
            return;
        }
        Storage.bPendingPhysicalSlotsUpload = false;
    }

    // A changed logical layout invalidates both lookup buffers immediately.
    // Retry backoff is only for repeated allocation failures on an unchanged layout;
    // it must never leave an old hash/neighbor table available to the new probes.
    if (bProbeLayoutChanged)
    {
        Storage.ProbeSpatialHashRetryCountdown = 0;
        Storage.ProbeNeighborRetryCountdown = 0;
    }

    // Hash allocation success/failure is deliberately not part of layout identity.
    if (!bProbePositionsMatch)
    {
        const FSceneRenderTargetItem& ProbePositionItem =
            Storage.ProbePositions->GetRenderTargetItem();
        if (!ProbePositionItem.ShaderResourceTexture.IsValid())
        {
            Storage.UploadedProbeCount = 0;
            Storage.UploadedProbeLayoutHash = 0;
            Storage.UploadedProbeSpacing = 0.0f;
            Storage.bNeedsInitialization = true;
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: sparse probe-position texture has no shader-resource RHI texture."));
            return;
        }

        FTexture2DRHIRef ProbePositionRHI =
            ProbePositionItem.ShaderResourceTexture->GetTexture2D();
        if (!ProbePositionRHI.IsValid())
        {
            Storage.UploadedProbeCount = 0;
            Storage.UploadedProbeLayoutHash = 0;
            Storage.UploadedProbeSpacing = 0.0f;
            Storage.bNeedsInitialization = true;
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: sparse probe-position texture has no 2D RHI resource."));
            return;
        }

        const FVector4* ProbePositionData = Config.ProbePositions.GetData();
        for (int32 Row = 0; Row < ProbePositionDimensions.Y; ++Row)
        {
            const int32 FirstProbe = Row * ProbePositionDimensions.X;
            const int32 RowProbeCount = FMath::Min(
                ProbePositionDimensions.X, ProbeCount - FirstProbe);
            if (RowProbeCount <= 0)
            {
                break;
            }

            const FUpdateTextureRegion2D UpdateRegion(
                0, Row, 0, 0, RowProbeCount, 1);
            const FVector4* RowData = ProbePositionData + FirstProbe;
            RHICmdList.UpdateTexture2D(
                ProbePositionRHI,
                0,
                UpdateRegion,
                static_cast<uint32>(RowProbeCount * sizeof(FVector4)),
                reinterpret_cast<const uint8*>(RowData));
        }
        Storage.UploadedProbeCount = ProbeCount;
        Storage.UploadedProbeLayoutHash = ProbeLayoutHash;
        Storage.UploadedProbeSpacing = SafeProbeSpacing;
    }

    // Build a deterministic open-addressed spatial hash for sparse probe cells.
    // Probe offsets are integer-lattice-relative and remain stable on translation.

    // The active window is camera-selected, but every probe remains on the
    // immutable world-origin lattice. Rebuild logical lookup data when its
    // identity hash changes while retaining physical slots for surviving keys.
    // Ray storage recreation also sets this flag. The change flag was captured
    // before the upload so hash allocation retries do not reinitialize history.
    if (bProbeLayoutChanged)
    {
        Storage.bNeedsInitialization =
            Storage.bNeedsDirectionInitialization ||
            Storage.NewlyAllocatedPhysicalSlotsCPU.Num() > 0;
        // A different layout is a new allocation target, so do not inherit
        // backoff from failures associated with the previous probe lattice.
        Storage.ProbeSpatialHashRetryCountdown = 0;
        Storage.ProbeNeighborRetryCountdown = 0;
    }

    const bool bProbeHashMatches =
        !bProbeLayoutChanged &&
        Storage.ProbeSpatialHash.IsValid() &&
        Storage.ProbeSpatialHashSRV.IsValid() &&
        Storage.ProbeSpatialHashCapacity == RequiredHashCapacity &&
        Storage.ProbeSpatialHashProbeCount == ProbeCountForHash &&
        Storage.ProbeSpatialHashLayoutHash == ProbeLayoutHash &&
        FMath::IsNearlyEqual(Storage.ProbeSpatialHashSpacing, SafeProbeSpacing, 1.0e-4f);
    if (bProbeHashMatches)
    {
        Storage.ProbeSpatialHashRetryCountdown = 0;
    }
    else if (Storage.ProbeSpatialHashRetryCountdown > 0)
    {
        --Storage.ProbeSpatialHashRetryCountdown;
    }
    else
    {
        Storage.ProbeSpatialHash.SafeRelease();
        Storage.ProbeSpatialHashSRV.SafeRelease();

        TResourceArray<FDDGIProbeSpatialHashEntry> HashEntries;
        HashEntries.SetNumUninitialized(static_cast<int32>(RequiredHashCapacity));
        for (FDDGIProbeSpatialHashEntry& Entry : HashEntries)
        {
            Entry = FDDGIProbeSpatialHashEntry{ 0, 0, 0, -1 };
        }

        bool bHashBuildSucceeded = true;
        for (uint32 ProbeIndex = 0u; ProbeIndex < ProbeCountForHash; ++ProbeIndex)
        {
            const FDDGIProbeCellKey& ProbeKey = ProbeCellKeys[static_cast<int32>(ProbeIndex)];
            const FIntVector& Cell = ProbeKey.Cell;
            const uint32 ProbeLevel = ProbeKey.Level;
            uint32 Slot = DDGIHashProbeCell(Cell, ProbeLevel) & (RequiredHashCapacity - 1u);
            bool bInserted = false;
            for (uint32 ProbeAttempt = 0u; ProbeAttempt < RequiredHashCapacity; ++ProbeAttempt)
            {
                FDDGIProbeSpatialHashEntry& Entry = HashEntries[static_cast<int32>(Slot)];
                if (Entry.EncodedProbe < 0)
                {
                    Entry = FDDGIProbeSpatialHashEntry{
                        Cell.X,
                        Cell.Y,
                        Cell.Z,
                        static_cast<int32>(ProbeIndex * 8u + ProbeLevel) };
                    bInserted = true;
                    break;
                }
                if (Entry.X == Cell.X && Entry.Y == Cell.Y && Entry.Z == Cell.Z &&
                    (static_cast<uint32>(Entry.EncodedProbe) & 7u) == ProbeLevel)
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
                sizeof(FDDGIProbeSpatialHashEntry),
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
            Storage.ProbeSpatialHashLayoutHash = 0;
            Storage.ProbeSpatialHashSpacing = 0.0f;
            Storage.ProbeSpatialHashRetryCountdown = 60;
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to build sparse probe spatial hash; retrying in 60 scene updates."));
        }
        else
        {
            Storage.ProbeSpatialHashCapacity = RequiredHashCapacity;
            Storage.ProbeSpatialHashProbeCount = ProbeCountForHash;
            Storage.ProbeSpatialHashLayoutHash = ProbeLayoutHash;
            Storage.ProbeSpatialHashSpacing = SafeProbeSpacing;
            Storage.ProbeSpatialHashRetryCountdown = 0;
        }
    }

    // Precompute each probe's 26 neighboring probe indices. Slot order exactly
    // matches the shader's Z/Y/X traversal, excluding the center cell.
    const bool bNeighborListMatches =
        !bProbeLayoutChanged &&
        Storage.ProbeNeighborIndices.IsValid() &&
        Storage.ProbeNeighborIndicesSRV.IsValid() &&
        Storage.ProbeNeighborProbeCount == ProbeCountForHash &&
        Storage.ProbeNeighborLayoutHash == ProbeLayoutHash &&
        FMath::IsNearlyEqual(Storage.ProbeNeighborSpacing, SafeProbeSpacing, 1.0e-4f);
    if (bNeighborListMatches)
    {
        Storage.ProbeNeighborRetryCountdown = 0;
    }
    else if (Storage.ProbeNeighborRetryCountdown > 0)
    {
        --Storage.ProbeNeighborRetryCountdown;
    }
    else
    {
        Storage.ProbeNeighborIndices.SafeRelease();
        Storage.ProbeNeighborIndicesSRV.SafeRelease();
        Storage.ProbeNeighborProbeCount = 0;
        Storage.ProbeNeighborLayoutHash = 0;
        Storage.ProbeNeighborSpacing = 0.0f;

        const TMap<FDDGIProbeCellKey, uint32>& ProbeCellToIndex = Storage.ProbeCellToLogicalIndex;
        bool bBuildSucceeded = ProbeCellToIndex.Num() == static_cast<int32>(ProbeCountForHash);

        TResourceArray<uint32> NeighborEntries;
        const uint32 NeighborEntryCount = ProbeCountForHash * 26u;
        if (bBuildSucceeded)
        {
            NeighborEntries.SetNumUninitialized(static_cast<int32>(NeighborEntryCount));
            for (uint32 I = 0u; I < NeighborEntryCount; ++I)
            {
                NeighborEntries[static_cast<int32>(I)] = MAX_uint32;
            }
            for (uint32 ProbeIndex = 0u; ProbeIndex < ProbeCountForHash && bBuildSucceeded; ++ProbeIndex)
            {
                const FDDGIProbeCellKey& CenterKey = ProbeCellKeys[static_cast<int32>(ProbeIndex)];
                const FIntVector& CenterCell = CenterKey.Cell;
                const int32 ProbeLevel = static_cast<int32>(CenterKey.Level);
                const int32 LevelCellStep = DDGIGetSpatialLevelStep(ProbeLevel);
                uint32 Slot = 0u;
                for (int32 Z = -1; Z <= 1; ++Z)
                for (int32 Y = -1; Y <= 1; ++Y)
                for (int32 X = -1; X <= 1; ++X)
                {
                    if (X == 0 && Y == 0 && Z == 0) continue;
                    const int64 NeighborX = static_cast<int64>(CenterCell.X) +
                        static_cast<int64>(X) * LevelCellStep;
                    const int64 NeighborY = static_cast<int64>(CenterCell.Y) +
                        static_cast<int64>(Y) * LevelCellStep;
                    const int64 NeighborZ = static_cast<int64>(CenterCell.Z) +
                        static_cast<int64>(Z) * LevelCellStep;
                    if (NeighborX >= MIN_int32 && NeighborX <= MAX_int32 &&
                        NeighborY >= MIN_int32 && NeighborY <= MAX_int32 &&
                        NeighborZ >= MIN_int32 && NeighborZ <= MAX_int32)
                    {
                        const FIntVector NeighborCell(
                            static_cast<int32>(NeighborX),
                            static_cast<int32>(NeighborY),
                            static_cast<int32>(NeighborZ));
                        const FDDGIProbeCellKey NeighborKey{
                            NeighborCell, static_cast<uint32>(ProbeLevel) };
                        if (const uint32* NeighborIndex = ProbeCellToIndex.Find(NeighborKey))
                        {
                            NeighborEntries[static_cast<int32>(ProbeIndex * 26u + Slot)] = *NeighborIndex;
                        }
                    }
                    ++Slot;
                }
                // Probes on the spherical boundary legitimately have fewer than 26 neighbors.
                // Missing slots remain MAX_uint32 and are skipped by the gather shader.
            }
        }

        if (bBuildSucceeded && NeighborEntryCount > 0u)
        {
            FRHIResourceCreateInfo NeighborInfo(TEXT("DDGIRadianceCascade_ProbeNeighborIndices"));
            NeighborInfo.ResourceArray = &NeighborEntries;
            Storage.ProbeNeighborIndices = RHICreateStructuredBuffer(
                sizeof(uint32),
                static_cast<uint32>(NeighborEntries.GetResourceDataSize()),
                BUF_ShaderResource,
                NeighborInfo);
            if (Storage.ProbeNeighborIndices.IsValid())
            {
                Storage.ProbeNeighborIndicesSRV = RHICreateShaderResourceView(Storage.ProbeNeighborIndices);
            }
        }
        if (!Storage.ProbeNeighborIndices.IsValid() || !Storage.ProbeNeighborIndicesSRV.IsValid())
        {
            Storage.ProbeNeighborIndices.SafeRelease();
            Storage.ProbeNeighborIndicesSRV.SafeRelease();
            Storage.ProbeNeighborRetryCountdown = 60;
            UE_LOG(LogTemp, Warning, TEXT("Radiance Cascades: precomputed neighbor buffer unavailable; using spatial-hash fallback (retry in 60 scene updates)."));
        }
        else
        {
            Storage.ProbeNeighborProbeCount = ProbeCountForHash;
            Storage.ProbeNeighborLayoutHash = ProbeLayoutHash;
            Storage.ProbeNeighborSpacing = SafeProbeSpacing;
            Storage.ProbeNeighborRetryCountdown = 0;
        }
    }

    // Upload the immutable CPU BVH once per snapshot. Master Actor movement
    // republishes probe origins but keeps the same shared BVH pointer.
    FDDGISoftwareBVHData* CurrentBVH = Config.SoftwareBVH.Get();
    const bool bBVHSourceChanged = Storage.BVHDataReference.Get() != CurrentBVH;
    const bool bBVHResourcesReady =
        Storage.BVHNodesSRV.IsValid() && Storage.BVHTrianglesSRV.IsValid();
    if (bBVHSourceChanged)
    {
        // Radiance and hit-distance history was traced against the previous
        // geometry snapshot. Reinitialize it before tracing with the new BVH.
        Storage.bInvalidateAllPhysicalSlots = true;
        Storage.bNeedsInitialization = true;
        Storage.BVHRetryCountdown = 0;
    }
    else if (!bBVHResourcesReady && Storage.BVHRetryCountdown > 0)
    {
        --Storage.BVHRetryCountdown;
    }

    if (bBVHSourceChanged || (!bBVHResourcesReady && Storage.BVHRetryCountdown == 0))
    {
        Storage.BVHNodes.SafeRelease();
        Storage.BVHNodesSRV.SafeRelease();
        Storage.BVHTriangles.SafeRelease();
        Storage.BVHTrianglesSRV.SafeRelease();
        Storage.BVHNodeCount = 0;
        Storage.BVHTriangleCount = 0;
        // Keep the exact CPU snapshot alive for as long as these GPU buffers
        // represent it. The config may be replaced before the next render update.
        Storage.BVHDataReference = Config.SoftwareBVH;

        if (CurrentBVH && CurrentBVH->NodeCount > 0 && CurrentBVH->TriangleCount > 0 &&
            static_cast<int64>(CurrentBVH->PackedNodes.Num()) >= static_cast<int64>(CurrentBVH->NodeCount) * 3ll &&
            static_cast<int64>(CurrentBVH->PackedTriangles.Num()) >= static_cast<int64>(CurrentBVH->TriangleCount) * 4ll)
        {
            FRHIResourceCreateInfo NodeCreateInfo(TEXT("DDGIRadianceCascade_SoftwareBVHNodes"));
            NodeCreateInfo.ResourceArray = &CurrentBVH->PackedNodes;
            Storage.BVHNodes = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(CurrentBVH->PackedNodes.GetResourceDataSize()),
                BUF_ShaderResource,
                NodeCreateInfo);

            FRHIResourceCreateInfo TriangleCreateInfo(TEXT("DDGIRadianceCascade_SoftwareBVHTriangles"));
            TriangleCreateInfo.ResourceArray = &CurrentBVH->PackedTriangles;
            Storage.BVHTriangles = RHICreateStructuredBuffer(
                sizeof(FVector4),
                static_cast<uint32>(CurrentBVH->PackedTriangles.GetResourceDataSize()),
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
                static_cast<uint32>(EmptyNodeData.GetResourceDataSize()),                BUF_ShaderResource,
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

        if (Storage.BVHNodesSRV.IsValid() && Storage.BVHTrianglesSRV.IsValid())
        {
            Storage.BVHRetryCountdown = 0;
        }
        else
        {
            Storage.BVHRetryCountdown = 60;
            UE_LOG(LogTemp, Warning, TEXT("Radiance Cascades: BVH GPU resources unavailable; retrying in 60 scene updates."));
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

bool DDGIBeginRadianceCascadeUpdate_RenderThread(const FSceneInterface* Scene, uint32 FrameNumber)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());

	// FSceneViewFamily uses MAX_uint32 before its frame number is initialized.
	if (!Scene || FrameNumber == MAX_uint32)
	{
		return false;
	}

	FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
	if (!Storage || Storage->LastRadianceUpdateFrameNumber == FrameNumber)
	{
		return false;
	}

	// The render thread serializes these callbacks, so claiming the frame here
	// prevents stereo eyes and additional views from dispatching the same GI work.
	Storage->LastRadianceUpdateFrameNumber = FrameNumber;

	// Retention age is measured in rendered DDGI frames, not config publishes.
	// Resource updates are only published when the active probe window recenters,
	// which can be much less frequent than the render loop.
	++Storage->ProbeUpdateSerial;
	if (Storage->ProbeUpdateSerial == 0u)
	{
		// Avoid zero after uint32 wrap without changing physical slot identities
		// or their accumulated GPU history.
		Storage->ProbeUpdateSerial = 1u;
		for (auto It = Storage->ProbeCellToPhysicalSlot.CreateIterator(); It; ++It)
		{
			It.Value().LastSeenUpdate = 1u;
		}
	}
	return true;
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
        Storage->bPendingPhysicalSlotsUpload || Storage->bInvalidateAllPhysicalSlots ||
        Storage->NewlyAllocatedPhysicalSlotsCPU.Num() == 0 ||
        !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() || !Storage->HitDistance.IsValid())
    {
        return false;
    }
    OutDirections = GraphBuilder.RegisterExternalTexture(Storage->Directions);
    OutRadiance = GraphBuilder.RegisterExternalTexture(Storage->Radiance);
    OutHitDistance = GraphBuilder.RegisterExternalTexture(Storage->HitDistance);
    return OutDirections && OutRadiance && OutHitDistance;
}

bool DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutDirections,
    FTexture2DRHIRef& OutRadiance,
    FTexture2DRHIRef& OutHitDistance,
    FTexture2DRHIRef& OutRadianceScratch,
    FTexture2DRHIRef& OutRadianceIntegrationScratch,
    FTexture2DRHIRef& OutHitTriangleIndex,
    uint32& OutPendingSlotCount,
    bool& bOutInitializeDirections)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutDirections.SafeRelease();
    OutRadiance.SafeRelease();
    OutHitDistance.SafeRelease();
    OutRadianceScratch.SafeRelease();
    OutRadianceIntegrationScratch.SafeRelease();
    OutHitTriangleIndex.SafeRelease();
    OutPendingSlotCount = 0;
    bOutInitializeDirections = false;
    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->bNeedsInitialization ||
        Storage->bPendingPhysicalSlotsUpload || Storage->bInvalidateAllPhysicalSlots ||
        (Storage->NewlyAllocatedPhysicalSlotsCPU.Num() == 0 && !Storage->bNeedsDirectionInitialization) ||
        !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() ||
        !Storage->HitDistance.IsValid() || !Storage->RadianceScratch.IsValid() ||
        !Storage->RadianceIntegrationScratch.IsValid() || !Storage->HitTriangleIndex.IsValid() || Storage->UploadedProbeCount <= 0)
    {
        return false;
    }
    const FSceneRenderTargetItem& DirectionsItem = Storage->Directions->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceItem = Storage->Radiance->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitDistanceItem = Storage->HitDistance->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceScratchItem = Storage->RadianceScratch->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceIntegrationScratchItem = Storage->RadianceIntegrationScratch->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitTriangleIndexItem = Storage->HitTriangleIndex->GetRenderTargetItem();
    if (!DirectionsItem.ShaderResourceTexture.IsValid() ||
        !RadianceItem.ShaderResourceTexture.IsValid() ||
        !HitDistanceItem.ShaderResourceTexture.IsValid() ||
        !RadianceScratchItem.ShaderResourceTexture.IsValid() ||
        !RadianceIntegrationScratchItem.ShaderResourceTexture.IsValid() ||
        !HitTriangleIndexItem.ShaderResourceTexture.IsValid())
    {
        return false;
    }
    OutDirections = DirectionsItem.ShaderResourceTexture->GetTexture2D();
    OutRadiance = RadianceItem.ShaderResourceTexture->GetTexture2D();
    OutHitDistance = HitDistanceItem.ShaderResourceTexture->GetTexture2D();
    OutRadianceScratch = RadianceScratchItem.ShaderResourceTexture->GetTexture2D();
    OutRadianceIntegrationScratch = RadianceIntegrationScratchItem.ShaderResourceTexture->GetTexture2D();
    OutHitTriangleIndex = HitTriangleIndexItem.ShaderResourceTexture->GetTexture2D();
    OutPendingSlotCount = static_cast<uint32>(Storage->NewlyAllocatedPhysicalSlotsCPU.Num());
    bOutInitializeDirections = Storage->bNeedsDirectionInitialization;
    return OutDirections.IsValid() && OutRadiance.IsValid() && OutHitDistance.IsValid() &&
        OutRadianceScratch.IsValid() && OutRadianceIntegrationScratch.IsValid() &&
        OutHitTriangleIndex.IsValid();
}

bool DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutDirections,
    FTexture2DRHIRef& OutRadiance,
    FTexture2DRHIRef& OutHitDistance,
    FTexture2DRHIRef& OutHitTriangleIndex,
    FTexture2DRHIRef& OutProbePositions,
    uint32& OutProbeCount,
    uint32& OutTotalRaysPerProbe)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutDirections.SafeRelease();
    OutRadiance.SafeRelease();
    OutHitDistance.SafeRelease();
    OutHitTriangleIndex.SafeRelease();
    OutProbePositions.SafeRelease();
    OutProbeCount = 0;
    OutTotalRaysPerProbe = 0;
    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() ||
        !Storage->HitDistance.IsValid() || !Storage->HitTriangleIndex.IsValid() ||
        !Storage->ProbePositions.IsValid() ||
        Storage->UploadedProbeCount <= 0 || Storage->TotalRaysPerProbe <= 0)
    {
        return false;
    }
    const FSceneRenderTargetItem& DirectionsItem = Storage->Directions->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceItem = Storage->Radiance->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitDistanceItem = Storage->HitDistance->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitTriangleIndexItem = Storage->HitTriangleIndex->GetRenderTargetItem();
    const FSceneRenderTargetItem& ProbePositionsItem = Storage->ProbePositions->GetRenderTargetItem();
    if (!DirectionsItem.ShaderResourceTexture.IsValid() ||
        !RadianceItem.ShaderResourceTexture.IsValid() ||
        !HitDistanceItem.ShaderResourceTexture.IsValid() ||
        !HitTriangleIndexItem.ShaderResourceTexture.IsValid() ||
        !ProbePositionsItem.ShaderResourceTexture.IsValid())
    {
        return false;
    }
    OutDirections = DirectionsItem.ShaderResourceTexture->GetTexture2D();
    OutRadiance = RadianceItem.ShaderResourceTexture->GetTexture2D();
    OutHitDistance = HitDistanceItem.ShaderResourceTexture->GetTexture2D();
    OutHitTriangleIndex = HitTriangleIndexItem.ShaderResourceTexture->GetTexture2D();
    OutProbePositions = ProbePositionsItem.ShaderResourceTexture->GetTexture2D();
    OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
    OutTotalRaysPerProbe = static_cast<uint32>(Storage->TotalRaysPerProbe);
    return OutDirections.IsValid() && OutRadiance.IsValid() &&
        OutHitDistance.IsValid() && OutHitTriangleIndex.IsValid() && OutProbePositions.IsValid();
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

bool DDGIGetRadianceCascadeIntegrationScratchTexture_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutRadianceIntegrationScratch)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutRadianceIntegrationScratch.SafeRelease();
    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->RadianceIntegrationScratch.IsValid())
    {
        return false;
    }
    const FSceneRenderTargetItem& Item = Storage->RadianceIntegrationScratch->GetRenderTargetItem();
    if (!Item.ShaderResourceTexture.IsValid())
    {
        return false;
    }
    OutRadianceIntegrationScratch = Item.ShaderResourceTexture->GetTexture2D();
    return OutRadianceIntegrationScratch.IsValid();
}

void DDGISwapRadianceCascadeRadianceBuffers_RenderThread(const FSceneInterface* Scene)
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
	if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
	{
		Swap(Storage->Radiance, Storage->RadianceScratch);
	}
}

void DDGISwapRadianceCascadeIntegrationBuffers_RenderThread(const FSceneInterface* Scene)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
    {
        Swap(Storage->Radiance, Storage->RadianceIntegrationScratch);
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

bool DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutLogicalToPhysicalSRV,
    FShaderResourceViewRHIRef& OutNewPhysicalSlotsSRV,
    uint32& OutNewPhysicalSlotCount,
    uint32& OutPhysicalSlotCapacity)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutLogicalToPhysicalSRV.SafeRelease();
    OutNewPhysicalSlotsSRV.SafeRelease();
    OutNewPhysicalSlotCount = 0;
    OutPhysicalSlotCapacity = 0;

    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage)
    {
        static bool bLoggedMissingStorage = false;
        if (!bLoggedMissingStorage)
        {
            UE_LOG(LogTemp, Error, TEXT("DDGI MAP DIAG: no GPU storage for Scene=%p."), Scene);
            bLoggedMissingStorage = true;
        }
        return false;
    }

    const bool bPendingUpload = Storage->bPendingPhysicalSlotsUpload;
    const bool bInvalidatingAll = Storage->bInvalidateAllPhysicalSlots;
    const bool bInitializationWithoutNewSlots =
        Storage->bNeedsInitialization && Storage->NewlyAllocatedPhysicalSlotsCPU.Num() == 0;
    const bool bMissingLogicalSRV = !Storage->LogicalToPhysicalSlotsSRV.IsValid();
    const bool bMissingNewSlotsSRV = !Storage->NewlyAllocatedPhysicalSlotsSRV.IsValid();
    const bool bLogicalCountMismatch =
        Storage->LogicalToPhysicalSlotCPU.Num() != Storage->UploadedProbeCount;
    const bool bNoPhysicalCapacity = Storage->PhysicalSlotCapacity == 0;
    if (bPendingUpload || bInvalidatingAll || bInitializationWithoutNewSlots ||
        bMissingLogicalSRV || bMissingNewSlotsSRV || bLogicalCountMismatch || bNoPhysicalCapacity)
    {
        static uint32 LastLoggedFailureMask = MAX_uint32;
        const uint32 FailureMask =
            (bPendingUpload ? 1u : 0u) |
            (bInvalidatingAll ? 2u : 0u) |
            (bInitializationWithoutNewSlots ? 4u : 0u) |
            (bMissingLogicalSRV ? 8u : 0u) |
            (bMissingNewSlotsSRV ? 16u : 0u) |
            (bLogicalCountMismatch ? 32u : 0u) |
            (bNoPhysicalCapacity ? 64u : 0u);
        if (FailureMask != LastLoggedFailureMask)
        {
            UE_LOG(LogTemp, Error,
                TEXT("DDGI MAP DIAG: mapping unavailable mask=0x%02X [pendingUpload=%d invalidateAll=%d initWithoutNewSlots=%d missingLogicalSRV=%d missingNewSlotsSRV=%d logicalCountMismatch=%d noCapacity=%d], logical=%d uploaded=%d newSlots=%d capacity=%u scene=%p."),
                FailureMask,
                bPendingUpload ? 1 : 0, bInvalidatingAll ? 1 : 0,
                bInitializationWithoutNewSlots ? 1 : 0,
                bMissingLogicalSRV ? 1 : 0, bMissingNewSlotsSRV ? 1 : 0,
                bLogicalCountMismatch ? 1 : 0, bNoPhysicalCapacity ? 1 : 0,
                Storage->LogicalToPhysicalSlotCPU.Num(), Storage->UploadedProbeCount,
                Storage->NewlyAllocatedPhysicalSlotsCPU.Num(),
                Storage->PhysicalSlotCapacity, Scene);
            LastLoggedFailureMask = FailureMask;
        }
        return false;
    }

    OutLogicalToPhysicalSRV = Storage->LogicalToPhysicalSlotsSRV;
    OutNewPhysicalSlotsSRV = Storage->NewlyAllocatedPhysicalSlotsSRV;
    OutNewPhysicalSlotCount = static_cast<uint32>(Storage->NewlyAllocatedPhysicalSlotsCPU.Num());
    OutPhysicalSlotCapacity = Storage->PhysicalSlotCapacity;
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

bool DDGIGetRadianceCascadeProbeNeighborList_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutNeighborListSRV,
    uint32& OutProbeCount)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutNeighborListSRV.SafeRelease();
    OutProbeCount = 0;
    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->ProbeNeighborIndicesSRV.IsValid() ||
        Storage->ProbeNeighborProbeCount == 0 ||
        Storage->ProbeNeighborProbeCount != static_cast<uint32>(Storage->UploadedProbeCount))
    {
        return false;
    }
    OutNeighborListSRV = Storage->ProbeNeighborIndicesSRV;
    OutProbeCount = Storage->ProbeNeighborProbeCount;
    return true;
}

bool DDGIGetRadianceCascadeLightingGeometry_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutSpatialHashSRV,
    FShaderResourceViewRHIRef& OutBVHTrianglesSRV,
    uint32& OutHashCapacity,
    uint32& OutTriangleCount,
    float& OutProbeSpacing)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());

    OutSpatialHashSRV.SafeRelease();
    OutBVHTrianglesSRV.SafeRelease();
    OutHashCapacity = 0;
    OutTriangleCount = 0;
    OutProbeSpacing = 0.0f;

    FShaderResourceViewRHIRef NodesSRV;
    uint32 NodeCount = 0;
    if (!DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
            Scene, OutSpatialHashSRV, OutHashCapacity, OutProbeSpacing) ||
        !DDGIGetSoftwareBVHResources_RenderThread(
            Scene, NodesSRV, OutBVHTrianglesSRV, NodeCount, OutTriangleCount) ||
        !OutSpatialHashSRV.IsValid() ||
        !OutBVHTrianglesSRV.IsValid() ||
        OutHashCapacity == 0 ||
        NodeCount == 0 ||
        OutTriangleCount == 0)
    {
        OutSpatialHashSRV.SafeRelease();
        OutBVHTrianglesSRV.SafeRelease();
        OutHashCapacity = 0;
        OutTriangleCount = 0;
        OutProbeSpacing = 0.0f;
        return false;
    }

    return true;
}

bool DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(
    const FSceneInterface* Scene, FTexture2DRHIRef& OutProbePositions, uint32& OutProbeCount)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutProbePositions.SafeRelease();
    OutProbeCount = 0;
    const FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->ProbePositions.IsValid() || Storage->UploadedProbeCount <= 0) return false;
    FTextureRHIRef Texture = Storage->ProbePositions->GetRenderTargetItem().ShaderResourceTexture;
    if (!Texture.IsValid()) return false;
    OutProbePositions = Texture->GetTexture2D();
    OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
    return OutProbePositions.IsValid();
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

bool DDGIGetRadianceCascadeLightingTextures_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTextureRef& OutDirections,
    FRDGTextureRef& OutRadiance,
    FRDGTextureRef& OutHitDistance,
    FRDGTextureRef& OutHitTriangleIndex,
    FRDGTextureRef& OutProbePositions,
    uint32& OutProbeCount,
    uint32& OutTotalRaysPerProbe)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());

    OutDirections = nullptr;
    OutRadiance = nullptr;
    OutHitDistance = nullptr;
    OutHitTriangleIndex = nullptr;
    OutProbePositions = nullptr;
    OutProbeCount = 0;
    OutTotalRaysPerProbe = 0;

    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage ||
        !Storage->Directions.IsValid() ||
        !Storage->Radiance.IsValid() ||
        !Storage->HitDistance.IsValid() ||
        !Storage->HitTriangleIndex.IsValid() ||
        !Storage->ProbePositions.IsValid() ||
        Storage->bNeedsInitialization ||
        Storage->UploadedProbeCount <= 0 ||
        Storage->TotalRaysPerProbe <= 0)
    {
        return false;
    }

    // The integration loop promotes whichever ping-pong output contains the
    // completed first-cascade result, so Radiance always identifies that result.
    OutDirections = GraphBuilder.RegisterExternalTexture(Storage->Directions);
    OutRadiance = GraphBuilder.RegisterExternalTexture(Storage->Radiance);
    OutHitDistance = GraphBuilder.RegisterExternalTexture(Storage->HitDistance);
    OutHitTriangleIndex = GraphBuilder.RegisterExternalTexture(Storage->HitTriangleIndex);
    OutProbePositions = GraphBuilder.RegisterExternalTexture(Storage->ProbePositions);
    OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
    OutTotalRaysPerProbe = static_cast<uint32>(Storage->TotalRaysPerProbe);

    return OutDirections != nullptr && OutRadiance != nullptr &&
        OutHitDistance != nullptr && OutHitTriangleIndex != nullptr &&
        OutProbePositions != nullptr;
}

void DDGIInvalidateRadianceCascadeHistory_RenderThread(const FSceneInterface* Scene)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
    {
        Storage->bInvalidateAllPhysicalSlots = true;
        Storage->bNeedsInitialization = true;
    }
}

void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
    {
        Storage->NewlyAllocatedPhysicalSlotsCPU.Reset();
        Storage->bNeedsDirectionInitialization = false;
        Storage->bInvalidateAllPhysicalSlots = false;
        Storage->bNeedsInitialization = false;
    }
}
