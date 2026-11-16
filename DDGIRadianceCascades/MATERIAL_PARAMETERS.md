# DDGI material parameters

The software BVH does not execute Unreal Material Graphs. During `RebuildStaticGeometryBVH`, it reads four named parameters from each supported material interface and copies the resulting values into the static triangle data. These parameters are a deliberately small, predictable material contract.

## Parameters

| Parameter name | Type | Default | Effect in DDGI |
|---|---|---:|---|
| `DDGI_BaseColor` | Vector Parameter | Linear RGB 0.18, 0.18, 0.18 | Diffuse albedo used when a ray hits the triangle. |
| `DDGI_EmissiveRadiance` | Vector Parameter | Black (0, 0, 0) | Adds emissive radiance at the hit. Values are clamped to non-negative RGB. |
| `DDGI_Metallic` | Scalar Parameter | 0 | Reduces diffuse energy by multiplying BaseColor by `1 - saturate(Metallic)`. It does not create specular reflections or metallic BRDF shading. |
| `DDGI_UseVertexColor` | Scalar Parameter | 0 | When greater than 0.5, multiplies BaseColor by the average RGB vertex color of each LOD0 triangle. |

All vector values are interpreted as linear RGB. The parameters are DDGI inputs; they do not automatically replace the material's visible Base Color, Emissive, or Metallic outputs. Connect the corresponding parameter nodes to the regular material outputs as well if the visible surface should match the DDGI approximation.

## Setup in Unreal

1. Open the parent Material used by the static mesh.
2. Add a **Vector Parameter** named exactly `DDGI_BaseColor`. Set its default to the diffuse color you want DDGI to use.
3. Add a **Vector Parameter** named exactly `DDGI_EmissiveRadiance`. Leave it black unless the surface should emit indirect light.
4. Add a **Scalar Parameter** named exactly `DDGI_Metallic`, with a value from 0 (fully diffuse) to 1 (no diffuse contribution).
5. Add a **Scalar Parameter** named exactly `DDGI_UseVertexColor`. Use 0 normally, or 1 to opt into the LOD0 vertex-color approximation.
6. Create or update a Material Instance and override these parameters as needed.
7. Call `RebuildStaticGeometryBVH` on the Radiance Cascades Master Actor after changing these values.

The parameter names are case-sensitive. The plugin reads the values when the BVH snapshot is built; it does not poll material instances every frame. Changing a Material Instance parameter therefore does not update the existing BVH until it is rebuilt.

## Scope and limitations

- Supported for opaque static meshes and static ISM/HISM instances included in the BVH. The first three parameters are also read for supported opaque BSP materials.
- `DDGI_UseVertexColor` applies only to static-mesh LOD0 CPU-readable vertex colors. It uses the triangle-centroid average, not exact barycentric interpolation at each ray hit.
- Static meshes need **Allow CPU Access** enabled so the plugin can read LOD0 geometry (and vertex colors when requested).
- Movable meshes are not in the current static BVH.
- Masked materials are skipped because opacity-mask textures are not evaluated. Translucent and other blended materials are not represented as transparent surfaces.
- Texture samples, material functions, world-position logic, and arbitrary Material Graph expressions are not evaluated.
- `DDGI_Metallic` is only a diffuse-energy control. It is not a physically based metallic workflow.

These limits are intentional: the CPU reads a few scalar/vector values once per BVH rebuild, rather than running a material bake or adding a per-ray material-evaluation pass.
