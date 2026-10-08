# DCLF architecture

How Drawcall Limit Fix (DCLF) is put together today: the order of a frame, the data from engine events to GPU draws,
how it takes objects away from the engine, where each piece lives, and the parity checks that say it is right. It
describes the present only.

-   [dclf-status.md](./dclf-status.md) is the coverage, and what the engine still does.
-   [dclf-open-defects.md](./dclf-open-defects.md) lists the defects and failing checks under investigation.
-   [drawcall-limit-fix.md](./drawcall-limit-fix.md) is the historical record of how DCLF got here, measurements
    included.
-   The engine behaviour everything below depends on is in [skyrim-engine-notes.md](./skyrim-engine-notes.md).

DCLF runs on Skyrim AE 1.6.1170 only. Every engine address in the code is that runtime's.

## What it does

The game draws each object with its own draw calls, after culling it on the CPU. DCLF keeps a persistent table of
every object it can draw, and draws those objects itself with indirect draws, culled on the GPU. The draws run on DXVK's
Vulkan device through the render graph ([render-graph.md](./render-graph.md)). The engine is told not to draw them, so
each object is drawn once.

This covers the main pass (the Z-prepass and the deferred colour pass, decals included), the shadow views and
Skylighting's occlusion map.

## A frame, by thread

The render thread captures what only it can read and submits the epochs; it builds nothing. The scene is kept on DCLF's
coordinator (`SceneScheduler`, a serialized lane) and its pool, and every frame draws the newest complete publication of
it ([dclf-async-publication.md](./dclf-async-publication.md)).

| Point in the frame | Render thread | Coordinator and pool | GPU |
| --- | --- | --- | --- |
| `Main::Draw`, before the main cull (`BeginSceneFrame`) | Joins the last scene work, applies the toggles, hands the tables over, selects the newest scene revision (`SelectRevision`) and the newest publication it covers (`SelectPublication`), then installs its claims, list filter and sun exclusion, or withdraws the set (below). Kicks the frame's values (`FrameValues::Kick`: placements, palettes, shading, and the payload ring entry the epochs read) and the scene work | **Scene work**: the frame's events applied, the walk, the set's commit; then the publication, its payloads built ahead on the pool (`BuildAhead`), and the revision's shapes (`MakeRevisionShapes`) | The frame values' uploads, on the copy queue |
| The engine's cull jobs | The engine registers the passes it keeps. `PassCapture` withholds DCLF's objects, `PrimaryCull` stands in for DCLF's references inside the list jobs, and the sun's cascades skip DCLF's entries | | |
| `BeforeShadowMaps` | The **reflection epoch**; the frame's shadow views (`ShadowViews`), their coverage by the selected revision (`DecideShadowCoverage`) | | The water reflection's faces |
| Each shadow view's `FinishAccumulating` | `CaptureShadowView`: where the engine drew, and with which constants | | The engine's own shadow draws |
| `AfterShadowMaps` | The **shadow epoch**: the installed shadow payload committed into the revision's shape | | Every placement's casters, culled per view |
| Skylighting's `RenderOcclusion` | The **occlusion epoch** | | The occlusion maps |
| `EarlyPrepass` | Kicks the **accumulate work**; requests the shadow pipelines for the keys in use | **Accumulate work**: what the main camera's registrations decided | |
| `Main_RenderDepth`, world drawn | `CaptureDepthPass`: the **Z-prepass epoch** | | DCLF's depth, the HZB, two-phase occlusion culling |
| `Prepass` | Latches the accumulator and refreshes the camera-dependent constants | | |
| The main pass's opaque batches (`BeforeOpaquePass`, `AfterOpaquePass`) | `CaptureMainPass`, then the **colour epoch** | | The drawn set into the G-buffer, depth-tested EQUAL, then decals |
| `Present` (`Reset`) | Joins the scene work, closes the engine-read window, applies the events no scene work took, releases what it let go of, writes the report, handles the menu toggle | | |

