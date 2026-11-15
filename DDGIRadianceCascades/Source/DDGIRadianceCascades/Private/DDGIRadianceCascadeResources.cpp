#include "DDGIRadianceCascadeResources.h"
#include "DDGIMasterSubsystem.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderTargetPool.h"
#include "RenderGraph.h"
#include "PostProcess/SceneRenderTargets.h"
#include "SceneInterface.h"
#include "Containers/ResourceArray.h"

namespace
{
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
}

namespace
{
	static const int32 GDDGIRadianceCascadeTextureWidth = 4096;
	static const int32 GDDGIRadianceCascadeMaxTextureDimension = 16384;
	// Four per-probe ray textures consume 24 bytes per ray record.
	// Directions are stored once per ray layout (8 bytes per direction), shared
	// by all probes. Bound this subsystem independently of RHI texture limits.
	static const int64 GDDGIRadianceCascadeMaxRayStorageBytes = 512ll * 1024ll * 1024ll;
	static const int64 GDDGIRadianceCascadeBytesPerProbeRay = 24ll;
	static const int64 GDDGIRadianceCascadeBytesPerSharedDirection = 8ll;

	struct FDDGIRadianceCascadeGPUStorage
	{
		TRefCountPtr<IPooledRenderTarget> Directions;
		TRefCountPtr<IPooledRenderTarget> Radiance;
		TRefCountPtr<IPooledRenderTarget> RadianceScratch;
		TRefCountPtr<IPooledRenderTarget> HitDistance;
		TRefCountPtr<IPooledRenderTarget> HitTriangleIndex;
		TRefCountPtr<IPooledRenderTarget> ProbePositions;
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
		FIntPoint Dimensions = FIntPoint::ZeroValue;
		FIntPoint DirectionDimensions = FIntPoint::ZeroValue;
		int32 TotalRaysPerProbe = 0;
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
			HitDistance.SafeRelease();
			HitTriangleIndex.SafeRelease();
			ProbePositions.SafeRelease();
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
			bNeedsInitialization = true;
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
    // Per-probe history uses four textures (24 bytes/texel); the direction
    // texture has its own row padding and is allocated only once per ray layout.
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
    const bool bRayLayoutChanged =
        Storage.TotalRaysPerProbe != 0 &&
        Storage.TotalRaysPerProbe != Config.TotalRaysPerProbe;
    const int64 ExistingProbeCapacity =
        (!bRayLayoutChanged && Storage.RayProbeCapacity > 0)
        ? static_cast<int64>(Storage.RayProbeCapacity)
        : 0;
    const int64 RequiredProbeCapacity = FMath::Max<int64>(ActiveProbeCount, ExistingProbeCapacity);
    const int64 RequiredCapacityTexels = RequiredProbeCapacity * static_cast<int64>(Config.TotalRaysPerProbe);
    const int64 CapacityHeight64 = (RequiredCapacityTexels + GDDGIRadianceCascadeTextureWidth - 1) /
        GDDGIRadianceCascadeTextureWidth;
    const FIntPoint RequiredDimensions(GDDGIRadianceCascadeTextureWidth, static_cast<int32>(CapacityHeight64));
    const FIntPoint RequiredDirectionDimensions(GDDGIRadianceCascadeTextureWidth, static_cast<int32>(DirectionHeight64));
    const bool bRayStorageMatches =
        Storage.Dimensions == RequiredDimensions &&
        Storage.DirectionDimensions == RequiredDirectionDimensions &&
        Storage.TotalRaysPerProbe == Config.TotalRaysPerProbe &&
        Storage.Directions.IsValid() &&
        Storage.Radiance.IsValid() &&
        Storage.RadianceScratch.IsValid() &&
        Storage.HitDistance.IsValid() &&
        Storage.HitTriangleIndex.IsValid();

