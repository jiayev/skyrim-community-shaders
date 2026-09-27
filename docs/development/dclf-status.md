# DCLF: where it stands (2026-09-24)

A snapshot of Drawcall Limit Fix after the primary view's cull cut ([drawcall-limit-fix.md](./drawcall-limit-fix.md),
"The primary's cull without DCLF's objects"): what it draws, what the engine still does on the render thread, and
what a frame without the engine's culling jobs would take. Measurements are Riverwood, full featureset, unless they
say otherwise. The plan that follows from it is [dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md).

## What DCLF draws

| | DCLF draws | Still native |
| --- | --- | --- |
| **Object classes** | Static rigid geometry, including TruePBR; trees; switch-node children; skinned bodies and armour; actors and faces (Facegen, hair, eyes); decals (opaque, and blended with supported blend modes); ProjectedUV; terrain (when Terrain Blending does not defer it); screen-door fades | Alpha-blended objects and refraction; blended fades (hint 9); a LOD cross-fade's old-level copy (hint 10); LOD terrain and objects; billboards; effect shaders; ParallaxOcc, MultilayerParallax and sparkle; skins beyond DCLF's limits; grass, water, sky, particles; first person |
| **Views** | Main pass (colour, Z-prepass, decals); the sun's cascades and their volumetric copy; local shadow lights; Skylighting's occlusion map | The water cube map and plane reflections; the precipitation mask; first person; the focus view; the local map |

**What the engine still culls:**

-   **The main camera.** DCLF's references are stood in for inside the engine's list jobs. The engine still culls:
    -   effects;
    -   `BSOrderedNode` subtrees;
    -   actors;
    -   portal interiors;
    -   entries not yet admitted.
-   **The sun.** DCLF's entries are taken out; actors and entries with a native caster remain.
-   **Skylighting's map.** No engine cull at all.
-   **Local shadow lights.** Still culled by the engine; DCLF claims their draws.
-   **Reflections and precipitation.** Fully native.

## What is still on the render thread

Riverwood, ms per frame:

| Work | Cost | Where it could go |
| --- | --- | --- |
| Waiting on the main camera's list and registration jobs | 0.30 | Shrinks with coverage; the rest is [dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md) |
| DCLF's accumulate phase: the per-frame table patch from registered and synthetic passes | about 0.35 | Per-object state from events, the sun's bits on the GPU |
| DCLF's late per-object constants (`RefreshFrameConstants`: shading, extras, wetness) | not split out | Per-frame globals on the GPU (projected-UV matrix, land blend, wind); events for the rest |
| DCLF's prologue (`GpuResources::Touch`, `UpdateSkin`) | 0.14-0.18 | Bone palettes on the GPU |
| DCLF's epochs (Z-prepass, colour, shadow, LLF) | 0.12 + 0.69 + 0.09-0.25 + 0.02 | The colour epoch's join on its build ("Epochs that only submit") |
| The water cube-map reflection | about 0.5, mostly draws | A DCLF-native reflection view, as Skylighting's map |
| The precipitation mask, when it rains | about 0.37 | The same |
| The sun's residue (actors, native casters, the full-frustum cull) | about 0.15 | Actors into DCLF's shadow set |
| DCLF's share of the primary's stand-in | 0.02 | Already small |

The scene walk (about 0.5 ms) and the table builds run on DCLF's worker.

## Decals and the primary's stand-in

Decals are still DCLF's. They are table objects, drawn by its decal pass as before. What changed is where their pass
comes from:

-   Under a stood-in entry, a decal is handed to the engine's registration (`AppendVirtual`).
-   DCLF captures and withholds the resulting pass as usual.
-   So decals cost registration, not traversal (153 geometries a frame across every class handed back this way).

Two things keep them from synthetic passes:

1.  **Order.** Overlapping decals draw in the engine's order, which DCLF takes from the capture's registration
    sequence. A synthetic decal needs that sequence reconstructed, from the job and the position in its traversal,
    and merged with the decals the engine still registers.
