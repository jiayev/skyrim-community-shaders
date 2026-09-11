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

The response combines a transmittance/soft term and the Nubis Evolved p54
`ms_volume` multiple-scattering volume, and applies one blended dual-lobe phase to
their sum, exactly as HFW's `MainLightResponse` does. Both Henyey-Greenstein
eccentricities are positive: the broad lobe dominates and the sharp one only adds
the forward rim. That split is what Nubis Evolved p43 describes as
`Direct = Transmittance * Primary Phase + Multiple Scattering * Secondary Phase`;
HFW encodes both in one blend, and its voxel path separates them explicitly
(`direct` takes `CloudPhase`, `indirect` is added with no phase).

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

## Default anchoring

The defaults come from HFW's volumetric cloud constant buffer (`b0, space8`,
`RenderingComputeShaderPS5`). The dump is verified against two independent
invariants: `c45.x = 74946` equals `CloudEarthRadius` in the game's own
`ProceduralCloudModelingSettings`, and `c34.zw = (256, 2048)` equals the cloud
base/top used by the `*1792 + 256` height encoding in the same shader. `c0`-`c35`
and `c46`-`c54` are per-frame host data (camera, light direction, dispatch bounds,
storm centre); `c36`-`c45` carry the cloud lighting and shaping constants.

```text
c37 = (0.9611693, 0.4378709, 0.5,       1        )
c38 = (0.891274,  1,         1,         0.5      )
c39 = (0.0408726, 0.2888307, 0.2267016, 0.5      )
c40 = (0.3664921, 3.611693,  15.37871,  0.2      )
c41 = (0.9,       1,         0.25,      0.5      )
c42 = (1,         1,         8000,      250      )
```

This is a **runtime** buffer, not authored data. Some entries are round authored
constants (`1`, `0.5`, `0.25`, `0.2`, `0.9`, `8000`); the rest carry seven
significant digits (`0.9611693`, `0.0408726`, `0.2267016`, `3.611693`), which is
what falls out of Decima's weather blending between presets. So the dump gives one
weather state, not a neutral baseline, and no entry here is a physical constant:
the whole response is an approximation whose absolute scale is carried by the
literal `42.375`, `8` and `1.5` inside the shader.

Defaults therefore use HFW's exact value where it is an authored constant of the
model, and a two-digit nominal value where the dump only supplies a blended
weather sample:

| setting                  | HFW field                              | default | source                             |
| ------------------------ | -------------------------------------- | ------- | ---------------------------------- |
| `lightingScale`          | c37.w `mainLightingScale`              | 1       | authored                           |
| `sunExtinction`          | c37.x `mainExtinction`                 | 1       | neutral; dump 0.9611693 is blended |
| `phaseForwardG`          | c40.w                                  | 0.2     | authored                           |
| `phaseBackwardG`         | c41.x                                  | 0.9     | authored                           |
| `phaseForwardWeight`     | c41.y                                  | 1       | authored                           |
| `phaseBackwardWeight`    | c41.z                                  | 0.25    | authored                           |
| `scatterVolumeStrength`  | c38.w                                  | 0.5     | authored                           |
| `scatterVolumeDepth`     | c39.x `cMultipleScatteringDepthPower`  | 0.04    | nominal, dump 0.0408726            |
| `scatterVolumeHeight`    | c39.y `cMultipleScatteringHeightPower` | 0.29    | nominal, dump 0.2888307            |
| `softScatteringStrength` | c38.y                                  | 1       | authored                           |
| `powderStrength`         | c41.w                                  | 0.5     | authored                           |
| `ambientStrength`        | c40.y                                  | 3.6     | nominal, dump 3.611693             |
| `ambientFloor`           | c39.z                                  | 0.23    | nominal, dump 0.2267016            |
| `ambientDensity`         | c39.w                                  | 0.5     | authored                           |
| `ambientBase`            | c42.y                                  | 1       | authored                           |

`CirrusSettings::lightingScale` is c37.z = 0.5.

Two things are worth recording, without pretending either is physics:

-   HFW's two phase weights sum to 1.25, so its phase integrates to 1.25 over the
    sphere rather than 1. The excess is absorbed by `lightResponse`; it is not
    corrected here, so that the phase shape matches the reference.
-   `ambientFloor * ambientStrength` is what keeps a self-shadowed cloud body from
    going black once `q` saturates the transmittance term. The previous project
    values gave 0.05 against 0.82 here, which is why cloud bodies read as flat grey
    while only the forward rim stayed lit.

`LowCloudSettings::densityScale` is HFW's c45.y `curvatureAndDensity.y`, which the
dump gives as 0.75 against our 0.09. It is deliberately not transferred: it scales
the NDF density reconstruction, and the view opacity, ground cloud shadow and
temporal history are all tuned against the current value. The response constants
above are independent of it.

HFW's density is also roughly an order of magnitude above ours, so its `q`
saturates in a dense cloud body and the body there is carried entirely by the
ambient pair. With our thinner density the direct term stays alive deeper into the
cloud.

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
`cirrus.densityScale` is the clear-weather density multiplier (default 1).
Density is `2 * profile * cirrus.densityScale`; the fixed factor 2 is part of
the profile model. Storm modulation is not implemented.

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
