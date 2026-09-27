# DCLF: object tables built from events

Status, 2026-09-24: the reverse engineering is done ("Reverse-engineering results"), and Phases 1 to 3 are built
("Phase 1: object slots", "Phase 2: the delta walk", "Phase 3: structural events"). It refines Phase 3 of
[dclf-gpu-driven-frame.md](./dclf-gpu-driven-frame.md) ("a persistent GPU scene").

## The question

DCLF rebuilds its object tables every frame by walking its whole tracked set. Since the walk moved to a worker,
it runs while the engine is still modifying the scene: culling, updating reflections, toggling nodes. Two bugs
have come from that so far:

-   the NPC head drops: transient `kHidden` bits;
-   `Salmon:0` drops, not yet traced.

The walk's own documentation already lists three other values that change under it (fades, billboards, texture
transforms). Each race so far was fixed on its own terms.

The alternative: keep the tables as persistent state, and change them only when the engine reports a change
(an object added, removed, hidden, shown, moved, or given another property). The full walk then becomes a
synchronous validation, not the mechanism. This document covers what that needs.

## What the walk does today

The tracked **set** is already event-driven. `SceneTracker` detours the `NiNode` child functions
(`AttachChild`, `DetachChild*`, `SetAt*`) and pushes events onto a lock-free stack from whichever thread
attaches or detaches. `SceneStore::ProcessEvents` applies them before the walk, and every Present. A slice of
tracked geometry is re-checked against its root each frame, as a safety net for missed detaches.

Everything after membership is per frame. `SceneStore::SceneWalk` iterates **every tracked geometry** (10,021 at
Riverwood, about 2,700 in Dragonsreach) and rebuilds the per-frame vectors from scratch (`objects`, `draws`,
`bones`, `faceStreams`, the shadow columns, and so on). It runs on the `CS DCLF worker` from `Main::Draw` to
`AfterShadowMaps`, and takes 1.0-1.6 ms (max 2.9). The render thread first runs `PrepareSceneJob`
(`GpuResources::Touch` and `UpdateSkin`, 0.14-0.18 ms). For each geometry the walk reads:

| Input | Source | Read how today |
| --- | --- | --- |
| Type, skin instance and partitions, renderer data, vertex and index buffers | the geometry, its skin | `ClassifyStatic`, cached per object for 64 frames behind a witness (renderer data, property, material, fade state) |
| Shader property, its cast, material, material alpha, property flags, lighting descriptors | the property and material | the same classification |
| Hidden, app-culled, switch-node selection, actor ancestry | `NiAVObject::flags` bit 0 on every node up to the category node; `NiSwitchNode` index | `ClassifyFrame`: every frame for face shapes and switch children, otherwise with the 64-frame cache |
| Fade | `BSFadeNode::currentFade`, written by the main camera's cull | `ClassifyFrame`, `FadeStateOf` |
| Skin partitions drawn | the fade node's LOD level, the dismember flags | `SkinPartitionMask`, every frame |
| World, previous world, world bound | the geometry | every frame |
| Bone palettes | `UpdateSkin` (render thread only), then `boneMatrices` | every frame per skinned object; a skin not updated ahead is a "skin miss", and the join rebuilds inline |
| Face positions | `FaceSnapshots`, from the morph job's hook | already an event |
| Shadow caster verdict and technique, alpha-tested diffuse | property flags, alpha property, material | every frame |
| Sun entry node | the ancestor chain | every frame (`SunEntryOf`) |
| Geometry slot | `GpuResources` | every frame, render thread for new buffers ("geometry miss") |

Two per-frame loops consume the engine's own walks and stay per frame whatever the tables do:

-   **The accumulate phase** (render thread, 0.32 ms) captures the main camera's registrations: which objects
    the engine found visible, and their pass descriptors. Replacing it is the GPU-driven plan's Phase 1, not
    part of this.
-   **`RefreshFrameConstants`** resamples per-frame values (technique constants, animated shading, UV extras,
    skin wetness). These are per-frame by nature.

## What actually changes: measured

`CS_DCLF_TEMP_CHANGE_RATES` (a temporary probe, removed) compared each tracked geometry's inputs with the
previous frame's. It ran with the walk on the render thread, before the culls start, so every value was read
at a quiet point. The run had 1,500 frames at Riverwood, then Dragonsreach.

