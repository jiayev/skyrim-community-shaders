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

1. a five-attribute NDF supplies the dimensional profile: minimum height, maximum
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
| Low-cloud height          |    `t7` | `Texture2D<float2>`       | GPU-generated    |
| Low-cloud modeling        |    `t8` | `Texture2D<float3>`       | GPU-generated    |
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

## Two-texture NDF

Both generated and imported NDFs use two linear 2D textures, sampled with wrap
filtering at mip 0. Generated maps are 512 x 512: height RG16_FLOAT and modeling
RGBA16_FLOAT with unused A zero.

| Texture  | R                         | G                         | B                   |
| -------- | ------------------------- | ------------------------- | ------------------- |
| Height   | minimum normalized height | maximum normalized height | unused              |
| Modeling | coverage                  | top profile type          | bottom profile type |

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

The generator remaps and powers two coverage signals, combines them with max
and gain, and separately remaps shared noise into profile types. Optional local
maps blend before bottom height variation. Four scalar noise inputs use local
Alligator, Perlin fBm or Perlin-Worley, individually replaceable by DDS.
See [procedural NDF generation](ndf-generator.md) for equations and noise presets.

Texture mode selects `heightPath` and `modelingPath` with the same channel
contract. Arrays, integer and sRGB views are rejected. Optional authored maps
enter the same density query as generated maps.

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
Top and bottom type occupy modeling G and B.

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

The procedural low NDF and acceleration rebuild on composition, noise or source
changes. Shader reload invalidates generation. Animated wind and world repeat
scale apply during sampling. Map replacement invalidates temporal history.
Imported pairs refresh acceleration before use. High-cloud generation remains
independent.

## Static validation checklist

-   `nubis.dds` loads as a tileable 3D RGBA texture and is bound at `t5`.
-   Low NDF uses linear height RG and modeling RGB textures at t7/t8.
-   NDF coverage is generated independently from `nubis.dds`.
-   Empty or reversed height intervals are rejected by the density query.
-   Noise is queried only inside positive NDF/profile support, with zero density outside it.
-   High Weather contains a valid mip 2 and keeps A zero outside coverage.

## Implementation references

-   low NDF generation and texture selection: `src/Features/PhysicalSky/Ndf.cpp`
-   low NDF settings: `src/Features/PhysicalSky/Ndf.h`
-   DDS loading and bindings: `src/Features/PhysicalSky/VolumetricClouds.cpp`
-   low NDF generator shader: `features/Physical Sky/Shaders/PhysicalSky/NdfGenerate.cs.hlsl`
-   independent high-cloud generator shader: `features/Physical Sky/Shaders/PhysicalSky/HighCloudMapGen.cs.hlsl`
-   density sampling: `features/Physical Sky/Shaders/PhysicalSky/Volumetrics.cs.hlsl`
-   noise reconstruction: `features/Physical Sky/Shaders/PhysicalSky/CloudNoise.hlsli`
