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
applies one blended dual-lobe phase to their sum. The default Henyey-Greenstein
response adds a broad lobe and a narrow forward lobe. Both controls also allow
negative eccentricity when configured.
The empirical terms are not separate physical scattering orders.

With local height `h`, dimensional profile `d`, coverage/type product `p`,
light density `rhoL`, view/light cosine `mu`, and occlusion `o`:

```text
e = saturate((d - 0.05) / 0.95)
t = 0.1 * (1 - saturate(mu))
a = saturate((e - t) / (1 - t))
q = sunExtinction * o
volume = 8 * scatterVolumeStrength * h^scatterVolumeHeightPower
       * (1 - 0.75 * saturate((mu - 0.5) * 2.0408163))
       * saturate(a * 6.666667 - 0.11111112) * p^0.25
       * exp(-q * scatterVolumeDepthPower)
softT = 1 - saturate(0.025 * q)
soft = 42.375 * softScatteringStrength * (a + rhoL)
     * [0.3 * (softT^(8 - 7*p^0.8) + softT^(16 - 14*p^0.8))
        + softT^(32 - 28*p^0.8)] * bottomAngularResponse
phase = HG(mu, phasePrimaryG) * phasePrimaryIntensity
      + HG(mu, phaseSecondaryG) * phaseSecondaryIntensity
direct = 1.5 * (soft + volume) * phase * lerp(1, powder, powderStrength)
```

`bottomAngularResponse` and `powder` depend on height, view/light cosine,
profile potential, light density and occlusion. Their full expressions are in
`CloudLightResponse`. They do not depend on the view integration step length.

## Brightness controls and defaults

All lighting controls live under `cloudLayer.lighting` and are numeric Scene
Manager controls. The UI separates shared lighting from low-cloud profile
scattering. `brightness` scales both layers; `lowBrightness` and
`cirrusBrightness` are independent relative gains. Cirrus retains a fixed 0.5
conversion before its sheet response. Exposure is applied by the existing
rendering pipeline, not by these controls. Changing brightness does not alter
view extinction or cloud shadows.

Phase, `sunExtinction` and `ambientStrength` are shared. Scattering volume,
soft scattering, powder, and the ambient floor/profile/base shaping affect only
low clouds. Cirrus has its own sheet response and still receives sunlight and
ambient radiance evaluated at its configured altitude.

The default uses one complete authored lighting preset rather than constructor
fallbacks or independently selected per-field averages. Its broad lobe supports
side and back illumination, while a weaker, narrower lobe supplies the forward
peak. Volume, powder and ambient shaping are kept together with their associated
gains. This is a practical model baseline, not an absolute radiometric calibration
or a guarantee of appearance under every density and atmospheric source.

The controls have distinct contracts:

-   Source quantities, such as solar irradiance and atmospheric transmission,
    retain their physical units and normalization.
-   Brightness corrections multiply the evaluated response. One preserves that
    response; it does not prove that an empirical response conserves energy.
-   Phase intensities, phase G, attenuation and height powers, and ambient shaping
    define the approximation. They remain editable and Scene Manager controlled.
-   Fixed coefficients belonging to the response formula stay with that formula.
    They must not absorb weather settings or arbitrary exposure compensation.

