# Volumetric Cloud Texture Resources

See [cloud sampling and motion](cloud-sampling.md) for the view march,
empty-space acceleration, motion units and history metadata. See
[cloud lighting and reconstruction](cloud-lighting.md) for local solar occlusion, profile scattering
and render-target formats.

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
| Cirrus weather            |   `t11` | `Texture2D<float2>`       | generated / DDS  |
| Low-cloud distance        |   `t12` | `Texture2D<float>`        | GPU-generated    |
| Cirrus patterns           |   `t13` | `Texture2D<float3>`       | generated / DDS  |
| Cloud ambient SH          |   `t16` | `Texture2D<sh2>`          | renderer         |
| Nubis top profile         |   `t17` | `Texture2D<unorm float>`  | `top_lut.dds`    |
| Nubis bottom profile      |   `t18` | `Texture2D<unorm float>`  | `bottom_lut.dds` |

Temporal reconstruction reads screen history at t26–t28 and compact screen
traces at t29–t31. Cube history occupies t32–t34 and compact cube traces t35–t37.
Each group is transmittance, radiance, metadata, with R16_FLOAT, RGBA16_FLOAT,
RGBA16_FLOAT storage respectively. See [cloud sampling](cloud-sampling.md) and
[cloud lighting](cloud-lighting.md) for scheduling and history contracts.

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
length; it is independent of the 3D noise repeat length.

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

## Cirrus inputs

Cirrus is a two-dimensional spherical sheet. It shares the main NDF world UV
and wind displacement, with an independent pattern repeat length (default
`1 / 0.0002331` metres). It does not sample `nubis.dds`.

| Texture       | Channels                                                  |
| ------------- | --------------------------------------------------------- |
| Weather, t11  | R coverage, G type, both 0–1                              |
| Patterns, t13 | R wispy, G round, B streaky; squared by the density query |

Both default to local GPU generation. Weather uses two independently selectable
noise inputs (generated Alligator, Perlin or Perlin-Worley, or external scalar
DDS), signed remapping, frequency and offset. Generated inputs default to Perlin.
Output remap endpoints control coverage and type independently; they are project
starting values, not a universal weather preset. No storm or local-influence
pass participates.

The RGB pattern generator is a project-authored substitute: warped anisotropic
gradient noise forms wispy/streaky patterns, with cellular round patterns. Seed,
warp and detail are configurable. This generator is not an implementation of an
original pattern-authoring algorithm. It supplies the channel meanings required
by the Nubis cirrus profile; visual equivalence to authored patterns is not claimed.

An external linear 2D DDS can replace the weather map (at least RG) or patterns
(at least RGB), independently. Inputs are selected in Cirrus texture inputs;
incompatible or missing explicit inputs disable the sheet and show an input
error instead of reusing stale textures. Generated weather is RG16_FLOAT and
patterns are RGBA16_FLOAT, each 512 square with a mip chain.

Imported RGB patterns are sampled directly at mip 0 with linear wrapping:
there is no resizing, channel repacking, sRGB conversion or procedural warp.
Only the physical pattern repeat length and wind transform their UVs. Loading
patterns bypasses the pattern generator; loading both maps bypasses all cirrus
generation and does not require its compute programs. The density and lighting
path is shared with local inputs, including the fixed factor 2 in density.
A 1024-square BC7_UNORM pattern with one mip is accepted unchanged.

Matching pattern data alone does not determine a weather state: coverage/type
RG, world-to-field scale/offset, wind, density multiplier and lighting inputs
also affect the result. Defaults do not infer these settings from image content.

## Generator lifecycle

The procedural low NDF and acceleration rebuild on composition, noise or source
changes. Shader reload invalidates generation. Animated wind and world repeat
scale apply during sampling. Map replacement invalidates temporal history.
Imported pairs refresh acceleration before use. Cirrus input changes rebuild
its maps and invalidate cloud history. Disabled cirrus skips generation.

## Static validation checklist

-   `nubis.dds` loads as a tileable 3D RGBA texture and is bound at `t5`.
-   Low NDF uses linear height RG and modeling RGB textures at t7/t8.
-   NDF coverage is generated independently from `nubis.dds`.
-   Empty or reversed height intervals are rejected by the density query.
-   Noise is queried only inside positive NDF/profile support, with zero density outside it.
-   Cirrus weather/pattern inputs are linear 2D RG/RGB resources at t11/t13.

## Implementation references

-   low NDF generation and texture selection: `src/Features/PhysicalSky/Ndf.cpp`
-   low NDF settings: `src/Features/PhysicalSky/Ndf.h`
-   DDS loading and bindings: `src/Features/PhysicalSky/VolumetricClouds.cpp`
-   low NDF generator shader: `features/Physical Sky/Shaders/PhysicalSky/NdfGenerate.cs.hlsl`
-   cirrus weather/pattern generator: `features/Physical Sky/Shaders/PhysicalSky/CirrusGenerate.cs.hlsl`
-   density sampling: `features/Physical Sky/Shaders/PhysicalSky/Volumetrics.cs.hlsl`
-   noise reconstruction: `features/Physical Sky/Shaders/PhysicalSky/CloudNoise.hlsli`
