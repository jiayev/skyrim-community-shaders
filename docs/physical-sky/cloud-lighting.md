# Cloud lighting

## Solar visibility cache

The main view and cubemap share two local density probes followed by a cached
solar column, following the sampling split described in Nubis Cubed. Density
remains procedural NDF/profile/noise sampling.

Local extent is `240 - saturate(height * 3.3333333) * 120` metres. The two
intervals use cubic spacing adjusted by view/light angle and deterministic
midpoint samples. The distant column starts after the local extent. Both paths
integrate `extinction * (1 + 3 * localHeight^4)` in metres. The same density
feeds ground cloud shadows; `sunExtinction` scales solar attenuation separately
from view opacity.

Each layer has one `densityScale` in inverse metres. Low-cloud noise returns a
normalized reconstruction and is multiplied by that scale once. High-cloud
weather/cell density includes its alpha modulation and is likewise scaled once;
its former density and view-absorption factors are consolidated. There is no
separate coverage-dependent light absorption. Zero density scale removes the
layer from view extinction, solar columns and its ground shadow density.
The low default remains 0.09; the high default is 0.175, combining the former
0.35 and 0.5 defaults. These are resource-specific optical scales. A CPU sample
of the bundled low noise at coverage 0.25/0.5/0.75, profile 0.5/1 and roundness
0.5 gives mean-density attenuation lengths of roughly 14–53 m at scale 0.09;
this is not a spatial cloud transmission measurement or a weather calibration.

Two 64 x 64 x 16 R16_FLOAT volumes store each layer's solar column, for 256 KiB
total. Low/high XY spans are 256/1024 km. Signed-square placement concentrates
cells near the captured camera origin; Z is spherical altitude. Coordinates
advect with shared wind. Columns use quadratic midpoint quadrature with
`cacheSteps` (default 16, 4–32) and stop at the planet.

Cache lookup locates the neighbouring nonuniform grid points, then computes
interpolation weights from their physical positions. Direct linear filtering
of signed-square-root UVs introduces a square-root cusp on both centre axes,
even for a perfectly linear optical-depth field. Physical-distance weights
reproduce that field across the axes without the cusp. The outer 10% blends
to a bounded four-probe-per-segment fallback.

Each successful capture updates one eighth of the cache slices. Initial
generation, density/texture changes, time discontinuities, a 1 km displacement
from the advected origin or a large light-direction change rebuild all slices.
DR changes retain temporal history with the previous capture's viewport.
`crossLayerShadows` samples the other layer at its solar entry point. It does
not add the other layer to both cache integrals.

The ambient response below uses the local profile rather than an upward
optical-depth cache. Finite solar quadrature and coarse distant cells can still
miss narrow occluders, especially at grazing sun angles.

## Profile scattering response

Lighting has one weighted forward/backward Henyey–Greenstein phase function.
The non-storm source combines a scattering volume and a soft-transmission
response, with a powder response applied to their sum. The phase is applied
once to the result. There is no separate multiple-scattering phase lobe.

With local height `h`, dimensional profile `d`, coverage/type product `p`,
light density `rhoL`, view/light cosine `mu`, and occlusion integral `o`:

```text
e = saturate((d - 0.05) / 0.95)
t = 0.1 * (1 - saturate(mu))
a = saturate((e - t) / (1 - t))
q = sunExtinction * o
volume = 8 * scatterVolumeStrength * h^scatterVolumeHeight
       * (1 - 0.75 * saturate((mu - 0.5) * 2.0408163))
       * saturate(a * 6.666667 - 0.11111112) * p^0.25
       * exp(-q * scatterVolumeDepth)
softT = 1 - saturate(0.025 * q)
soft = 42.375 * softScatteringStrength * (a + rhoL)
     * [0.3 * (softT^(8 - 7*p^0.8) + softT^(16 - 14*p^0.8))
        + softT^(32 - 28*p^0.8)] * bottomAngularResponse
sunResponse = 1.5 * (volume + soft) * lerp(1, powder, powderStrength)
```

`bottomAngularResponse` and `powder` depend on height, view/light cosine,
profile potential, light density and occlusion. Their full expressions are in
`CloudLightResponse`. They do not depend on the view integration step length.

Ambient response uses height, coverage/type product, broad solar transmission
`exp(-0.03*q)`, and empty-profile fraction `(1-e)`. Its controls are
`ambientBase`, `ambientDensity`, `ambientFloor` and `ambientStrength`. It
multiplies isotropic radiance reconstructed from the project's atmospheric SH
probe. `lightingScale` scales both source terms. Atmospheric solar transmission
and other-layer solar visibility multiply the solar source; aerial perspective
is applied after view integration.

High clouds use the same response with their existing procedural weather/cell
profile and extinction. This is an adaptation for the project's volumetric high
layer, not a replacement with a thin cirrus texture or a voxel density model.
The bundled low-cloud noise reducer is unchanged. Low and high cloud density
therefore remain project resource adaptations. Default lighting values are
project starting values, not a universal weather preset.

Old top/bottom ambient multipliers, upward AO, separate high-cloud phase/MS
controls, high sky blending, tinted extinction, and secondary-phase scaling are
absent from settings, serialization and shaders. Obsolete saved keys are ignored;
new response controls load their defaults. Cloud-ground shadow shared-buffer
fields retain their existing ABI but carry the scalar solar extinction scale.

## Height contract

Low-cloud NDF height decodes as `ndfAltitudeOffset + ndfAltitudeScale * value`
metres above the worldspace reference altitude. The Low Clouds controls are
NDF Base Altitude (default 256 m) and NDF Height Span (default 1792 m). The NDF
RG pair controls each column's bottom and top within that interval. The same
validated interval bounds ray traversal, light caches and ground cloud shadows.
The base is limited to 0–20000 m and the span to 1–20000 m. Changing either
control invalidates temporal history and rebuilds the lighting cache.

Shear shifts modeling coverage/type using the local NDF height fraction and
leaves height sampling at the original column. Generated and imported maps
share this decoding. Missing settings use the defaults; obsolete low-cloud
base/thickness keys are ignored. High clouds retain their independent
procedural weather height band because that representation does not use the
low NDF.

## Validation

Static/CPU checks cover host/shader fields and resources, nonuniform cache
interpolation across both axes, the non-storm response equations, and parameterized NDF
height decoding. These checks do not establish runtime appearance or performance.
GPU acceptance still needs camera translation across both axes, cache refreshes,
layer overlap, sunrise/sunset, and generated/imported NDF heights.

-   [Nubis Evolved, SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf)
-   [Nubis Cubed, SIGGRAPH 2023 materials](https://advances.realtimerendering.com/s2023/)
-   [Sampling and temporal reconstruction](cloud-sampling.md)
-   [NDF generation and input contract](ndf-generator.md)
