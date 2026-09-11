# Cloud lighting

## Local solar occlusion

The main view and cubemap share local weighted sunlight probes. Their occlusion
feeds the non-storm profile response below. Probe count is
`int(10 - 6 * saturate((viewDistanceMetres - 512) * 0.00040192925))`.
The nominal extent is `240 - saturate(height * 3.3333333) * 120` metres.
Fractional sample indices start at 0.5; cubic spacing, a deterministic spatial
jitter, and forward-angle density weights form the local estimate. All local
noise probes use mip 2 without the near-camera folded detail.

The response is calibrated to that local estimator, not an integral through the
entire cloud shell. A long grazing column can drive the soft-transmission term
to zero and over-attenuate the scattering volume. Distant same-layer occluders
outside the local probe support and cross-layer occlusion are not represented
by this lighting approximation.

Local probes use `extinction * (1 + 3 * localHeight^4)` in metres. The same density
feeds ground cloud shadows; `sunExtinction` scales solar attenuation separately
from view opacity.

The NDF layer has one `densityScale` in inverse metres. Its normalized noise
reconstruction is multiplied by that scale once; zero removes its view
extinction and shadow density. The default 0.09 is a resource-specific optical
scale, not a universal weather calibration.

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
multiplies the solar source; aerial perspective is applied after view integration.

Solar colour uses the top-of-atmosphere irradiance and per-position atmospheric
transmittance. Planet visibility is evaluated against each position's local
horizon and a finite solar disc, so elevated clouds can remain sunlit after
lower clouds enter the planet shadow. Sun/moon source selection retains the
astronomical sun while any part of the visible cloud region can receive it.
Ground-level sunset fading is not applied to the solar source. Spatial RGB
variation therefore comes from the atmosphere and geometry, not a uniform
sunset colour multiplier.

The bundled low-cloud noise reducer is unchanged. Its density and the default
lighting values remain project resource adaptations, not a universal weather
preset.

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
validated interval bounds ray traversal and ground cloud shadows.
The base is limited to 0–20000 m and the span to 1–20000 m. Changing either
control invalidates temporal history.

Shear shifts modeling coverage/type using the local NDF height fraction and
leaves height sampling at the original column. Generated and imported maps
share this decoding. Missing settings use the defaults; obsolete low-cloud
base/thickness keys are ignored. Cirrus has one spherical altitude, default
2048 m; it has no volumetric thickness or low-NDF height channel.

## Cirrus sheet

Cirrus uses a separate two-dimensional weather map: R is coverage and G is type.
The RGB pattern channels are wispy, round and streaky; each is squared before
interpolating B to R to G at type 0, 0.5 and 1. With coverage `c` and interpolated
pattern `p`, the unscaled profile is `max(p, 1e-10)^(1.9 - 1.8*c) * saturate(2*c^3)`.
`cirrus.densityScale` multiplies this profile (default 2). It absorbs the constant
clear-weather density multiplier; storm modulation is not implemented.

Four deterministic light probes use indices 0.5, 1.5, 2.5 and 3.5, angular cubic
spacing over a nominal 120 m extent, and `13.5 * sqrt(density)` weights. Solar
response is `cirrus.lightingScale * 64 * exp(-0.1 * sunExtinction * occlusion)`
times the shared phase function and per-position atmospheric sunlight. Ambient
response is `ambientStrength * cirrus.lightingScale * (viewDirection.z + 1)`
times `(1 - 0.3*profile)^0.2` and sky radiance. Both clear-weather storm factors
are one. The sheet integrates `T_step = exp(-10*density)` once; its opacity does
not gain a grazing-angle path-length multiplier.

The main volume and sheet compose in distance order, then use the existing
representative-depth aerial perspective and full-resolution temporal history.
The old weather/cell/warp/wisp volume and its view-step budget are absent.
Old `cloudLayer.high` saved settings are ignored; `cloudLayer.cirrus` loads its
own defaults. See [cirrus inputs](volumetric-cloud-texture-resources.md#cirrus-inputs).

## Validation

Static/CPU checks cover host/shader fields and resources, the non-storm response
equations, local probe positions/weights, and parameterized NDF height decoding. CPU atmosphere checks
cover spatial RGB differences and altitude-dependent sunset visibility. These checks do not establish runtime appearance or performance.
GPU acceptance still needs camera motion, layer overlap, sunrise/sunset, and
generated/imported NDF heights.

-   [Nubis Evolved, SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf)
-   [Nubis Cubed, SIGGRAPH 2023 materials](https://advances.realtimerendering.com/s2023/)
-   [Sampling and temporal reconstruction](cloud-sampling.md)
-   [NDF generation and input contract](ndf-generator.md)