| Per frame | Riverwood (10,021 tracked) | Dragonsreach (2,712) |
| --- | --- | --- |
| World transform changed | 181-207 (max 309), about half actor-owned | 133-178 (max 590 during the load) |
| Distinct geometries that moved in a 300-frame window | 210-222 (416 in the window with the load) | 150 |
| Property, material, renderer data, alpha property, skin instance | 0 (0.01 during the load) | 0 |
| Property flags | 0 (0.05 in the first window) | 0 |
| Effective hidden state, read at a quiet point | 0 | 0 |
| Fade value | 63, then 27, then 0 as the scene settled | 12 during the load, then 0.1 |
| Membership events (attach / detach) | about 0.04 / 0.1 | a burst of about 11,600 / 3,700 at the load |
| Distinct ancestor nodes below the category nodes | 7,195 | 1,790 |

What that says:

-   **About 2% of the tracked set moves per frame, and it is the same 2%.** Actors, animated references,
    critters and physics. The other 98% is re-read every frame to find that nothing changed.
-   **Structural changes are events, not a stream.** In steady state, no property, material, buffer,
    alpha or hidden state changed in 1,200 frames. They happen at loads and at gameplay events (equipping,
    dismemberment, enable and disable).
-   **Polling per node instead of per geometry would not help.** The tracked set has 7,195 distinct
    ancestor nodes against 10,021 geometries.
-   **The hidden races are view-transient.** The bit read at a quiet point never changed. The drops came from
    bits the engine sets for part of one view's rendering, then restores.

## The design: tables as persistent state

### Stable object slots, and a live list

Each tracked, eligible geometry holds one object slot for as long as it stays eligible. Every per-object
column (record, draw, bone offsets, face stream, shadow columns, lights, shading) is indexed by slot. The builds
iterate the slots and skip free ones by their flags (Phase 1). A separate live list is only worth adding if
fragmentation after large cell changes makes that loop measurable.

"The transform witness cannot pay for itself" rejected stable slots as a *performance* change: a record costs
13-26 ns to write. The argument here is different. Slots are what lets the tables persist across frames, so
that per-frame work is proportional to what changed and the worker never has to walk the live scene.

### Every input gets a source

| Input | Event today | What is needed | Per-frame remainder |
| --- | --- | --- | --- |
| Membership | `SceneTracker` attach and detach | done; the rolling re-check stays as an alarm. An attach or detach under a sun entry node also re-evaluates that node's other dependents (Phase 3) | none |
| Moves (world, previous world, bound) | membership, for references (a move is a disable and an enable) | **The movable set** (see "Reverse-engineering results"): actor-owned, a controller, a non-fixed rigid body, or a billboard on the chain; and, for the sun entry, anything moving under the reference root (Phase 2). Taken at classification. **Built (Phase 3):** Havok's node writes and runtime controller additions are events, which take the traits again | the moving slots' placement |
| Bone palettes | none | every skin, every frame: the wind moves a tree's bones with no controller or body in the reference (Phase 2); the GPU-driven plan's bones-on-GPU removes `UpdateSkin` later | per skinned record |
| Property, material, alpha property, skin instance, renderer data | materials: `MaterialSources` write hooks; renderer data: `GpuResources` | **Built (Phase 3):** `BSShaderProperty::SetFlags` and `SetMaterial` are hooked. Pointer swaps have no choke point: an actor's entries re-read them every frame, and static slots take them at attach, with the validation walk as the alarm. | actors' entries only |
| Persistent hidden and shown | membership (disable and enable release and load the 3D) | **Built (Phase 3):** an actor's entries, and a moving entry whose verdict is hidden, re-read their chain every frame, which covers actors and the visibility controllers. A static the engine accumulates while its kept verdict says hidden is taken again. The remaining static writers (portal graph, queued trees, cell paths) are hooked as the validation walk finds them; it has found none. | actors' entries and hidden movers |
| Per-view hides | none | A per-slot **view mask** for DCLF's views (main, sun, and later the others), set by rules reverse-engineered per writer: the first-person skeleton is only in the first-person view; the player's 3D is hidden in the cube-map reflection; portal traversal's toggles are internal to the cull and don't count. `CaptureCullHiddenBits` is the first, hand-listed version of this. | none |
| Switch-node selection | none | no setter: switch children stay a per-frame class | one switch-node read per switch child (Phase 2) |
| Fade | written by the main camera's cull: `BSFadeNode::OnVisible` and the fade update `FUN_14147a160` | **built (Phase 2):** both writers are detoured, and a change is an event for the records that read the node | the records of fading nodes (tens a frame while the camera moves) |
| Partition LOD | the fade node's LOD level, written by the cull | per frame for skinned records, which are per frame anyway | the skinned records |
| Face positions | `FaceSnapshots` | done | per morphing head |
| Animated shading, texture transforms | controller write hooks (`MaterialSources`) | done for shading; texture transforms follow the same hooks | none |
| Per-frame globals (fog, eye, wind, clocks) | none | GPU (`RefreshFrameConstants`' job, split out) | per frame, not per object |

### One quiet point, and no walk on the worker

The render thread has a moment when the scene is final and no cull or reflection update runs: the `Main::Draw`
hook where the walk used to be kicked. Phase 2 does the table work there, on the render thread. It drains the
event queues (membership, fades) and evaluates only what they and the per-frame set name. Nothing reads the scene
for the tables off the render thread, so the whole class of race goes away: nothing can see a transient bit or a
half-written transform.

The plan was for the render thread to copy the movable slots' inputs and hand them to the worker to apply. It
didn't survive measurement: the reads are the cost, not the writes, so a hand-off would only add to it (see
"Phase 2: the delta walk").

