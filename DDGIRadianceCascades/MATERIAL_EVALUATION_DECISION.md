# DDGI material evaluation decision (UE 4.27.2)

## Decision

Keep the plugin-only software-BVH material path based on explicit, documented DDGI material inputs. Do not claim that this path evaluates arbitrary Unreal Material Graphs.

Supported inputs remain:
- `DDGI_BaseColor` (vector parameter)
- `DDGI_EmissiveRadiance` (vector parameter)
- `DDGI_Metallic` (scalar diffuse-energy reduction; not specular reflection)
- `DDGI_UseVertexColor` (opt-in CPU-readable LOD0 vertex-color approximation)

Opaque triangles are included. Masked triangles are skipped because the current BVH has no per-hit opacity-mask representation. Translucent/blended materials are not represented as transmissive surfaces.

## Why arbitrary Material Graph evaluation is not a plugin-only change

UE 4.27's material outputs are evaluated by renderer material shaders during mesh passes. A faithful material capture needs to submit the actual mesh/material pair, evaluate Base Color, Emissive and Opacity Mask at interpolated UVs, and write those outputs to a material atlas (or equivalent GPU resource) that the software BVH tracer can sample.

Epic's UE 4.27 Mesh Drawing Pipeline documentation states that the renderer is not extensible to plugins for registered mesh passes; the documented plugin escape hatch is the immediate `DrawDynamicMeshPass` path. A cached custom mesh pass requires renderer-module integration. Using the immediate path as a hidden per-frame material bake would defeat the current bounded, persistent snapshot design and introduce expensive scene-wide draw work. A proper cached capture pass therefore requires changes to the UE4 renderer source.

CPU-reading texture assets is not an equivalent substitute:
- cooked/streamed texture data is not guaranteed to be available as CPU-readable source data;
- sampling a few UV points cannot reproduce arbitrary graph operations, world-position effects, vertex interpolation, masks or material functions;
- treating masked geometry as opaque causes false blockers, while dropping it causes missing occluders.

## Available choices

1. **Keep the current plugin-only constraint (selected):** use the explicit DDGI parameters above; masked and translucent geometry remain unsupported and are reported as limitations.
2. **Allow an Engine source fork:** add a cached renderer mesh pass that evaluates the material graph into a stable atlas, with UV/material indirection in the BVH and shader-side sampling. This is the route required for faithful arbitrary Material Graph, texture and opacity-mask evaluation. It is not a plugin-only implementation.

## Required validation before expanding support

- Verify atlas addressing and UV seams on multiple material sections.
- Verify texture streaming/resource lifetime and material-instance parameter changes.
- Verify masked clip threshold and dithered masks.
- Verify emissive HDR values and color-space handling.
- Verify snapshot invalidation on material changes.
- Profile bake cost separately from per-frame BVH traversal.
- Compile shaders and test on the actual UE 4.27.2 DX11/SM5 target.

Until those items exist and pass, the feature must remain documented as unsupported. No 60 FPS or visual-quality guarantee can be made from static inspection.
