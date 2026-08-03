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
| Low weather map        |           `t7` | `Texture2D<float4>`       | GPU-generated           | No               |
| High weather map       |           `t8` | `Texture2D<float4>`       | GPU-generated           | No               |
| Low-cloud profile LUT  |          `t11` | `Texture2D<float4>`       | GPU-generated           | No               |
| Stratocumulus cell map |          `t12` | `Texture2D<float4>`       | GPU-generated           | No               |
| High-cloud cell map    |          `t13` | `Texture2D<float4>`       | GPU-generated           | No               |
| High-cloud warp map    |          `t14` | `Texture2D<float4>`       | GPU-generated           | No               |
| High-cloud wisp map    |          `t15` | `Texture2D<float4>`       | GPU-generated           | No               |

The renderer requires all nine SRVs to exist. Only the 3D Nubis volume must be supplied on disk; the seven 2D cloud-description resources are generated on the GPU and may be replaced individually with DDS files.

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

| Convention | Filtering          | Addressing  | Resources                                                                  |
| ---------- | ------------------ | ----------- | -------------------------------------------------------------------------- |
| Clamped    | Min/mag/mip linear | Clamp U/V/W | Profile LUT                                                                |
| Tileable   | Min/mag/mip linear | Wrap U/V/W  | Nubis, low weather, high weather, Sc cell, high cell, high warp, high wisp |

Tileable resources must be seamless across every wrapped axis. For the 3D volume this means seamless X, Y, and Z boundaries. For 2D maps it means seamless U and V boundaries.

The weather maps are tileable. They describe a repeating synoptic pattern rather than a finite rectangle of cloud, so an override that is not seamless will show a visible grid of discontinuities at every tile boundary.

The profile LUT is the only clamped resource. Both of its axes are bounded quantities — normalized height and normalized distance from the cloud centre — so wrapping them would be meaningless.

### Mips

-   Mips must be generated from linear data, not gamma-encoded data.
-   `nubis.dds` is sampled at explicit fractional LODs. At least mip levels 0 through 4 must exist; a complete chain is preferred.
-   High Weather R is sampled at mip 2 as a whole-ray high-cloud gate. An override should contain at least mip levels 0 through 2. A complete chain is preferred.
-   The other current 2D lookups use mip 0, although generated resources still receive complete mip chains.
-   Type and mask channels must remain semantically valid after filtering. Keep categorical regions spatially coherent and avoid isolated one-pixel type changes.

### Weather-map world coordinates

Weather UV is calculated as:

```text
uv = (worldXY - weatherCenter - windOffset) / weatherWorldSize + 0.5
```

Therefore:

-   U increases with world X;
-   V increases with world Y;
-   `(0.5, 0.5)` is `weatherCenter` at zero wind displacement;
-   one tile covers a square with side length `weatherWorldSize`, and the pattern repeats outside it;
-   UV is wrapped, not rejected, so the map has no boundary and needs no edge fade;
-   `windOffset` is the accumulated wind displacement shared with the 3D shape noise, so the whole cloud system advects downwind rather than staying pinned to world coordinates.

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

| Property                | Value                      |
| ----------------------- | -------------------------- |
| Binding                 | `t7`                       |
| Resource type           | 2D texture                 |
| Generated dimensions    | `weatherDim x weatherDim`  |
| Default dimension       | `512 x 512`                |
| Allowed generated range | `128` to `1024` per axis   |
| Generated format        | `R8G8B8A8_UNORM`           |
| Addressing              | Wrap; must tile seamlessly |
| Current sampled mip     | 0                          |

### Channel contract

| Channel | Meaning                    | Required range                          |
| ------- | -------------------------- | --------------------------------------- |
| R       | Low-cloud coverage         | `0` = clear, `1` = maximum coverage     |
| G       | Cu/TCu/Cb type coordinate  | `0` = Cu, `0.5` = TCu, `1` = Cb         |
| B       | Stratocumulus region mask  | `0` = normal low cloud, `1` = Sc region |
| A       | Cloud-body radial distance | `0` = cloud centre, `1` = cloud edge    |

G is a continuous selector. Values between the three anchors interpolate profile, detail strength, density multiplier, and cloud-top development. Authored maps may use continuous transition bands, but broad regions should remain near the anchor values so distinct species do not collapse into one generic blend.

B selects the separate stratocumulus path. It is not a fourth interval in G. The generated map writes a binary B mask; linear filtering supplies soft boundaries.

A is the second axis of the profile LUT lookup. It must be the distance from the centre of the cloud body that owns the pixel, normalized so that `0` is the core and `1` is the outer edge, and it must reset per cloud body rather than growing monotonically across the map. This is what gives a cumulus its dome: the profile collapses toward the cloud's own rim. A single map-wide gradient here flattens every cloud into a slab.

### Generated behavior

The generator builds three morphologies with distinct spatial signatures and blends them by the Character setting:

-   **convective cells** — jittered feature points gated on synoptic moisture, producing discrete cloud bodies with real gaps and a natural per-body radial coordinate;
-   **stratiform sheets** — a broad smooth field modulated by open or closed cells depending on instability;
-   **frontal bands** — narrow contours stretched along an integer lattice direction so they stay tileable.

It then solves the requested coverage and species shares as histogram quantiles:

1. the low-cloud potential threshold is solved so the covered area equals the requested sky coverage;
2. the stratocumulus share is solved over the covered area only;
3. Cu/TCu/Cb thresholds are solved over the covered area that stratocumulus did not claim, with the weights biased by instability;
4. the same procedure runs independently for the high-cloud layer.