    if (!bRayStorageMatches)
    {
        Storage.Directions.SafeRelease();
        Storage.Radiance.SafeRelease();
        Storage.RadianceScratch.SafeRelease();
        Storage.HitDistance.SafeRelease();
        Storage.HitTriangleIndex.SafeRelease();
        Storage.Dimensions = FIntPoint::ZeroValue;
        Storage.DirectionDimensions = FIntPoint::ZeroValue;
        Storage.TotalRaysPerProbe = 0;
        Storage.RayProbeCapacity = 0;
        Storage.bNeedsInitialization = true;

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
        GRenderTargetPool.FindFreeElement(RHICmdList, DistanceDesc, Storage.HitDistance,
            TEXT("DDGIRadianceCascade_HitDistance"), ERenderTargetTransience::NonTransient);
        GRenderTargetPool.FindFreeElement(RHICmdList, HitTriangleIndexDesc, Storage.HitTriangleIndex,
            TEXT("DDGIRadianceCascade_HitTriangleIndex"), ERenderTargetTransience::NonTransient);
        if (!Storage.Directions.IsValid() || !Storage.Radiance.IsValid() || !Storage.RadianceScratch.IsValid() || !Storage.HitDistance.IsValid() || !Storage.HitTriangleIndex.IsValid())
        {
            Storage.Release();
            UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to allocate persistent GPU textures."));
            return;
        }

        Storage.Dimensions = RequiredDimensions;
        Storage.DirectionDimensions = RequiredDirectionDimensions;
        Storage.TotalRaysPerProbe = Config.TotalRaysPerProbe;
        // The final texture row is fully allocated, even when the active ray
        // count does not fill it. Count every complete probe-sized range that
        // physically fits in the allocated texture, not only active probes.
        const int64 AllocatedRayTexels =
            static_cast<int64>(Storage.Dimensions.X) * Storage.Dimensions.Y;
        Storage.RayProbeCapacity = static_cast<int32>(
            AllocatedRayTexels / Config.TotalRaysPerProbe);
    }

    // Keep grid-relative probe offsets in a separate float4 texture. A pure
    // Master Actor translation changes only ProbeGridOrigin and does not require
    // rewriting this texture.
    const int32 ProbeTextureWidth = 4096;
    // Probe positions are metadata, not ray history. Reserve the configured
    // maximum active-probe capacity up front so ordinary window growth does
    // not recreate this texture and invalidate the uploaded-layout snapshot.
    // At the hard limit of 32768 probes this is only an 8-row RGBA32F texture.
    const int32 ProbeTextureHeight = FMath::Max(
        1, FMath::DivideAndRoundUp(Config.MaxActiveProbes, ProbeTextureWidth));
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
    const bool bProbePositionsMatch =
        Storage.ProbePositions.IsValid() &&
        Storage.UploadedProbeCount == ProbeCount &&
        Storage.UploadedProbeLayoutHash == ProbeLayoutHash &&
        FMath::IsNearlyEqual(Storage.UploadedProbeSpacing, SafeProbeSpacing, 1.0e-4f);
    const bool bProbeLayoutChanged = !bProbePositionsMatch;

