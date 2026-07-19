# Volumetric Cloud Texture Resources

## Scope

This document defines every texture input used by the current Physical Sky volumetric-cloud implementation. It covers:

-   repository and runtime locations;
-   required and optional resources;
-   shader bindings;
-   dimensions, formats, channels, coordinate systems, samplers, and mip requirements;
-   generated fallbacks and external DDS overrides;
-   authoring constraints and validation;
-   bundled legacy resources that are not used by the current renderer.

It does not specify transient render targets such as cloud luminance, transmittance, history, cubemap, ambient-probe, or shadow-cookie textures. Those are allocated by the renderer and are not authored assets.

## Quick inventory

| Logical resource       | Shader binding | Resource type             | Default source          | File required?   |
| ---------------------- | -------------: | ------------------------- | ----------------------- | ---------------- |
| Base shape volume      |           `t5` | `Texture3D<unorm float4>` | `nubis.dds`             | Yes              |
| Detail erosion volume  |           `t6` | `Texture3D<unorm float4>` | Same SRV as `nubis.dds` | No separate file |
| Low weather map        |           `t7` | `Texture2D<float4>`       | CPU-generated           | No               |
| High weather map       |           `t8` | `Texture2D<float4>`       | CPU-generated           | No               |
| Low-cloud profile LUT  |          `t11` | `Texture2D<float4>`       | CPU-generated           | No               |
| Stratocumulus cell map |          `t12` | `Texture2D<float4>`       | CPU-generated           | No               |
| High-cloud cell map    |          `t13` | `Texture2D<float4>`       | CPU-generated           | No               |
| High-cloud warp map    |          `t14` | `Texture2D<float4>`       | CPU-generated           | No               |
| High-cloud wisp map    |          `t15` | `Texture2D<float4>`       | CPU-generated           | No               |

The renderer requires all nine SRVs to exist. Only the 3D Nubis volume must be supplied on disk; the seven 2D cloud-description resources are created automatically and may be replaced individually with DDS files.

## Directories and deployment

### Repository source

The required volume texture is stored at:

```text
features/Physical Sky/Textures/PhysicalSky/nubis.dds
```

The feature packaging rules copy the contents of the feature directory into the mod's data root. The installed asset is therefore:

```text
Textures/PhysicalSky/nubis.dds
```

When addressed from the game executable's working directory, the loader uses:

```text
Data/Textures/PhysicalSky/nubis.dds
```

### External overrides

The generated 2D inputs have no mandatory on-disk directory. An override may be loaded from any path accepted by `CreateDDSTextureFromFile`. In a normal game launch, a relative path is resolved from the game working directory, so a conventional override path is:

```text
Data/Textures/PhysicalSky/Overrides/<name>.dds
```

Absolute paths also work for local development, but should not be used in distributable settings.

The persisted override keys are:

```text
cloudMap.overrides.lowWeatherPath
cloudMap.overrides.highWeatherPath
cloudMap.overrides.profilePath
cloudMap.overrides.scCellPath
cloudMap.overrides.highCellPath
cloudMap.overrides.highWarpPath
cloudMap.overrides.highWispPath
```

## Common requirements

### File type and color space

-   Disk-loaded resources must be DDS files. The current loader is the DirectX DDS loader, not a general image loader.
-   Cloud data is linear scalar data. Use UNORM or floating-point linear formats, never an sRGB SRV format.
-   `R8G8B8A8_UNORM` is the preferred 2D override format because it exactly matches generated resources and preserves every defined channel.
-   Block compression is not recommended for weather masks, type selectors, profiles, or vector warps. Compression artifacts can create false coverage, wrong cloud types, broken profile edges, or biased warp vectors.
-   All defined scalar channels use normalized `[0, 1]` values.

### Samplers

Two sampler conventions are used:

