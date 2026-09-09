# Procedural NDF generation

`NdfGenerate.cs.hlsl` creates the low-cloud control field locally, without
offline content generation or a world-space density volume. The existing Nubis
threshold reconstruction, view/light density consumers and profile LUTs remain
the material contract. This generator constructs the larger cloud shapes that
those resources refine.

## Shape construction

A periodic, two-octave value field organizes cloud occurrence into weather
regions. Wrapped integer cell IDs and a seed determine cloud centers, size,
orientation and vertical rise. Distances are evaluated in kilometres before
rotation and anisotropic scaling, so rectangular maps do not stretch circular
domes. Rotating an ellipse changes its outline without rotating the tile domain.

Each mass contains a main elliptical dome and three smaller shoulder domes.
Coverage follows their softened horizontal envelopes; height follows their
merged cap functions. Vertical development scales rise independently of the
coverage signal. A slower field supplies a shared, gently varying condensation
base. Another independent field selects top-profile type; bottom type has its
own control. No three-dimensional density-noise lookup is used in generation.

| Form             | Construction                                                        |
| ---------------- | ------------------------------------------------------------------- |
| 0: Cumulus       | Separated or overlapping masses with pronounced domes and shoulders |
| 1: Stratocumulus | Wider masses, lower rise, and weather-controlled sheet connections  |
| 2: Stratus       | Continuous weather regions with slow thickness variation            |

Cloud amount changes occurrence and horizontal expansion. It is not a solved
area fraction or an opacity guarantee after noise erosion. Zero produces zero
coverage in every form. Vertical development and height variation leave coverage
unchanged; cloud height remains bounded by the low layer's configured thickness.
Changing cloud amount can change the height envelope where masses appear or
expand, but does not substitute for the independent development control.

## Settings

Settings are saved under `cloudMap.procedural`.

| Field                                       | Meaning / units                                                   |
| ------------------------------------------- | ----------------------------------------------------------------- |
| `seed`, `form`                              | Stable layout seed and form selection                             |
| `coverage`                                  | Cloud amount, 0–1                                                 |
| `weatherStrength`, `weatherScale`           | Weather modulation, 0–1; weather scale in km                      |
| `cloudSize`                                 | Requested spacing between cloud centers, in km                    |
| `sizeVariation`, `clustering`               | Size diversity and weather-grouped occurrence/alignment, 0–1      |
| `elongation`, `bearing`                     | Major/minor axis ratio, 1–3; preferred direction in radians       |
| `shoulders`, `edgeSoftness`                 | Secondary dome strength, 0–1; normalized outline transition width |
| `development`, `heightVariation`            | Vertical development and variation between masses, 0–1            |
| `baseVariation`                             | Slow variation above the common condensation altitude, 0–1        |
| `topType`, `topTypeVariation`, `bottomType` | Existing normalized profile selectors                             |

The 256 x 256 map uses integer cell counts for seamless wrapping. Cloud spacing
is rounded to fit each world repeat length, with at least eight texels per cell
and at most 32 cells per axis. Weather periods are also rounded and bounded.
Scales larger than a tile or smaller than its supported resolution cannot be
represented exactly. The cloud radius bound includes every rotated shoulder,
so a fixed 5 x 5 cell search includes all contributors.

## Output and lifetime

The generated `Texture2DArray` has two RGBA16_FLOAT slices:

| Slice | Channels                                                |
| ----- | ------------------------------------------------------- |
| 0     | R: base height, G: top height, B: coverage, A: top type |
| 1     | R: bottom type; GBA: reserved zero                      |

All attributes are linear and normalized. Physical altitude is
`low.baseAltitude + normalizedHeight * low.thickness`. Sampling uses wrapping
and mip 0. Optional DDS input retains five scalar slices in base, top, coverage,
top-type and bottom-type order; malformed array layouts are rejected before use.
Both representations feed the same NDF query and noise reducer.
Imported formats support scalar R8/R16 UNORM, R16/R32 float, BC4 UNORM, and
RGBA8/RGBA16 UNORM or RGBA16/RGBA32 float. Integer and sRGB views are rejected.

The CPU uploads a 96-byte constant buffer. Sanitized shape parameters and world
repeat size determine regeneration; time and wind do not enter the generator.
Wind offsets the map at density-query time, matching existing reprojection.
Resource creation, shader reload and parameter changes invalidate the generated
field. A successful generation dispatch invalidates temporal history and the
occupancy/distance map. Source changes also refresh acceleration, including
switches between imported and procedural maps. The occupancy pass reads actual
stored coverage and includes the bilinear footprint; shape shear retains its
existing conservative skip margin.

`cloudMap.type` keeps 0 for imported textures and 1 for procedural generation.
Old `cumuliform` configurations preserve their top/bottom profile selectors.
Their three noise frequencies, relative velocities, rotations, product clipping
and thickness-coupling controls are retired; the new shape settings take their
defaults. Existing presets therefore require visual retuning. Saving writes
only the new procedural settings and the texture source.

## Validation limits

Static review covers constant-buffer layout, settings/UI paths, generation and
acceleration invalidation, source switches, finite output bounds and periodic
cell support. CPU reference evaluation can inspect control maps and envelopes;
it does not compile or execute the shader and is not a rendered-cloud comparison.
The CPU reference checked 512 positions for each of 15 form/world-size
combinations, including 1 x 50 km and 50 x 1 km tiles. Periodic translations
differed by less than 3.7e-13 in double precision; extending the cell search to
7 x 7 did not change the result. Zero coverage, ordered heights, seed changes,
directional elongation and coverage-independent development also passed.

No C++ build, shader compilation or GPU acceptance is included. Runtime review
should first isolate macro outlines and vertical profiles, then inspect the
existing erosion and lighting. Single base/top intervals cannot represent
arbitrary overhangs, separated vertical layers or enclosed cavities. Custom
three-dimensional noise generation and additional import layouts remain
separate work.