    if (bProbeLayoutChanged)
    {
        const float LatticeTolerance = FMath::Max(0.01f, SafeProbeSpacing * 1.0e-4f);
        Storage.ProbeCellKeys.SetNumUninitialized(ActiveProbeCount);
        Storage.ProbeCellToLogicalIndex.Reset();
        Storage.ProbeCellToLogicalIndex.Reserve(ActiveProbeCount);
        for (int32 ProbeIndex = 0; ProbeIndex < ActiveProbeCount; ++ProbeIndex)
        {
            const FVector4& Position = Config.ProbePositions[ProbeIndex];
            const int32 ProbeLevel = FMath::RoundToInt(Position.W);
            if (ProbeLevel < 0 || ProbeLevel > 7 ||
                !FMath::IsNearlyEqual(Position.W, static_cast<float>(ProbeLevel), 1.0e-3f))
            {
                DDGIReleaseRadianceCascadeResources_RenderThread(Scene);
                UE_LOG(LogTemp, Error,
                    TEXT("Radiance Cascades: probe %d has invalid spatial level %.3f; supported levels are integer values 0..7."),
                    ProbeIndex, Position.W);
                return;
            }

            const int32 LevelCellStep = 1 << ProbeLevel;
            const FIntVector Cell(
                FMath::RoundToInt(Position.X / SafeProbeSpacing),
                FMath::RoundToInt(Position.Y / SafeProbeSpacing),
                FMath::RoundToInt(Position.Z / SafeProbeSpacing));
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
                FMath::IsNearlyEqual(Position.X, static_cast<float>(Cell.X) * SafeProbeSpacing, LatticeTolerance) &&
                FMath::IsNearlyEqual(Position.Y, static_cast<float>(Cell.Y) * SafeProbeSpacing, LatticeTolerance) &&
                FMath::IsNearlyEqual(Position.Z, static_cast<float>(Cell.Z) * SafeProbeSpacing, LatticeTolerance);
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

    // Probe positions currently describe a fixed world-origin lattice. They are not
    // recentered around the camera, and changing ProbeGridOrigin alone would only
    // translate their world-space locations. A future camera-selected active window
    // must preserve stable cell identity and initialize only newly assigned slots.
    // Ray storage recreation also sets this flag.
    // The change flag was captured before the upload, so a failed hash
    // allocation cannot trigger repeated initialization on an unchanged layout.
    if (bProbeLayoutChanged)
    {
        Storage.bNeedsInitialization = true;
        // A different layout is a new allocation target, so do not inherit
        // backoff from failures associated with the previous probe lattice.
        Storage.ProbeSpatialHashRetryCountdown = 0;
        Storage.ProbeNeighborRetryCountdown = 0;
    }

    const bool bProbeHashMatches =
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
                const int32 LevelCellStep = 1 << ProbeLevel;
                uint32 Slot = 0u;
                for (int32 Z = -1; Z <= 1; ++Z)
                for (int32 Y = -1; Y <= 1; ++Y)
                for (int32 X = -1; X <= 1; ++X)
                {
                    if (X == 0 && Y == 0 && Z == 0) continue;
                    const FIntVector NeighborCell = CenterCell + FIntVector(X, Y, Z) * LevelCellStep;
                    const FDDGIProbeCellKey NeighborKey{ NeighborCell, static_cast<uint32>(ProbeLevel) };
                    if (const uint32* NeighborIndex = ProbeCellToIndex.Find(NeighborKey))
                    {
                        NeighborEntries[static_cast<int32>(ProbeIndex * 26u + Slot)] = *NeighborIndex;
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
    FTexture2DRHIRef& OutHitTriangleIndex,
    uint32& OutProbeCount)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    OutDirections.SafeRelease();
    OutRadiance.SafeRelease();
    OutHitDistance.SafeRelease();
    OutRadianceScratch.SafeRelease();
    OutHitTriangleIndex.SafeRelease();
    OutProbeCount = 0;
    FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene);
    if (!Storage || !Storage->bNeedsInitialization ||
        !Storage->Directions.IsValid() || !Storage->Radiance.IsValid() ||
        !Storage->HitDistance.IsValid() || !Storage->RadianceScratch.IsValid() ||
        !Storage->HitTriangleIndex.IsValid() || Storage->UploadedProbeCount <= 0)
    {
        return false;
    }
    const FSceneRenderTargetItem& DirectionsItem = Storage->Directions->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceItem = Storage->Radiance->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitDistanceItem = Storage->HitDistance->GetRenderTargetItem();
    const FSceneRenderTargetItem& RadianceScratchItem = Storage->RadianceScratch->GetRenderTargetItem();
    const FSceneRenderTargetItem& HitTriangleIndexItem = Storage->HitTriangleIndex->GetRenderTargetItem();
    if (!DirectionsItem.ShaderResourceTexture.IsValid() ||
        !RadianceItem.ShaderResourceTexture.IsValid() ||
        !HitDistanceItem.ShaderResourceTexture.IsValid() ||
        !RadianceScratchItem.ShaderResourceTexture.IsValid() ||
        !HitTriangleIndexItem.ShaderResourceTexture.IsValid())
    {
        return false;
    }
    OutDirections = DirectionsItem.ShaderResourceTexture->GetTexture2D();
    OutRadiance = RadianceItem.ShaderResourceTexture->GetTexture2D();
    OutHitDistance = HitDistanceItem.ShaderResourceTexture->GetTexture2D();
    OutRadianceScratch = RadianceScratchItem.ShaderResourceTexture->GetTexture2D();
    OutHitTriangleIndex = HitTriangleIndexItem.ShaderResourceTexture->GetTexture2D();
    OutProbeCount = static_cast<uint32>(Storage->UploadedProbeCount);
    return OutDirections.IsValid() && OutRadiance.IsValid() && OutHitDistance.IsValid() &&
        OutRadianceScratch.IsValid() && OutHitTriangleIndex.IsValid();
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

    // The integration loop swaps the persistent Radiance/RadianceScratch
    // references when necessary, so Radiance always identifies the completed result.
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
        Storage->bNeedsInitialization = true;
    }
}

void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene)
{
    check(IsInRenderingThread() || IsInParallelRenderingThread());
    if (FDDGIRadianceCascadeGPUStorage* Storage = GDDGIRadianceCascadeStorage_RenderThread.Find(Scene))
    {
        Storage->bNeedsInitialization = false;
    }
}