| Convention     | Filtering          | Addressing  | Resources                                       |
| -------------- | ------------------ | ----------- | ----------------------------------------------- |
| Finite/clamped | Min/mag/mip linear | Clamp U/V/W | Low weather, high weather, profile              |
| Tileable       | Min/mag/mip linear | Wrap U/V/W  | Nubis, Sc cell, high cell, high warp, high wisp |

Tileable resources must be seamless across every wrapped axis. For the 3D volume this means seamless X, Y, and Z boundaries. For auxiliary 2D maps it means seamless U and V boundaries.

### Mips

-   Mips must be generated from linear data, not gamma-encoded data.
-   `nubis.dds` is sampled at explicit fractional LODs. At least mip levels 0 through 4 must exist; a complete chain is preferred.
-   High Weather R is sampled at mip 2 as a whole-ray high-cloud gate. An override should contain at least mip levels 0 through 2. A complete chain is preferred.
-   The other current 2D lookups use mip 0, although generated resources still receive complete mip chains.
-   Type and mask channels must remain semantically valid after filtering. Keep categorical regions spatially coherent and avoid isolated one-pixel type changes.

### Weather-map world coordinates

Weather UV is calculated as:

```text
uv = (worldXY - weatherCenter) / weatherWorldSize + 0.5
```

Therefore:

-   U increases with world X;
-   V increases with world Y;
-   `(0.5, 0.5)` is `weatherCenter`;
-   the map covers a square with side length `weatherWorldSize`;
-   values outside `[0, 1]` are explicitly rejected before sampling;
-   coverage should fade to zero near every map edge to prevent a vertical rectangular cloud wall.

`weatherCenter` and `weatherWorldSize` are authored in kilometres and converted to game units before shader use.

## 1. Nubis 3D volume

### Location and current metadata

| Property        | Current value                                          |
| --------------- | ------------------------------------------------------ |
| Repository path | `features/Physical Sky/Textures/PhysicalSky/nubis.dds` |
| Runtime path    | `Data/Textures/PhysicalSky/nubis.dds`                  |
| Resource type   | 3D volume texture                                      |
| Dimensions      | `128 x 128 x 128`                                      |
| Format          | 32-bit RGBA UNORM (`8 bits/channel`)                   |
| Mip levels      | 8, complete chain                                      |
| Addressing      | Wrap X/Y/Z                                             |
| Filtering       | Trilinear with explicit LOD                            |

The loader rejects this file if its SRV is not a `Texture3D` view. It currently does not validate the exact dimensions or format, so asset validation must happen before shipping.

### Dual binding

The same loaded SRV is deliberately bound twice:

-   `t5` as the base-shape noise volume;
-   `t6` as the detail-erosion noise volume.

There is no separate erosion DDS in the current implementation. The two shader paths sample the same data using independent world-space frequencies, offsets, wind speeds, and mip levels.

### Channel contract

| Channel | Current interpretation                                  |
| ------- | ------------------------------------------------------- |
| R       | Base shape at `t5`; low-frequency wispy erosion at `t6` |
| G       | High-frequency wispy erosion at `t6`                    |
| B       | Low-frequency billowy erosion at `t6`                   |
| A       | High-frequency billowy erosion at `t6`                  |

The base path currently evaluates `pow(abs(R), 0.6)`. The detail path forms two weighted signals:

```text
billowy = B * billowyLow + A * billowyHigh
wispy   = R * wispyLow   + G * wispyHigh
```

### Authoring requirements

-   All four channels must contain continuous, correlated 3D noise rather than independent 2D slice images.
-   Adjacent Z slices must be continuous; discontinuities appear as horizontal layers during vertical sampling and wind animation.
-   The volume must tile seamlessly across all three axes.
-   R must work both as a usable base density field and as one component of wispy erosion.
-   Lower-frequency and higher-frequency pairs should be meaningfully separated. If the two channels in a pair contain the same scale, their UI weights become redundant.
-   Preserve the full mip chain. Base sampling uses dynamic LOD and detail sampling increases toward mip 4 with view distance.

### Failure behavior