Each epoch is submitted from the selected scene revision's recording (ORG's revision-driven epochs): the commit writes the
frame's values into the revision's shape (its latch, bucket plan and latched copies) and reads its payload from the
frame's ring entry. Nothing is built, prepared or checked in the frame.

A frame's set is **withdrawn** (the engine draws everything, and the next covered frame installs the whole set again) only
when no publication applies: startup until the first one, a toggle until a publication committed under the new toggles
is installed, a load, or `failed`. Every other frame draws the
installed publication's claims.

## The data

```
engine writers ──events──▶ SceneStore tables ──change log──▶ kept stores ──▶ payload ──commit──▶ epoch (GPU)
 (hooks)                    (Scene/, coordinator)            (Draws/)        (pool)    (render thread)
```

1.  **Events.** Hooks on the engine's writers push events from whatever thread makes the change:
    -   a subtree attached or detached (`SceneTracker`);
    -   a fade stepped, or a property's flags or material set;
    -   a controller added, or Havok moving a node;
    -   a switch node's index changed;
    -   a material's value changed (`MaterialSources`).

    The render thread drains them into a batch (`EventQueue`), and the scene work applies it. Nothing about an object
    is re-read on a schedule: a change the events miss is a defect, and the walk and mirror parity checks find it.
2.  **Tables.** `SceneStore` holds a record per object, and shared tables of geometries, pipelines and materials
    (`SlotTable`: slots with generations and reference counts). Every change a write makes to the tables is appended
    to the change log (`Tables::changeLog`), with its causes.
3.  **Lookups.** What a build needs from the render thread's services is resolved ahead of the build (`Lookups`):
    -   the pipeline set indices and shader constant tables, at `EarlyPrepass`;
    -   the texture and sampler descriptor indices, in the epoch's preparation.

    A build never calls the engine, D3D11 or ORG.
4.  **Kept stores.** Each epoch kind keeps its GPU-side state across frames, and rewrites only what the change log
    names:
    -   the object records (`ObjectRecordStore`), bone rows (`BonesStore`) and geometry table (`GeometryStore`);
    -   the main pass's material and pipeline rows (`MainRows`), shared by the Z-prepass and colour builds;
    -   the resident objects' draw inputs (`ResidentRegion`);
    -   the drawn set (`DrawnMarks`);
    -   the shadow inputs per render mode and the shadow material rows (`ShadowKept`).
5.  **Payload.** A build is a pure function from the tables and lookups to a payload: the rows it wrote, the draw
    inputs and the kept stores' changes. `BuildMainPayload` (`Draws/IndirectDraws/MainBuild.cpp`)
    and `BuildShadowPayload` are the two builders. They run on the pool with each publication (`BuildAhead`); the
    frame's producer copies what the payloads changed into the frame's payload ring entry.
6.  **Commit and epoch.** On the render thread, the commit adds what only it has: the frame's constant buffers
    (`ConstantMirror`), the per-frame textures and the latch, written into the selected revision's shape. Then the
    revision's recording of the epoch runs the GPU work:
    -   `BuildDrawsCS` culls the inputs (frustum, and in the Z-prepass two-phase HZB occlusion) and writes the
        indirect draw sequences;
    -   the draw passes execute them;
    -   the colour epoch draws exactly the set the Z-prepass decided (the per-object visibility words).

## Growable tables

The GPU tables DCLF keeps are moving from compile-time capacities (`GpuLayouts.h`), whose overflow skipped or waited,
to tables that grow, the way BasicRenderer's are (a slot allocator with a free list, a capacity that doubles, the old
buffer released after the GPU is done with it). Each table holds one copy of its data, indexed by what the data depends
on, and every row is a constant-buffer block (256-byte aligned), so a table can be read as an array of constant buffers.

**Shadow views (done).** A draw's binding is split three ways (`DrawPipelines.h`, `kShadowPushWords`):
-   **per draw:** the draw's words name its material row (`ShadowMaterialRow`: the Utility vertex shader's
    `PerMaterial` block, `b1`, which the pushed address names directly, and the diffuse's index for `t0`);
