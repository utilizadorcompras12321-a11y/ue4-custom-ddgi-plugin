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
    uint32& OutProbeCount);

bool DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutDirections,
    FTexture2DRHIRef& OutRadiance,
    FTexture2DRHIRef& OutHitDistance,
    FTexture2DRHIRef& OutHitTriangleIndex,
    FTexture2DRHIRef& OutProbePositions,
    uint32& OutProbeCount,
    uint32& OutTotalRaysPerProbe);

/** Returns the ping-pong radiance texture used by the cascade integration pass. */
bool DDGIGetRadianceCascadeScratchTexture_RenderThread(
    const FSceneInterface* Scene,
    FTexture2DRHIRef& OutRadianceScratch);

/** Swaps persistent radiance and scratch textures after an odd number of integration passes. */
void DDGISwapRadianceCascadeRadianceBuffers_RenderThread(const FSceneInterface* Scene);

/** Returns the uploaded static-scene BVH buffers for GPU traversal. */
bool DDGIGetSoftwareBVHResources_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutNodesSRV,
    FShaderResourceViewRHIRef& OutTrianglesSRV,
    uint32& OutNodeCount,
    uint32& OutTriangleCount);

/** Returns the deterministic GPU spatial hash for sparse probe cell lookup. */
bool DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
    const FSceneInterface* Scene,
    FShaderResourceViewRHIRef& OutSpatialHashSRV,
    uint32& OutHashCapacity,
    float& OutProbeSpacing);

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

/** Marks initialization as scheduled after all required GPU resources were bound and dispatch was issued. */
void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene);