| Setting                                            |  Default | Model role                                                     |
| -------------------------------------------------- | -------: | -------------------------------------------------------------- |
| `brightness`                                       |      0.5 | Common response gain                                           |
| `lowBrightness`, `cirrusBrightness`                |     1, 1 | Independent layer gains                                        |
| `sunExtinction`                                    |        1 | Additional solar occlusion multiplier                          |
| `phasePrimaryG`, `phaseSecondaryG`                 | 0.2, 0.9 | Broad response plus narrow forward peak                        |
| `phasePrimaryIntensity`, `phaseSecondaryIntensity` |  1, 0.25 | Independent lobe intensities, not normalized blend weights     |
| `scatterVolumeStrength`                            |      0.5 | Scattering volume gain                                         |
| `softScatteringStrength`                           |        1 | Soft-transmission response gain                                |
| `scatterVolumeDepthPower`                          |     0.05 | Attenuation of the volume response                             |
| `scatterVolumeHeightPower`                         |      0.5 | Sublinear vertical response                                    |
| `powderStrength`                                   |      0.5 | Half-strength in-scattering probability modulation             |
| `ambientStrength`                                  |        2 | Ambient source gain                                            |
| `ambientFloor`                                     |      0.1 | Minimum ambient response before source and gain multiplication |
| `ambientProfilePower`                              |      4.1 | Actual empty-profile exponent                                  |
| `ambientBase`                                      |        1 | No additional suppression by the ambient base-height factor    |

### Why these response curves

Each phase lobe contains `1/(4*pi)`. The default weighted sum integrates to
1.25, so it is an empirical angular response, not a normalized scattering PDF.
Before brightness scaling it is 3.929138 sr^-1 toward the sun, 0.073582 at
90 degrees and 0.044761 opposite the sun. The broad lobe supplies most of the
side/back response. Normalizing this sum alone would change brightness without
making the full model energy conserving.

Nubis Cubed p29 illustrates separately phased transmission and multiple
scattering. This implementation instead phases the sum of its empirical soft
and volume terms. The illustration explains their intended roles; it does not
justify substituting its phase arrangement or a microscopic water-droplet G
without changing the response model as well. The voxel treatment on p136 uses
voxel distance and different optical-path data, so its attenuation coefficients
are not defaults for the local-probe model used here.

Let `q = sunExtinction * occlusion`. The soft response's base is
`max(0, 1 - 0.025*q)`, so it reaches zero at q=40. The volume response is
`exp(-q*scatterVolumeDepthPower)`, equivalent to raising `exp(-q)` to that power,
as in Nubis Evolved p54. Its role is to retain an illumination tail; the power
is not a scattering phase moment or a physical distance.

|   q | Soft base | Volume at power 0.05 | Ambient broad transmission |
| --: | --------: | -------------------: | -------------------------: |
|   5 |     0.875 |             0.778801 |                   0.860708 |
|  10 |     0.750 |             0.606531 |                   0.740818 |
|  20 |     0.500 |             0.367879 |                   0.548812 |
|  40 |         0 |             0.135335 |                   0.301194 |
|  80 |         0 |             0.018316 |                   0.090718 |

The volume has an e-folding q of 20; its default coefficient is
`8 * 0.5 = 4` before height, angle and profile modulation. The separate ambient
broad-transmission term retains its formula coefficient 0.03. Neither coefficient
is inferred from `1-g`.

These q values are relevant to the actual sampling code. In a homogeneous
region above normalized height 0.3, the 120 m probe extent gives
`q = 0.45*120*rhoL*(1+mu)*(1+0.2*N*mu)` for nonnegative view/light cosine mu,
unit solar extinction and N probes. At rhoL=0.25/m, q is 13.5 sideways and
48.6/81 toward the sun for 4/10 probes. This is a controlled homogeneous example,
not a distribution measured from a scene; jitter and spacing affect nonuniform
clouds. Its view-angle and probe-count dependence also shows why q cannot be
interpreted as a physical solar optical depth.

The volume height power 0.5 gives factors 0.316 at height 0.1, 0.5 at 0.25 and
0.707 at 0.5. Powder strength 0.5 gives `0.5 + 0.5*powder`, preserving half the
unmodulated response. Ambient base 1 makes its own height factor one; the
separate height dependence coupled to broad solar transmission remains.

`ambientProfilePower` stores the actual exponent. An authored normalized
ambient definition of 0.5 maps through `0.2 + 7.8*definition` to 4.1. The
published square-root illustration describes the approach but does not override
this fitted exponent. At eroded profiles 0.1, 0.5 and 0.9, the isolated
`(1-e)^4.1` factor is 0.649224, 0.058315 and 0.00007943.

