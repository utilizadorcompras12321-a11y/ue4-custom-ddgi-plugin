#include "DDGIRadianceCascadesComponent.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "ShaderCore.h"

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

FDelegateHandle FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle;

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

void FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLight_RenderThread(
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

    const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
        FIntPoint(1, 1),
        PF_FloatRGBA,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV);

    FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
        OutputDesc,
        TEXT("DDGIRadianceCascades_BootstrapOutput"));

    TShaderMapRef<FDDGIRadianceCascadesBootstrapCS> ComputeShader(
        GetGlobalShaderMap(ERHIFeatureLevel::SM5));

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
}

void UDDGIRadianceCascadesComponent::Startup()
{
    FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& RDILDelegate =
        FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

    FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle =
        RDILDelegate.AddStatic(
            FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLight_RenderThread);

    UE_LOG(LogDDGIRadianceCascades, Log, TEXT("Experimental GI callback registered."));
}

void UDDGIRadianceCascadesComponent::Shutdown()
{
    FGlobalIlluminationExperimentalPluginDelegates::FRenderDiffuseIndirectLight& RDILDelegate =
        FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight();

    check(FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle.IsValid());
    RDILDelegate.Remove(FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle);

    FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle.Reset();
    GDDGIRadianceCascadesBootstrapOutput.SafeRelease();
    GDDGIRadianceCascadesBootstrapPassQueued = false;
}
