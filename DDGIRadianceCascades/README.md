# DDGI Radiance Cascades Plugin

Independent Unreal Engine 4.27.2 Plus runtime plugin. The implementation target is DX11 / SM5 and does not require DXR, hardware ray tracing, or edits to Engine source.

## Current status: bootstrap

- Dedicated plugin descriptor and runtime module.
- Runtime shader directory mapping: `/Plugin/DDGIRadianceCascades`.
- Render-thread-safe toggle: `r.DDGIRadianceCascades` (default: 0).
- Minimal SM5 compute shader registered to validate the plugin shader path.
- No RTXGI probe grid or RTXGI runtime code is used by this bootstrap.
- No engine files are modified.

The bootstrap shader is intentionally not dispatched yet. The next integration step is to bind the plugin to the UE4.27.2 Plus Experimental Global Illumination callback and dispatch a tiny RDG compute pass there. The exact callback interface must be matched to the user's engine fork before implementation; guessing its signature would risk breaking compilation.

## Planned architecture

1. Validate the Experimental GI callback and DX11 RDG dispatch.
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