Events that arrive from other threads during the frame (loader threads, the cull jobs' fade writes, morph jobs,
Havok) queue up and apply at the next quiet point. That is the same one-frame rule membership follows.

### The full walk as synchronous validation

The current walk stays as the reference. `CS_DCLF_WALK_PARITY=1` runs it on the render thread at the quiet
point every 60 frames and compares its tables with the incremental ones, object by object (built in Phase 1;
there the "incremental" tables are the slot tables). The gate for every step: 0 differences through the standard
matrix (road, Riverwood, Dragonsreach, a dungeon, cell changes, save and load, the live toggle), with the full
featureset.

## What it needs

What is left after Phases 1 to 3 (the slots, the delta walk, the structural events and `CS_DCLF_WALK_PARITY` are
built): the per-slot view mask, and its use in the shadow build and the draws' visibility (Phase 4).

## Reverse-engineering results

All from AE 1.6.1170, via the decrypted `.text` scans and Ghidra. Two temporary probes, since removed, ran with
the walk on the render thread before the culls start.

**A static reference never moves in place.**

-   `TESObjectREFR::SetPosition` (`0x1402ea960`) and `SetAngle` (`0x1402ea6a0`) write only the reference's
    data. They never touch its 3D.
-   A script or console move goes through `TESObjectREFR::MoveTo_Impl` (`0x140a447f0`). For a non-actor, that
    first calls the reference's `Disable` (`0x1402ec7e0`, named `ArrowProjectile::Disable` in the database).
    `Disable` calls `Set3D(nullptr)` (vtable `+0x360`) through a reentrancy guard (`FUN_14017a6e0`), so the 3D
    is released: a detach event.
-   `MoveTo_Impl`'s in-place transform write (local transform, then `NiAVObject::Update`) therefore finds no 3D
    unless the reference was already disabled, and its 3D is loaded again on `Enable`: an attach event.
-   Actors take their own path (`Actor` move functions), and actors are movable anyway.

**The movable set is structural, and covers every mover.** A slot is movable when any node from the geometry up
to its category node has one of these traits. Riverwood, 10,021 tracked geometries:

| Trait | Tracked geometries |
| --- | --- |
| owned by an actor | 192-199 |
| has a time controller (`NiObjectNET::controllers`) | 579-594 |
| has a rigid body whose motion type isn't `kFixed` | 186-189 |
| is under an `NiBillboardNode` | 63 |
| any of them | 915-922 (9% of the tracked set) |

In three 600-frame windows, 416, 251 and 234 geometries moved. Every one was inside the set, and 0 outside it.

