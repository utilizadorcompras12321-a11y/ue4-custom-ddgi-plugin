#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogDDGIRadianceCascades, Log, All);

static TAutoConsoleVariable<int32> CVarDDGIRadianceCascades(
    TEXT("r.DDGIRadianceCascades"),
    0,
    TEXT("Enable DDGI Radiance Cascades. 0: disabled, 1: enabled."),
    ECVF_RenderThreadSafe);

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

class FDDGIRadianceCascadesModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DDGIRadianceCascades"));
        if (!Plugin.IsValid())
        {
            UE_LOG(LogDDGIRadianceCascades, Error, TEXT("Plugin descriptor could not be located."));
            return;
        }

        const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/DDGIRadianceCascades"), ShaderDirectory);

        UE_LOG(LogDDGIRadianceCascades, Log, TEXT("DDGI Radiance Cascades module loaded. Runtime toggle: r.DDGIRadianceCascades=1"));
    }

    virtual void ShutdownModule() override
    {
    }
};

IMPLEMENT_MODULE(FDDGIRadianceCascadesModule, DDGIRadianceCascades)