# Cloud lighting

## Local detail and distant visibility

The rendering split follows Nubis Cubed slides 123–124: two local density probes,
then a cached distant solar column. Density remains procedural NDF/profile/noise
sampling; the lighting cache contains optical depth, not a voxel cloud model.
The main view and cubemap share the cache and lighting functions.

Local extent is `240 - saturate(height * 3.3333333) * 120` metres. Its two
intervals use cubic spacing adjusted by view/light angle and deterministic
midpoint samples. Each density is weighted by the physical interval length.
Low-cloud local noise uses mip 1 without near-camera folded detail. The cached
solar remainder starts at the end of the local extent, excluding that extent
from the distant column.

Two 64 x 64 x 16 RG16_FLOAT volumes store low/high layer columns:

| Channel | Integral                                        |
| ------- | ----------------------------------------------- |
| R       | Own layer towards the directional light         |
| G       | Own layer towards the local radial up direction |

Together they use 512 KiB. The low/high XY spans are 256/1024 km. Signed-square
coordinates concentrate cells near the captured camera origin; Z represents
spherical altitude in the layer. Cached coordinates advect with shared wind.
Columns use quadratic midpoint quadrature with `cacheSteps` (default 16, 4–32).
They retain the configured in-layer range limit and stop at the planet.

Each successful capture updates one eighth of the cache slices; a complete
refresh spans eight captures. Initial generation, density/texture changes,
time discontinuities, a 1 km displacement from the advected origin or a large
light-direction change rebuild all slices. Screen-resolution changes invalidate
screen history independently. Updates compute only the two own-layer columns. Other-layer visibility samples that layer's cache at its
entry point, instead of constructing duplicate columns in both volumes.
Outside cache support, solar visibility has a fixed four-probe-per-segment
fallback. Ambient visibility uses the local profile when its own cached column
is unavailable. There are no full-quality upward/cross-layer fallback marches
at every view sample. Finite quadrature and coarse distant cells can miss narrow
occluders, particularly at grazing sun angles.

`crossLayerShadows` controls other-layer solar attenuation and upward high-cloud
attenuation. Other-layer solar transmittance multiplies incident direct light.
The separate low-cloud ground shadow volume retains its existing receiver path.

## Scattering model

Primary light uses normalized dual-lobe Henyey–Greenstein. Low-cloud secondary
light uses the Nubis Evolved dimensional-profile scattering volume:

```text
volume = saturate((3 * dimensionalProfile - 0.1) / 0.9)
       * (coverage * topType)^0.25 * height^msHeightPower
light = exp(-tintedOpticalDepth) * primaryPhase
      + volume * exp(-tintedOpticalDepth * msDepthPower)
        * secondaryPhase * msContribution
```

The fixed 3 m reference length replaces a variable view step in the shaping
term so changing the view budget does not change the scattering source.
`msDepthPower` defaults to 0.1 and `msHeightPower` to 0.5; these are project
controls, not universal weather values. High clouds use their own profile,
phase and attenuation controls with the same two-component lighting structure.
Atmospheric solar transmittance is evaluated at each scattering position.

Ambient visibility follows the profile factor from Nubis Evolved slide 59 and
the additional column attenuation from Nubis Cubed slide 144:
`sqrt(1 - dimensionalProfile) * exp(-upwardOpticalDepth * aoUpwardScale)`.
It modulates the sky/ground environment radiance reconstructed from the ambient
SH probe. Aerial perspective is applied once at the visible-opacity-weighted
cloud depth after front-to-back integration.

The previous PhiFwd diffusion integral, selectable legacy step-dependent
scattering, three-octave directional sum, light-step LOD and optional high-cloud
thin-layer branch have been removed. Their settings are ignored on loading
older configurations and are omitted on save. Existing NDF generation,
imported map support and the bundled noise contract remain independent.

## Validation and references

CPU/static checks do not establish GPU performance or final appearance. Runtime
acceptance needs main/cubemap timings, sunset and night lighting, cloud edges,
camera/wind motion, cache refreshes, layer overlap and geometry disocclusion.

-   [Nubis Evolved, SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf), slides 48, 54, 59 and 187.
-   [Nubis Cubed, SIGGRAPH 2023 materials](https://advances.realtimerendering.com/s2023/), slides 123–124, 144 and 151.
-   [Sampling, temporal reconstruction and storage](cloud-sampling.md).