The ambient floor and gain must be considered with that exponent. At height
0.5, coverage/type product 0.5 and q=20, the complete default ambient response
including low-cloud brightness is 0.456738 at e=0, 0.296525 at e=0.1 and 0.1
at e=0.5 or higher. The latter is the floor:
`brightness * lowBrightness * ambientStrength * ambientFloor = 0.1`.
It multiplies ambient source radiance, so it cannot emit light when that source
is zero. It is still an empirical minimum, not a physically solved transport
term. Increasing the profile exponent without retaining the paired floor and
gain would produce a different preset.

### Parameter upload and limits

```text
lowLightingScale    = brightness * lowBrightness
cirrusLightingScale = brightness * cirrusBrightness * 0.5
ambientStrengthGPU  = ambientStrength
softStrengthGPU     = softScatteringStrength
ambientProfilePowerGPU = ambientProfilePower
```

With the default brightness settings, the uploaded low-cloud scale is 0.5 and
the cirrus scale is 0.25 (0.5 _ 1 _ 0.5).

The fixed sheet conversion of 0.5 is retained as part of the source model's
parameter contract. It is not a physical albedo, a phase normalization, or a
reason to divide all cloud radiance by 32. The sheet's sunlight coefficient 64
and the low-cloud soft and volume coefficients remain unchanged.

Run `python tools/analyze-cloud-lighting.py` to reproduce the tables from the
current header defaults, without compiling or running the renderer. This checks
component behavior, not cloud reflectance, energy conservation or runtime image
quality. The local probes and empirical scattering approximation cannot provide
a unique physically exact preset for every geometry and optical thickness.

Existing configurations are not overwritten. **Reset Cloud Lighting** applies
the full preset; scene overrides are edited separately. Removed settings keys
are ignored. Density reconstruction is independent of lighting gains.

### Ambient and solar sources

Ambient response uses height, coverage/type product, broad solar transmission
`exp(-0.03*q)`, and empty-profile fraction `(1-e)`. Its controls are
`ambientBase`, `ambientProfilePower`, `ambientFloor` and `ambientStrength`.

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
8000 m; it has no volumetric thickness or low-NDF height channel.

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
response is `cirrusLightingScale * 64 * exp(-0.1 * sunExtinction * occlusion)`
times the shared phase function and per-position atmospheric sunlight. Ambient
response is `ambientStrengthGPU * cirrusLightingScale * (viewDirection.z + 1)`
times `(1 - 0.3*profile)^0.2` and the ambient radiance at the sheet's position.
Both clear-weather storm factors
are one. The sheet integrates `T_step = exp(-10*density)` once; its opacity does
not gain a grazing-angle path-length multiplier.

The main volume and sheet compose in distance order, then use the existing
representative-depth aerial perspective and full-resolution temporal history.
Sheet altitude is independent of the low-cloud trace interval and ambient-probe
reference height. The spherical sheet uses the cloud curvature radius rather
than the atmosphere radius; density and lighting use its ray intersection.
The global Cirrus UV Size enlarges both weather and detail patterns without
changing sheet altitude or physical wind speed. Self-shadow probes retain their
metre-based distances and sample the same scaled field. Raising the sheet or
changing UV size does not add view-ray steps or light samples. The
sheet intersection is not clipped by the low-cloud march range, and cloud depth
is stored in kilometres. Scene geometry and the planet can still occlude it;
aerial perspective can reduce distant contrast. Height and the lighting controls are
numeric Scene Manager controls.
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

Scattering references:

-   [PBRT: Phase functions](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions) — normalization of individual phase functions and interpretation of mean cosine.
-   [PBRT: Volume scattering processes](https://www.pbr-book.org/4ed/Volume_Scattering/Volume_Scattering_Processes) — scattering albedo and extinction.
