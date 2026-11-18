# Additional UE4.27.2 engine files

These are complete engine-source replacements required by the DDGI integration.

## Replacement locations

| File in this folder | Replace this local file |
|---|---|
| `SceneViewExtension.h` | `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h` |
| `PostProcessing.cpp` | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp` |
| `CompositionLighting.cpp` | `Engine/Source/Runtime/Renderer/Private/CompositionLighting/CompositionLighting.cpp` |

For this checkout, the full destination paths are:

- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Engine\Public\SceneViewExtension.h`
- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessing.cpp`
- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Renderer\Private\CompositionLighting\CompositionLighting.cpp`

## Relevant renderer behavior

- Adds `EPostProcessingPass::BeforeTemporalAA` to the Scene View Extension API.
- Prevents duplicate assignment of the `BeforeTemporalAA` pass.
- Treats a registered `RenderDiffuseIndirectLight` experimental GI delegate as a consumer of native SSAO. This ensures UE4.27 generates its SSAO texture in scenes without a skylight, ambient cubemap, reflection capture, or another built-in AO consumer.
- Does not add diagnostic logging or replace UE4's SSAO implementation.

## Installation

1. Close Unreal Editor and any process using this engine build.
2. Back up the original engine files.
3. Copy each replacement over its matching destination above.
4. Build the Editor/Renderer target.