-   **per view:** pushed once per view: the view's `PerTechnique` (`b0`) and `VS_PerFrame` (`b12`) blocks, the zero
    block, `SharedData` (`b5`), `FeatureData` (`b6`), and the frame record (`DrawBindings`: every other texture and
    sampler, and the object and bone tables);
-   **per material:** the rows, one table for every view (`ShadowResources::materialRows`, a `GrowableRows`).

The table grows on the render thread before the shadow build is taken, to what the last build wanted plus a quarter
(`IndirectDraws::Impl::ReserveShadowRows`), through `Buffer::ResizeBytes`: the graph resource stays the same, and the
old backing goes through ORG's deletion queue. Every resize runs inside `MutateBackings` (`GrowableRows.h`), which keeps
it apart from ORG's host thread while that thread prepares tickets (ORG's `persistent-epochs.md`, "Backing changes").
A new backing holds nothing, so every row is sent again. A material
past the capacity waits one frame and its casters stay the engine's meanwhile. `CS_DCLF_TABLE_START=small` starts the
table at 4 rows, to exercise the growth.

**Draw outputs (done).** No sequence buffer has a fixed draw capacity: each is grown before its epoch to hold every draw
the scene's tracked objects can produce (`SceneDrawBound`), through `Buffer::ResizeStructured`
(`Impl::ReserveMainSequences`, `ReserveShadowSequences`). The main buffer's ranges (phase 1 and colour, phase 2, one per
decal group) travel to `BuildDrawsCS` and `SortSequencesCS` in their constants (`BuildDrawsConstants::phaseTwoBase`,
`decalBase`, `decalStride`). Each indirect draw's max count is its epoch's own draws, grown as a power of two so the
recording settles. A bound past the device's max sequence count, or the sort's 2^20 ranks, is a hard failure.

**Main pass (done).** The same split, with a pipeline row as well (`DrawPipelines.h`, `kDrawPush*`):
-   **per draw:** six words of push data: the pipeline row's address (words 0-1), the material row's (2-3) and the object
    (4). `BuildDrawsCS` computes both addresses from the input's `y` (`RowsOf`: the pipeline slot in the high 12 bits, the
    material slot in the low 20) and the tables' bases in its constants;
-   **per material slot** (`MaterialRow`, 1024 bytes): the vertex and pixel `PerMaterial` blocks (`b1`), then a header
    with t0-t15, s0-s15 and the feature textures (t71, t74). A material slot is keyed by (material, pass descriptor), and
    the pack tables depend only on the technique, so the row is packed once for every pipeline and both segments;
-   **per pipeline slot** (`PipelineRow`, 2048 bytes): the technique blocks (`b0`), the geometry template (`b2`), the
    permutation (`b4`), and the shadow mask (t14, s14);
-   **per pass:** the frame push: the frame slots' addresses and the frame record (`DrawBindings`: the frame textures
    from t16, and the object and bone tables).

The rows are kept across frames (`MainRows`, a `KeptArray` each): a row is written again only when its key changes, and
the tables are sent what changed since the version they hold. Each row's header holds its blocks' offsets until the
upload adds the row's own address (`EmitMainRows`), so a table that grows is simply sent again. Both tables
(`Resources::materialRows`, `pipelineRows`) are `GrowableRows`, grown before each epoch to the scene's slot counts plus a
quarter (`Impl::ReserveMainSequences`). Whether a (material, pipeline) pair can draw - a texture, sampler or constant
block the pipeline reads that the row lacks - is checked per build, from what each row holds (`MainBuild::AssembleRecord`).

The DGC push-data token of a `Constant` argument is at most 16 bytes on NVIDIA; BasicRHI splits larger ones, so the
six-word draw push is two tokens (BasicRHI `README.md`, and "Resolved: device loss in every DCLF draw pass" in
`dclf-open-defects.md`).

