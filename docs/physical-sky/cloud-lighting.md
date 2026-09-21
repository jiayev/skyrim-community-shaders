# Cloud lighting

## Local solar occlusion

The main view and cubemap share local weighted sunlight probes. Their occlusion
feeds the non-storm profile response below. Probe count is
`int(10 - 6 * saturate((viewDistanceMetres - 512) * 0.00040192925))`.
The nominal extent is `240 - saturate(height * 3.3333333) * 120` metres.
Fractional sample indices start at 0.5; cubic spacing, a deterministic spatial
jitter, and forward-angle density weights form the local estimate. All local
noise probes use `floor(dimensionalProfile * 3 + 2)`. Their B/G wisp transition
uses the originating view sample's distance, not each probe's camera distance.

The response is paired with that local estimator, not an integral through the
entire cloud shell. A long grazing column can drive the soft-transmission term
to zero and over-attenuate the scattering volume. Distant same-layer occluders
outside the local probe support and cross-layer occlusion are not represented
by this lighting approximation.

Local probes use `extinction * (1 + 3 * localHeight^4)` in metres. The same density
feeds ground cloud shadows; `sunExtinction` scales solar attenuation separately
from view opacity.

The NDF layer has one `densityScale` in inverse metres. Its normalized noise
reconstruction is multiplied by that scale once; zero removes its view
extinction and shadow density. The initial value 0.75 comes from one captured
frame and remains provisional; neither the capture nor the formula comparison
establishes a suitable density default for the project.

## Profile scattering response

The response combines a soft-transmission term and a scattering volume, then
applies one blended dual-lobe phase to their sum. Both default Henyey-Greenstein
eccentricities are positive: a broad lobe plus a narrower forward lobe.
The second lobe also allows negative eccentricity when configured.
The empirical terms are not separate physical scattering orders.

With local height `h`, dimensional profile `d`, coverage/type product `p`,
light density `rhoL`, view/light cosine `mu`, and occlusion `o`:

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
phase = HG(mu, phaseForwardG) * phaseForwardWeight
      + HG(mu, phaseBackwardG) * phaseBackwardWeight
direct = 1.5 * (soft + volume) * phase * lerp(1, powder, powderStrength)
```

`bottomAngularResponse` and `powder` depend on height, view/light cosine,
profile potential, light density and occlusion. Their full expressions are in
`CloudLightResponse`. They do not depend on the view integration step length.

## Defaults and calibration

The current lighting defaults are based on one supplied captured parameter set.
They are adjustable weather controls, not physical constants or universal
settings. The phase weights intentionally retain a sum of 1.25. This is a
response gain as well as an angular distribution, and is not normalized away.

| Setting                  | Default |
| ------------------------ | ------: |
| `lightingScale`          |       1 |
| `sunExtinction`          |       1 |
| `phaseForwardG`          |     0.2 |
| `phaseBackwardG`         |     0.9 |
| `phaseForwardWeight`     |       1 |
| `phaseBackwardWeight`    |    0.25 |
| `scatterVolumeStrength`  |     0.5 |
| `scatterVolumeDepth`     |    0.04 |
| `scatterVolumeHeight`    |    0.29 |
| `softScatteringStrength` |       1 |
| `powderStrength`         |     0.5 |
| `ambientStrength`        |     3.6 |
| `ambientFloor`           |    0.23 |
| `ambientDensity`         |     0.5 |
| `ambientBase`            |       1 |

`CirrusSettings::lightingScale` defaults to 0.5. The regular profile density
reconstruction uses a default density scale of 0.75 /m. Its base shaping,
profile construction and mip selection are part of the same density contract;
a density multiplier alone does not reproduce that contract.

Ambient response uses height, coverage/type product, broad solar transmission
`exp(-0.03*q)`, and empty-profile fraction `(1-e)`. Its controls are
`ambientBase`, `ambientDensity`, `ambientFloor` and `ambientStrength`.
`lightingScale` scales both source terms.

It multiplies a per-sample ambient radiance. The atmospheric probe holds the sky
hemisphere's average radiance, reconstructed at the layer centre altitude; the
planet-facing half is filled in from the ground's reflected skylight,
`groundAlbedo * skyRadiance`, attenuated by the vertical air column between the
ground and the sample. The two halves are weighted by the planet's angular
radius at the sample, so their weights sum to one and the ground term fades with
altitude. Sampling is per view step, so a base under a low deck and a high sheet
see different ambient. Neither the ground's direct solar reflection nor its own
shadowing is modelled; the profile-density and occlusion shaping above
suppresses the ambient in dense regions instead. The ground column costs one
extra transmittance lookup per lit step. Atmospheric solar transmission
multiplies the solar source; aerial perspective is applied after view integration.

Solar colour uses the top-of-atmosphere irradiance and per-position atmospheric
transmittance. Planet visibility is evaluated against each position's local
horizon and a finite solar disc, so elevated clouds can remain sunlit after
lower clouds enter the planet shadow. Sun/moon source selection retains the
astronomical sun while any part of the visible cloud region can receive it.
Ground-level sunset fading is not applied to the solar source. Spatial RGB
variation therefore comes from the atmosphere and geometry, not a uniform
sunset colour multiplier.

The low-cloud density query uses the regular profile shape volume, dimensional
profile mip selection, and base density response described in the
[noise contract](noise-contract.md). The voxel threshold reducer is not used.

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
blends an upstream modeling sample, while leaving height sampling at the original column. Generated and imported maps
share this decoding. Missing settings use the defaults; obsolete low-cloud
base/thickness keys are ignored. Cirrus has one spherical altitude, default
2048 m; it has no volumetric thickness or low-NDF height channel.

## Cirrus sheet

Cirrus uses a separate two-dimensional weather map: R is coverage and G is type.
The RGB pattern channels are wispy, round and streaky; each is squared before
interpolating B to R to G at type 0, 0.5 and 1. With coverage `c` and interpolated
pattern `p`, the unscaled profile is `max(p, 1e-10)^(1.9 - 1.8*c) * saturate(2*c^3)`.
`cirrus.densityScale` is the clear-weather density multiplier (default 1).
Density is `2 * profile * cirrus.densityScale`; the fixed factor 2 is part of
the profile model. Storm modulation is not implemented.

Four deterministic light probes use indices 0.5, 1.5, 2.5 and 3.5, angular cubic
spacing over a nominal 120 m extent, and `13.5 * sqrt(density)` weights. Solar
response is `cirrus.lightingScale * 64 * exp(-0.1 * sunExtinction * occlusion)`
times the shared phase function and per-position atmospheric sunlight. Ambient
response is `ambientStrength * cirrus.lightingScale * (viewDirection.z + 1)`
times `(1 - 0.3*profile)^0.2` and the ambient radiance at the sheet's position.
Both clear-weather storm factors
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
