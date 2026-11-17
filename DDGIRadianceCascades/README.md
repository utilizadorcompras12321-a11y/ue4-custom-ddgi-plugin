# DDGI Radiance Cascades for Unreal Engine 4.27.2

This repository contains a native Unreal Engine plugin implementing a software-traced sparse clipmap Radiance Cascades GI path. It is not the original NVIDIA RTXGI plugin and does not require DXR.

## Current architecture

- One GI path: sparse clipmap probe lattice + Radiance Cascades ray hierarchy.
- Up to four spatial clipmap levels with power-of-two spacing (1, 2, 4, 8), limited by the configured probe radius.
- Deterministic equal-solid-angle ray directions reused across distance cascades (no angular splitting), reducing ray tracing and storage cost.
- With shared directions, only the farthest active cascade performs the full software-BVH trace; nearer cascade radiance is propagated through one-to-one integration, and their redundant spatial gathers are skipped.
- Software BVH traversal in SM5 compute shaders.
- One secondary diffuse bounce.
- Separate persistent raw-trace, gathered-radiance and final-integrated textures; tracing never overwrites integrated history.
- Final integrated radiance uses packed R11G11B10 values in an R32_UINT texture, avoiding optional half-float typed UAV loads on DX11.0.
- Logical-to-physical probe-slot mapping with retained history for stable world-lattice cells.
- Trace, gather and integration process the same bounded probe batch; untouched probes retain their last valid integrated result.
- A quantized fingerprint covers directional/local-light inputs and static-background settings; changing them invalidates cached probe radiance instead of preserving lighting from the previous state.
- A persistent round-robin trace cursor advances only for dispatched batches and resets after global history invalidation, refreshing the nearest active probes first.
- Spatial gather with surface/normal/plane rejection.
- Deferred lighting application with clipmap blending and probe-to-receiver visibility rejection.

## Requirements and scope

- Unreal Engine 4.27.2 source build.
- Windows 64-bit, deferred renderer, SM5-capable GPU.
- The current tracing backend is software BVH; it does not use DXR or hardware ray tracing.
- Material input uses four explicit parameters: vector `DDGI_BaseColor`, vector `DDGI_EmissiveRadiance`, scalar `DDGI_Metallic`, and scalar `DDGI_UseVertexColor`. They are read when the static BVH is rebuilt, not every frame. See [MATERIAL_PARAMETERS.md](MATERIAL_PARAMETERS.md) for defaults, setup and limitations. These are explicit overrides, not general Material Graph evaluation.
- The current BVH is built from supported static geometry, including static ISM/HISM instances. Movable components and movable instances are skipped; they are not represented in indirect lighting.
- Direct-light injection supports the strongest visible directional light plus up to 32 captured point/spot lights. The eight strongest local lights are shadow-tested at primary hits. Secondary-bounce direct lighting is deliberately unshadowed to avoid nested shadow-BVH traversals; this can increase indirect light leaks. Local-light transforms, intensity, radius and spot cones are refreshed at runtime; attenuation is an approximation of UE4's light falloff.
- This is a custom experimental renderer feature. GPU cost and image quality depend on scene triangle count, active probe count, cascade settings and trace budget. A fixed 60 FPS guarantee is not possible without profiling on the target scene and GPU.

## Setup

1. Copy `DDGIRadianceCascades` into the project's `Plugins` directory.
2. Enable **DDGI Radiance Cascades** in the Plugins window and restart the editor.
3. Place one **Radiance Cascades Master Actor** in the level.
4. Ensure the project uses the deferred rendering path and SM5.
5. Build/rebuild the static geometry BVH after changing static scene geometry or DDGI material parameter values by calling `RebuildStaticGeometryBVH` on the Master Actor (or its Blueprint-callable function).
6. Set `bEnableDynamicGI` on the Master Actor to enable or disable the feature.

The plugin accepts one Master Actor per world. Additional actors are ignored.

## Material parameters

The four supported material parameters are documented in [MATERIAL_PARAMETERS.md](MATERIAL_PARAMETERS.md). Their values are captured into the static BVH at rebuild time; Material Instance changes require another rebuild.

## Main controls

| Property | Purpose |
|---|---|
| `ProbeSpacing` | Base spacing for the finest spatial clipmap level, in cm |
| `ProbeGridDistance` | Spatial radius covered by the active clipmaps, in cm |
| `MaxActiveProbes` | Hard cap on active sparse probes |
| `ProbeUpdateBudget` | Maximum number of probes traced per frame |
| `LocalLightGIIntensityScale` | Scale applied to point/spot-light contributions |
| `MaxSoftwareTriangles` | Maximum number of triangles admitted to the static software BVH |
| `CascadeCount` | Number of distance cascades |
| `BaseRayCount` | Fixed angular direction count reused by every distance cascade |
| `BaseSegmentLength` | Length of the first cascade interval, in cm |
| `CascadeScale` | Geometric growth factor for cascade intervals |
| `DynamicGIDistance` | Maximum software ray distance, in cm |

### Console variables

- `r.DDGI.RadianceProbeTraceBudget`: `-1` uses the Master Actor's `ProbeUpdateBudget`; `0` disables tracing; a positive value overrides the actor budget (clamped to 8192).
- `r.DDGI.UsePrecomputedProbeNeighbors`: `1` uses the precomputed neighbor list; `0` uses the spatial hash fallback.
- `r.DDGI.DebugApply`: `0` normal output; `1` probe coverage; `2` raw irradiance; `3` output-path magenta test; `4`–`7` individual spatial clipmap levels.

## Validation

The plugin includes an Unreal Automation Test for cascade packing, interval coverage and GPU storage estimates: run `Automation RunTests DDGI.RadianceCascades.Layout` in the editor or automation framework. This is a CPU-side layout test; it does not replace shader compilation, GPU validation or scene profiling.

Material-evaluation architecture constraints and the two viable implementation paths are recorded in [MATERIAL_EVALUATION_DECISION.md](MATERIAL_EVALUATION_DECISION.md). In particular, faithful arbitrary Material Graph evaluation requires a cached renderer material pass; UE 4.27 does not expose registered cached mesh passes as a plugin extension point.

## Important limitations

This implementation is still experimental. It does not currently provide dynamic-mesh BVH updates, probe relocation, SkyLight/RectLight or IES evaluation, arbitrary Material Graph evaluation, texture sampling, opacity-mask evaluation, translucency, or a temporal denoiser. Lighting changes invalidate the complete retained radiance cache; with a bounded trace budget this can temporarily reduce indirect lighting until probes are refreshed. Deterministic ray directions avoid stochastic sampling noise, but they do not replace missing geometry/material support or scene-specific performance validation.

Do not treat the presence of the plugin or a successful C++ build as proof of production-ready GI. Validate the output with moving cameras, thin walls, corners, enclosed rooms, emissive surfaces and changing directional and local lights. Profile GPU time at the target resolution and on the target hardware.