**Scene tables (done).** One set of object records (`t127`), bone rows (`t126`), geometry rows and face positions serves
every epoch - the shadow views', Skylighting's, the Z-prepass's and the colour segment's (`SceneBuffers`, shared by
`Resources` and `ShadowResources`). One store keeps each (`Impl::objectStore`, `boneStore`, `geometryStore`). Their builds
run in frame order - the shadow build (kicked at `BeforeShadowMaps`, joined at `AfterShadowMaps`), then the Z-prepass's
(kicked at `EarlyPrepass`), then the colour build's - and each kick drops a job still outstanding, so no two builds write a
store at once. Each commit sends what changed since the version the buffers hold (`SceneBuffers::held`), which the commit
before it wrote. Object slots are fixed from the scene phase on, so Skylighting's epoch, which draws the shadow build's
inputs after the Z-prepass commit, reads the same objects' records.

Every scene table grows (`Impl::ReserveSceneTables`, on the render thread before any build's inputs are taken): to the
objects, the geometry slots plus one row per face stream, every palette current and previous plus the extras, and the
highest face region, doubling. A growth gives the buffer a new SRV slot or address (the old ones are retired once the GPU
is done with them), resets its held version so the next commit sends it whole, and counts in `SceneBuffers::generation`,
so a batch staged before it is not submitted after it. The per-object buffers - the visibility and frustum words, the
draw inputs of both segments and every shadow mode, the feedback slots - follow the object capacity
(`ReserveObjectBuffers`): a segment or a mode has at most one input per object, so no input share or loop reserve is
needed. A build past a reserved capacity is a hard failure at its commit (`CheckSceneCapacity`), never data dropped.

**Shadow view and key slots (done).** Skylighting's map draws through view slot 0 and the frame's shadow views through the
slots after it, as many as the frame has (`Impl::ReserveShadowViews`, before the epoch). A slot is a sequence buffer, a
count buffer and a row of the view blocks (`ShadowResources::viewBlocks`: the view's `b0` and `b12`, out of the constants
arena, so the worker's staged arena does not depend on the view count). More slots are more buffers the passes declare,
so the shadow extension is added again and the graph is built with them; the kick hands the worker the count buffers it
zeroes, because slots are added while it runs. The pipeline map rows hold every key slot the epoch can name: the lookups'
keys plus every key a used mode may add when the epoch body refreshes them. Either growth is a new latch block
(`ShadowLatchLayout`), which the passes take from the published frame (`ShadowFrame::latch`), never from the resources,
since async epochs prepare them on the host thread. A view is never left native for want of a slot.

**The sun's plane sets and the view rasterizer states (done, 2026-09-29).** Nothing in the shadow path is capped any more:
- **The sun's full-frustum processes** (the engine culls the scene once per `fullFrustumCullingProcessArray` entry before
  the cascades, and a cascade only walks entries some process kept; 6 at the exterior save). They were copied into every
  sun view's latch, at most 8; past 8 the entry rule went to the CPU and the kept shadow state was bypassed. Now they are
  written once per frame slot into a region of the shadow latch block (`ShadowLatchLayout::SunEntryOffset`, a count and
  112-byte `SunEntryProcess` records), which every sun view's latch names (`BuildDrawsLatch::sunEntryOffset`). The input
  always carries its entry sphere, and the kept state is always used.
- **The sun's cascades** for the colour pass's sun test. They were at most 4 (`SunAccumulation::kMaxCascades`, the
  latch's `sunMasks/sunPlanes[4]`), and a fifth was silently dropped. The engine has no bound: the count is `iNumSplits`,
  the shadow map array's slices (Ghidra: `ShadowSceneNode` constructor to `BSShadowDirectionalLight::SetShadowMapCount`,
  a `BSTArray` resize). Now `SunAccumulation` keeps them in a vector, grown at the sun's `Accumulate` while `bitsReady` is
  clear, and the colour epoch writes them into a region after the main latch (`MainLatchLayout`, 208-byte
  `SunAccumulation::GpuCascade` records), growing the block when it needs to (`ReserveMainLatch`; a `PassFrame` keeps
  its own block).
- **View rasterizer states.** A state id was 4 bits of the shadow key and a bit of 16-bit masks: the 16th distinct state
  left its view native. Now the id is a field of `ShadowPipelineKey` (`viewState`, also in the pipeline recipe id), a
  mode's states are sorted lists (`ModeRasterStates`: casters and volumetric-only), the lookups' map rows are a vector,
  and the latch's map rows are a grown dimension (`ShadowLatchLayout::rasterStates`).

`BuildDrawsLatch` shrank from 2,048 to 256 bytes. `Impl::ReserveShadowLatch` grows view slots, key slots, state rows and
processes before the epoch (the sky epoch reserves too, for a state first seen by its capture), and
`CS_DCLF_TABLE_START=small` starts states and processes at 1 and the main latch at 1 cascade.

Still native, by design rather than capacity: focus shadows (an actor's own shadow; DCLF has no caster set for them)
and views whose rasterizer state no pipeline can express (wireframe, no depth clip).

**Object rows (measured, 2026-09-29).** An object's row (`BindlessObject`, `DCLFObjectRecord`) is 256 bytes, padded from
208 so the table stays viewable as an array of constant-buffer blocks. The shaders read it as the structured buffer at
`t127`, indexed by the draw's object word. Reading it as a constant buffer instead (a per-draw push address, `b189`,
written by `BuildDrawsCS`; 8 push words and a 100-byte `DrawSequence`) was built and A/B'd on the RTX 3090 Ti, with the
same rows and the camera turning, two 60 s runs each: colour 1.03 against 1.02 ms, shadow views 1.98 against 1.94 ms,
Z-prepass depth 0.286 against 0.277 ms. The constant buffer was no faster on any pass, so it was not kept.

## Ownership: how the engine stops drawing DCLF's objects

An object is withheld from the engine only while the installed publication claims it, and the publication's payloads
draw every claim. The set of phases DCLF claims for is a capability (the toggles and what DCLF has set up), not the views
a frame happens to have.

-   **Main pass.** `PassCapture` hooks `BSBatchRenderer::RegisterPass`. A claimed geometry's pass is recorded (the
    accumulate phase reads it) but never reaches the batch renderer, so the native loop has nothing to draw. An object
    joins the set only once everything it is drawn with is ready (`PhaseReady`); one that is not waits, the engine's.