If `nubis.dds` is missing, fails to load, or is not a 3D texture, both base and detail SRVs become unavailable and volumetric-cloud rendering returns before dispatch.

## 2. Low Weather map

### Definition

| Property                | Value                             |
| ----------------------- | --------------------------------- |
| Binding                 | `t7`                              |
| Resource type           | 2D texture                        |
| Generated dimensions    | `weatherDim x weatherDim`         |
| Default dimension       | `512 x 512`                       |
| Allowed generated range | `128` to `1024` per axis          |
| Generated format        | `R8G8B8A8_UNORM`                  |
| Addressing              | Clamp; outside-map UV is rejected |
| Current sampled mip     | 0                                 |

### Channel contract

| Channel | Meaning                   | Required range                          |
| ------- | ------------------------- | --------------------------------------- |
| R       | Low-cloud coverage        | `0` = clear, `1` = maximum coverage     |
| G       | Cu/TCu/Cb type coordinate | `0` = Cu, `0.5` = TCu, `1` = Cb         |
| B       | Stratocumulus region mask | `0` = normal low cloud, `1` = Sc region |
| A       | Reserved                  | Write `0`                               |

G is a continuous selector. Values between the three anchors interpolate profile, detail strength, density multiplier, and cloud-top development. Authored maps may use continuous transition bands, but broad regions should remain near the anchor values so distinct species do not collapse into one generic blend.

B selects the separate stratocumulus path. It is not a fourth interval in G. The generated map writes a binary B mask; linear filtering supplies soft boundaries.

### Generated behavior

The current generator builds coherent moisture systems, frontal bands, dry intrusions, convection, and stable regions. It then:

1. determines the actually covered low-cloud pixels;
2. allocates the requested stratocumulus share to stable regions;
3. normalizes Cu/TCu/Cb weights over the remaining covered area;
4. ranks those pixels by convective suitability;
5. assigns Cu to lower suitability, TCu to intermediate suitability, and Cb to the strongest convection.

The generated map fades R to zero near its finite boundary.

### Override authoring requirements

-   R must be zero at the outer border unless a deliberate hard map boundary is wanted.
-   G should describe coherent cloud systems, not high-frequency noise.
-   Cb regions should be sparse and associated with high coverage or convective structures.
-   B should favor broad, stable regions and should not overlap every high-convection region.
-   Set A to zero for forward compatibility.
-   Do not use the channel layout of an unrelated weather-map format without explicitly repacking it.

## 3. High Weather map

### Definition

| Property                | Value                             |
| ----------------------- | --------------------------------- |
| Binding                 | `t8`                              |
| Resource type           | 2D texture                        |
| Generated dimensions    | `weatherDim x weatherDim`         |
| Default dimension       | `512 x 512`                       |
| Allowed generated range | `128` to `1024` per axis          |
| Generated format        | `R8G8B8A8_UNORM`                  |
| Addressing              | Clamp; outside-map UV is rejected |
| Sampled mips            | 0 and 2                           |

### Channel contract

| Channel | Meaning                                | Required range                              |
| ------- | -------------------------------------- | ------------------------------------------- |
| R       | High-cloud coverage                    | `0` = clear, `1` = maximum coverage         |
| G       | High-cloud type                        | `0` = As, `1` = Ac                          |
| B       | Reserved                               | Write `0`                                   |
| A       | Thickness / multiple-scattering weight | `0` outside coverage; positive inside cloud |

G controls the strength of cellular thickness and wisp erosion. Values between 0 and 1 blend the As and Ac responses.

A is not optional in practice. The current shader uses it for:

-   low-cloud density-edge softness at the same weather coordinate;
-   high-cloud density softness;
-   high-cloud density scaling;
-   high-cloud view extinction and multiple-scattering weighting.

The generated value is:

```text
A = R * (0.35 + 0.65 * coherentThicknessField)
```

Consequently A is always zero where R is zero and normally remains positive where high cloud exists.

### Override authoring requirements

