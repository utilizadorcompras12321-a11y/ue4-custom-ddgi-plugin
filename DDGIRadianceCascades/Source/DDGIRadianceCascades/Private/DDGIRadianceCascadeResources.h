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
/** Returns the uploaded sparse probe-origin texture for later tracing passes. */
bool DDGIGetRadianceCascadeProbePositions_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneInterface* Scene,
	FRDGTextureRef& OutProbePositions,
	uint32& OutProbeCount);

/** Marks initialization as scheduled after all required GPU resources were bound and dispatch was issued. */
void DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(const FSceneInterface* Scene);
