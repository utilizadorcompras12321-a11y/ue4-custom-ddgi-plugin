# Additional UE4.27.2 engine files

These files are engine-source replacements required by the DDGI pre-TAA integration.

## Files and replacement locations

Copy each file over the matching file in your local Unreal Engine 4.27.2 source tree:

| File in this folder | Replace this local file |
|---|---|
| `SceneViewExtension.h` | `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h` |
| `PostProcessing.cpp` | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp` |

For this checkout, the full destination paths are:

- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Engine\Public\SceneViewExtension.h`
- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessing.cpp`

## What these changes do

- Add `EPostProcessingPass::BeforeTemporalAA` to the Scene View Extension API.
- Run subscribed view-extension callbacks after the HDR scene color is ready and before desktop TAA.
- Keep the pass enabled only when an extension actually subscribes, avoiding the pass-sequence ordering assertions seen when no extension is registered.

The DDGI plugin callback must subscribe to `EPostProcessingPass::BeforeTemporalAA`.

## Installation

1. Close Unreal Editor and any process using this engine build.
2. Back up the two original files.
3. Copy the two files from this folder to the exact destinations above, replacing the originals.
4. Regenerate project files only if your normal source workflow requires it.
5. Build the Editor/Renderer target.
6. If compilation fails, do not continue using a mixture of old and new files; restore both backups or install both replacements together.

These are complete source files, not patches. They are intended for the UE4.27.2 engine tree used by this project.