-   Include at least mip 2. Mip 2 R is used to skip the complete high-cloud ray loop over clear regions.
-   Generate mips using area averaging so small high-cloud features contribute correctly to the gate.
-   Keep A zero wherever R is zero.
-   Do not fill A with an unrelated noise floor; it would alter low-cloud softness even in nominally clear high-cloud regions.
-   G should form coherent As/Ac regions. The shader samples G at mip 0.
-   Fade R and A to zero near the finite weather-map boundary.

## 4. Low-cloud Profile LUT

### Definition

| Property                 | Value                          |
| ------------------------ | ------------------------------ |
| Binding                  | `t11`                          |
| Resource type            | 2D texture                     |
| Generated dimensions     | `profileWidth x profileHeight` |
| Default dimensions       | `256 x 64`                     |
| Allowed generated width  | `64` to `512`                  |
| Allowed generated height | `16` to `256`                  |
| Generated format         | `R8G8B8A8_UNORM`               |
| Addressing               | Clamp                          |
| Current sampled mip      | 0                              |

### Coordinates

```text
U = local normalized cloud height
V = radial distance from the weather-map center
```

U is zero at the shared cloud-layer bottom and increases upward after coverage-driven cloud-top development and stratocumulus height compression. V is:

```text
saturate(length(weatherUV - 0.5) * 2)
```

### Channel contract

| Channel | Meaning                      |
| ------- | ---------------------------- |
| R       | Cu vertical density profile  |
| G       | TCu vertical density profile |
| B       | Cb vertical density profile  |
| A       | Unused; write `1`            |

The Low Weather G channel selects or interpolates RGB. Stratocumulus uses the Cu profile after compressing its local height into the configured physical Sc depth.

### Generated behavior

The generated profiles use physical vertical development depths in kilometres. The shared lowest/highest cloud altitudes are only bounds. Increasing the highest altitude therefore does not proportionally stretch Cu and TCu through the entire shell. Each generated channel contains a smooth cloud-bottom ramp and a tapered cloud top.

### Override authoring requirements

-   The bottom edge should rise smoothly from zero to prevent a hard planar cloud base.
-   Every channel must return to zero before or at its intended cloud top.
-   R should be shallow, G should support deeper vertical development, and B may reach the upper shell for Cb.
-   Avoid a profile that remains near one over its full U range; it produces solid vertical columns.
-   Keep profiles smooth enough for linear sampling. Sharp one-texel height transitions produce horizontal density bands.
-   V variation should be gradual. It represents large radial variation, not per-cloud high-frequency noise.

## 5. Stratocumulus Cell map

### Definition and channels

| Property             | Value                |
| -------------------- | -------------------- |
| Binding              | `t12`                |
| Resource type        | Tileable 2D texture  |
| Generated dimensions | Same as `weatherDim` |
| Generated format     | `R8G8B8A8_UNORM`     |
| Used channel         | R                    |
| Addressing           | Wrap U/V             |
| Current sampled mip  | 0                    |

R is a cellular thickness/coverage field:

-   `1` represents a dense cell center;
-   `0` represents a gap or cell boundary.

The generated texture combines warped periodic cellular fields at two scales. RGB are identical and A is one, but only R is consumed.

### Authoring requirements

-   The texture must tile seamlessly in U and V.
-   It should contain broad connected cell centers separated by visible gaps.
-   Avoid pure high-frequency white noise; it produces granular breakup rather than stratocumulus cells.
-   Preserve a useful value range near both zero and one so strength and contrast controls remain effective.

## 6. High-cloud Cell map

### Definition and channels

| Property             | Value                |
| -------------------- | -------------------- |
| Binding              | `t13`                |
| Resource type        | Tileable 2D texture  |
| Generated dimensions | Same as `weatherDim` |
| Generated format     | `R8G8B8A8_UNORM`     |
| Used channel         | R                    |
| Addressing           | Wrap U/V             |
| Current sampled mip  | 0                    |

R controls high-cloud cell thickness:

