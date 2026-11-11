#pragma once

// Match the working UE4.27 RTXGI public-header include sequence.
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/EngineTypes.h"
#include "Components/SceneComponent.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RendererInterface.h"

#include "DDGIRadianceCascadesComponent.generated.h"

class FRDGBuilder;
class FScene;
class FViewInfo;
class FGlobalIlluminationExperimentalPluginResources;

class FDDGIRadianceCascadesSceneProxy
{
public:
    static void RenderDiffuseIndirectLight_RenderThread(
        const FScene& Scene,
        const FViewInfo& View,
        FRDGBuilder& GraphBuilder,
        FGlobalIlluminationExperimentalPluginResources& Resources);

    static FDelegateHandle RenderDiffuseIndirectLightHandle;
};

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class UDDGIRadianceCascadesComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    static void Startup();
    static void Shutdown();
};
