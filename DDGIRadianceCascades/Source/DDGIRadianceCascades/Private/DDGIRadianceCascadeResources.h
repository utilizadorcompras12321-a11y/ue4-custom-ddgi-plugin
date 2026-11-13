#pragma once

#include "CoreMinimal.h"
#include "RenderGraph.h"

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


/** Returns the uploaded sparse probe-origin texture for later tracing passes. */
bool DDGIGetRadianceCascadeProbePositions_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneInterface* Scene,
    FRDGTextureRef& OutProbePositions,
    uint32& OutProbeCount);
