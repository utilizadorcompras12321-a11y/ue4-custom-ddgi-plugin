using UnrealBuildTool;
using System.IO;

public class DDGIRadianceCascades : ModuleRules
{
    public DDGIRadianceCascades(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "Projects"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "RenderCore",
            "Renderer",
            "DeveloperSettings",
            "RHI"
        });

        // Match the renderer include visibility used by the working RTXGI module.
        PrivateIncludePaths.AddRange(new string[]
        {
            EngineDirectory + "/Source/Runtime/Renderer/Private",
            EngineDirectory + "/Source/Runtime/RenderCore/Public"
        });
    }
}
