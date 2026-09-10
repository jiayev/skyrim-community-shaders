# Cloud sampling and motion

## View integration

The low-cloud view march uses `(distance * 0.003662109375 + 3) * marchStepScale`,
with one jittered sample inside each interval. Distance is evaluated in metres;
the existing `cloudMaxStep` setting maps to `97 / cloudMaxStep`. This is a
project-specific quality calibration. `cloudMaxStep` is a sampling-quality
control, not a hard iteration limit. Finite layer intervals and the existing
opacity termination bound the march.

Physical Sky retains its spherical-shell intersections, spatiotemporal jitter,
local light march, atmospheric integration and 0.003 transmittance cutoff.
High clouds remain volumetric by default, with an optional distant thin-layer
approximation described in [cloud lighting and reconstruction](cloud-lighting.md).
The bundled low-cloud noise uses the separate coverage, roundness and profile
contract in [noise reconstruction](noise-contract.md).

## Empty-space acceleration

`NdfAcceleration.cs.hlsl` builds a 64 x 64 occupancy map, then a bounded
two-dimensional distance map using a bounded neighbourhood search. The map
encodes horizontal distance to occupied support; it does not encode density,
height or boundary slopes.

Occupancy uses maximum coverage across the source footprint, including the
neighbouring texels needed by bilinear filtering. No filtered erosion mip is
used to decide that a region is empty. Distance search wraps with the NDF and
subtracts two cell widths from centre distances. The ray converts that bound
using the smaller world-space cell dimension and subtracts the maximum shape
shear before skipping. Skips stop at the current spherical-shell segment.

The procedural distance map is regenerated after NDF changes and before cloud rendering.
Texture and generated NDF modes both participate. If acceleration resources or
shaders are unavailable, the renderer uses the ordinary distance-based march.
The auxiliary maps use R32_FLOAT to avoid losing small positive coverage to
UNORM quantization. Two 64 x 64 maps require 32 KiB of texel storage.

This makes empty-space skips conservative for the threshold reconstruction,
which does not create density outside positive coverage/profile support. It does not make finite-step quadrature exact, nor guarantee that every
sub-step cloud detail is sampled. Dense, long rays can cost more than the former
iteration-limited march.

## Independent controls

-   Low-cloud quality uses the existing `cloudMaxStep` JSON field, clamped to 1–200.
-   `cloudLayer.high.viewSteps` defaults to 194 and divides only the high layer's
    occupied shell intervals; clear approach distance and the low layer are excluded.
-   `cloudLayer.high.lightSteps` defaults to 6 independently of low-cloud lighting.
-   `cloudMap.procedural.parameters` controls coverage, shared type noise,
    separate remaps/exponents, local blending and bottom height variation.
-   `cloudMap.procedural.noise` controls four generated inputs and DDS overrides.
-   `cloudLayer.low.shapeShear` offsets the NDF with height, in kilometres along
    the wind. Zero preserves the unsheared profile.

See [procedural NDF generation](ndf-generator.md) for all controls and migration.
Version 2 replaces previous array layouts with height RG and modeling RGB.

## Motion and history

NDF sampling and low-cloud noise share the existing wind displacement.
Reprojection subtracts the displacement since the last successfully written main
history. This compensates for the common rigid wind translation.

The procedural NDF contains no time-dependent inputs. Coverage, height and
profiles move together under the shared wind offset. An editable static weather
offset shifts generation inputs. Parameter or texture changes rebuild the field
and invalidate history. World scale changes sampling and history without
rebuilding the normalized map. High-cloud pattern drift still reduces its
separate history confidence.

Auxiliary W stores `1 + visibleHighCloudFraction`; zero remains invalid. Spatial
fallback uses its validity, not its magnitude, as a filter weight. Mixed layers
and changes in layer contribution reduce history confidence. The representation
still uses one depth/history for both layers; it is not exact multilayer motion.
Representative cloud depth uses each step's visible opacity contribution so
changing interval lengths does not change depth weights solely by sample count.

Cloud configuration changes, backward time and gaps over 0.25 seconds invalidate
history. Wind/time snapshots advance only when the main history is written.

## Verification scope

Static review covers CPU/HLSL field order, constant-buffer layout, new JSON/UI
controls, resource dependencies, wrap/footprint bounds, segment termination and
history metadata consumers. No C++ build, shader compilation or GPU validation
is included. Runtime acceptance should compare moving clouds with a stationary
camera, thin cloud edges, grazing rays, non-square NDF scales, shape shear, terrain
occlusion, mixed layers and changing high-cloud settings. Profile acceleration,
view/light marching and reconstruction separately.