-   The movers by base type: actors (about 125 geometries), animated furniture and activators, animated
    movable statics, and physics clutter settling after the load.
-   The movable statics with no controller or body of their own are effect planes and water jets under
    billboards, which DCLF already leaves native (`billboard`).

A slot's traits are fixed at attach, apart from two events that are hookable. A rigid body's motion type can
change (`SetMotionType`, a grab, a kick), and a controller can be added at runtime. Both are rare.

**`kHidden` has 133 writers.** The complete scan (direct `or`/`and` on `+0xF4`, and register-mediated
read-modify-writes) found 70 direct sets, 67 direct clears, 34 register sets and 27 register clears, in 133
functions. Some are false positives: `+0xF4` is a common offset. By caller and by the functions they call:

-   **Outside the tracked roots:** the sky (clouds, moon, stars, sun), the map menu and camera, inventory 3D,
    projectiles, particles, the water system, and the console toggles (`ToggleCellNode`, `ToggleLODLand`,
    `ToggleGrass`, `ToggleWaterSystem`, `Show1stPerson`). Nothing to do.
-   **Before attach:** `LoadGraphics`, `Clone3D`, `Load3D`, `BSRangeNode::LoadBinary`. Membership covers them.
-   **Render-time, view-transient:**
    -   `Main::Draw` and first-person culling (`0x140645d30`);
    -   `ShadowSceneNode::OnVisible`;
    -   `TESWaterReflections::Update` and its caller;
    -   the sun and shadow helpers (`0x1414cb640`, `0x1414cb6d0`, `0x1414ccb30`, `0x1414cdaa0`, `0x1404155e0`);
    -   what looks like the local map's render (`0x140242e00`, `0x140243b00`, `0x140243ea0`).

    These become per-view rules.
-   **On movable slots, re-read every frame anyway:**
    -   actors: arrows, shields and weapon draw, biped and body parts, dismemberment, death and resurrection,
        `PlayerCharacter::Load3D` and `Update`;
    -   the visibility controllers (`NiVisController::Update`, `BShkVisibilityController`,
        `BGShkMatFadeController`), whose nodes carry controllers and so are movable.
-   **Left to confirm, on static references:**
    -   the portal-graph functions (`0x140e15160`, `0x140e153d0`, `0x140e15490`: clears, called from the
        render side and from cell load);
    -   queued trees (`QueuedTree::sub`);
    -   cell load and attach paths (`0x1401a1590` family, `0x14028b9d0`, `0x1402a8b00`, `0x1402b7840`);
    -   temporary effects (`0x14050e430`);
    -   the LOD multi-stream shapes (`0x140552df0`).

    The probe saw 0 hidden changes on static slots at a quiet point. The synchronous validation walk is the
    gate for these; each difference it finds names a writer to hook.

**Property and alpha-property pointers have no choke point.** Stores to the geometry's `+0x120` (alpha) and
`+0x128` (shader) are in about 240 functions, and the offsets are too common to tell geometry writes from others.
The rest:

-   Property flags mostly change through `BSShaderProperty::SetFlags` (`0x14147bee0`, 91 callers), and materials
    through `BSShaderProperty::SetMaterial` (`0x14147bff0`, 39 callers). Both are hookable.
-   In steady state the probe saw 0 pointer swaps.
-   The swaps gameplay causes (equipping, enchantment and invisibility shaders, fading) are on actors, which
    are movable.

So static slots take their property pointers at attach, and the validation walk is the alarm.

**Switch-node selection** has no named setter. `NiSwitchNode` stores its index at `+0x12C`, and harvesting
(`TESObjectREFR::SetHarvestedFlag`, `0x1401e0e00`) only sets a reference flag. Switch children stayed a per-frame class
until the culling-job elimination's phase 3 found the stores themselves: the tree manager's, the local map's and
harvesting's. They are now events ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Switch selection by event").

## Phases

1.  **Slots, same walk.** Done; see below.
2.  **The delta walk.** Done; see below.
3.  **Structural events.** Done; see below.
4.  **View masks.** The transient hides become rules. `CaptureCullHiddenBits` is folded in.
5.  **The walk leaves the frame.** Phase 2 already runs it only under `CS_DCLF_WALK_PARITY`, with
    `CS_DCLF_SCENE_DELTA=0`, and for a full evaluation (a reset, a load, a live toggle). What remains is making
    those full evaluations incremental too.

