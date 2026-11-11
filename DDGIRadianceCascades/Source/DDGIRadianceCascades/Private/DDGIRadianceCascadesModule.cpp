#include "DDGIRadianceCascadesComponent.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FDDGIRadianceCascadesModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("DDGIRadianceCascades"));

        if (!Plugin.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("DDGI Radiance Cascades plugin descriptor could not be located."));
            return;
        }

        const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/DDGIRadianceCascades"), ShaderDirectory);

        StartupDDGI();
    }

    virtual void ShutdownModule() override
    {
        ShutdownDDGI();
    }

private:
    void StartupDDGI()
    {
        UDDGIRadianceCascadesComponent::Startup();
    }

    void ShutdownDDGI()
    {
        if (FDDGIRadianceCascadesSceneProxy::RenderDiffuseIndirectLightHandle.IsValid())
        {
            UDDGIRadianceCascadesComponent::Shutdown();
        }
    }
};

IMPLEMENT_MODULE(FDDGIRadianceCascadesModule, DDGIRadianceCascades)
