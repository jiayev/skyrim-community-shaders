# Local cloud weighted states

## Authored state

Physical Sky stores one local-cloud envelope at `cloudMap.localCloud`.
Asset identity, worldspace, placement, altitude decoding and composition options
belong to each complete state. The menu edits one state with an enable control.
There is no instance list, priority stack or external manifest.

## Standalone persistence

`SceneBlend` is a JSON data helper with no Scene Manager dependency. A serialized
value has a `states` array, each element containing a complete `value` object
and a nonnegative finite `weight`; total weight is at most one (within
floating-point tolerance). An empty array is the identity state.

The menu and `PhysicalSky::SetLocalCloudState` write a single endpoint at weight
one. Physical Sky saves and loads the envelope with its normal settings. Cloud
DDS files remain ordinary texture-mod dependencies under
`Textures/PhysicalSky/LocalNdf`.

Local clouds have no Scene Manager integration, override/inherit controls or
automatic weather/time transitions. The renderer supports weighted endpoints loaded
from settings. Editing a multi-endpoint envelope captures the strongest endpoint
as a single state. `SceneBlend::Interpolate` can blend complete envelopes without
interpolating resource IDs or placement fields individually, but no scheduler
invokes it here.

## Rendering

Each active endpoint is validated independently and cached in a texture-array
slice. The current worldspace filters compatible endpoints. Missing/invalid
resources fall back to the global NDF for their share of the transition.
Resource loading uses the existing synchronous texture cache; weight-only ticks
neither reload DDS files nor dispatch the local map generator.

All slices share the union footprint and altitude envelope. Encoded heights are
converted into that common physical interval. Each endpoint first applies its
Interpolate or Maximum operator to the current global NDF, then the resulting
control fields are blended by endpoint weights. Mixing maximum floors before
applying the operator would produce incorrect intermediate cloud shapes.

Different transforms crossfade their stationary world-space fields. This avoids
interpolating incompatible rotations, dimensions or DDS height encodings, and
requires no artificial patch velocity. Fading to a disabled or empty state
restores global clouds continuously. This is a control-field transition, not a
crossfade of independently lit volumes or a guarantee of topology-preserving
cloud morphing.

A structured weight buffer changes during blending; cached slice content changes
only with endpoint identity/content, source revision or atlas settings. Local
states remain in the whole-cloud history-reset key, and resource rebuilds also
reset history. Debug exposes endpoint weights, cached slices and the source
textures. Multiple endpoints cost more than a single state because each
contributing operator must be evaluated.

## Shared weather shape

`bottomSpreadScale`, `bottomSpreadHeight`, `bottomSoftness`,
`bottomSoftnessHeight`, `topExpansionScale` and `densityScale` remain independent
Physical Sky settings. They are converted to GPU coefficients alongside
curvature, global NDF controls, wind and cirrus state. Local endpoint objects
carry NDF placement/composition only; they do not define independently lit or
independently shaped weather volumes.
