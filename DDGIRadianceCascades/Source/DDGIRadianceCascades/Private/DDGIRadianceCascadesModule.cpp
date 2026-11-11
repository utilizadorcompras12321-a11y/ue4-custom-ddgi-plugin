#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "RHIResources.h"
#include "RenderTargetPool.h"

// Same renderer-private headers used by the working RTXGI integration.
#include "SceneRendering.h"
#include "ScenePrivate.h"

// Renderer-private callback types are intentionally only referenced here.
class FScene;
class FViewInfo;
class FRDGBuilder;
struct FGlobalIlluminationExperimentalPluginResources;

DEFINE_LOG_CATEGORY_STATIC(LogDDGIRadianceCascades, Log, All);

static TAutoConsoleVariable<int32> CVarDDGIRadianceCascades(
    TEXT("r.DDGIRadianceCascades"),
    0,
    TEXT("Enable DDGI Radiance Cascades. 0: disabled, 1: enabled."),
    ECVF_RenderThreadSafe);

// Keeps the bootstrap output alive after the RDG graph executes. This is a
// one-pixel validation target; it is not part of the final GI data layout.
static TRefCountPtr<IPooledRenderTarget> GDDGIRadianceCascadesBootstrapOutput;
static bool GDDGIRadianceCascadesBootstrapPassQueued = false;
static FDelegateHandle GDDGIRadianceCascadesLightingDelegate;

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

static void RenderDiffuseIndirectLight_RenderThread(
    const FScene& Scene,
    const FViewInfo& View,
    FRDGBuilder& GraphBuilder,
    FGlobalIlluminationExperimentalPluginResources& Resources)
{
    if (CVarDDGIRadianceCascades.GetValueOnRenderThread() == 0)
    {
        return;
    }

    // The first milestone only validates that our plugin callback, RDG
    // allocation, shader registration and SM5 compute dispatch work. It
    // deliberately does not touch SceneColor or alter the rendered image.
    if (GDDGIRadianceCascadesBootstrapPassQueued)
    {
        return;
    }

    // Mark it before queuing: the callback can be invoked more than once
    // while the same view family is being assembled, before extraction runs.
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

    // Extraction makes the otherwise unconsumed test output survive RDG
    // culling and keeps it alive until the next frame.
    GraphBuilder.QueueTextureExtraction(
        OutputTexture,
        &GDDGIRadianceCascadesBootstrapOutput);

    UE_LOG(
        LogDDGIRadianceCascades,
        Log,
        TEXT("Bootstrap RDG compute dispatch queued."));
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

        FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& Delegate =
            FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

        GDDGIRadianceCascadesLightingDelegate =
            Delegate.AddStatic(RenderDiffuseIndirectLight_RenderThread);

        UE_LOG(
            LogDDGIRadianceCascades,
            Log,
            TEXT("Module loaded and Experimental GI callback registered. Enable with r.GlobalIllumination.ExperimentalPlugin=1 and r.DDGIRadianceCascades=1."));
    }

    virtual void ShutdownModule() override
    {
        if (GDDGIRadianceCascadesLightingDelegate.IsValid())
        {
            FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& Delegate =
                FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

            Delegate.Remove(GDDGIRadianceCascadesLightingDelegate);
            GDDGIRadianceCascadesLightingDelegate.Reset();
        }

        GDDGIRadianceCascadesBootstrapOutput.SafeRelease();
        GDDGIRadianceCascadesBootstrapPassQueued = false;
    }
};

IMPLEMENT_MODULE(FDDGIRadianceCascadesModule, DDGIRadianceCascades)