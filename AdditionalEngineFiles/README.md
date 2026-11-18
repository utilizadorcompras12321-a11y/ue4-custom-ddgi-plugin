# Additional UE4.27.2 engine files

These files are engine-source replacements required by the DDGI integration and its SSAO diagnostics.

## Files and replacement locations

Copy each file over the matching file in your local Unreal Engine 4.27.2 source tree:

| File in this folder | Replace this local file |
|---|---|
| `SceneViewExtension.h` | `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h` |
| `PostProcessing.cpp` | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp` |
| `CompositionLighting.cpp` | `Engine/Source/Runtime/Renderer/Private/CompositionLighting/CompositionLighting.cpp` |

For this checkout, the full destination paths are:

- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Engine\Public\SceneViewExtension.h`
- `C:\Users\Utilizador~\Desktop\UnrealEngine\Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessing.cpp`

## What these changes do

- Add `EPostProcessingPass::BeforeTemporalAA` to the Scene View Extension API.
- Keep the engine's `BeforeTemporalAA` pass sequence from assigning the same pass twice.
- Add temporary SSAO diagnostics to `FCompositionLighting::ProcessAfterBasePass()`.
- Log the AO show flag, lighting flag, post-process intensity/radius, LPV state, ray-tracing overlay gate, computed SSAO levels, and whether AO generation completed. Logging is limited to the first 10 frames.

## Installation

1. Close Unreal Editor and any process using this engine build.
2. Back up the original engine files.
3. Copy each replacement from this folder to its matching destination above.
4. Regenerate project files only if your normal source workflow requires it.
5. Build the Editor/Renderer target.
6. Read the `SSAO DIAG` lines in the Output Log. They are emitted for the first 10 frames only.
7. If compilation fails, restore the backups.

These are complete source files, not patches. They are intended for the UE4.27.2 engine tree used by this project.
