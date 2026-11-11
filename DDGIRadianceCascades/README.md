# DDGI Radiance Cascades Plugin

Independent Unreal Engine 4.27.2 Plus runtime plugin. The implementation target is DX11 / SM5 and does not require DXR, hardware ray tracing, or edits to Engine source.

## Current status: Experimental GI callback + RDG bootstrap

- Dedicated plugin descriptor and runtime module.
- Runtime shader directory mapping: `/Plugin/DDGIRadianceCascades`.
- Runtime-safe toggle: `r.DDGIRadianceCascades` (default: 0).
- Registers `FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight()`.
- Queues a one-pixel SM5 compute dispatch through RDG when enabled.
- Extracts the tiny test output so the RDG pass remains live.
- Does not read or modify SceneColor; the test must not change the rendered image.
- No RTXGI probe grid or RTXGI runtime code is used by this bootstrap.
- No engine files are modified.

Enable both engine and plugin gates:

```
r.GlobalIllumination.ExperimentalPlugin 1
r.DDGIRadianceCascades 1
```

The callback and dispatch are implemented against the UE4.27 Experimental GI API used by the RTXGI 4.27 plugin. This source has not yet been compiled against the user's exact UE4.27.2 Plus fork, so API/build validation is still required.

## Planned architecture

1. Validate the callback and DX11 RDG compute dispatch.
2. Build sparse surface representation and conservative visibility data.
3. Implement deterministic 3D radiance cascades with ray splitting.
4. Add software BVH traversal and first-bounce diffuse transport.
5. Integrate irradiance into deferred diffuse indirect lighting.

RTXGI source in the repository is retained as reference material only. Its DXR ray-generation path, regular probe-grid architecture, and typed-UAV assumptions are not part of this implementation.

## Compatibility target

- Unreal Engine 4.27.2 Plus
- Windows 64-bit
- DX11 / Shader Model 5
- No DXR or dedicated RT hardware
