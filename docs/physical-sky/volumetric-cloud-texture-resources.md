# Volumetric Cloud Texture Resources

See [cloud sampling and motion](cloud-sampling.md) for the view march,
empty-space acceleration, motion units and history metadata. See
[cloud lighting and reconstruction](cloud-lighting.md) for light caches, distant
thin high clouds and render-target formats.

## Low-cloud model

Physical Sky uses a vertical-profile representation inspired by publicly
described Nubis techniques. Its threshold-noise reconstruction is documented
in [noise reconstruction](noise-contract.md). Low-cloud density
is the composition of two independent resources:

1. a five-layer NDF supplies the dimensional profile: minimum height, maximum
   height, coverage, top type, and bottom type;
2. `nubis.dds` supplies the tileable three-dimensional density-noise composite.

The noise volume does not generate the NDF, and the NDF is not a second erosion
pass. The profile determines where cloud mass may exist; the composite describes
the internal and boundary variation of that mass.

## Runtime bindings

| Resource                  | Binding | Type                      | Default source   |
| ------------------------- | ------: | ------------------------- | ---------------- |
| Nubis noise composite     |    `t5` | `Texture3D<unorm float4>` | `nubis.dds`      |
| Aerial-perspective sun    |    `t6` | `Texture3D<float4>`       | GPU-generated    |
| Low-cloud NDF             |    `t7` | `Texture2DArray<float>`   | GPU-generated    |
| Aerial-perspective shadow |    `t9` | `Texture2D<unorm float>`  | renderer         |
| Sky view                  |   `t10` | `Texture2D<float4>`       | renderer         |
| High weather              |   `t11` | `Texture2D<float4>`       | GPU-generated    |
| Low-cloud distance        |   `t12` | `Texture2D<float>`        | GPU-generated    |
| High cell                 |   `t13` | `Texture2D<float4>`       | GPU-generated    |
| High warp                 |   `t14` | `Texture2D<float4>`       | GPU-generated    |
| High wisp                 |   `t15` | `Texture2D<float4>`       | GPU-generated    |
| Cloud ambient SH          |   `t16` | `Texture2D<sh2>`          | renderer         |
| Nubis top profile         |   `t17` | `Texture2D<unorm float>`  | `top_lut.dds`    |
| Nubis bottom profile      |   `t18` | `Texture2D<unorm float>`  | `bottom_lut.dds` |
| Low-cloud light cache     |   `t24` | `Texture3D<float4>`       | GPU-generated    |
| High-cloud light cache    |   `t25` | `Texture3D<float4>`       | GPU-generated    |

The three fixed assets are loaded from `Data/Textures/PhysicalSky/` and live in
`features/Physical Sky/Textures/PhysicalSky/` in the source tree.

## Five-layer NDF

The NDF is sampled with tileable linear filtering at mip 0. Texture mode
accepts a `256 x 256 x 5`, `R8_UNORM` array. The Cumuliform generator packs the
same five attributes into two RGBA8 array slices: minimum/maximum height, coverage
and top type in slice 0, and bottom type in slice 1 R.

| Slice | Meaning                                               |
| ----: | ----------------------------------------------------- |
|     0 | minimum normalized height in the NDF coordinate frame |
|     1 | maximum normalized height in the NDF coordinate frame |
|     2 | coverage                                              |
|     3 | top type used to sample `top_lut.dds`                 |
|     4 | bottom type used to sample `bottom_lut.dds`           |

At a ray sample the shader computes:

```text
local_height = (altitude - minimum_height) / (maximum_height - minimum_height)
vertical_profile = top_lut(top_type, local_height)
                 * bottom_lut(bottom_type, local_height)
dimensional_profile = coverage * vertical_profile
```

`Low Cloud Base Altitude` and `Layer Thickness` map normalized NDF height 0-1
into physical altitude. `NDF Scale` independently controls the X/Y repeat
length; it is not inherited from the high-cloud weather map or the 3D noise.

The original Cumuliform generator multiplies three independently scaled,
rotated, and animated three-octave Worley fields. Coverage is the clipped and
powered product. The same fields derive per-column minimum and maximum height;
the top-type slice receives the shaped noise and the bottom-type slice receives
the configured wispiness.

Texture mode accepts a linear, non-sRGB 256 x 256 DDS `Texture2DArray` with five
slices in the order above. This is the route for arbitrary authored cloud
distributions.

## `nubis.dds`

The bundled asset is a 128 x 128 x 128, linear RGBA8 volume with eight mip
levels. Its legacy DDS channel masks map bytes to R/G/B/A. It is sampled once
at t5 with wrapping and the caller's explicit mip level.

Coverage mixes G towards R. A and B produce another threshold using the
coverage-dependent exponent 1/16. `Noise Roundness` selects between these
thresholds. Coverage is then eroded and normalized by the remaining threshold
range, before applying the independent vertical-profile response.

This contract and its near-camera folded detail are specified in
[noise reconstruction](noise-contract.md). It uses no auxiliary warp texture
or rotated second sample. `Noise Composite Scale` controls the physical repeat
length of the volume; it does not change NDF coverage, height or cloud species.

## Profile LUTs

`top_lut.dds` and `bottom_lut.dds` are required `128 x 128` R8 UNORM assets.
U is profile type and V is `1 - localHeight`, matching their stored orientation.
Top and bottom type are independent NDF layers.

## Independent high clouds

High clouds retain their separate implementation and absolute altitude band.
They do not sample the low-cloud NDF or `nubis.dds`. High Weather at `t11` uses:

| Channel | Meaning                                  |
| ------- | ---------------------------------------- |
| R       | high-cloud coverage                      |
| G       | Altostratus/Altocumulus type             |
| B       | reserved                                 |
| A       | thickness and multiple-scattering weight |

## Generator lifecycle

The procedural low NDF is regenerated each frame because its three Worley
layers have independent velocities. There is no generator-version field,
histogram pass, quantile solver, species preset, or settings migration in this
pre-HP path. High-cloud map generation remains an independent implementation.

## Static validation checklist

-   `nubis.dds` loads as a tileable 3D RGBA texture and is bound at `t5`.
-   Low NDF is a five-slice, linear `Texture2DArray` in the documented order.
-   NDF coverage is generated independently from `nubis.dds`.
-   Procedural minimum and maximum height form a valid interval.
-   Noise is queried only inside positive NDF/profile support, with zero density outside it.
-   High Weather contains a valid mip 2 and keeps A zero outside coverage.

## Implementation references

-   low NDF generation and texture selection: `src/Features/PhysicalSky/Ndf.cpp`
-   low NDF settings: `src/Features/PhysicalSky/Ndf.h`
-   DDS loading and bindings: `src/Features/PhysicalSky/VolumetricClouds.cpp`
-   low NDF generator shader: `features/Physical Sky/Shaders/PhysicalSky/NdfCumuliform.cs.hlsl`
-   independent high-cloud generator shader: `features/Physical Sky/Shaders/PhysicalSky/HighCloudMapGen.cs.hlsl`
-   density sampling: `features/Physical Sky/Shaders/PhysicalSky/Volumetrics.cs.hlsl`
-   noise reconstruction: `features/Physical Sky/Shaders/PhysicalSky/CloudNoise.hlsli`
