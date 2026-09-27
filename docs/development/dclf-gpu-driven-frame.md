# DCLF: a GPU-driven frame, and what it would take

Status: investigation, 2026-09-24. Phase 0 measured and the reverse-engineering questions answered; nothing built. The facts come from the Ghidra reading cited inline
(AE 1.6.1170), plus the measurements in [drawcall-limit-fix.md](./drawcall-limit-fix.md) and
[skyrim-engine-notes.md](./skyrim-engine-notes.md).

## The question

Even with stages 2 and 3, there is still per-view, per-object iteration on the render thread, only cheaper. The
target is no per-view walk on the render thread at all:

-   Everything that can be decided on the GPU is decided there.
-   What must stay on the CPU, such as reacting to gameplay events or the engine's material writes, runs on
    workers, driven by events rather than by re-reading the scene every frame.

## What still walks the scene today

Riverwood, DCLF on (about 6,200 table objects, 10,000 tracked geometries):

| Walk | Thread | Per | Cost / status |
| --- | --- | --- | --- |
| Main camera cull (`DrawWorld_BuildSceneLists`, list accumulation) | engine jobs; render thread waits at `Finish` | object | DCLF's references out ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "The primary's cull without DCLF's objects"): 1.0 -> 0.80 ms of job CPU (DCLF's stand-in included), the wait 0.69 -> 0.30 ms; DCLF's render-thread share 0.02 ms |
| Main camera registration (`GetRenderPasses` per visible object) | engine jobs during the shadow render (`FUN_1414cbff0`) | object | 0.93 -> 0.31 ms of job CPU with the same cut; DCLF builds the left-out objects' passes itself, on its worker |
| Sun: full-frustum cull | jobs, render thread waits | object | 0.04 ms |
| Sun: `Accumulate` (cascade culls, then registration or mask writes) | render thread | cascade × object | 0.10 ms with the entry exclusion: what is left is the actors and the entries with a native caster (0.59 with M1 alone, 0.69 without) |
| Point and spot lights: `Accumulate` | render thread (`CalculateActiveShadowCasterLights`) | light × object | not measured (interiors) |
| Skylighting's occlusion map (`Precipitation::SetupMask`, `RenderMask`) | render thread | object | drawn by DCLF from its tables, the engine's cull skipped: 0.02 ms left of 0.77 ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Skylighting's occlusion map, drawn by DCLF"); the precipitation mask is still native |
| Focus view, first person, water reflections, cubemaps | render thread / jobs | view × object | native, not measured |
| DCLF scene walk | worker | object | 1.2-1.6 ms (worker) |
| DCLF accumulate phase (the main registration's capture into tables) | render thread, `EarlyPrepass` | object | 0.32 ms (max 0.74) |
| DCLF `RefreshFrameConstants` (shading, extras, wetness) | render thread, Prepass | object | not separately measured |
| DCLF prologue (`GpuResources::Touch`, `UpdateSkin` for last frame's skins) | render thread | slot / skin | 0.14-0.18 ms |
| DCLF commits (lookups, skip set, uploads) | render thread | used material / object | 0.02-0.13 ms per epoch |
| `PassCapture` hook | whichever thread registers | pass | per registration |

DCLF's own per-object work has largely moved already (payloads, the scene walk, the builds). What is left on the
render thread is mostly the engine's walks, plus DCLF's two per-object loops that consume them (the accumulate
phase and `RefreshFrameConstants`). The engine side is now measured ("Phase 0: measured" below): its main-thread
walks come to about 0.7 ms a frame, comparable to DCLF's epochs with both async switches on (0.9-1.1 ms).

## The principle: DCLF's objects leave the engine's walks

Today DCLF lets the engine cull, accumulate and register every object, and then removes its passes: withholding,
and M1's mask-only registration. The shift is to take DCLF-owned geometry out of the engine's culls for **every**
view. The engine then walks only the residue, meaning what DCLF doesn't draw, and DCLF becomes the only consumer
of its own objects' visibility.

It also replaces stage 3's M2. With owned objects out of the engine's culls, their sun bits are no longer the
engine's to compute. DCLF chooses the pass's runtime bits itself, on the GPU (below), and the engine computes
masks only for the residue it still culls. A CPU replica of the engine's cascade cull is never needed.

**Where to cut.** Every cull goes through `NiAVObject::Cull` (AE `0x140d1c570`), then `BSCullingProcess::Process1`
(`0x140e28390`), then the object's `OnVisible`. For geometry, `OnVisible` goes to the process's `AppendVirtual`,
that is, the registration. Two granularities:

-   **Leaf:** skip `AppendVirtual` for owned geometry. This is cheap to decide (a side table keyed by
    geometry, or a flag), and it removes registration in every view. It doesn't remove traversal.
-   **Subtree:** return early from `Cull` at a reference root whose whole subtree DCLF owns. This removes traversal
    too, but needs per-root "fully owned" bookkeeping from DCLF's scene events. Static references are mostly fully
    owned; actors are not yet.

## What the engine's walks produce, and what replaces it

Anything an engine cull or registration writes, and something reads, must come from somewhere else once DCLF's
objects are out of the walks.

| Output | Written by | Read by | Replacement |
| --- | --- | --- | --- |
| Passes and pass descriptors | the registration (`GetRenderPasses`) | DCLF's tables (accumulate phase), native draws | DCLF's property derivation. It is exact outside `kRuntimePassBits` (`SceneStore::Stats::derivation*`); the runtime bits are next. |
| Runtime bits: ShadowDir and DefShadow (13, 14), the shadow light count (6-8) | `activeLightMask`, filled by every shadow light's accumulation | `GetRenderPasses`, which picks the technique | **GPU:** `BuildDrawsCS` tests each object against the sun's cascade volumes and the shadowed point lights, and picks the pipeline variant per draw. The pipeline set already selects by index per draw. |
| `activeLightMask` for residue objects | light accumulations | the main pass | unchanged: the engine still culls the residue |
| `BSFadeNode::currentFade`, and `+0x13C` (last visible frame) | `BSFadeNode::OnVisible`, main camera only (`cameraRelatedUpdates`), via `FUN_14147a160` / `FUN_14147b110` | DCLF's fading, the shadow caster rule; `+0x13C` only by the fade and LOD code | Call the engine's own fade functions from a worker for DCLF's visible nodes (see "Reverse-engineering answers"). |
| Leaf and tree animation | `BSLeafAnimNode::OnVisible` → `FUN_14147a430`; `BSTreeNode::OnVisible` (LOD flags at `+0x153`) | the animation and tree LOD | Call the same functions from a worker for the nodes DCLF found visible last frame (GPU visibility readback). Check their thread-safety: the engine already calls them from cull jobs. |
| `NiAVObject::kAccumulated` (flags bit 26) | `Process1`, when the process has `updateAccumulateFlag` | only the tree animation clock (`FUN_1404381e0`) | DCLF sets it on the trees it found visible, or advances their `+0x164` itself. |
| `lastAccumulatedFrameCount` | `FUN_1414b2a60` | `Actor::ModifyAnimationUpdateData`, only for actors whose root is `kNotVisible` | none for what DCLF draws; noted for M1 |
| Multibound frustum cache | `BSMultiBoundNode::OnVisible` | the same frame's cull | none |

**Not cull-driven, so safe:**

-   Face morphing is queued from `BSFaceGenNiNode::UpdateDownwardPass` (`FUN_1404330c0`), part of the update.
-   The skin palette update is keyed on the frame counter, not visibility.

## Per-object data: from per-frame reads to change events

The scene walk re-reads every tracked object each frame (1.2-1.6 ms on a worker). "The transform witness cannot
pay for itself" found that detecting a change by comparing transforms costs as much as reading them. The
alternative is to be told:

-   **World transforms.** Mark dirty where the engine writes them (`NiAVObject::UpdateWorldData` and the
    downward-pass paths; reverse-engineer the full set, including Havok and animation). That is a store per
    updated node on the update thread. A worker then uploads only the dirty objects into a persistent GPU object
    buffer. Static scenes cost nothing per frame.
-   **Bones.** For animated actors, a worker gathers the bone world transforms. The GPU computes the palettes
    against inverse binds uploaded once. That replaces the engine's `UpdateSkin` on the render thread for DCLF's
    skins.
-   **Materials.** They are already event-driven (`MaterialSources`). But the stand-in `SetupMaterial` runs on the
    render thread, because it touches the D3D11 context. Two options: a pure CPU reconstruction of
    `SetupMaterial` (and Community Shaders' hooks on it) runnable on a worker, or keeping the stand-in but only on
    write events, so its cost is O(changes).
-   **Animated shading** (controllers: emissive, alpha, UV scroll) is event-driven through the write hooks, so it
    can run on a worker.
-   **Per-frame globals computed on the GPU:**
    -   the ProjectedUV matrix, from the eye;
    -   the MTLand blend, from the global clock;
    -   tree wind, from the timers;
    -   fade, from the distance, once reverse-engineered.
-   **Gameplay state** (Advanced Skin's wetness: actor values). A worker snapshot per actor per frame, taken
    after the update.

## What stays on the render thread

-   Engine hook points, and each epoch's commit and submit (O(1); about 20 µs of upload recording today).
-   Capturing the frame's D3D11 bindings (per-frame feature textures and constants).
-   The per-pipeline technique and geometry constant evaluation (about 50 pipelines, not per object), until it
    is reimplemented without the D3D11 context.
-   Handing events off to the workers.

## What "completely" can and cannot mean

The engine still walks whatever DCLF doesn't draw. At Riverwood, the tracked-but-native classes are:

-   not a lighting shader: 3,051 geometries, mostly effect shaders;
-   under switch nodes: 532;
-   decals: 425;
-   not a `BSTriShape`: 204;
-   other techniques: 100.

The residue also includes whole systems outside the tracked cells:

-   LOD terrain and objects, and tree LOD;
-   grass, water, sky and particles;
-   first person;
-   reflections and cubemaps;
-   the focus view.

Each keeps an engine walk until DCLF takes it over. So "no per-view walk on the render thread" means the engine's
walks shrink to the residue, and the residue shrinks class by class, ordered by measured cost.

## Proposed phases

1.  **Phase 0: measure.** Main-thread time per engine walk and per wait, using the SkyrimEngineTelemetry zones,
    which already name them:
    -   `ListAccumulationJob`;
    -   `BSAccumProcess::RegisterSceneListJob`;
    -   `CalculateActiveShadowCasterLights`;
    -   `BSShadowDirectionalLight::Accumulate`;
    -   `BSCullingProcess::Process`;
    -   `BSLightingShaderProperty::GetShadowRenderPasses`.

    The mod needs installing in MO2. Also split DCLF's `RefreshFrameConstants` timing out of the tables' timing.
2.  **Phase 1: static objects leave the engine's culls.**
    -   Leaf exclusion first, then subtree exclusion, for static references without fades or leaf animation.
    -   The runtime bits (sun cascades) on the GPU, picked per draw.
    -   The accumulate phase derives descriptors instead of capturing registrations.
    -   Gate: the capture parity comparison, recast as "DCLF's derived descriptor vs the native one", on frames
        where the exclusion is off.
3.  **Phase 2: the side effects.**
    -   Fades (formula on a worker or the GPU).
    -   Leaf and tree animation (worker calls, driven by the GPU visibility readback).
    -   The `kAccumulated` and `+0x13C` readers.
    -   This brings trees, fading objects and actors into the exclusion.
4.  **Phase 3: a persistent GPU scene.** Worked out, with measured change rates, in
    [dclf-event-driven-tables.md](./dclf-event-driven-tables.md).
    -   Dirty-transform events, bone palettes on the GPU.
    -   The scene walk goes away, replaced by event ingestion on a worker.
5.  **Phase 4: material evaluation off the render thread.** A pure `SetupMaterial` reconstruction, or the
    stand-in on events only.
6.  **Phase 5: the rest.**
    -   Point and spot lights: runtime shadow-light bits on the GPU, and their views drawn by DCLF (paraboloid
        claims exist).
    -   The focus view (stage 4).
    -   Residue classes in cost order.

## Phase 0: measured

Riverwood, steady state, 45 s runs. SkyrimEngineTelemetry ran in Trace mode with a coarse zone set (per-view and
per-list zones only). The full zones file's per-object zones (`ProcessObject`, `OnVisible`) inflate everything
about 20x: `Accumulate` read 9.8 ms under it, against 0.47 ms timed directly. Both runs are vsync-bound at
16.7 ms a frame, so busy time is what counts.

**Main thread, ms per frame:**

| Main-thread work | DCLF on | DCLF off |
| --- | --- | --- |
| `Main::RenderPlayerView` (inclusive) | 9.10 | 6.04 |
| Culls called from `RenderPlayerView` (6 a frame: first person and the other small views) | 0.32 | 0.36 |
| Sun cascade culls, in `BSShadowDirectionalLight::Accumulate` | 0.28 (M1) | 0.41 |
| Water reflection culls | 0.08 | 0.08 |
| Waiting for the main camera's cull jobs (`PreparePlayerView`) | **0.38** | 0.00 |
| DCLF epochs (`CS_ORG_EPOCH_STATS`): Z-prepass, main opaque, shadow views, LLF | **1.09 + 1.16 + 0.84 + 0.09** | - |
| The same with `CS_ORG_ASYNC_EPOCHS=1` | 0.81 + 0.93 + 0.52 + 0.02 | - |
| With `CS_ORG_ASYNC_EPOCHS=1` and `CS_DCLF_ASYNC=on` | 0.12 + 0.69 + 0.09-0.25 + 0.02 | - |

**Other threads, ms of CPU per frame:**

| Work | DCLF on | DCLF off |
| --- | --- | --- |
| Main camera cull (list accumulation, `AccumulateVisible`) | about 1.0 | about 1.1 |
| Main camera registration (`RegisterSceneListJob`) | **0.81** | 0.32 |
| DCLF scene walk and builds (DCLF's own timers) | 1.2-1.6 | - |

What this says:

-   **The engine's per-view walks on the main thread are small**: about 0.7 ms a frame at Riverwood, with the sun
    already on M1. The main camera's own cull and registration run on job threads.
-   **DCLF's epochs cost 0.9-1.1 ms a frame with both async switches on.** The first two rows are not a
    regression: those runs left `CS_DCLF_ASYNC` at its default (off), so the builds ran inline in the epochs,
    as they do in the probe runs (about 1.2 ms for the main opaque epoch). "Epochs that only submit" recorded
    about 0.3 ms. The difference is now almost entirely the main-opaque epoch's join on the colour build:
    -   the colour job waits 0.59 ms. It builds in 0.79-0.87 ms but gets only 0.24 ms between its kick at
        `Prepass` and the epoch (the new worker stats: `queued 0.007 ms, window 0.241 ms`). It is not queued
        behind another job.
    -   On 2026-09-22 the window was about 0.57 ms and the build 0.61 ms. The build grew with the scene (6,210
        tracked objects at Riverwood against 5,348) and the window shrank: the native work between `Prepass` and
        the epoch is what DCLF has not claimed, and there is less of it.
    -   The colour build has to start after `RefreshFrameConstants`, which writes the late values it packs
        (technique constants, animated shading, projected-UV and land-blend extras, skin wetness). Splitting
        those out of the build, or having the GPU read them from a table uploaded at the commit, would let the
        colour build start with the Z-prepass one.
-   The async runs also showed a shadow parity regression under `CS_ORG_ASYNC_EPOCHS=1`, now fixed:
    `drawcall-limit-fix.md`, "Epochs that only submit".
-   **Two costs DCLF causes on the engine's side:**
    -   the main thread waits 0.38 ms for the main camera's cull jobs, against nothing with DCLF off (likely DCLF's
        scene walk competing with them for cores, not confirmed);
    -   the registration jobs cost 2.5x as much, because of `PassCapture`'s hook.

    Both disappear if DCLF's objects leave the engine's walks.

## Baseline, 2026-09-24: per view, after the delta walk

Riverwood, steady state, 20 s Tracy captures (about 1,210 frames), full featureset, the event-driven tables
([dclf-event-driven-tables.md](./dclf-event-driven-tables.md), Phase 3) and the sun on M1. The run is still
vsync-bound at 16.7 ms a frame. Three captures, one per SkyrimEngineTelemetry zone set:

| Zone set | Zones per frame | What it is for |
| --- | --- | --- |
| Full (992 hooks) | about 185,000 | where the per-object calls happen; its times are inflated |
| Coarse (the 103 per-object zones off) | about 6,400 | the costs below |
| Medium (coarse, plus the per-object register and the deferred-cull drain) | about 12,700 | splitting a view's cull from its registration |

The full set inflates culling about 12x: the sun's `Accumulate` reads 4.65 ms under it against 0.39 ms coarse. That
comes from the per-call zones (about 35 ns each, the hook stub plus Tracy), not from BasicTelemetry, whose capture
and sampler were off.

**Zone names versus addresses.** Several tentative names in the zones file are wrong. The categories here go by
address:

| Zone name | Address | What it is |
| --- | --- | --- |
| `BSCullingProcess::Process` | `0x1414bf320` | the per-list driver: a cull, then the registration below |
| `BSCullingProcess::AccumulateVisible` | `0x140e28f70` | `NiCullingProcess::Process`, the traversal: culling |
| `BSCullingProcess::ProcessJob` | `0x140e28af0` | drains deferred cull work (`FUN_140e28d20`, about 1 us), then registers every visible object: accumulation |
| `BSShaderAccumulator::BuildJob` | `0x1414b2140` | one object's registration into an accumulator |
| `BSShadowDirectionalLight::UpdateShadowMap` | `0x141511f30` | the sun's full-frustum cull |
| `BSAccumProcess::RegisterSceneList` / `...Job` | `0x1414bf730` / `0x1414bf7a0` | the full-frustum cull per scene list / the primary's registration job per scene list (queued by `Main::PreparePlayerView`, `0x1414cbff0`) |
| `ListAccumulationJob`, `FirstListAccumulationJob` | `0x1414cc3f0`, `0x1414cc260` | the primary's cull, one job per scene list. `NiCamera::CalculateAndDrawShadowCasterLights` queues them with the main camera, runs the sun while they run, and waits in `JobList::Finish` |

**Main thread, ms per frame** (coarse; medium in brackets where it differs):

| View | Cull | Registration | Waiting on jobs | Drawing | Other | Total |
| --- | --- | --- | --- | --- | --- | --- |
| Primary | - | - | 0.44 (0.52) | 2.36 | 0.29 | 3.09 |
| Sun shadow (cascade culls in `Accumulate`, full-frustum cull, render) | 0.27 | 0.12 | 0.03 | 0.03 | 0.10 | 0.55 |
| Reflection: water cube map, 2 faces a frame | 0.06 | 0.03 | - | 0.35 | 0.08 | 0.52 |
| Precipitation mask (`Precipitation::SetupMask`, 6 lists) | 0.15 | 0.22 | - | - | - | 0.37 |
| First person and small views | - | 0.02 | - | - | - | 0.02 |
| Shadow masks, local map, water wrapper | - | - | - | 0.01 | 0.06 | 0.07 |

The rest of `RenderPlayerView` (DCLF's epochs, image space, the other passes) is 2.5 ms. Per frame, the sun's
`Accumulate` is 0.37 ms at the median and 0.54 at p95; the cube-map reflection is 0.50 and 0.70.

**Job threads, ms of CPU per frame, summed:**

| View | Cull | Registration |
| --- | --- | --- |
| Primary | 1.10 (1.04 in the list accumulation jobs, 0.06 in `DrawWorld_BuildSceneLists`) | 0.89 (4,743 objects a frame) |
| Sun shadow (full-frustum cull jobs) | 0.09 | - |

What this says:

-   **Culling and registration on the main thread total about 0.9 ms a frame.** That is 0.39 for the sun, 0.37 for
    the precipitation mask, 0.09 for the reflection and 0.02 for the small views. The sun's cull is still the largest
    single piece, and the precipitation mask is almost as large, with registration as its bigger half.
-   **The primary costs the main thread only its wait**: 0.44 ms, for the registration jobs. Its 2 ms of cull and
    registration run on job threads, the cull overlapping the sun.
-   **Reflections are mostly drawing** (0.35 of 0.52 ms): the engine redraws the cube map's faces through its
    own batch renderer.
-   **Compared with Phase 0,** the primary's wait is 0.44 ms against 0.38, and its registration 0.89 ms against 0.81.
    The sun's cascade culls are 0.27 ms against 0.28.

## Reverse-engineering answers

All from AE 1.6.1170. Reader scans were run over the decrypted `.text`: Ghidra's memory, dumped and disassembled
function by function from `.pdata`.

**`NiAVObject::kAccumulated` (flags bit 26).**

-   **Writers:** `BSCullingProcess::Process1` and its siblings (`0x140d3ff3a`, `0x140d40081`, `0x140e2848d`,
    `0x140e28595`, the parabolic process at `0x141519b85`/`c15`/`fe0`), and `DrawWorld_BuildSceneLists`.
-   **One reader: `FUN_1404381e0`**, the tree animation update (`BSFadeNodeCuller`, `BSTreeManager`), called from
    `FUN_140437d40`. For each node with the bit set, it advances the animation time at `+0x164` and derives the
    wind term at `+0x15C`. It is time-budgeted and runs under a mutex.
-   **Consequence:** a tree left out of the main cull stops animating, unless DCLF sets the bit for the trees it
    found visible (or advances `+0x164` itself). Nothing gameplay-side reads it.

**`BSFadeNode+0x13C` (the frame the node was last visible).**

-   **Writers:** `BSFadeNode::OnVisible`, `FUN_14147a160` and `FUN_14147aa20`.
-   **Readers:** only `BSFadeNode::OnVisible`, `FUN_14147a160` and `FUN_14147a430`, all fade and LOD code,
    comparing it with the fade frame counter `DAT_142032e50`. That counter's other references are
    `Main::Update` (the increment) and a settings snapshot (`0x141515b02`).
-   **Consequence:** it matters only to the fade and LOD state machines themselves.

**`BSShaderProperty::lastAccumulatedFrameCount` (`+0x80`).**

-   **Writer:** the shadow registration (`FUN_1414b2a60`, `gFrameCounter` at `0x14328cc6c`). M1 skips this write
    for claimed geometry.
-   **Reader: `Actor::ModifyAnimationUpdateData`** (`0x1406a3750`). An actor's animation update counts as
    "visible" when its 3D root lacks `kNotVisible` (flags bit 20; a race flag bypasses the check). Only when the
    root *is* `kNotVisible` does it fall back to "was `MiddleHighProcessData::lightingProperty` (the skin)
    accumulated within a threshold of frames".
-   **Consequence:** it matters only for actors whose root is `kNotVisible`, which DCLF doesn't draw. It is
    recorded because it links the render registration to animation LOD: a hidden actor keeps full-rate animation
    while its skin is still being registered (shadows included).

**The fade** (`BSFadeNode::OnVisible` → `FUN_14147a160` → `FUN_14147b110`). It runs only for a process with
`cameraRelatedUpdates`, meaning the main camera. It is a per-node state machine:

-   `FUN_14147b110` takes the distance from the camera to the world bound's centre (`+0xE4`), scaled by the
    camera's `lodAdjust` over a per-type multiplier (the table `DAT_142032e00`, indexed by the low nibble of
    `+0x153`). It gives a target fade between the near and far fade distances (`+0x128`, `+0x12C`, times
    `DAT_142032e45`).
-   `FUN_14147a160` steps `currentFade` (`+0x130`) toward that target by `DAT_142033084` (the frame delta) over a
    fade time, capped by `DAT_142032e4c`. It snaps to fully faded in when the node was not visible within the
    last 2 frames (`+0x13C`).
-   It also keeps the "fully faded" flag (`+0xF4` bit 14) and `+0x140`, and calls `FUN_14147a430` for
    leaf-animated nodes.

**`FUN_14147a430`** is not wind. It is the tree and leaf LOD transition state machine: the LOD level in
`+0x152`, transition state in `+0x153`, and blend in `+0x14C`, from the same distance.

Both functions read only the node and globals, and write only the node. The engine already runs them on cull
job threads. So a DCLF worker can call them for its own nodes (excluded from the engine's cull), fed by DCLF's
visibility. The 2-frame visibility window means a one-frame-late GPU readback still gives the engine's
behaviour.

**What writes world transforms** (for dirty events): `NiAVObject::UpdateWorldData` and its overrides
(`NiBillboardNode`, `BGSDecalNode`), `NiAVObject::RecalculateWorldTransform`, `UpdateManagedNodesJob` (a job),
and the Havok sync (`bhkBlendCollisionObject`, `bhkRigidBodyT`). Several run on job threads, so dirty marking
must be lock-free and thread-safe. The scan for stores to `+0xA0..0xA8` is noisy (the offsets are common), so the
list is a starting point. The gate is a probe comparing every tracked object's transform change against the dirty
set.

**The shadow bits in the Lighting shader:**

-   **The sun.** `DefShadow` and `ShadowDir` are tested at runtime from the permutation constant buffer
    (`Lighting.hlsl`, around line 1674). Whether an object samples the screen-space shadow mask can be a
    per-object value chosen on the GPU (DCLF's cascade test for parity, or "every in-world object" as the
    improvement), with no pipeline variant and no engine mask.
-   **Point lights.** The shadow-light count (descriptor bits 6-8) feeds only the vanilla path (`numShadowLights`
    in `NumLightNumShadowLight`). With Light Limit Fix, a clustered light is shadowed on an object when the
    object's `ShadowBitMask` has the light's bit (`LightLimitFix.hlsli`, `IsLightIgnored`). DCLF takes that mask
    from the engine's per-object light assignment during registration (`LightLimitFix::GetShadowBitMask`). On
    the GPU it becomes a bound-versus-light-sphere test per object. Matching the engine exactly needs its
    selection rule reverse-engineered (`FUN_1414fcf80`: priorities and caps).

**Residue side effects of exclusion.** Per-object light assignment happens inside each object's own registration
(`GetRenderPasses`), so excluding other objects does not change it. Rooms, portals and multibounds are nodes, so
**leaf** exclusion keeps them traversed. **Subtree** exclusion must stop at reference roots below them and never
skip a room, portal or multibound node, because the room state feeds Light Limit Fix's `RoomIndex`.

**The main open item** is the engine's point-light selection rule (`FUN_1414fcf80`), needed only for parity mode.

## Phase 1, step 1: the primary's scene lists, measured

The primary's cull has one input per list job: the scene lists (`DAT_14338c870`, `DAT_14338c868` of them, 6),
`BSTArray<NiPointer<NiAVObject>>` that `DrawWorld_BuildSceneLists` fills round robin with reference roots.
`CalculateAndDrawShadowCasterLights` (`0x1414cbb90`) passes them to the sun's full-frustum cull (`FUN_141511f30`,
whose `objectArray` entries the sun's exclusion already filters), then queues one `ListAccumulationJob` per list and
waits in `JobList::Finish` (`0x1414cbf4d`). Nothing else reads the lists between those two points, so an entry removed
there and put back after `Finish` leaves only the primary: its traversal, `OnVisible`, the main registration (mode 0,
`*0x14338c830`) and the depth-prepass registration (mode 0xC, `*0x14338c828`). Entry 0 of each list stays, because
`Process2` sets the process's frustum up from it.

**The census** (`CS_DCLF_PRIMARY_EXCLUDE=probe`, `PrimaryCull`; Riverwood, per frame):

| | |
| --- | --- |
| List entries | 4,140 in 6 lists (plus 6 in the first job's extra list) |
| ... that are sun entry candidates (`SunCandidates`) | 4,016 (97 %); 478 candidates are not list entries (nested in multibounds) |
| Main registrations | 2,360, of which 1,860 (79 %) under a listed candidate |
| Depth-prepass registrations | the same 2,360 and 1,860 |
| Not candidates | effect-shader FX (waterfalls, mist, snow), animated objects, `ObjectLODRoot`'s children |

**Actors** are not list entries of their own: the player's third-person root hangs under a `BSMultiBoundNode` under
`ObjectLODRoot`'s second child, which is one list entry holding every actor. Taking actors out needs the per-object cut
(`Process1`, filtered on the list processes), not the entry filter.

**What the objects under the candidates take from their registration** (the same probe, in the accumulate phase:
1,693 registered objects a frame):

-   None is ineligible or left underived, none has a shadowed point light (bits 6-8, Light Limit Fix's mask), none is
    fading or screen-door, and no skinned LOD row differs.
-   The derived pass descriptor differed outside the sun's bits on 171: the derivation left out `kSkinned` (bit 1,
    which `GetRenderPasses` copies from the flag whether or not there is a skin: static fish, buckets) and
    `kProjectedUV` (bit 15, which it sets from the flag alone; the snow conditions only add bits 19 and 21). Both are
    derived now; 1 object a frame is left (a hint-3 decal's DoAlphaTest).
-   **The sun's bits (13, 14) differ on 1,300**, as expected: the derivation never sets them. In `GetRenderPasses`
    ShadowDir is the light selection's output (`FUN_1414fcf80`, from the sun's mask bits), and DefShadow is the
    accumulator's deferred flag (`+0x178`) under alpha and fade conditions, cleared when there is neither ShadowDir nor a
    shadow light, and both are cleared for a property with no shadow passes (`shadowMapOrMaskPasses`) unless flags
    `0x800c000100` say otherwise. Everything but the mask is per property, so the per-frame input is only whether the
    bound meets a cascade.
-   The specular LOD fade differs on 20 (open), and 179 are decals (hints 2 and 3), whose depth the engine's depth
    registration still draws.

## Relation to stage 3

The sun is the first view DCLF's objects have left ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "The sun's
cascades without DCLF's objects"): the entries whose content DCLF draws entirely are removed from the cascade culls'
input, and DCLF writes their sun bits at the main registration, from the engine's own cascade planes. That is a CPU
test at a point the engine already visits, not the replica M2 would have been. It lasts until the main pass stops
registering DCLF's objects (Phase 1), when the bits move into DCLF's derived descriptors. M1 remains for the entries
that stay in the culls.
