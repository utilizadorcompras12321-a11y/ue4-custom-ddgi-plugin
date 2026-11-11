#pragma once

// Keep the same public Renderer interface include sequence as the working
// UE4.27 RTXGI integration. This header is the only place where the plugin
// exposes the experimental GI callback signature.
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineTypes.h"
#include "Components/SceneComponent.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RendererInterface.h"

class FRDGBuilder;
class FScene;
class FViewInfo;
class FGlobalIlluminationExperimentalPluginResources;

class FDDGIRadianceCascadesRendererBridge
{
public:
    static void RenderDiffuseIndirectLight_RenderThread(
        const FScene& Scene,
        const FViewInfo& View,
        FRDGBuilder& GraphBuilder,
        FGlobalIlluminationExperimentalPluginResources& Resources);

    static FDelegateHandle RenderDiffuseIndirectLightHandle;
};
