# Procedural NDF generation

Four locally generated scalar noise inputs drive the low-cloud NDF. Every input
can instead use DDS. Optional local NDF maps blend before bottom height variation.
No offline preparation or world-space voxel volume is required.

## Two-texture contract

| Texture  | Channels                                 | Generated storage             | Density binding |
| -------- | ---------------------------------------- | ----------------------------- | --------------- |
| Height   | R: bottom height, G: top height          | 512 x 512 RG16_FLOAT          | t7              |
| Modeling | R: coverage, G: top type, B: bottom type | 512 x 512 RGBA16_FLOAT, A = 0 | t8              |

Values are linear and normalized. Altitude is `ndfAltitudeOffset +
ndfAltitudeScale * height` metres above the worldspace reference altitude.
The Low Clouds controls default to a 256 m base and a 1792 m span. The resulting
shell bounds traversal and storage; each NDF column supplies its own bottom/top. Types select the existing Nubis profile LUTs. Both generated and
imported maps use wrap filtering at mip 0. Empty/reversed height intervals produce
no density. Height bounds the profile; continuous coverage and Nubis noise then
reconstruct density inside it. No dome caps or height reordering are generated.

Texture mode selects `cloudMap.texture.heightPath` and `modelingPath`.
Supported linear 2D DDS formats, with unused channels ignored:

-   Noise/masks: R8/R16 UNORM, R16/R32 float, BC4 UNORM, or any format below.
-   Height: RG8/RG16 UNORM, RG16/RG32 float, BC5 UNORM, or any format below.
-   Modeling: RGB32 float, RGBA8/RGBA16 UNORM, RGBA16/RGBA32 float, BC1/2/3/7 UNORM.

Arrays, volumes, integer and sRGB views are rejected. Inputs should contain
normalized finite values and share spatial extent; resolution may differ.
The old five-slice array is no longer accepted.

## Composition equations

`cloudMap.procedural.parameters` contains six noise layers, each with a slot,
frequency, offset, exponent and range (input min/max, output min/max).

```text
R(x, r) = saturate((x - r.x) / (r.y - r.x)) * (r.w - r.z) + r.z
S(layer) = 2 * noise[layer.slot](((layer.offset - wind) + uv) * layer.frequency) - 1
P(x, e) = exp2(log2(x) * e)
G(layer) = layer.range.w if range.z == range.w, otherwise R(S(layer), layer.range)

first  = P(R(S(primary), primary.range), primary.exponent)
second = P(R(S(secondary), secondary.range), secondary.exponent)
coverage = G(coverageGain) * max(first, second)
topType = G(modelingGain) * P(R(S(modeling), modeling.range), modeling.exponent)
bottomType = G(modelingGain) * P(R(S(modeling), bottomTypeRange), bottomTypeExponent)
```

When the primary output range is constant, **both** coverage inputs become that
constant and bypass power. Secondary settings then have no effect. Types share
one sampled noise but use independent remaps/exponents. Slot 4 returns zero
before signed conversion, hence S = -1.

Local maps are sampled at `uv - wind * localWindScale`:

```text
mode 0: model = (mask * localModelingWeight) * (localRGB - model) + model
mode 1: model = max(model, localRGB * localModelingWeight)
height = baseHeight + saturate(mask) * localHeightWeight * (localRG - baseHeight)
driver = heightFromCoverage ? model.coverage : S(heightVariation)
variation = R(P(driver, heightVariation.exponent), heightVariation.range)
height.r += variation
```

Maximum blending deliberately ignores the mask. Optional `local.heightPath` and
`local.modelingPath` independently enable their weights. `localMaskPath` supplies
R influence for both blends; absent means one. Keeping influence separate avoids
giving modeling A a second contract. Zero weights preserve procedural values.

Height power occurs **before** remapping. Its coverage driver is post-blend,
pre-saturation. A constant variation output range bypasses driver and power.
Only height R changes; G stays at its base/local value. Final height RG and
modeling RGB are saturated. Height auxiliary data and storms are omitted.

The CPU sanitizes finite parameters and zero-width remaps. Power returns zero
for nonpositive inputs; exponent is limited to 0.01–8. This defines otherwise
invalid logarithmic inputs while retaining positive-input operation order.

## Noise synthesis

`cloudMap.procedural.noise` has four entries, each with `parameters` and optional
`texturePath`. DDS R replaces the generated input. Slot choices are editable;
defaults are project tuning, not recovered synthesis settings or descriptor order.

| Slot | Algorithm                          | Frequency | Octaves | Persistence | Repetitions | Contrast | Response exponent |  Bias |
| ---- | ---------------------------------- | --------: | ------: | ----------: | ----------: | -------: | ----------------: | ----: |
| 0    | Alligator                          |        12 |       4 |         0.5 |           1 |        1 |                 1 |     0 |
| 1    | Perlin fBm, coarse response preset |         4 |       6 |         0.6 |           2 |      1.6 |               1.8 | -0.02 |
| 2    | Perlin fBm                         |        32 |       4 |         0.5 |           1 |        1 |                 1 |     0 |
| 3    | Perlin-Worley                      |        12 |       4 |         0.5 |           1 |      1.1 |                 2 | -0.05 |

