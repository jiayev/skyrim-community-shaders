# Cloud lighting and reconstruction

## Implementation scope

Physical Sky uses independent directional and upward optical-depth caches,
local light integration and an optional distant thin-layer approximation.
Cache resolution, interval quadrature and approximation thresholds are
project-specific choices. No runtime performance or visual equivalence is
claimed. The density contract and its adaptation are recorded in [noise reconstruction](noise-contract.md).

## Independent light columns

`CloudLighting.hlsli` integrates scalar optical depth separately towards the
sun and world +Z. Ambient attenuation now uses upward optical depth instead of
solar optical depth multiplied by the light's vertical component. Solar tint
is applied when evaluating light transmittance, not when storing optical depth.

Low and high layers each have a 32 x 32 x 16 RGBA16_FLOAT cache:

| Channel | Column from the cached world position      |
| ------- | ------------------------------------------ |
| R       | Own layer, towards the directional light   |
| G       | Own layer, upwards                         |
| B       | Other layer, towards the directional light |
| A       | Other layer, upwards                       |

Both volumes together use 256 KiB of texel storage. XY bounds follow the camera.
The low cache uses `shadowVolumeRange`; the high cache uses the larger of that
range and the high weather map's world size. Z covers the respective altitude
band. Density sampling still uses spherical altitude.

Each enabled main-view pass regenerates the low cache and, when high clouds
are enabled, the high cache. Generation reads density resources directly and
never reads either lighting cache. Main-view and cubemap tracing then share the
results. Output UAVs are unbound before the caches become SRVs at t24/t25.

Cached optical depth is filtered only inside its valid box. Within the outer
2.5 voxel widths it blends towards direct integration; outside the box it uses
direct integration entirely. Missing cache shaders or disabled caching also
select direct integration. No missing column is assumed to be clear sky.
Quadratic interval boundaries concentrate samples near each layer entry, with
one midpoint sample per interval and the full interval length as its weight.
Spherical segments exclude clear gaps, stop at the planet, and retain the
configured in-layer march range limit.

The low cloud's local light march covers up to 6 km, bounded by the layer exit.
Its interval endpoints are `distance * (i / steps)^2`, with one jittered sample
per interval weighted by its length. Increasing the budget refines the entire
column, including the largest far interval; there is no minimum step that can
exhaust the column before the requested sample count. The maximum interval is
`distance * (2 * steps - 1) / steps^2`. Local noise mip follows interval length
in noise texels, clamped to 0–3. This is bounded noise filtering, not an exact
average of the nonlinear reconstructed density. The view-density formula and
its mip remain unchanged. The remainder begins at the local march's actual end.
High clouds use three local samples over the first
250 m when a cached remainder is available. The cached remainder starts at the
end of that interval, preventing the local and cached solar columns from being
added twice. Direct high-cloud fallback uses `high.lightSteps` for its full
column. This is a finite-sample approximation, not an exact integral.

`cloudLayer.lighting.useLightCache` defaults to true. `cacheSteps` defaults to
16, clamped to 4–32, and controls cache construction and upward/cross-layer
fallback quadrature. Larger bounds make voxels coarser; boundary fallbacks and
local low-cloud lighting can still be expensive. Cache generation is separately
profiled as `PhysicalSky::CloudLightCache`; no speedup has been measured yet.

## Cross-layer shadows

`cloudLayer.lighting.crossLayerShadows` defaults to true. Each scattering point
receives the other layer's solar attenuation and upward ambient attenuation.
The other layer's solar attenuation multiplies external illumination before the
local multiple-scattering sum, so local scattering octaves do not weaken the
occluding layer's shadow. Disabling this setting skips the two cross-layer
columns during cache generation and direct fallback.

The existing ground-receiver shadow volume remains separate. This change adds
cloud-to-cloud shadowing; it does not add high-cloud shadows to ground receivers.

## Distant thin high clouds

`cloudLayer.high.thinLayer` defaults to false. When enabled, `thinLayerStart`
and `thinLayerEnd` blend from volume tracing to the thin approximation over
15–25 km by default. The approximation requires a sky ray from below the layer,
a complete unclipped layer crossing, and a sufficiently steep crossing angle.
It fades out between 500 m and 1 km of layer thickness and near grazing angles.
Inside/above the layer, thick layers, clipped rays and near distances retain
volume tracing. The default thick high-cloud layer therefore remains volumetric.

The ray intersects the middle-altitude sphere. Four radial profile samples
estimate vertical optical depth using the current high-cloud density model.
Path length includes the incidence correction; one extinction-weighted position
supplies lighting and representative depth. This preserves the authored high
weather/cell/wisp inputs without requiring a separate cloud-pattern asset.

Transitions blend premultiplied radiance, transmittance and opacity-weighted
depth moments. Full thin weight skips the high-cloud view loop. Partial weight
computes both paths, so transition pixels can cost more. Fine vertical features
or strong lighting variation within the layer can differ from volume tracing;
this mode needs visual evaluation with the intended thin-cloud preset.

## History and radiance storage

After temporal reconstruction and full-resolution upscale finish, the three
half-resolution intermediate/history texture owners swap. Their SRVs and UAVs
are unbound before the swap. The next frame reads the newly accumulated history
and overwrites the old history as its destination. This removes three per-frame
`CopyResource` operations while keeping the same history representation.

Trace, intermediate and history luminance use RGBA16_FLOAT. Every color input
and feedback destination in temporal reconstruction therefore has equal RGB
precision. R11G11B10_FLOAT has fewer blue mantissa bits than red/green; repeated
filtering and quantization in that format can introduce channel-dependent error.

Only the full-resolution output and cubemap use R11G11B10_FLOAT when the D3D11
device reports texture, sampling/load and typed UAV support. Neither feeds the
cloud temporal history. Unsupported devices use RGBA16_FLOAT for those outputs
as well. Transmittance and auxiliary depth/history metadata remain RGBA16_FLOAT.
Combined screen cloud targets use 33.5 bytes per full-resolution pixel with
packed output, or 37.5 without it, before dimension rounding. This excludes
other sky resources and the lighting caches.

## Verification scope

Changes have static review only: CPU/HLSL field order, settings serialization,
entry-point registration, cache dispatch coverage, binding transitions, disjoint
shadow intervals, thin/volume composition and history resource ownership.
No C++ build, shader compilation, GPU capture or frame-time measurement was run.
Runtime acceptance should cover low sun, camera movement across cache bounds,
high-cloud toggles, thin-layer transitions, geometry occlusion, dark gradients
and repeated history invalidation, with cache and direct paths compared.
