# DDGI Master Actor

The **DDGI Master Actor** is the level-wide configuration entry point for
the software-traced sparse clipmap Radiance Cascades implementation. Place
one instance in a level to configure probe spacing, clipmap radius, cascade
layout, probe update budget, background radiance and light contribution.

## Current implementation status

The actor settings are consumed by the runtime subsystem and render-thread
tracing path. The active GI path is the native software BVH + Radiance
Cascades implementation; it does not use DXR or the removed NVIDIA RTXGI
volume path.

Supported direct-light injection:
- The strongest visible directional light that has **Affect Dynamic
  Indirect Lighting** enabled.
- Up to 32 visible point/spot lights with **Affect Dynamic Indirect
  Lighting** enabled.
- Local-light position, direction, intensity, attenuation radius and spot
  cone are refreshed at runtime. The software tracer approximates UE4's
  attenuation and shadowing; it does not reproduce every light feature.

Geometry and material scope:
- Opaque static mesh and BSP geometry with CPU-accessible mesh data.
- Movable and instanced meshes are excluded from the software BVH.
- Material evaluation is limited to the `DDGI_BaseColor` and
  `DDGI_EmissiveRadiance` vector parameters, with a neutral diffuse fallback.
  Arbitrary material graphs, texture sampling, opacity masks and full PBR
  evaluation are not implemented.

The presence of the actor and shader files is not proof of a successful
engine build or production-ready image quality. The plugin still requires
UE4.27.2 compilation, GPU validation and profiling in the target project.
