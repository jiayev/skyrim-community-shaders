# Local NDF weather transitions — design only

## Scope

The implemented side provides optional assets, persistent instances, composition,
renderer bindings and `SetLocalCloudInstances`. This document specifies the future
controller interface. No weather scheduler, asynchronous loader, state blending,
storm controller, savegame integration or transition code is implemented here.

The controller owns weather identity, time, easing and interruption policy.
Physical Sky owns resource validation, GPU fields, coherent bounds and history.
The controller must not manipulate SRVs or infer altitude from DDS contents.

## Proposed API

```cpp
PrepareCloudState(requestId, CloudStateDescriptor);  // main/render-thread handoff
QueryCloudState(requestId);                         // Pending / Ready / Failed + diagnostics
BeginCloudTransition(fromHandle, toHandle, durationSeconds, easing);
SetCloudTransitionWeight(weight);                   // normalized absolute weight
CommitCloudState(handle);
CancelCloudTransition(policy);                     // retain current mixed state or revert
```

A descriptor contains global NDF settings, noise/profile source IDs, low-cloud
height mapping, cirrus settings, lighting/wind settings and local instances. Asset
IDs are resource identity; stable instance IDs are placement identity. A state
handle retains a complete immutable resource set and generation counters. Readiness
requires valid required pairs and all generated replacements to be complete.
Invalid optional inputs have a recorded fallback decision; no partly loaded pair
may be presented as Ready. The proposed calls enqueue work; only the renderer
thread may mutate D3D11 resources or publish handles.

Existing `SetLocalCloudInstances` remains an immediate authoring/layout API. A
controller should not approximate transitions by repeatedly replacing the saved
configuration: it would rebuild maps and reset cloud history every frame. Active
runtime state and editable persistent settings must be separated in that phase.

## Interpolation semantics

-   Scalars such as influence, altitude, dimensions and lighting interpolate in
    their documented physical domains. Rotation follows the shortest angular arc.
    Worldspace, asset IDs, source mode and blend ordering are discrete.
-   Instances with the same stable ID and asset may interpolate transforms and
    influence, provided their worldspace is identical. Teleports require explicit
    reset semantics. Source texture replacement must not masquerade as movement.
-   Unmatched instances fade their influence in or out. An instance with zero
    coverage can intentionally clear the background; opacity cannot be derived
    from coverage. Local modeling and height weights retain separate meanings.
-   Fading premultiplied fields must also fade their weights. Decode each endpoint's
    height into metres before interpolation, or normalize both to a common envelope.
    Never interpolate encoded heights from different altitude ranges directly.
-   Independent maps have no correspondence. Crossfade of NDF controls can make
    surfaces rise or merge; crossfade of density avoids that deformation but doubles
    density-query cost. Default to control-field transitions for compatible maps;
    expose density transition as an explicit future quality mode, not a hidden cost.
-   Keep wind integrators continuous while interpolating velocities. Do not restart
    accumulated offsets or linearly interpolate wrapped phases across their seam.
    Local macro anchoring and global detail evolution remain separate decisions.

## GPU strategy and performance

Prepare both endpoint fields before starting. Choose a fixed common XY rectangle,
texel size and altitude envelope for the complete transition. Resample endpoints
once if their atlas grids differ. During the transition, combine endpoint maps in
a dedicated compute pass only when weight changes. Ray samples still read one
composed height/modeling pair. Dispatch rate can be limited by a measured bound on
screen-space change; do not rebuild every individual patch at every frame.

Moving instances require rebuilding their affected region or a separate placement
pass. The first implementation should prioritize a bounded instance count and
full-atlas recomposition over complex dirty-rectangle tracking. Expose memory and
composition cost in Debug. Two endpoint maps plus output/scratch increase memory;
reserve a budget before preparation and report failure rather than silently losing
resolution midway through a transition. Widely separated instances may eventually
need tiled fields, but should not add an unbounded per-ray texture loop.

## Bounds, shadows and history

The published state is a tuple of maps, XY rectangle, height encoding, acceleration
bounds, worldspace and revision. Switch the tuple atomically at a frame boundary,
before both shared constants and shadow-volume dispatch. Use the union of endpoint
bounds for the transition lifetime, so growing clouds cannot escape predraw or
cubemap acceleration. Shrink bounds only when committing the destination.

Ordinary authoring edits use history invalidation today. Smooth transitions need
an additional continuous change signal, distinct from structural revision. Track
placement displacement for matched instances; reject/reduce history where source
identity or local influence changes invalidate that motion. Derive reconstruction
uncertainty from change in density/support, rather than resetting the complete
screen every tick or accepting history indefinitely. The main view and cube share
the same state generation even if their samples update on different schedules.

## Failure and interruption

Preparation failure leaves the currently published state untouched and returns
asset-specific diagnostics. Cancellation never releases resources still referenced
by submitted GPU work. A new transition during a blend should capture the current
mixed control field as its new source; reverting to the previous endpoint would
produce a visible jump. Worldspace changes cancel incompatible transitions and
publish the destination world's prepared layout or procedural fallback.

Persist endpoint descriptors and controller progress only when a scene/savegame
controller defines that contract. Do not add a second Physical Sky JSON format.
The current feature settings remain the editable default layout.

## Acceptance criteria

Test 0/1 endpoints, interrupted/reversed transitions, all assets missing, one invalid
pair, optional mask addition/removal, identical IDs with changed assets, different
altitude scales, rotations through ±180°, worldspace changes, cloud growth beyond
current bounds, camera movement and sun occlusion. Compare all views/shadows at a
fixed weight; measure preparation spikes, steady GPU time, VRAM and reconstruction
stability. A continuous weight must not trigger full history reset every frame.