## Phase 1: object slots

`CS_DCLF_OBJECT_SLOTS` (default on; `=0` restores the dense layout for an A/B) gives every object a slot it keeps
for as long as it has a record.

-   **Slot-indexed arrays.** Every per-object array of `SceneStore::Tables` is indexed by slot and persists
    across walks. The walk writes each object at its slot (`AcquireObjectSlot`: its own, a free one, or a new
    one), and stamps it (`objectSeen`).
-   **Free slots.** After the walk, `SweepObjectSlots` frees every slot the walk did not write. A free slot holds
    `FreeObjectRecord()`: `kObjectFree` with `kObjectNoBindings` and `kObjectNoShadow`, and a null geometry.
    So every loop that already skipped unbound or non-casting objects skips it. The main build skips it
    before its cull-only candidates.
-   **Leaving the tracked set.** A `Tracked` entry releases its slot when it is erased (detach, validation, a
    rescan), so a rescan after a load reuses the slots of the entries it replaces.
-   **What stays per frame.** `bones`, `previousBones`, `extraRows`, `faceStreams`, `actorObjects`, the shadow
    texture set and keys, and `decalOrdinal` are refilled by every walk and accumulate phase. The slots' offsets
    into them are rewritten with them.
-   **One consumer assumed dense order.** Capture parity's actor check binary-searches `actorObjects`, which
    was sorted only because indices followed walk order. Its skin parity reported 2,556 ownership mismatches on
    the first slot run. The walk now sorts the list; "sorted by index" is part of the table's contract.
-   **Tracked entries.** `objectId` is the slot, and `FindObject` works unchanged.

**The gate.** `CS_DCLF_WALK_PARITY=1`: every 60 frames the walk runs on the render thread, then a dense rebuild
(the walk with `denseWalk` set: new indices in walk order, no `Tracked` entry touched), and the two are compared
object by object. The comparison covers:

-   every per-object array;
-   bone offsets and rows;
-   face streams;
-   the per-frame lists;
-   free-slot hygiene and the live count.

Everything the second walk overwrites is restored.

| Run (full featureset) | Result |
| --- | --- |
| Riverwood, live toggle, then Dragonsreach | walk parity OK in every window (12,000-31,000 objects compared per report, 0 differ, 0 missing, 0 extra); 0 holes, 0 claimed but not drawn; shadow maps match across the toggle (0.605 / 0.605 on against 0.602 / 0.604 off) |
| Riverwood, save and load, with BuildDraws and capture parity | walk parity OK, 6,221 slots and 0 free after the load; BuildDraws, decal, bone palette, draw, light data, permutation and skin parity OK; capture parity OK except in the load's window (22 material mismatches; see Phase 2) |

The slot walk was also faster. At Riverwood it built in 0.89-1.00 ms against 1.44-1.55 ms dense, and in
Dragonsreach in 0.31 ms against 0.51 ms: the per-object vectors are no longer cleared and appended every frame.
Two findings predate slots and are unchanged by them, both seen with `CS_DCLF_OBJECT_SLOTS=0` and in older logs:

-   **Withheld passes handed back** after a cell change: 15-23 a window.
-   **"Feature binding parity" at Riverwood** (`t26`, `t55`, `t81`).

## Phase 2: the delta walk

`CS_DCLF_SCENE_DELTA` (default on; it needs the object slots and AE; `=0` restores the full walk, on the worker)
replaces the per-frame walk. The scene phase evaluates, on the render thread at `Main::Draw`'s early hook, only the
entries whose inputs can have changed. Every other slot keeps its record. What it evaluates each frame:

| Source | What | Riverwood, per frame |
| --- | --- | --- |
| The per-frame set | entries whose inputs change every frame: see below | about 1,690 |
| Pending | new entries (`AddGeometry`), a changed parent reason, a verdict the accumulate phase found stale, a record `CheckObjectSlots` neutralised, and a static whose previous transform has not caught up with its current one yet | 0 in steady state |
| The refresh queue (until Phase 3, which replaced it with events) | a kept entry whose classification is `kCandidateRefreshFrames` old, which is when the full walk took it again (a FIFO by classification frame) | 110-140 |
| Fade events | the records that read a fade node whose `currentFade` changed (the node events themselves are tens a frame while the camera moves) | under 1 |
| Geometry | a kept slot whose geometry slot lost its resolve, or was re-resolved in place for another object | 0 |

