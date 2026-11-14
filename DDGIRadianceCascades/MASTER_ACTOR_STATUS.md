# DDGI Master Actor

The `DDGI Master Actor` is the intended level-wide configuration entry point
for the Radiance Cascades implementation. Place one instance in a level to
configure dynamic GI range, sparse-probe spacing and update budget, and the
static diffuse background used at distance. The background color is linear
space diffuse illumination; it is not intended to replace direct lighting.

## Current implementation status

This actor currently exposes and stores configuration in the editor. The
existing probe update and lighting path in this branch is still the imported
RTXGI implementation and still depends on DXR. The new actor settings are not
yet consumed by the renderer. Its presence does not mean that the non-DXR
Radiance Cascades runtime has been implemented.