-   the sampled value is shaped by `highCellThickPow`;
-   High Weather G blends between the weaker As response and stronger Ac response;
-   the result changes both the local vertical band and final density.

The generated texture combines two warped periodic cellular scales. RGB are identical and A is one, but only R is consumed.

### Authoring requirements

-   Must tile seamlessly.
-   Prefer larger, softer cells than the low-cloud 3D detail noise.
-   Retain clear spaces between cells if Ac breakup is desired.
-   Do not bake wind displacement into the texture; the shader animates its UV.

## 7. High-cloud Warp map

### Definition and channels

| Property             | Value                |
| -------------------- | -------------------- |
| Binding              | `t14`                |
| Resource type        | Tileable 2D texture  |
| Generated dimensions | Same as `weatherDim` |
| Generated format     | `R8G8B8A8_UNORM`     |
| Used channels        | R, G                 |
| Addressing           | Wrap U/V             |
| Current sampled mip  | 0                    |

RG stores a signed 2D displacement encoded in UNORM:

```text
signedWarp = RG * 2 - 1
```

Therefore:

-   `0.5, 0.5` is the neutral vector;
-   `0, 0` is maximum negative displacement;
-   `1, 1` is maximum positive displacement.

Generated B is `0.5` and A is `1`; neither is currently used.

### Authoring requirements

-   Must tile seamlessly.
-   The average RG value should be approximately `0.5, 0.5` to avoid net directional drift.
-   Use smooth, low-frequency vector variation. High-frequency warp creates folded or noisy cells.
-   Avoid lossy compression that biases the neutral value or introduces block boundaries.

## 8. High-cloud Wisp map

### Definition and channels

| Property             | Value                |
| -------------------- | -------------------- |
| Binding              | `t15`                |
| Resource type        | Tileable 2D texture  |
| Generated dimensions | Same as `weatherDim` |
| Generated format     | `R8G8B8A8_UNORM`     |
| Used channel         | R                    |
| Addressing           | Wrap U/V             |
| Current sampled mip  | 0                    |

R is squared in the shader and then subtracts density according to wisp strength and High Weather G. It therefore behaves as an erosion mask:

-   `0` preserves cloud density;
-   `1` applies maximum wisp erosion.

The generated texture uses thin periodic ridges modulated by a secondary cross-field. RGB are identical and A is one, but only R is consumed.

### Authoring requirements

-   Must tile seamlessly.
-   Prefer elongated, coherent streaks rather than isotropic white noise.
-   Because R is squared, preserve mid-to-high values where erosion should remain visible.
-   Avoid excessive uniform brightness; it removes most Ac density instead of creating localized wisps.

## Generated-resource lifecycle

Generated 2D textures are GPU `R8G8B8A8_UNORM` resources with render-target and shader-resource bindings. The CPU uploads mip 0 and the D3D11 runtime generates the complete mip chain.

Generation is divided so unrelated changes do not rebuild everything:

-   changing `weatherDim` rebuilds all weather and auxiliary maps;
-   changing low coverage, low contrast, Sc share, or Cu/TCu/Cb weights rebuilds Low Weather;
-   changing high coverage, high contrast, or As/Ac weights rebuilds High Weather;
-   changing profile dimensions, shared cloud-layer depth, or Cu/TCu/Cb physical depth rebuilds the Profile LUT;
-   while a relevant UI slider is actively dragged, the previous generated texture remains active and one rebuild occurs after release.

Generated texture names visible in graphics debuggers are GPU labels, not disk filenames.

## Override loading and fallback behavior

The Cloud Map UI contains a shared DDS path loader followed by one selector for each overridable resource.

Recommended workflow:

1. Enter the DDS path in the Cloud Map texture loader.
2. Press **Load** to create or refresh its SRV.
3. Select that loaded path in the appropriate override combo.
4. Leave a combo on **Generated** to use the procedural fallback.

If an override path is empty, the generated texture is used. If a configured override fails to load or resolves to a null SRV, the generated texture is also used.