Everything else is a full evaluation, the old walk on the render thread: after a reset, a load, a rescan or a live
toggle that enters the classification. That is also where the frame's worst case moved to: the frame after a
load, 20-28 ms, which the full walk spent in the join's inline rebuild at `AfterShadowMaps` instead.

**The per-frame set.** An entry is per frame when its verdict lets it have a record (or it is under a switch, or
is a face shape) and it has one of these traits (`PerFrameTraits`):

-   a face shape, or owned by an actor;
-   under a switch node;
-   skinned;
-   a controller on its shader or alpha property (animated shading);
-   a controller or a non-fixed rigid body on its chain;
-   **its reference root moves** (`RootMoves`). A record's sun entry is the bound of its reference's root node, and
    that bound moves when anything else in the reference moves. Walk parity caught it on the first run: a nail
    inside a dead salmon's reference, whose Havok body is on another branch, and the frame of an alchemy workbench,
    whose animated part is on another branch. The root's subtree is walked once, and again after an event under it
(Phase 3).

**The light path.** Most per-frame entries change in one or two known inputs, so while their classification stands
they take only what the full walk would have taken again:

-   under a switch: the selection, read from the one switch node on the chain. The rest of the chain is a
    static's, fixed like any kept record's;
-   animated shading: a hash of what the record reads from the shader and alpha properties (flags, the alpha test
    and threshold, the material and its alpha, the diffuse view). The record is written in full when it changes;
-   skinned: the engine's palette update, the palette rows and the partitions the fade node's LOD level draws;
-   moving: the placement (the transforms, the bound and the sun entry).

Face shapes, actors and anything else go through the full `WriteObject`. At Riverwood 622 per-frame entries a
frame are kept as they are and 834 only get their placement or palette.

**Kept slots.** Three things make a kept slot read as a fresh walk would have written it:

-   `RestoreAccumulated` puts back the scene half of every slot the last accumulate phase patched (flags, pipeline
    and material indices, shading, lights, tree animation, extras), from `Tables::sceneFlags`;
