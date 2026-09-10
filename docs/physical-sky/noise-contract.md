# Bundled cloud noise reconstruction

## Scope

The bundled `nubis.dds` is used as a four-channel threshold-noise resource.
`CloudNoise.hlsli` defines its reconstruction contract. This is independent of
how cloud coverage, shape type and vertical profile are stored: the current
implementation obtains them from two-dimensional control maps and profile LUTs.
No world-space density volume or new texture asset is introduced.

The public [Nubis, Evolved presentation](https://www.guerrilla-games.com/read/nubis-evolved)
is background for vertical-profile cloud modeling. This implementation is not
claimed to reproduce the presentation's illustrated sky-noise asset or all its
rendering behavior.

## Verified file properties

| Property        | Value                                                            |
| --------------- | ---------------------------------------------------------------- |
| Dimensions      | 128 x 128 x 128                                                  |
| Mip count       | 8                                                                |
| Encoding        | Legacy DDS, uncompressed RGBA8                                   |
| Channel masks   | 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000                   |
| Payload offset  | 128 bytes                                                        |
| File length     | 9,587,108 bytes                                                  |
| Payload SHA-256 | d3179e35ce38febce126f7d683de45ded3154b6bfcca9c625d8d12620b47afea |

Statistics below are over the complete mip-0 volume, decoded to linear [0, 1]
and accumulated in float64. They describe numerical data, not channel meaning.

| Channel |     Mean | Standard deviation |
| ------- | -------: | -----------------: |
| R       | 0.126204 |           0.181594 |
| G       | 0.135350 |           0.180776 |
| B       | 0.388657 |           0.209794 |
| A       | 0.396538 |           0.217184 |

## Controls and channel use

| Input                | Physical Sky mapping                                  |
| -------------------- | ----------------------------------------------------- |
| Coverage `c`         | NDF coverage, clamped to [0, 1]                       |
| Roundness `r`        | `cloudLayer.low.noiseRoundness`, clamped to [0, 1]    |
| Vertical profile `v` | Product of the evaluated top and bottom profile LUTs  |
| Noise RGBA           | One wrapped linear sample of the bundled noise volume |

Coverage and vertical profile remain separate until the final density response.
Roundness is a dedicated shape control; it is not the evaluated bottom-profile
density and is not implicitly inferred from the authored bottom-type selector.
The default 0.5 is a project starting value, not a recovered art configuration.
Missing configuration fields receive that default.

The reconstruction is:

```text
base      = lerp(G, R, c)
cellular  = lerp(0.2 * A + 0.1, 0.3 * B, c^(1/16))
threshold = lerp(base, cellular, r)
eroded    = saturate((c - threshold) / (1 - threshold))
h         = v^4
density   = (h * eroded)^(0.3 + 0.3 * h)
```

R/G are alternative base-threshold signals, with R approached as coverage
increases. B/A contribute a cellular threshold; they are not two arbitrarily
interchangeable density channels. At low coverage A and its offset matter more;
at higher coverage the B term becomes dominant. Roundness selects between the
base and cellular thresholds. Extinction coefficient is applied after this
normalized reconstruction.

An empty coverage/profile returns zero. If `coverage <= threshold`, the sample
returns zero before division or powering. This also defines the fully saturated
`coverage == threshold == 1` corner as empty instead of producing 0/0.
The guards keep zero support from becoming a tiny positive density through a
logarithm clamp. Density remains in [0, 1] before physical extinction scaling.

## Near detail and sampling

For detail-enabled samples below 150 m, the same fetched G/A channels also
supply folded signals:

```text
foldA = abs(abs(2 * A - 1) * 2 - 1)
foldG = abs(abs(2 * G - 1) * 2 - 1)
nearThreshold = lerp(1 - foldG^4, foldA^2, roundness)
threshold = lerp(nearThreshold, threshold,
                 0.9 + 0.1 * saturate((distanceMeters - 50) / 100))
```

This changes at most 10% of the threshold and fades to the ordinary threshold
at 150 m. It uses no second noise texture fetch. Distance is measured from the
actual camera position in the cloud coordinate frame and converted to metres.
The 50–150 m interval is the project's physical-unit interpretation.

The texture keeps its physical repeat scale, offset and shared wind advection.
Samples use the caller's explicit mip level. Coarse light-cache and ground-shadow
queries retain their existing coarser mip and disable the near folded term;
local light queries also omit the folded term, while view queries can include
it. Local light mip is `clamp(log2(max(intervalNoiseTexels, 1)), 0, 3)`, using
the interval's world length, noise frequency and largest texture dimension.
The upper bound limits smoothing because filtered noise is not filtered density
after nonlinear threshold reconstruction. All use the same reconstruction
helper. A logarithmic view-distance mip ramp requires separate footprint
calibration and is not inferred from density or profile strength here.

Camera-dependent density fading and additional view-only exponent/intensity
compensation are not part of this material contract. Omitting them avoids
introducing a separate view-only cloud density response into the lighting and
shadow integration. The near threshold remains an explicitly bounded detail
approximation, not a claim of identical view and cache sampling.

## Integration changes

The old profile-based R/G mixture, scaled B/A mixture and bottom-profile mixture
were replaced together with their coverage-relief term and kilometre-distance
channel fade. The extra lower-frequency rotated texture sample, hidden noise
height shear and bottom warp were removed. The CPU-generated warp texture and
its bindings were also removed; t8 now carries the NDF modeling texture.

The old 0.7 profile cap, 0.975 composite gain and derived 0.025 profile cutoff
are absent. These do not belong to the threshold contract above. NDF shape
shear remains a separate control-map operation. Acceleration stays conservative
because its occupancy bounds positive coverage before any noise reduction;
no new density is created outside the profile or coverage support.

This changes cloud opacity and shape for existing presets. Noise repeat scale
and density-to-extinction scale retain their units, but old visual tuning is not
expected to give the same image. The mapping of NDF coverage, profile and the
new roundness control is a deliberate project adaptation.

## Verification scope

Static review checks the original expression structure against the reconstructed
helper and verifies the CPU/HLSL field layout, settings/UI, every density consumer
and removed-resource references. An external CPU algebra check used 250,000
sampled noise/control combinations; the maximum double-precision difference
between the direct log/exp expression and the power form was below 4e-13.
The zero guards intentionally define otherwise singular boundary cases.

No C++ build, shader compilation or GPU comparison was performed. Runtime
acceptance should cover roundness endpoints, low coverage, profile edges,
near-detail transitions, moving cameras and cloud/ground shadow agreement.
The local control-field generator is described in
[procedural NDF generation](ndf-generator.md). Modeling R supplies coverage
independently of the vertical profile. At coverage one, normalized threshold
reduction becomes one wherever the threshold is below one, removing that noise
variation. Continuous coverage below one preserves sensitivity to this reducer.