The **Reload Cloud Textures** button reloads the fixed `nubis.dds` volume. To refresh an override after replacing its file, enter/select its path and use the Cloud Map loader's **Load** button again.

The override loader currently performs no semantic validation. It does not verify resource type, dimensions, format, channel count, mip count, color space, or tileability. An invalid override can bind incorrectly or produce undefined-looking clouds without a clear load error. Validate assets externally before distribution.

## Bundled but currently unused resources

The following DDS files are present in the Physical Sky texture directory but have no loader, SRV binding, or shader reference in the current volumetric-cloud path:

| File             | Current DDS metadata                                      | Current status  |
| ---------------- | --------------------------------------------------------- | --------------- |
| `bottom_lut.dds` | `128 x 128`, R8 UNORM, 8 mips                             | Bundled, unused |
| `top_lut.dds`    | `128 x 128`, R8 UNORM, 8 mips                             | Bundled, unused |
| `ndf.dds`        | `512 x 512`, five-slice `Texture2DArray`, R8 UNORM, 1 mip | Bundled, unused |

Their names do not establish a current channel contract. Do not author replacements or rely on them until code explicitly loads and defines their semantics.

## Runtime-only textures that are not asset inputs

The volumetric-cloud renderer also consumes or produces several textures that must not be supplied as cloud DDS assets:

-   atmospheric transmittance, multiple-scattering, aerial-perspective, and sky-view LUTs;
-   scene depth and accumulated aerial-perspective shadow;
-   direct shadow maps and terrain shadow data;
-   quarter-resolution cloud trace transmittance, luminance, and auxiliary data;
-   upscaled and temporal-history cloud buffers;
-   cloud cubemap transmittance and luminance;
-   cloud ambient spherical-harmonic data;
-   cloud shadow cookie and filter target.

These are created or provided elsewhere in the render pipeline.

## Authoring and validation checklist

Before shipping a texture set, verify all of the following:

-   [ ] `nubis.dds` loads as a true 3D texture.
-   [ ] Nubis dimensions and format are appropriate for the memory budget; the current reference is `128^3 RGBA8`.
-   [ ] Nubis tiles across X, Y, and Z and contains continuous adjacent slices.
-   [ ] Nubis includes at least mips 0 through 4; a full chain is preferred.
-   [ ] Every 2D override is a non-sRGB `Texture2D`, not an array, cubemap, or volume.
-   [ ] Low Weather uses `R=coverage, G=Cu/TCu/Cb, B=Sc, A=0`.
-   [ ] High Weather uses `R=coverage, G=As/Ac, B=0, A=thickness/MS weight`.
-   [ ] High Weather contains a valid mip 2 with area-averaged R coverage.
-   [ ] Weather coverage and High Weather A fade to zero at the finite map boundary.
-   [ ] Profile uses `U=height, V=radial distance, RGB=Cu/TCu/Cb` and returns to zero at each cloud top.
-   [ ] Sc Cell, High Cell, High Warp, and High Wisp tile seamlessly in U/V.
-   [ ] High Warp is centered around neutral RG `0.5, 0.5`.
-   [ ] High Wisp uses bright values for erosion, not preservation.
-   [ ] No data texture is tagged or viewed as sRGB.
-   [ ] Override paths are distributable relative paths rather than developer-machine absolute paths.
-   [ ] The result is checked at multiple cloud-layer heights, weather-map sizes, camera distances, and mip-dependent high-cloud gating conditions.

## Implementation references

-   Fixed DDS loading and sampler creation: `src/Features/PhysicalSky/VolumetricClouds.cpp`
-   Generated textures and override resolution: `src/Features/PhysicalSky/Ndf.cpp`
-   Settings and override fields: `src/Features/PhysicalSky/Ndf.h`
-   Shader bindings and sampling semantics: `features/Physical Sky/Shaders/PhysicalSky/Volumetrics.cs.hlsl`
-   Feature asset packaging: `CMakeLists.txt`
