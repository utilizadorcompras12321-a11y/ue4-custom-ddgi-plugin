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
	FRDGTextureRef& OutHitDistance,
	FRDGTextureRef& OutRayOrigins);

/** Returns persistent RHI textures while initialization is pending. */
bool DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
	const FSceneInterface* Scene,
	FTexture2DRHIRef& OutDirections,
	FTexture2DRHIRef& OutRadiance,
	FTexture2DRHIRef& OutHitDistance,
	FTexture2DRHIRef& OutRayOrigins,
	FTexture2DRHIRef& OutProbePositions,
	uint32& OutProbeCount);

/** Returns ray textures for the per-frame tracing pass, regardless of initialization state. */
bool DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
	const FSceneInterface* Scene,
	FTexture2DRHIRef& OutDirections,
	FTexture2DRHIRef& OutRadiance,
	FTexture2DRHIRef& OutHitDistance,
	FTexture2DRHIRef& OutRayOrigins,
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

/** Returns the uploaded sparse probe-origin texture for later tracing passes. */
bool DDGIGetRadianceCascadeProbePositions_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneInterface* Scene,
	FRDGTextureRef& OutProbePositions,
	uint32& OutProbeCount);

/** Marks initialization as scheduled after all required GPU resources were bound and dispatch was issued. */
void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene);