-   **Shadow views.** The same, per shadow render mode, from the shadow build's claim sets. This includes the direct
    group insertions the registration makes without `RegisterPass`.
-   **The sun's registrations.** `SunAccumulation` skips the registration of a claimed caster in the sun's
    `Accumulate`, reproducing its mask write, so no pass is built only to be withheld.
-   **The sun's culls.** The sun entry exclusion removes entries whose every object is DCLF's from the cascade culls'
    object arrays. The GPU applies the same entry rule to DCLF's inputs (`kCullSunEntry`).
-   **The main camera's cull.** `PrimaryCull` stands in for DCLF's references inside the engine's list jobs. It keeps
    the per-object state the cull maintains (fades, LOD, the tree clock's bit), so the engine never traverses those
    references. The cut it stands in with is kept per candidate entry, planned again only for the entries that moved.

The menu's toggles (`Common/Toggles`) change all of this live: a toggle withdraws the set until a publication committed
under the new toggles is installed. Switching DCLF off clears every claim and bypasses the capture.

## Where the code is

`src/Features/DrawcallLimitFix.{h,cpp}` is the feature: its hooks, the frame's order above and the menu. Everything
else is under `src/Features/DrawcallLimitFix/`:

| Folder | What is in it |
| --- | --- |
| `Common/` | The switch registry (`Switches`) and the live toggles (`Toggles`), the coordinator and pool (`SceneScheduler`; `AsyncWorker`'s scene lane until it is folded in), the event queue, the render-thread budget, and the building blocks of the kept state: `KeptState`, `SlotTable` |
| `Engine/` | Everything that hooks or reads the engine directly: `PassCapture`, `PrimaryCull`, `SunAccumulation`, `ShadowViews`, `SceneTracker`, `FaceSnapshots`, `ConstantMirror`, `EngineStates`, and `EngineAccess.h` (the shared raw-access helpers) |
| `Scene/` | `SceneStore` (interface `SceneStore.h`, implementation `SceneStore/`), the record layouts (`Records.h`), the lookups, and what a record is derived from: `MaterialSources`, `ConstantEvaluator`, `LightingConstants`, `LightingDescriptors`, `VertexInput` |
| `Draws/` | `IndirectDraws` (interface `IndirectDraws.h`, statistics `IndirectDrawStats.h`, implementation `IndirectDraws/`), the pipelines (`DrawPipelines`), the SPIR-V programs (`ShaderPrograms`, `SpirvReflection`), and the game's buffers and textures as the graph sees them (`GpuResources`, `GpuTextures`) |
| `Published/` | The scene graph the producers run on (`SceneGraph`, `PublishedSceneExecutor`), and the capture foundation ([dclf-async-publication.md](./dclf-async-publication.md)) |
| `Diagnostics/` | Capture parity, the periodic report (`Report.cpp`), the open defects' probes and the test harness |

Inside `SceneStore/` and `IndirectDraws/`, `Internal.h` is shared by that folder's files only:

-   `SceneStore/` is split by the tables' life cycle: tables, events, classification, the walk, the accumulate phase,
    the frame constants, residents, sun candidates, slots and parity.
-   `IndirectDraws/` is split by the path a frame takes: passes, resources, kept stores, the main and shadow builds,
    material lookups, the main and shadow epochs, uploads and diagnostics. The layouts the shaders read are in
    `GpuLayouts.h`, and must match `BuildDrawsCS.hlsl` and `Lighting.hlsl`.

Includes within a folder use bare names. Includes across folders are rooted at `src/`
(`Features/DrawcallLimitFix/Scene/SceneStore.h`).

## Two modes: trusted, and parity

DCLF runs in one of two configurations: the main path (asynchronous, persistent, every check off) and parity (the same path,
with the parity switches observing it). A parity check never changes what is drawn, so a parity run draws exactly what a
normal run draws.

**The invariants.** Each has a flag a run must not show after its first scene.

1.  **Capability, not occurrence.** The phases DCLF draws are the toggles plus what it has set up (the shadow state
    catalog is enumerated at setup, so no state appears mid-session). Which views the engine asks for in a frame never
    changes the set. A change outside a toggle, a load or `failed` is `<- PHASES`.
2.  **Every view served.** Every view of a claimed phase is drawn from the persistent payload: the shadow revision's
    shape covers every view slot, a slot without a view doing no work. A view not drawn while casters are withheld
    from it is `<- HOLES`. Focus views (one actor's casters) are the engine's by design.
3.  **Incremental only.** Every per-frame update is bounded by what changed: the candidate tables keep stable indices and
    per-entry versions, and their consumers (the cut, the exclusions, the light filter) follow by version difference;
    the set's commit evaluates only the slots an event or a readiness source named; the shapes are made again only when
    what they are made from moves (`RevisionShapesKey`). Each has a parity counterpart that evaluates in full.
4.  **No overflow fallbacks.** Event queues and logs grow; there are no caps.
5.  **The normal path trusts its producers.** It does no comparison, re-derivation or rebuild of upstream work, and has no
    fallback for a producer's mistake: a mistake shows as a visual error, which parity finds. The only checks it keeps are
    O(1) memory-safety bounds (a CPU write clamped to the region it targets, counted). A GPU hazard on a freed or
    undersized buffer is impossible by construction: versions move only at a revision's selection.

**The flags** a parity run reports, each with its count and first instance:

| Flag | Switch | What it means |
| --- | --- | --- |
| `<- PHASES`, `<- HOLES` | always | The capability changed outside a toggle, a load or `failed`; a view not drawn while casters were withheld from it |
| `<- REVISION` | `CS_DCLF_REVISION_PARITY` | A commit's values differ from the revision's shape (latch, bucket plan, latched copies) |
| `<- LATCH` | `CS_DCLF_REVISION_PARITY` | Values clamped to the revision's latch: the latch grew after the revision was made |
| `<- SHAPE KEY` | `CS_DCLF_REVISION_PARITY` | Shapes made around a parity frame differ under an unchanged shapes key: an input the key does not name |
| `<- STALE`, `<- UNREVISED`, `<- UNBUILT` | `CS_DCLF_REVISION_PARITY`; always | The installed payload built for other inputs; an epoch without the revision's recording; a publication without a payload for the resources |
| `<- CANDIDATES` | `CS_DCLF_PERSISTENT_PARITY` | The candidate tables against a fresh build |
| `<- CUT` | `CS_DCLF_PERSISTENT_PARITY` | The stand-in cut against a whole plan |
| `<- COMMIT` | `CS_DCLF_SET_PARITY` | The set's incremental commit against a full evaluation |
| `<- DETACH` | `CS_DCLF_PERSISTENT_PARITY` | The tables' slots against the tracked set (`CheckObjectSlots`, `ValidateSlice`) |
| `<- SUN EXCLUSION` | `CS_DCLF_PERSISTENT_PARITY` | A caster the cascades skipped under an excluded entry that DCLF does not draw |

## Switches

Every switch is a row of the registry in `Common/Switches.cpp`, with its kind and a one-line description:

-   **Feature**: on by default. The row says which values reduce the featureset; a run that sets one logs
    `featureset: REDUCED by ...`.
-   **Parity**: the checks below.
-   **Diagnostic**, **test**: reports, probes and the unattended test harness.

A switch is read once, from the environment first and then from `CommunityShaders-DCLF.ini` in `Documents\My
Games\Skyrim Special Edition\SKSE` (`CS_ORG_*` from the environment only). The startup log lists the switches that
are set, grouped by kind, and names any `CS_DCLF_*` it does not know.

Test runs keep the full featureset: no feature switch set.

## Parity gates

A change is validated with the parity checks on: every check must report its baseline count. A check that already
fails on the baseline is listed in [dclf-open-defects.md](./dclf-open-defects.md), with its numbers.

| Switch | What it compares |
| --- | --- |
| `CS_DCLF_PERSISTENT_PARITY` | Every kept store (object records, bindings, bones, geometry, shadow state, shading resample, sun exclusion and sun entry) against a rebuild |
| `CS_DCLF_WALK_PARITY` | The tables, as the events left them, against a dense walk of the scene |
| `CS_DCLF_CHANGE_LOG_PARITY` | The change log against a diff of the tables |
| `CS_DCLF_RESIDENT_PARITY`, `CS_DCLF_RESIDENT_DRAW_PARITY` | Resident records against the engine's registrations; the resident region's inputs against the tables |
| `CS_DCLF_BUILD_PARITY` | The GPU's draw sequences against the CPU's templates |
| `CS_DCLF_SKYLIGHT_PARITY` | DCLF's Skylighting occlusion map against the engine's |
| `CS_DCLF_CAPTURE_PARITY` (with `CS_DCLF_PARITY_BOTH=1`) | The tables against the engine's own lighting draws: descriptors, transforms, every constant group, textures |
| `CS_DCLF_SET_PARITY`, `CS_DCLF_BINDLESS_PARITY`, `CS_DCLF_PASS_PARITY`, `CS_DCLF_CAPTURE_POINT_PARITY` | Narrower checks of one mechanism each (see their registry rows) |

The standard validation is two 60-second runs on the same save:

1.  The first seven rows together.
2.  Capture parity with ownership off.

The CPU tests in `tests/DrawcallLimitFixCPU` cover the kept state and the publication foundation.
