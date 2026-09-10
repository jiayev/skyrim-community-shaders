# Cloud sampling and motion

## Ray integration

The base step follows Nubis Evolved slide 39: `3 + 60 * distance / 16384`,
with distances in metres. A geometric step increase fits the remaining occupied
intervals into a finite view budget. `lowViewSteps` defaults to 192 (32–512);
`cloudLayer.high.viewSteps` defaults to 64 (8–256). Layers intersected by a ray
contribute their budgets to a shared march. The budget counts density probes,
including empty probes and backtracking, rather than truncating the cloud range.

Sphere intersections retain both near and far pieces of each layer and clip
against geometry and the planet. Their endpoints partition the ray into ordered
intervals. Overlapping layers add extinction and extinction-weighted light
sources at the same sample; separated layers integrate in front-to-back order.
`rayMarchRange` limits occupied distance per layer, excluding clear approach
and gaps. Each future occupied interval reserves at least one probe.

NDF distance bounds skip empty horizontal support, limited to the current
interval. Empty density doubles the next step. A hit following a coarse probe
retries that interval at the fine step when budget permits. The 64 x 64 distance
map retains conservative coverage footprints, tiling, and shape-shear margins.
Noise mip follows step length in noise texels, bounded to levels 0–2. The
bundled volume retains its [existing noise contract](noise-contract.md).

Samples within 250 m use temporal ray-start jitter. Distant samples use stable
spatial jitter. Lighting uses deterministic local midpoints, independently of
the view-ray jitter. Integration uses `T_step = exp(-extinction * stepMetres)`
and `weight = T * (1 - T_step)`. Marching stops at `T <= 0.1`. Final transmittance
is remapped to `saturate((T - 0.1) / 0.9)`; premultiplied radiance is adjusted to
the same opacity. This cutoff/remap is an artistic approximation; the individual
Beer step integral is invariant to subdivision for a constant source.

## Full-resolution 16-phase reconstruction

`CloudTemporal.hlsli` traces one actual full-resolution pixel from each 4 x 4
block. A 16-entry permutation visits all offsets. The compact trace textures
have dimensions `ceil(width / 4)` by `ceil(height / 4)` and store those rays;
they do not define a lower-resolution camera or pixel footprint.

History and resolved output both have full framebuffer dimensions. Each frame
reprojects history using opacity-weighted cloud depth, the previous successful
capture's camera matrix/position, and wind displacement. The camera matrix
includes the capture's projection jitter, matching current depth/ray coordinates.
The active viewport comes from the draw's per-frame DR parameters and the cloud
texture allocation. Ray, depth and motion UVs use that same coordinate system.
History stores the previous successful capture's viewport extent, including its
fractional part, and clamps taps to that capture's integer active bounds. DR size
changes retain history and do not reset cubemap accumulation. Trace dispatches
cover only the current active region. Traced pixels update from their actual
sample. Other valid pixels retain reprojected history. Four trace neighbours
supply only missing history, with scene-depth rejection; there is no spatial
upscale pass applied to the accumulated result.

Temporary hole fills have validity 0.5; actual samples have validity 1. Their
first traced update replaces the fill. Colour, opacity and projected motion
control later temporal blending. Clear sky does not contribute to cloud-depth
moments. Configuration changes, shader/texture reload, time reversal and gaps
over 0.25 seconds invalidate history. Snapshots and the phase index advance only
after main and cubemap reconstruction complete.

The 64 x 64 x 6 cubemap follows the same schedule: 16 x 16 actual rays on every
face, then per-face full-resolution temporal reconstruction. Cube history is
sampled as a cube to cross face boundaries. It compensates camera translation
and wind, independently of screen rotation. Depth and high-layer fraction are
stored as opacity-weighted moments for cube filtering. Steady-state ray count
is 1,536 per frame, versus 8,192 for the former two-full-face schedule.

Both layers share one representative depth. Mixed-layer motion, high-pattern
relative drift, disocclusion and rapidly changing light still need runtime
assessment; a single history cannot represent arbitrary multilayer motion.

## Resources and validation

Transmittance is scalar R16_FLOAT. Trace, output and history radiance all use
RGBA16_FLOAT with equal RGB precision; metadata uses RGBA16_FLOAT. Full-resolution
outputs and history swap ownership after unbinding their views. Screen storage
is 37.125 bytes per framebuffer pixel before dimension rounding, excluding
lighting caches, shadows and atmosphere resources.

Static/CPU checks cover the buffer contract, 16-phase coverage (including odd
sizes), native pixel recovery, all cube texel orientations, ordered interval
coverage under finite budgets, Beer integration and edge-depth moments.
No C++ build, shader compilation, GPU render or timing is part of these checks.

## Reference

[Nubis Evolved, SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf):
slides 39, 48, 54, 59, 157 and 187. Budget fitting, spherical interval ordering,
phase permutation, cube history and storage formats are adaptations for Physical Sky.