Because coverage is fixed by a quantile, the shaping controls (edge width, break-up, cloud size) change how the cloud is distributed without changing how much of it there is.

### Override authoring requirements

-   R, G, B, and A must all tile seamlessly. There is no edge fade and no map boundary.
-   G should describe coherent cloud systems, not high-frequency noise.
-   Cb regions should be sparse and associated with high coverage or convective structures.
-   B should favor broad, stable regions and should not overlap every high-convection region.
-   A must be per cloud body. Writing `0` everywhere makes every cloud a full-depth slab; a map-wide radial gradient reintroduces the flattening this channel exists to fix.
-   Do not use the channel layout of an unrelated weather-map format without explicitly repacking it.

## 3. High Weather map

### Definition

| Property                | Value                      |
| ----------------------- | -------------------------- |
| Binding                 | `t8`                       |
| Resource type           | 2D texture                 |
| Generated dimensions    | `weatherDim x weatherDim`  |
| Default dimension       | `512 x 512`                |
| Allowed generated range | `128` to `1024` per axis   |
| Generated format        | `R8G8B8A8_UNORM`           |
| Addressing              | Wrap; must tile seamlessly |
| Sampled mips            | 0 and 2                    |

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
-   All channels must tile seamlessly. There is no finite map boundary to fade toward.

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
V = radial distance from the centre of the cloud body
```

U is zero at the shared cloud-layer bottom and increases upward after coverage-driven cloud-top development and stratocumulus height compression. V is read directly from the Low Weather alpha channel:

```text
saturate(lowWeather.a)
```

V is per cloud body, not per map. It is `0` at the core of the cloud the sample belongs to and `1` at that cloud's rim, so the profile can thin toward each cloud's own edge and produce a dome. Sourcing V from the distance to the weather-map centre instead would apply one gradient across the entire sky and flatten every cloud.

### Channel contract

| Channel | Meaning                      |
| ------- | ---------------------------- |
| R       | Cu vertical density profile  |
| G       | TCu vertical density profile |
| B       | Cb vertical density profile  |
| A       | Unused; write `1`            |

The Low Weather G channel selects or interpolates RGB. Stratocumulus uses the Cu profile after compressing its local height into the configured physical Sc depth.

### Generated behavior

The generated profiles use physical vertical development depths in kilometres, scaled by the Instability setting. The shared lowest/highest cloud altitudes are only bounds. Increasing the highest altitude therefore does not proportionally stretch Cu and TCu through the entire shell.

Along V, the usable depth falls off as `sqrt(1 - V^2)` scaled by Dome Strength, while the cloud base stays anchored at `U = 0` for every V. That matches a real cumulus, which has a flat base at the lifting condensation level and a domed top.

### Override authoring requirements

-   The bottom edge should rise smoothly from zero to prevent a hard planar cloud base.
-   Every channel must return to zero before or at its intended cloud top.
-   R should be shallow, G should support deeper vertical development, and B may reach the upper shell for Cb.
-   Avoid a profile that remains near one over its full U range; it produces solid vertical columns.
-   Keep profiles smooth enough for linear sampling. Sharp one-texel height transitions produce horizontal density bands.
-   Depth should decrease as V increases, and the base should stay anchored at `U = 0` across V. A profile that is constant in V renders flat-topped slabs.

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

Generated 2D textures are GPU `R8G8B8A8_UNORM` resources with unordered-access, render-target, and shader-resource bindings. They are written directly by compute shaders in `features/Physical Sky/Shaders/PhysicalSky/CloudMapGen.cs.hlsl`, after which the D3D11 runtime generates the complete mip chain.

Generation runs as five compute passes:

1. **generateFields** — evaluates the convective, stratiform, and frontal morphologies into intermediate field targets;
2. **buildHistogram** — accumulates 256-bin histograms of those fields with atomics;
3. **solveThresholds** — a single 256-thread group scans each histogram and solves the threshold that yields the requested area fraction;
4. **composeMaps** — writes Low Weather, High Weather, and the four auxiliary maps;
5. **composeProfile** — writes the Profile LUT.

Passes 2 and 3 run three times. Each round is gated on thresholds the previous round solved: coverage first, then the stratocumulus split measured over the covered area, then the Cu/TCu/Cb split measured over what stratocumulus did not claim. This is what makes the species shares shares of cloudy sky rather than of the whole map.

Because the whole rebuild is a handful of small dispatches, there is no deferral, throttling, or partial-invalidation scheme. Any changed generation input rebuilds every map in the same frame, so UI sliders and external controllers driving the same settings behave identically. A hash of the generation inputs skips redundant dispatches on unchanged frames; it is an optimization, not a correctness dependency.

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
-   [ ] Low Weather uses `R=coverage, G=Cu/TCu/Cb, B=Sc, A=cloud-body radial`.
-   [ ] High Weather uses `R=coverage, G=As/Ac, B=0, A=thickness/MS weight`.
-   [ ] High Weather contains a valid mip 2 with area-averaged R coverage.
-   [ ] Low Weather and High Weather tile seamlessly in U/V; neither relies on an edge fade.
-   [ ] Low Weather A resets per cloud body rather than forming one map-wide gradient.
-   [ ] Profile uses `U=height, V=cloud-body radial, RGB=Cu/TCu/Cb`, thins as V increases, and returns to zero at each cloud top.
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