-   `FinishDeltaWalk` keeps their geometry slots (and a skin's partition chain) this frame's, and touches their
    buffer references every 64 frames, staggered, well inside `GpuResources`' 600-frame eviction;
-   the shadow texture and pipeline-key sets cover every record, and are rebuilt only when a record's inputs to them
    changed.

**The fade watch.** `BSFadeNode::currentFade` is written in the cull by `BSFadeNode::OnVisible` (`0x141479f50`),
directly for one LOD mode and through the fade update `FUN_14147a160` otherwise, and `FUN_1402cff60` calls that
update outside a cull. Both are detoured. Each compares the value before and after, and pushes the node onto a
lock-free stack when it moved. `ProcessEvents` drains it, and the delta walk re-evaluates the node's dependents: the
kept records whose property names it (`fadeDependents`). It is what a record's `Faded` shadow verdict reads.

**The gate.** `CS_DCLF_WALK_PARITY=1` compares the delta walk's tables with a dense full walk every 60 frames. The
comparison matches palette rows by content, and the shadow texture set as a set, since the two walks visit in
different orders.

| Run (full featureset) | Result |
| --- | --- |
| Riverwood, live toggle, the player carried 4,800 units, then Dragonsreach, shadow-map probe | walk parity OK in every window (12,000-31,000 objects compared per report, 0 differ); 0 holes, 0 claimed but not drawn, 0 slot violations; shadow maps match across the toggle (0.601 / 0.600 on, 0.598 / 0.598 off) |
| Riverwood, save and load, with BuildDraws and capture parity | walk parity OK; BuildDraws, decal, bone palette, draw, light data, permutation and skin parity OK; capture parity OK except in the load's window (13 material mismatches), which the full walk's run shows too (22 in Phase 1's) |
| Bleak Falls Barrow, then Whiterun's exterior | walk parity OK; 0 holes; the 13-14 passes handed back after each transition are all pipelines still compiling, the same class as with the full walk |

Walk parity also caught three wrong shortcuts before these runs:

-   **The sun entry of a static in a moving reference** (above).
-   **A switch child that fades.** Its record's `Faded` verdict went stale: the light path only reads the
    selection. Switch children are fade dependents now.
-   **Skins that looked still.** Pine trees are skinned, 505 of them at Riverwood, and the wind moves their bones
    with no controller or body anywhere in the reference. A "settled palette" shortcut, which kept the rows once the
    current and previous palettes matched, left 28 stale after a load. Every skin gets its palette update every
    frame, as before.

One probe sample read every shadow slice 100% clear while the player was carried under the river. The full walk
does the same at the same spot: the engine draws no sun shadows with the camera deep underwater.

**Cost.** Riverwood, full featureset, no parity (render thread, per frame):

| | Delta walk | Full walk (`CS_DCLF_SCENE_DELTA=0`) |
| --- | --- | --- |
| Scene phase | 0.52-0.56 ms | 0.32 ms |
| Of it, the engine's palette update (669 skins) | 0.19 ms | 0.19 ms (`PrepareSceneJob`) |
| Walk on the worker | none | 1.0 ms (max 2.8) |

In Bleak Falls Barrow the scene phase is 0.34-0.35 ms against 0.15-0.16 ms, and in Whiterun's exterior 0.43 ms
against 0.18 ms. So the render thread pays 0.15-0.25 ms more than it did with the full walk, and the worker saves
1.0 ms a frame. Of the delta walk's time at Riverwood:

-   the evaluations are about 0.42 ms, of which the palette update is 0.19 ms;
-   scheduling is 0.05 ms, most of it 1,700 hash lookups of the per-frame set;
-   finishing the kept slots is 0.03 ms.

What would take the rest down:

-   Phase 3 removed the refresh queue's re-classifications (see there);
-   keeping `Tracked` entries in a stable pool removes the per-frame set's lookups;
-   bones on the GPU (the GPU-driven plan) remove the palette update.

## Phase 3: structural events

A classification now stands until an event takes it again. The 64-frame expiry of the classify cache and the refresh
queue are gone from the delta walk (the full walk, `CS_DCLF_SCENE_DELTA=0`, keeps them). The events, all AE 1.6.1170,
installed by `SceneStore::InstallSceneEvents`, pushed from the writer's thread onto lock-free stacks and drained at
`ProcessEvents`:

| Event | Writer hooked | What the delta walk takes again | Riverwood, per frame |
| --- | --- | --- | --- |
| A property changed | `BSShaderProperty::SetFlags` (`0x14147bee0`) and `SetMaterial` (`0x14147bff0`), when the flags or the material pointer changed; `NiObjectNET::PrependController` (`0x140d268d0`, which `NiTimeController::SetTarget` calls) on a property | the entries listed under that shader or alpha property (`propertyDependents`), classified again. The key is never dereferenced | 0.9 events in the load's window, 0 after |
| Havok moved a node | `FUN_140ea55a0`, which writes a node's transform from its rigid body; every collision object class's `SetNodeTransformsFromWorldTransform` (vfunc `0x2B`) and the island activation listener call it | the tracked entries under the node and the dependents of every sun entry node above it, classified again, except what is placed every frame already and what has neither a record nor a verdict of the frame's | about 10 events (clutter that never sleeps), 0 evaluations |
| A controller was added to a node | `NiObjectNET::PrependController` | as for a Havok move: the entries under it gain the moving trait | 0 |
| A sun entry node's membership changed | `SceneTracker`'s attach and detach (`AddGeometry`, `EraseTracked`) | the other dependents of that node (`rootDependents`): its bound takes the new one in, or no longer does | 0 at rest |
| A fade changed | Phase 2's fade watch | now classified again as well, since a fade is also a classification input | under 1 |
| A static the engine drew while its kept verdict says hidden or unselected | the accumulate phase | classified again | 0 |

A node event holds a reference to its node, as `SceneTracker`'s attach events do, and is applied only when the node
hangs under a drawn category node (`FindCategoryNode`), so a subtree still loading is never walked.

**The per-frame set gained one rule.** Walk parity caught a guard's shield and an NPC's chopping axe left hidden after
they were shown: an actor's equipment is shown, hidden and swapped with no event of its own, and the entries were not
per frame because a hidden verdict lets them have no record. Now an actor's entries are per frame whatever their
verdict, and so is a moving entry whose verdict is hidden, unselected or actor (`PerFrameOf`). A per-frame entry
written in full re-reads its hidden, actor and switch verdict every frame, and its static verdict when what it read
(the renderer data, skin, properties, flags, material, material alpha and fade state; `ClassifyInputsOf`) changed.
That added 13 per-frame entries at Riverwood.

**Parity classifies from scratch.** The reference walk used to share the classify cache, so it could not see a stale
classification. It now classifies every entry from scratch, and the report adds two counts:

-   **stale verdicts:** kept classifications the reference no longer reaches, records or not;
-   **stale traits:** entries a fresh classification would evaluate every frame, or on a heavier path, while the delta
    walk keeps them.

With the refresh still in place, a baseline run at Riverwood had 0 of either.

**Synthetic tests** (a temporary harness, since removed) made engine calls whose effect only the events carry:

| Test | Events on | Events dropped |
| --- | --- | --- |
| `SetFlags` flips the two-sidedness of 12 kept static records | walk parity OK | 12 records differ in every window after |
| Without the body trait, so sleeping clutter becomes kept, 6 clutter references are thrown with a velocity | walk parity OK except one keyframed lever (below) | about 185 records differ in every window, from clutter that never sleeps |

-   **The keyframed branch.** Havok moves a keyframed body's node through `NiAVObject::RecalculateWorldTransform`,
    not `FUN_140ea55a0`. Such a node follows an animation, so the controller trait covers it, and the body trait stays
    on for non-fixed bodies.
-   **Fixed bodies stay fixed.** `NiAVObject::SetMotionType(kDynamic)` on a static reference's fixed body returned
    true but left it `kFixed`.

**What stays with the alarm.** Walk parity is still the only check for four kinds of change on a static slot:

-   pointer swaps (property, alpha property, skin, renderer data) outside attach;
-   hidden writes;
-   in-place changes to an alpha property;
-   in-place changes to a material.

None showed up in the runs below.

**Twelve statics in Bleak Falls Barrow never settle.** Their previous transform differs from their current one, and
the engine never updates them again. So they are written every frame (`settling 12`), as in Phase 2.

**The gate.** Full featureset, walk parity on:

| Run | Result |
| --- | --- |
| Riverwood, live toggle, the player carried 4,800 units, then Dragonsreach, shadow-map probe | walk parity OK in every window: 0 differ, 0 stale verdicts, 0 stale traits. 0 holes. Shadow maps match across the toggle (0.596 / 0.581 on, 0.594 / 0.579 off). The 36 passes handed back after entering Dragonsreach are the cold-pipeline class; Phase 2 handed back 39 in the same window |
| Riverwood, save and load, with BuildDraws and capture parity | walk parity OK. BuildDraws, bone palette, draw, light data, permutation and skin parity OK. Capture parity OK except in the load's window (15 material mismatches, as before) |
| Bleak Falls Barrow, then Whiterun's exterior | walk parity OK, 0 holes |

**Cost.** Scene phase, full featureset, no parity (render thread, per frame):

| | Phase 3 | Phase 2 |
| --- | --- | --- |
| Riverwood | 0.46-0.49 ms (1,707 evaluated) | 0.52-0.56 ms (1,834) |
| Bleak Falls Barrow | 0.27-0.28 ms (813) | 0.34-0.35 ms |
| Whiterun's exterior | 0.38 ms in the window after the load | 0.43 ms |

## Relation to the GPU-driven plan

This is the CPU side of the GPU-driven plan's Phase 3, stated precisely enough to build. It doesn't remove the
per-frame loops that depend on the engine's culls: the accumulate phase, and fades. Those go when DCLF's objects
leave the engine's walks (that plan's Phases 1 and 2). The two are independent in order, but slots make both
easier: GPU visibility, fades and view masks are all per-slot data.
