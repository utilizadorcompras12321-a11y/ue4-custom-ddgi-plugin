#pragma once

#include "CoreMinimal.h"
#include "RenderGraph.h"
#include "RHIResources.h"

class FSceneInterface;
struct FDDGIMasterConfig;
class FRHICommandListImmediate;

/** Persistent per-scene GPU storage for packed Radiance Cascades ray segments. */
void DDGIUpdateRadianceCascadeResources_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const FSceneInterface* Scene,
	const FDDGIMasterConfig& Config);

void DDGIReleaseRadianceCascadeResources_RenderThread(const FSceneInterface* Scene);

/** GPU-resident storage reserved for the surface-probe allocator. */
struct FDDGISurfaceProbeAllocatorRHIResources
{
    FShaderResourceViewRHIRef PositionsSRV;
    FUnorderedAccessViewRHIRef PositionsUAV;
    FShaderResourceViewRHIRef NormalsSRV;
    FUnorderedAccessViewRHIRef NormalsUAV;
    FShaderResourceViewRHIRef StableKeysSRV;
    FUnorderedAccessViewRHIRef StableKeysUAV;
    FShaderResourceViewRHIRef LastSeenFramesSRV;
    FUnorderedAccessViewRHIRef LastSeenFramesUAV;
    FShaderResourceViewRHIRef ActiveIndicesSRV;
    FUnorderedAccessViewRHIRef ActiveIndicesUAV;
    FShaderResourceViewRHIRef HashSRV;
    FUnorderedAccessViewRHIRef HashUAV;
    FShaderResourceViewRHIRef CountersSRV;
    FUnorderedAccessViewRHIRef CountersUAV;
    uint32 ProbeCapacity = 0u;
    uint32 HashCapacity = 0u;
};

bool DDGIGetSurfaceProbeAllocatorResources_RenderThread(
    const FSceneInterface* Scene,
    FDDGISurfaceProbeAllocatorRHIResources& OutResources);

/** RDG views of the persistent surface-probe allocator buffers. */
struct FDDGISurfaceProbeAllocatorRDGResources
{
    FRDGBufferRef Positions = nullptr;
    FRDGBufferRef Normals = nullptr;
    FRDGBufferRef StableKeys = nullptr;
    FRDGBufferRef LastSeenFrames = nullptr;
    FRDGBufferRef ActiveIndices = nullptr;
    FRDGBufferRef Hash = nullptr;
    FRDGBufferRef Counters = nullptr;
    uint32 ProbeCapacity = 0u;
    uint32 HashCapacity = 0u;
};

bool DDGIRegisterSurfaceProbeAllocatorBuffers_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FDDGISurfaceProbeAllocatorRDGResources& OutResources);

/** Claims this scene's DDGI update slot for the supplied render frame. */
bool DDGIBeginRadianceCascadeUpdate_RenderThread(const FSceneInterface* Scene, uint32 FrameNumber);

bool DDGIGetRadianceCascadeInitializationTextures_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTextureRef& OutDirections,
    FRDGTextureRef& OutRadiance,
    FRDGTextureRef& OutHitDistance);

bool DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutDirections,
    FTexture2DRHIRef& OutRadiance,
    FTexture2DRHIRef& OutHitDistance,
    FTexture2DRHIRef& OutRadianceScratch,
    FTexture2DRHIRef& OutRadianceIntegrationScratch,
    FTexture2DRHIRef& OutHitTriangleIndex,
    uint32& OutPendingSlotCount,
    bool& bOutInitializeDirections);

bool DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutDirections,
    FTexture2DRHIRef& OutRadiance,
    FTexture2DRHIRef& OutHitDistance,
    FTexture2DRHIRef& OutHitTriangleIndex,
    FTexture2DRHIRef& OutProbePositions,
    uint32& OutProbeCount,
    uint32& OutTotalRaysPerProbe);

/** Returns the immutable Gather output texture used by cascade integration. */
bool DDGIGetRadianceCascadeScratchTexture_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutRadianceScratch);

/** Returns the second output buffer used to ping-pong integrated cascade results. */
bool DDGIGetRadianceCascadeIntegrationScratchTexture_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutRadianceIntegrationScratch);

/** Promotes the Gather output when no integration dispatch is required. */
void DDGISwapRadianceCascadeRadianceBuffers_RenderThread(const FSceneInterface* Scene);

/** Promotes the alternate integration output when the final cascade result resides there. */
void DDGISwapRadianceCascadeIntegrationBuffers_RenderThread(const FSceneInterface* Scene);

/** Returns the uploaded static-scene BVH buffers for GPU traversal. */
bool DDGIGetSoftwareBVHResources_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutNodesSRV,
    FShaderResourceViewRHIRef& OutTrianglesSRV,
    uint32& OutNodeCount,
    uint32& OutTriangleCount);

/** Returns logical-to-physical ray-slot mapping and slots requiring fresh history initialization. */
bool DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutLogicalToPhysicalSRV,
    FShaderResourceViewRHIRef& OutNewPhysicalSlotsSRV,
    uint32& OutNewPhysicalSlotCount,
    uint32& OutPhysicalSlotCapacity);

/** Returns the deterministic GPU spatial hash for sparse probe cell lookup. */
bool DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutSpatialHashSRV,
    uint32& OutHashCapacity,
    float& OutProbeSpacing);

bool DDGIGetRadianceCascadeProbeNeighborList_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutNeighborListSRV,
    uint32& OutProbeCount);

/** Returns immutable spatial-hash and BVH-triangle SRVs required by lighting surface rejection. */
bool DDGIGetRadianceCascadeLightingGeometry_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutSpatialHashSRV,
    FShaderResourceViewRHIRef& OutBVHTrianglesSRV,
    uint32& OutHashCapacity,
    uint32& OutTriangleCount,
    float& OutProbeSpacing);

/** Returns the persistent sparse probe-origin texture for RHI compute passes. */
bool DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutProbePositions,
    uint32& OutProbeCount);

/** Returns the uploaded sparse probe-origin texture for later tracing passes. */
bool DDGIGetRadianceCascadeProbePositions_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneInterface* Scene,
	FRDGTextureRef& OutProbePositions,
	uint32& OutProbeCount);

/** Registers the completed per-probe cascade data with RDG for deferred lighting.
 * Storage->Radiance is kept as the final integrated texture after ping-pong swaps.
 */
bool DDGIGetRadianceCascadeLightingTextures_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTextureRef& OutDirections,
    FRDGTextureRef& OutRadiance,
    FRDGTextureRef& OutHitDistance,
    FRDGTextureRef& OutHitTriangleIndex,
    FRDGTextureRef& OutProbePositions,
    uint32& OutProbeCount,
    uint32& OutTotalRaysPerProbe);

/** Invalidates per-probe radiance after the camera-relative probe lattice moves. */
void DDGIInvalidateRadianceCascadeHistory_RenderThread(const FSceneInterface* Scene);

/** Marks initialization as scheduled after all required GPU resources were bound and dispatch was issued. */
void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene);
