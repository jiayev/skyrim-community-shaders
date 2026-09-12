# Profile cloud noise reconstruction

## Resources

The regular NDF layer uses `NubisCloudShapeNoise.dds`, a linear 128³ RGBA8
volume with eight mip levels. The channels supply rounded shape (R), distant
wisps (G), nearby wisps (B), and erosion (A). `CloudNoise.hlsli` combines these
signals using dimensional profile, top type, local height and view distance.

`NubisOrographicDetailNoise.dds` is a linear 32³ RGBA8 volume with six mips,
reserved for a future orographic cloud path. That path uses its R channel for
nearby erosion. It is not sampled by the regular NDF layer.

`NubisVoxelNoise.dds` preserves the original 128³ RGBA8 voxel detail resource
without modifying its bytes. It is not loaded or bound by this renderer. Its
threshold/roundness reconstruction does not apply to regular profile clouds.

Two linear 64² lookup textures accompany the shape volume:

-   `NubisVerticalProfile.dds`: BC5, one mip; R is the bottom profile and G is
    the top profile. U is type and V is local height, increasing bottom to top.
-   `NubisVerticalAdjustment.dds`: BC7, seven mips; R expands the top profile,
    and GB supplies horizontal noise displacement near the base. Profile lookup
    clamps UVs; displacement lookup wraps and always uses mip 0.

## Dimensional profile

NDF inputs remain height RG and coverage/top-type/bottom-type RGB. The spherical
layer maps the height pair into physical altitude before noise evaluation.

Modeling UVs use height-dependent shear. An upstream modeling sample, displaced
by `60 * cloudShapeShear`, blends types and increases coverage with height.
The blend is `smoothstep(0, 1, saturate((height - 0.1) * 1.5384616))`.
Top type is multiplied by `saturate(coverage * 10)`.

```text
f = saturate(height / coverageHeightRange)
coverageExponent = lerp(lerp(coverageBottomPower, 1, f), 1, f)
top = lerp(topProfile, max(topProfile, expandedTop), topExpansion)
dimensionalProfile = max(top * bottomProfile, 0) * coverage^coverageExponent
```

Lighting receives the dimensional profile before noise erosion. Its remap
`saturate((profile - 0.05) / 0.95)` is applied once in the lighting response.
The coverage/type product remains `coverage * topType`.

## Noise coordinates and mip

Positions are converted to metres relative to the existing worldspace altitude
reference; XY follows the shared wind displacement.

```text
bottomFade = saturate((height - 0.02) * 33.333332)
xy = position.xy - windOffset
displacement = adjustmentGB(xy * 0.004) * 2 - 1
noiseXY = xy * 0.0043545123 + displacement * (0.125 - 0.125*bottomFade)
noiseZ = (position.z - 20 + 10*bottomFade) * 0.0034834063
mip = floor(dimensionalProfile * 3 + mipBias)
```

View rays use mip bias 0; sunlight and ground-shadow queries use 2. The spatial
frequency is fixed by the reconstruction, independently of NDF map repeat.
Noise scale, roundness and voxel folded-detail controls are absent.

## Density

The R/A billow and B/G/A wisp branches are combined using top type and height.
B transitions to G between view distances 1000 and 2000 metres. The combination
is multiplied by 0.975, then eroded by subtracting `1-min(0.7, profile)`.
The complete channel equations are in `ReconstructCloudNoiseDensity`.

Positive eroded density `e` receives the base response:

```text
boost = 8 * (1-e)^10
width = 10 - 9 * saturate((bottomDensityWidth-1)/9)
base = height^0.3 * saturate(height*width)^bottomDensityPower
density = (lerp(boost, 1, saturate(height*5)) * e * base)
          ^(0.35 + 0.3*saturate((height-0.25)*4))
viewExtinction = density * densityScale
lightDensity = viewExtinction * (1 + 3*height^4)
```

Empty eroded samples return exactly zero before the power response. The RG
height contract has no additional per-column base-raising channel; this path
uses zero base raising. Storm modifications are not implemented.

Each local sunlight probe retains the originating view sample's distance for
the B/G transition. Ground-shadow queries use distance from the camera. View
opacity, sunlight occlusion and ground shadows share the same reconstruction.

The current initial values below were copied from a single captured frame.
They are provisional reproduction values, not calibrated project defaults.
The capture alone cannot distinguish an authored preset from an interpolated
weather state; decimal precision is not evidence of either. Rounding these
values would not establish suitable defaults.

| Control               | Provisional initial value |
| --------------------- | ------------------------: |
| Coverage Bottom Power |               0.390888989 |
| Coverage Height Range |               0.306688964 |
| Bottom Density Power  |                         6 |
| Bottom Density Width  |                6.74295807 |
| Top Expansion         |                         1 |
| Density Scale         |                   0.75 /m |

Project defaults need evaluation with the bundled noise and LUTs across low and
high coverage, multiple top/bottom types, and thin and thick layers. Coverage
response and base shaping should be assessed before optical density is tuned.
The captured density scale is not a required setting for existing configurations.

## Verification

CPU comparisons cover 200,000 noise/base responses using actual shape-volume
texels from six mip levels and 200,000 profile queries using the bundled LUTs.
Static checks cover all density callers, host/shader field order and stride,
settings serialization, and the absence of reserved detail volumes from runtime
bindings. The empty-space margin includes both the local and upstream shear
displacements. These checks do not calibrate defaults or replace game rendering
and performance tests.
