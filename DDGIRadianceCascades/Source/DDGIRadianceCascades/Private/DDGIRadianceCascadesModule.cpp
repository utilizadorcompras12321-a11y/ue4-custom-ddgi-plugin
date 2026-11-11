#include "DDGIRadianceCascadesRendererBridge.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"

#include "SceneRendering.h"
#include "ScenePrivate.h"

DEFINE_LOG_CATEGORY_STATIC(LogDDGIRadianceCascades, Log, All);

static TAutoConsoleVariable<int32> CVarDDGIRadianceCascades(
    TEXT("r.DDGIRadianceCascades"),
    0,
    TEXT("Enable DDGI Radiance Cascades. 0: disabled, 1: enabled."),
    ECVF_RenderThreadSafe);

static TRefCountPtr<IPooledRenderTarget> GDDGIRadianceCascadesBootstrapOutput;
static bool GDDGIRadianceCascadesBootstrapPassQueued = false;

FDelegateHandle FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLightHandle;

class FDDGIRadianceCascadesBootstrapCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FDDGIRadianceCascadesBootstrapCS);
    SHADER_USE_PARAMETER_STRUCT(FDDGIRadianceCascadesBootstrapCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
        SHADER_PARAMETER(FIntPoint, OutputSize)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FDDGIRadianceCascadesBootstrapCS,
    "/Plugin/DDGIRadianceCascades/Private/Bootstrap.usf",
    "MainCS",
    SF_Compute);

void FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLight_RenderThread(
    const FScene& Scene,
    const FViewInfo& View,
    FRDGBuilder& GraphBuilder,
    FGlobalIlluminationExperimentalPluginResources& Resources)
{
    if (CVarDDGIRadianceCascades.GetValueOnRenderThread() == 0 ||
        GDDGIRadianceCascadesBootstrapPassQueued)
    {
        return;
    }

    GDDGIRadianceCascadesBootstrapPassQueued = true;

    FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
        FIntPoint(1, 1),
        PF_FloatRGBA,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV);

    FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
        OutputDesc,
        TEXT("DDGIRadianceCascades_BootstrapOutput"));

    FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
    TShaderMapRef<FDDGIRadianceCascadesBootstrapCS> ComputeShader(GlobalShaderMap);

    FDDGIRadianceCascadesBootstrapCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FDDGIRadianceCascadesBootstrapCS::FParameters>();

    PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
    PassParameters->OutputSize = FIntPoint(1, 1);

    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DDGI Radiance Cascades Bootstrap Dispatch"),
        ComputeShader,
        PassParameters,
        FIntVector(1, 1, 1));

    GraphBuilder.QueueTextureExtraction(
        OutputTexture,
        &GDDGIRadianceCascadesBootstrapOutput);

    UE_LOG(LogDDGIRadianceCascades, Log, TEXT("Bootstrap RDG compute dispatch queued."));
}

class FDDGIRadianceCascadesModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("DDGIRadianceCascades"));

        if (!Plugin.IsValid())
        {
            UE_LOG(LogDDGIRadianceCascades, Error, TEXT("Plugin descriptor could not be located."));
            return;
        }

        const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/DDGIRadianceCascades"), ShaderDirectory);

        FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& RDILDelegate =
            FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

        FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLightHandle =
            RDILDelegate.AddStatic(
                FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLight_RenderThread);

        UE_LOG(LogDDGIRadianceCascades, Log,
            TEXT("Experimental GI callback registered. Enable with r.GlobalIllumination.ExperimentalPlugin=1 and r.DDGIRadianceCascades=1."));
    }

    virtual void ShutdownModule() override
    {
        FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& RDILDelegate =
            FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

        if (FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLightHandle.IsValid())
        {
            RDILDelegate.Remove(FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLightHandle);
            FDDGIRadianceCascadesRendererBridge::RenderDiffuseIndirectLightHandle.Reset();
        }

        GDDGIRadianceCascadesBootstrapOutput.SafeRelease();
        GDDGIRadianceCascadesBootstrapPassQueued = false;
    }
};

IMPLEMENT_MODULE(FDDGIRadianceCascadesModule, DDGIRadianceCascades)