Seed defaults to 1337 and integer lacunarity to 2. Coarse and ordinary Perlin
use the same gradient family with different spectral and response settings.
Repetitions tile the underlying field within the input texture. Noise presets
are calibrated independently from any particular weather composition. Matching
the family and distribution does not establish identical authored texels,
spectra or original synthesis recipes.

Perlin uses unit gradients and quintic interpolation in 2D. Alligator samples a
3D random cell field: the two largest random-amplitude smooth radial contributions
are subtracted. This follows the public
[SideFX definition](https://www.sidefx.com/docs/hdk/alligator_2alligator_8_c-example.html),
using project PCG hashes, a fixed Z slice and factor-two range adjustment. It is
not a nearest-distance difference. Perlin-Worley uses `lerp(W, 1, P)` per octave,
where W is inverted nearest distance and P is normalized Perlin. Octaves are
normalized by persistence weights, followed by:

```text
value = (normalizedNoise - 0.5) * contrast + 0.5
response = responseExponent == 1 ? value : pow(saturate(value), responseExponent)
output = saturate(response + bias)
```

Exponent one bypasses the power input clamp to preserve the original linear
contrast/bias operation for existing settings.

Inputs use R16_FLOAT with full mips. Integer periods wrap XY. The base frequency
is bounded and octaves are omitted above half the resolution, accounting for
repetitions. Compute sampling derives mip from source dimensions,
output dimensions and layer frequency, replacing implicit pixel derivatives.
Imported inputs use available mips. Noninteger layer frequencies can introduce
a seam across the NDF tile; integer frequencies are preferable for repeating skies.

Default primary uses slot 3 with output [0, 0.65], exponent 1.4; secondary uses
slot 1 with output [0, 0.4], exponent 1; types use slot 2. Default coverage stays
below one, preserving Nubis threshold sensitivity. There is no hidden final cap;
local maps or edited ranges can intentionally produce full coverage.

Composition controls remain independent. A weather may deliberately use the same
noise, frequency and offset for coverage and types, but that is not a universal
rule. A single weather sample does not define default height, coverage, types or
which layers must be disabled. Comparing two weather outputs without matching
their inputs cannot establish an error in the composition equations.

## Coordinates and lifecycle

Generation evaluates one canonical UV tile, corresponding to a centered
16384-unit weather domain before normalization. Static `windOffset` is multiplied
by 0.00005 before each layer's frequency. Physical repeat length is `low.ndfScale`.
Animated common wind and height shear remain in density queries/reprojection;
time does not enter generation.

UI exposes all composition parameters, noise inputs, DDS loading and local maps.
Selected paths persist with settings and load on first use. Missing paths are
not retried every frame; Load retries/reloads explicitly. Failed reloads preserve
valid resources. Noise edits rebuild the affected input. Composition/source
changes or shader/resource reload regenerate the NDF, invalidating main history
and occupancy/distance maps. Imported acceleration uses actual bilinear coverage
support. World scale changes history and sampling, not the normalized map.

CPU/HLSL constant-buffer payloads are 352 bytes for composition and 48 for noise.
Compute restores its sampler and clears SRV/UAV bindings. All view, light,
shadow and cubemap consumers use the pair. Cirrus uses a separate coverage/type map; ordinary stratus remains a main-NDF type.

`cloudMap.version` is 2. Type 0 imports a pair; type 1 generates locally.
Older settings and five-slice selections reset to procedural defaults, requiring
visual retuning rather than silently reinterpreting incompatible parameters.
Existing version 2 settings retain their saved noise tuning. Omitted repetitions
and response exponent default to one, preserving the previous noise operation.

## Validation limits

Static review and external CPU evaluation cover expression order, branches,
local blends, buffer layout, density reconstruction and resource lifecycle.
The 100,000-case algebra comparison differs by at most 8.9e-16 in double
precision. Density slices use the bundled Nubis volume and both profile LUTs.
A 2,048-point periodicity check and an expanded cellular-neighbor search agree.
An independent weather sample also exercises shared coverage/type input, zero
secondary coverage and zero bottom type; these are sample-specific controls.
Its local-noise reconstruction has 43.90% zero coverage versus 44.25% using the
comparison input with identical composition settings. This checks one input
case, not general cloud quality or the identity of an external noise enum.
No C++ build, shader compilation or GPU acceptance is included. Runtime review
must inspect silhouettes, details, lighting, motion and imported replacement.
CPU density slices are diagnostic, not a rendered-cloud quality claim.