2.  **Depth.** A decal with `kZBufferWrite` has its depth drawn by the native depth pass, which a registration keeps
    and a synthetic pass loses. DCLF writes no decal depth. Either DCLF's Z-prepass draws it with the decal's bias
    state, or a measurement shows nothing reads it (it is the host's depth pulled toward the camera).

The blend and write states are already derived without a native draw (drawcall-limit-fix.md, "Decals").

## What removing the culling jobs takes

There are two separate problems: the per-object engine state the cull maintains, and the residue the engine still
draws.

### The per-object state

-   **Switch selection.**
    -   The selected child changes when game logic writes the switch's index: harvested flora, destructible states,
        lit and unlit variants.
    -   Hooking those writes gives a deterministic scene event, so "which child is live" becomes table state rather than
        a per-frame test.
    -   The engine also brings a newly selected child up to date the first time it culls it (`UpdateDownwardPass` when
        `childRevID[index] != revID`). That update moves into the event's handler.
    -   Done in phase 3 ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Switch selection by event"). The writers are
        the tree manager's LOD selection and harvesting; there are no destructible or lit-state writers. The catch-up
        runs on the render thread when the walk applies the event, before anything culls.
-   **Fade.**
    -   `currentFade` steps each frame toward a target computed from the camera distance, the node's fade distances and
        the camera's `lodAdjust` (`FUN_14147b110`), by the frame time up to a cap (`FUN_14147a160`).
    -   A compute pass can keep each object's fade in a persistent buffer, compute the target from the eye and step it.
        The Lighting shader reads it for the screen-door dither and `MaterialData.z`.
    -   The engine's own consumers read `currentFade` on the CPU: the native views that still register DCLF's objects,
        and the shadow caster rule. They need a one-frame-late write-back, or those views become DCLF's.
    -   The engine's "visible within the last 2 frames" snap means late visibility behaves as the engine does.
    -   Blended fades (hint 9) stay native until DCLF draws them.
-   **LOD level and cross-fade.**
    -   The level comes from the same distance (`FUN_14147a430`: level `+0x152`, state `+0x153`, blend `+0x14C`).
    -   The LOD row picks the skin partitions, which DCLF already applies per object as a partition mask.
    -   The cross-fade copy of the old level becomes a second, dithered draw emitted on the GPU.
-   **The tree clock.**
    -   The tree manager advances a tree's animation time (`+0x164`) only when its `kAccumulated` bit is set,
        time-budgeted and under a mutex (`FUN_1404381e0`).
    -   Either DCLF sets that bit from a one-frame-late GPU visibility readback, which keeps the engine's clock as it
        is, or the clock moves into a GPU buffer, advanced for trees visible last frame (after reverse engineering
        `FUN_1404381e0` and its budget).

### The residue

The list jobs also cull what the engine still draws: effects, blended objects, LOD, billboards, actors, and the extra
list (sky, weather). Removing the jobs needs either of two things:

-   DCLF draws those classes;
-   or DCLF decides their visibility and hands the visible ones to the engine's registration directly, with no
    traversal, as the stand-in does now.

The residue's visibility has to be this frame's, because the engine is drawing it. A one-frame-late GPU readback
would pop objects in at the screen's edges, so the residue pass is a flat CPU frustum test on a worker, or GPU
visibility against a widened frustum. The per-object state above is latency-tolerant; the residue's visibility is not.

What survives in any case:

-   the extra list;
-   the room and portal visibility Light Limit Fix's `RoomIndex` needs;
-   the list processes' camera and planes, which other code reads (the tree manager, light culling).

None of it is per object.

### The other views

-   **The sun:** GPU-driven once actors join DCLF's shadow set. The sun's bits already have a GPU-ready rule
    (`PrimaryCull::SunShadowBits`).
-   **Local shadow lights:** their shadow-light selection rule (`FUN_1414fcf80`).
-   **Reflections and the precipitation mask:** DCLF-native variants, as Skylighting's map.
