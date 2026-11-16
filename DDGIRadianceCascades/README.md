# DDGI Radiance Cascades for Unreal Engine 4.27.2

This repository contains a native Unreal Engine plugin implementing a software-traced sparse clipmap Radiance Cascades GI path. It is not the original NVIDIA RTXGI plugin and does not require DXR.

## Current architecture

- One GI path: sparse clipmap probe lattice + Radiance Cascades ray hierarchy.
- Up to four spatial clipmap levels with power-of-two spacing (1, 2, 4, 8), limited by the configured probe radius.
- Deterministic equal-solid-angle ray directions and deterministic probe scheduling.
- Software BVH traversal in SM5 compute shaders.
- One secondary diffuse bounce.
- Persistent GPU ray data with logical-to-physical probe-slot mapping and retained probe history.
- Spatial gather with surface/normal/plane rejection.
- Deferred lighting application with clipmap blending and probe-to-receiver visibility rejection.

## Requirements and scope

- Unreal Engine 4.27.2 source build.
- Windows 64-bit, deferred renderer, SM5-capable GPU.
- The current tracing backend is software BVH; it does not use DXR or hardware ray tracing.
- The current BVH is built from supported static geometry. Movable components and instanced geometry are skipped and are not represented in indirect lighting.
- The current direct-light injection supports the strongest visible directional light. Point lights, spot lights, arbitrary material graphs, textured albedo and animated geometry are not fully represented by the software scene snapshot.
- This is a custom experimental renderer feature. GPU cost and image quality depend on scene triangle count, active probe count, cascade settings and trace budget. A fixed 60 FPS guarantee is not possible without profiling on the target scene and GPU.

## Setup

1. Copy `DDGIRadianceCascades` into the project's `Plugins` directory.
2. Enable **DDGI Radiance Cascades** in the Plugins window and restart the editor.
3. Place one **Radiance Cascades Master Actor** in the level.
4. Ensure the project uses the deferred rendering path and SM5.
5. Build/rebuild the static geometry BVH after changing static scene geometry by calling `RebuildStaticGeometryBVH` on the Master Actor (or its Blueprint-callable function).
6. Set `bEnableDynamicGI` on the Master Actor to enable or disable the feature.

The plugin accepts one Master Actor per world. Additional actors are ignored.

## Main controls

| Property | Purpose |
|---|---|
| `ProbeSpacing` | Base spacing for the finest spatial clipmap level, in cm |
| `ProbeGridDistance` | Spatial radius covered by the active clipmaps, in cm |
| `MaxActiveProbes` | Hard cap on active sparse probes |
| `ProbeUpdateBudget` | Maximum number of probes traced per frame |
| `MaxSoftwareTriangles` | Maximum number of triangles admitted to the static software BVH |
| `CascadeCount` | Number of distance cascades |
| `BaseRayCount` | Number of base angular regions |
| `RaySplitFactor` | Angular subdivision factor between cascades |
| `BaseSegmentLength` | Length of the first cascade interval, in cm |
| `CascadeScale` | Geometric growth factor for cascade intervals |
| `DynamicGIDistance` | Maximum software ray distance, in cm |

### Console variables

- `r.DDGI.RadianceProbeTraceBudget`: `-1` uses the Master Actor's `ProbeUpdateBudget`; `0` disables tracing; a positive value overrides the actor budget (clamped to 8192).
- `r.DDGI.UsePrecomputedProbeNeighbors`: `1` uses the precomputed neighbor list; `0` uses the spatial hash fallback.
- `r.DDGI.DebugApply`: `0` normal output; `1` probe coverage; `2` raw irradiance; `3` output-path magenta test; `4`–`7` individual spatial clipmap levels.

## Important limitations

This implementation is still experimental. It does not currently provide dynamic-mesh BVH updates, probe relocation, complete material evaluation, multiple direct-light types, or a temporal denoiser. Deterministic ray directions avoid stochastic sampling noise, but they do not replace missing geometry/material support or scene-specific performance validation.

Do not treat the presence of the plugin or a successful C++ build as proof of production-ready GI. Validate the output with moving cameras, thin walls, corners, enclosed rooms, emissive surfaces and changing directional light. Profile GPU time at the target resolution and on the target hardware.
