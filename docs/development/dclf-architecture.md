# DCLF architecture

How Drawcall Limit Fix (DCLF) is put together today: the order of a frame, the data from engine events to GPU draws,
how it takes objects away from the engine, where each piece lives, and the parity checks that say it is right. It
describes the present only.

-   [dclf-status.md](./dclf-status.md) is the coverage, and what the engine still does.
-   [dclf-open-defects.md](./dclf-open-defects.md) lists the defects and failing checks under investigation.
-   [drawcall-limit-fix.md](./drawcall-limit-fix.md) is the historical record of how DCLF got here, measurements
    included.
-   [dclf-pool-followups.md](./dclf-pool-followups.md) lists what should move into the pooled versions next.
-   The engine behaviour everything below depends on is in [skyrim-engine-notes.md](./skyrim-engine-notes.md).

DCLF runs on Skyrim AE 1.6.1170 only. Every engine address in the code is that runtime's.

## Standing policies

1.  **DCLF will take over all culling on the GPU.** No design may rely on the engine's culling or registration results.
    The places that still read them are transitional (the engine-culling roadmap removes them); nothing new reads them.
2.  **No per-view lists.** The scene is one persistent list. What an object takes part in is a per-object bitmask of view
    types, tested by each view's GPU cull; the CPU never builds a list or cut per view.
3.  **The normal path trusts its producers.** Checks are parity-only observers; the build is incremental, with no caps or
    fallbacks.

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
| `Main::Draw`, before the main cull (`FrameDriver::BeginFrame`) | Applies the toggles, adopts the newest complete snapshot the snapshot builder posted (`IndirectDraws::AdoptSnapshot`: its revision, its draws and its publication, installed by `FrameView::HandOverAtFrameStart`), then installs its claims, list filter and sun exclusion, or withdraws the set (below). Serves the coordinator's requests and kicks the frame's values (`FrameValues::Kick`: placements, palettes, shading, fade and tree seeds, each into a backing leased from its pool) | **Scene pass** (`Coordinator::RunSceneWork`, whenever its producers wake the pump): the events applied, the walk, the set's commit; then the publication (`PublishScene`), handed to the **snapshot builder** on the pool (`IndirectDraws::PostSnapshotWork`: the payloads built ahead, the revision made and sealed, the payload's pooled versions published) | The frame values' uploads, on the copy queue; the payload's catch-up, on the uploader |
| The engine's cull jobs | The engine registers the passes it keeps. `PassCapture` withholds DCLF's objects, `PrimaryCull` stands in for DCLF's references inside the list jobs, and the sun's cascades skip DCLF's entries | | |
| `BeforeShadowMaps` | The **reflection epoch**; the frame's shadow views (`ShadowViews`), their coverage by the selected revision (`DecideShadowCoverage`) | | The water reflection's faces |
| Each shadow view's `FinishAccumulating` | `CaptureShadowView`: where the engine drew, and with which constants | | The engine's own shadow draws |
| `AfterShadowMaps` | The **shadow epoch**: the installed shadow payload committed into the revision's shape | | Every placement's casters, culled per view |
| Skylighting's `RenderOcclusion` | The **occlusion epoch** | | The occlusion maps |
| `EarlyPrepass` | Kicks the **accumulate work**; requests the shadow pipelines for the keys in use | **Accumulate work**: what the main camera's registrations decided | |
| `Main_RenderDepth`, world drawn | `CaptureDepthPass`: the **Z-prepass epoch** | | DCLF's depth, the HZB, two-phase occlusion culling |
| `Prepass` | Latches the accumulator and refreshes the camera-dependent constants | | |
| The main pass's opaque batches (`BeforeOpaquePass`, `AfterOpaquePass`) | `CaptureMainPass`, then the **colour epoch** | | The drawn set into the G-buffer, depth-tested EQUAL, then decals |
| `Present` (`Reset`) | Joins the scene work, closes the engine-read window, binds what the frame read to its last submission (`BindAdopted`: the adopted set and the installed payload; `FrameValues::EndFrame`), applies the events no scene work took, releases what it let go of, writes the report, handles the menu toggle | | |

Each epoch is submitted from the selected scene revision's recording (ORG's revision-driven epochs): the commit writes the
frame's values into the revision's shape (its latch, bucket plan and latched copies) and hands the epoch the installed
payload's parts by value (`PayloadVersions`, below). Nothing is built, prepared or checked in the frame.

An epoch's position in the frame matters to the work after it. Streamline's upscalers and frame generation must not run on the GPU
directly behind an epoch: see `render-graph.md`, "Hazard: external Vulkan work directly after a graph epoch". Check that before adding
an epoch or moving one late in the frame.

A frame's set is **withdrawn** (the engine draws everything, and the next covered frame installs the whole set again) only
when no publication applies: startup until the first one, a toggle until a publication committed under the new toggles
is installed, a load, or `failed`. Every other frame draws the
installed publication's claims.

## The data

```
engine writers ──events──▶ Coordinator tables ──change log──▶ kept stores ──▶ payload ──commit──▶ epoch (GPU)
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
2.  **Tables.** `Scene::Coordinator` holds a record per object, and shared tables of geometries, pipelines and materials
    (`SlotTable`: slots with generations and reference counts). Every change a write makes to the tables is appended
    to the change log (`Tables::changeLog`), with its causes.
3.  **Lookups.** What a build needs from the render thread's services is resolved ahead of the build (`Lookups`):
    -   the pipeline set indices and shader constant tables, at `EarlyPrepass`;
    -   the texture and sampler descriptor indices, in the epoch's preparation.

    A build never calls the engine, D3D11 or ORG.
4.  **Kept stores.** Each epoch kind keeps its GPU-side state across frames, and rewrites only what the change log
    names:
    -   the object records (`ObjectRecordStore`), extras rows (`ExtrasStore`) and geometry table (`GeometryStore`);
    -   the main pass's material and pipeline rows (`MainRows`), shared by the Z-prepass and colour builds;
    -   the scene list's entries (`ResidentRegion`, below);
    -   the drawn set (`DrawnMarks`);
    -   the shadow state (which modes hold each object, what waits) and the shadow material rows (`ShadowKept`).
5.  **Payload.** A build is a pure function from the tables and lookups to a payload: the rows it wrote, the draw
    inputs and the kept stores' changes. `BuildMainPayloads` (`Draws/IndirectDraws/Builders/MainBuild.cpp`, both main segments
    in one build) and `BuildShadowPayload` are the two builders. They run on the pool with each publication
    (`IndirectDraws::PostSnapshotWork`), the main build first; the snapshot builder then publishes the payloads as pooled
    versions (`PayloadBuilder`, "The payload" below).
6.  **Commit and epoch.** On the render thread, the commit adds what only it has: the frame's constant buffers
    (`ConstantMirror`), the per-frame textures and the latch, written into the selected revision's shape. Then the
    revision's recording of the epoch runs the GPU work:
    -   `BuildDrawsCS` culls the inputs (frustum, and in the Z-prepass two-phase HZB occlusion) and writes the
        indirect draw sequences;
    -   the draw passes execute them;
    -   the colour epoch draws exactly the set the Z-prepass decided (the per-object visibility words).

## Growable tables

No GPU table DCLF keeps has a compile-time capacity: every one grows, and nothing past a capacity is dropped or falls back.
Each table holds one copy of its data, indexed by what the data depends on, and every row is a constant-buffer block
(256-byte aligned), so a table can be read as an array of constant buffers.

### The scene's set: requirement, batch, sealed set

Every versioned buffer DCLF makes is a buffer of one set (`SceneBufferSet`, over ORGModuleServices'
`GrowableBufferSet`), and so is every latch block. The scene snapshot is the only source of growable buffers: no
recording binds a version its snapshot's set does not name. The scene payload is not in the set: no recording binds it,
and the snapshot carries it as pooled versions of its own ("The payload", below).

1.  **Requirement.** Each revision make (`MakeRevision`, on the snapshot builder) starts a build and requires what the
    publication's tables, the frame's posted counts and the builds ahead need (`Revision/Requirements.cpp`):
    `RequireSceneTables`, `RequireMain` (which updates the one `MainLayout`), `RequireIndexPool`,
    `RequireShadowSequences`, `ReserveShadowLatch`, each view's `RequireView`, the latched copies' blocks, and last
    `RequireLatches`. Buffers indexed alike are one `UnitGroup`: one requirement fans out to every member, and the group
    holds the least any member's named version holds.
2.  **One batch, as state-graph work.** `Seal` names the newest ready versions when they meet every requirement. When one
    does not, it posts one growth batch for every unmet buffer on DCLF's state graph (`SceneGraph::PostGrowth`, the
    `BufferVersion` artifact: `PostVersionGrowth`), and the make waits; the batch's completion wakes the builder, which
    makes the revision again. Each new version is made into a backing leased from the set's pool
    (`cs.dclf.scene-buffers`, an ORGModuleServices `BackingPool` with DCLF's `PooledBackings` hooks), and the version owns
    its lease: once its last reference goes and the frames that bound it retired, the backing returns to the pool.
3.  **Sealed set.** A sealed set (`NamedVersions`) is immutable: every buffer's version, units, bytes, device address and
    SRV index, and every latch block. While nothing changed, `Seal` returns the very same set (its `Serial` and fragment),
    so every recording made against it stands; the shapes key (`RevisionShapesKey`) mixes that serial and the layouts.
4.  **Adoption.** The frame adopts the snapshot it selects (`AdoptSnapshot`: `NamedVersions::AdoptAll` and the revision).
    Commits and latch writes read the adopted set (`Impl::Adopted()`); builds ahead fill every address they embed from the
    sealed set (`AddressesOf`). After the frame's last submission, `BindAdopted` binds the adopted set's backings
    (`NamedVersions::Bind(SubmittedRetirementPoint())`), so none is reused while that frame's GPU work reads it. Nothing
    flows from the frame back to the producer.

Naming: `sealed` is a set the producer sealed (the snapshot builder's side), `adopted` the one the frame installed (the
render thread's). A set's `serial` is its identity, a buffer's `version` an `org::BufferVersion`, and a `contentEpoch` a
journal position (a `KeptArray`'s, or what a pooled backing holds).

A requirement a set cannot meet is never sealed. A bound past the device's max sequence count or the sort's 2^20 ranks is a
hard failure (`CheckSequenceLimits`). `CS_DCLF_TABLE_START=small` starts the tables, slots and latch layouts small, to
exercise every growth.

### Contents

A new version holds what its registration says (`GrowableContents`):
-   **Fresh:** nothing; its writers write it whole.
-   **Zero:** zero-filled through the uploader before it turns ready (the fade states, the reported words, the tree wind).
-   **Capture:** laid out from immutable state by the producer, with that state's contents stamp. ORGModuleServices keeps
    it; no DCLF buffer uses it since the main rows, its only user, became payload parts.

Every writer of a growable buffer keeps a `FilledVersion` or `FilledContents` for it. Before it writes, it compares the
version it filled with the one the set names: on a mismatch it writes whole (or from the Capture stamp), then records it.
No adoption callback resets anything. The index pool is one instance: its indices are laid out again, and its first
indices sent whole, into any version the update did not fill (`IndexPool::indicesFilled`, `firstsFilled`).

### Capacities

Every capacity a shape or a commit uses is read from the named units, never from an owner's field: the shadow slots'
sequences and bucket counts, the view blocks, the views' sequences and the index pool. The views are
planned from the one `MainLayout` (`PlanViewBuckets`), and the plan's draws are a requirement on the very entry the view's
recording resolves, so a shape can never index past its buffer.

The payload's parts have no capacity to require: each publication leases, per part, a backing of the size class its
contents need, so no draw waits on a fit.

The index pool's room is twice the geometry indices. A pool the frame found fragmented publishes that version's serial
(`IndexPool::fragmentedAt`), and the next make requires a version newer than it (`RequireNewer`): laid out again at the same
size.

### Latch blocks

Every latch block is the set's (`SceneBufferSet::RegisterLatch`, once): the main latch, the main segments' two
latched-copies blocks, the shadow latch, zeros and latched-copies block, and each view's latch and zeros. The producer
keeps only their layouts (`RevisionLatchLayouts`); the reserves grow them (`ReserveMainLatch`, `ReserveShadowLatch`,
`RequireView`), and `RequireLatches` requires each block at its layout (`LatchLayoutKey`). Another layout is a new block,
so a frame in flight never reads one laid out anew; a replaced block moves the set's serial. Shapes take the blocks from
the sealed set (`NamedVersions::Latch`).

### The payload

The scene payload of a snapshot is one immutable `PayloadVersions` (`Draws/IndirectDraws/Core/PayloadVersions.{h,cpp}`):
seven parts, each a backing leased from its own pool, with its SRV index, device address (the row parts) and units:
objects, extras, geometries, material rows, pipeline rows, inputs (the scene list, below) and shadow rows. No recording
binds it, and the draws embed none of it.

1.  **Publication.** `PayloadBuilder` (one ORGModuleServices `JournaledBufferSet`; the snapshot builder is its one
    acquirer) publishes a payload once the snapshot's draws are built. Each part leases the free backing nearest its
    journal (the highest `contentEpoch` not past it) and is sent the journal's runs since that backing's `contentEpoch`.
    A row is patched with the address of the backing it goes to (`PatchRowAddresses`), and the inputs' frame tails are
    written whole. A material row is journalled in four 256-byte parts (`kMaterialRowParts`: the VS blocks, the PS
    blocks' two halves, the header), so an animated material's per-frame values move only the part holding them. `ready`
    is built from the upload tickets; nothing waits on the GPU.
2.  **The gate.** Only a snapshot the frame will adopt is published. The frame must have taken the last posted snapshot,
    and that frame's scene pass must have ended (`IndirectDraws::NoteScenePassDone`), since a publication it makes
    replaces the one waiting. A sealed but unpublished build is replaced by newer work and sends nothing. A kept
    publication (the same draws made into a new snapshot) keeps its payload and uploads nothing. The Recordings stage
    posts the snapshot only once the payload's uploads completed; their completion wakes the builder.
3.  **Adoption.** The payload is installed with the draws (`InstalledPayload()`), and a covered frame always has a ready
    one; there is no fallback. The epochs take its parts by value: the BuildDraws latch's payload block (`inputsIndex`,
    `geometriesIndex`, the rows' addresses), the shadow and occlusion latches, and the frame record's object and extras
    registers. `BindAdopted` binds its leases with the adopted set's.
4.  **Trimming.** A part's journals keep their changes back to the oldest `contentEpoch` a backing of its pool holds
    (`PayloadBuilder::OldestContentEpoch`). With none holding anything, everything may go, and the next backing is
    written whole.

### Pooled versions and the retirement clock

The payload's parts, the frame values' five row sets (placements, palettes, shading, fade seeds, tree seeds) and the set's
growth all lease their backings from ORGModuleServices `BackingPool`s:
-   **The clock.** DCLF has one `RetirementClock` (`Runtime/FrameRetirement.h`), advanced from the graph host's
    completed-frame callback: the exact GPU completion of each host frame, not a frames-in-flight window. A frame binds
    what it read after its last submission (`SubmittedRetirementPoint()`); `BackingLease::Bind` keeps the latest point.
-   **Reuse.** A backing is reused once the clock has reached its bound point and no lease holds it. The acquirer prefers
    the free backing whose contents are nearest what it needs, so a catch-up is about one publication's delta. No pool has
    a cap. Each keeps one warm spare per game frame in flight, less one (`PooledBackings::PerFrameSpares`).
-   **Release.** A released backing is destroyed on the graph host's resource cleanup lane (`PooledBackings::Retire`),
    never on the thread that let it go: the render thread is the frame values' pools' acquirer.
-   **Frame values.** `FrameValues::Kick` leases the frame's five backings on the render thread. The producer catches each
    up from its journal on the pool, without a GPU wait, and `FrameValues::EndFrame` binds them.

The readback staging is not pooled: it is GPU-to-host and rewritten whole every frame, one host buffer per frame slot for
each list capacity a sealed set names (`HostEventRing::AddSet`). What else could move into the pools is in
[dclf-pool-followups.md](./dclf-pool-followups.md).

### Observers

With `CS_DCLF_PERSISTENT_PARITY`, observers check the set; the normal path is unaware of them:
-   `CheckSnapshotAddresses`: every address and SRV index a snapshot's draws embed is the sealed set's;
-   `CheckCommitTarget`: every commit target is named, and current, in the adopted set;
-   `CheckNamedResolves`: no recording resolved a buffer its set does not name (`NamedVersions::Misses`);
-   `CheckShapeCapacities`: every shape's ranges and counts lie within the named versions of the buffers it indexes;
-   the pools' leak watch (`PoolLeakWatch`): a payload or frame values pool whose leased count stays above the frames in
    flight plus two for three report windows is `<- POOL LEAK`. The live count is no measure: the pools keep spares warm.

### Bindings

**Shadow views.** A draw's binding is split three ways (`DrawPipelines.h`, `kShadowPushWords`):
-   **per draw:** the draw's words name its material row (`ShadowMaterialRow`: the Utility vertex shader's
    `PerMaterial` block, `b1`, which the pushed address names directly, and the diffuse's index for `t0`);
-   **per view:** pushed once per view: the view's `PerTechnique` (`b0`) and `VS_PerFrame` (`b12`) blocks, the zero
    block, `SharedData` (`b5`), `FeatureData` (`b6`), and the frame record (`DrawBindings`: every other texture and
    sampler, and the payload's object and extras tables);
-   **per material:** the rows, one table for every view (the payload's shadow rows part).

The rows table grows with the payload: each publication's backing holds every row the shadow payload wrote, so no
material waits for capacity.

**Draw outputs.** Every sequence buffer is required to hold every draw the scene's tracked objects can produce
(`SceneDrawBound`). The main buffer's ranges (phase 1 and colour, phase 2, one per decal group) travel to `BuildDrawsCS`
and `SortSequencesCS` in their constants (`BuildDrawsConstants::phaseTwoBase`, `decalBase`, `decalStride`). Each indirect
draw's max count is its epoch's own draws, grown as a power of two so the recording settles.

**Main pass.** The same split, with a pipeline row as well (`DrawPipelines.h`, `kDrawPush*`):
-   **per draw:** six words of push data: the pipeline row's address (words 0-1), the material row's (2-3) and the object
    (4). `BuildDrawsCS` computes both addresses from the input's `y` (`RowsOf`: the pipeline slot in the high 12 bits, the
    material slot in the low 20), the rows' strides in its constants and the tables' bases in its latch (the installed
    payload's: `BuildDrawsLatch::materialRowsLo`, `pipelineRowsLo`);
-   **per material slot** (`MaterialRow`, 1024 bytes): the vertex and pixel `PerMaterial` blocks (`b1`), then a header
    with t0-t15, s0-s15 and the feature textures (t71, t74). A material slot is keyed by (material, pass descriptor), and
    the pack tables depend only on the technique, so the row is packed once for every pipeline and both segments;
-   **per pipeline slot** (`PipelineRow`, 2048 bytes): the technique blocks (`b0`), the geometry template (`b2`), the
    permutation (`b4`), and the shadow mask (t14, s14);
-   **per pass:** the frame push: the frame slots' addresses and the frame record (`DrawBindings`: the frame textures
    from t16, and the payload's object and extras tables).

The rows are kept across frames (`MainRows`, a `KeptArray` each): a row is written again only when its key changes (a
material row by part), and a backing is sent what changed since its `contentEpoch`. Each row's header holds its blocks'
offsets until the publication adds the address of the row in the backing it goes to (`EmitMainRowParts`,
`PatchRowAddresses`). Both tables are payload parts (material rows, pipeline rows). Whether a (material, pipeline) pair
can draw - a texture, sampler or constant block the pipeline reads that the row lacks - is checked per build, from what
each row holds (`MainBuild::AssembleRecord`).

The DGC push-data token of a `Constant` argument is at most 16 bytes on NVIDIA; BasicRHI splits larger ones, so the
six-word draw push is two tokens (BasicRHI `README.md`, and "Resolved: device loss in every DCLF draw pass" in
`dclf-open-defects.md`).

**Scene tables.** One set of object records (`t127`), extras rows (`t126`) and geometry rows - the installed payload's
parts - and one face positions buffer (`SceneBuffers::facePositions`, the set's) serve every epoch: the shadow views',
Skylighting's, the Z-prepass's and the colour segment's. One store keeps each of the payload's (`Impl::objectStore`,
`extrasStore`, `geometryStore`), and each publication catches a backing up from its journal. The set requires the face
positions at the highest face region and the per-object buffers - the visibility and frustum words, the reported
verdicts - as one unit group at the tables' objects (`RequireSceneTables`), so a segment or a mode never has more inputs
than its buffers hold.

**Shadow view and key slots.** Skylighting's map draws through view slot 0 and the frame's shadow views through the
slots after it. A slot is a sequence buffer, a count buffer and a row of the view blocks (`ShadowResources::viewBlocks`:
the view's `b0` and `b12`). More slots are more buffers the passes declare, so the render thread makes them
(`ShadowSlotsRequest`) and builds the graph with them; no revision is sealed before that build. The pipeline map rows
hold every key slot the epoch can name. Either growth is a new shadow latch layout (`ShadowLatchLayout`), so a new
block of the next set. A view is never left native for want of a slot.

**The sun's plane sets and the view rasterizer states.** Nothing in the shadow path is capped:
- **The sun's full-frustum processes** (the engine culls the scene once per `fullFrustumCullingProcessArray` entry before
  the cascades, and a cascade only walks entries some process kept; 6 at the exterior save) are written once per frame
  slot into a region of the shadow latch block (`ShadowLatchLayout::SunEntryOffset`, a count and 112-byte
  `SunEntryProcess` records), which every sun view's latch names (`BuildDrawsLatch::sunEntryOffset`). The input always
  carries its entry sphere, and the kept state is always used.
- **The sun's cascades** for the colour pass's sun test. The engine has no bound: the count is `iNumSplits`, the shadow
  map array's slices (Ghidra: `ShadowSceneNode` constructor to `BSShadowDirectionalLight::SetShadowMapCount`, a
  `BSTArray` resize). `SunAccumulation` keeps them in a vector, grown at the sun's `Accumulate` while `bitsReady` is
  clear, and the colour epoch writes them into a region after the main latch (`MainLatchLayout`, 208-byte
  `SunAccumulation::GpuCascade` records), whose layout grows when it needs to (`ReserveMainLatch`: a new block of the next
  set; a `PassFrame` keeps its own).
- **View rasterizer states.** The id is a field of `ShadowPipelineKey` (`viewState`, also in the pipeline recipe id), a
  mode's states are sorted lists (`ModeRasterStates`: casters and volumetric-only), the lookups' map rows are a vector,
  and the latch's map rows are a grown dimension (`ShadowLatchLayout::rasterStates`).

`BuildDrawsLatch` is 320 bytes, its payload block included. The revision producer's `ReserveShadowLatch` grows view
slots, key slots, state rows and
processes to what the frame's epochs can name, and `CS_DCLF_TABLE_START=small` starts states and processes at 1 and the
main latch at 1 cascade.

Still native, by design rather than capacity: focus shadows (an actor's own shadow; DCLF has no caster set for them)
and views whose rasterizer state no pipeline can express (wireframe, no depth clip).

**Object rows (measured).** An object's row (`BindlessObject`, `DCLFObjectRecord`) is 256 bytes, padded from
208 so the table stays viewable as an array of constant-buffer blocks. The shaders read it as the structured buffer at
`t127`, indexed by the draw's object word. Reading it as a constant buffer instead (a per-draw push address, `b189`,
written by `BuildDrawsCS`; 8 push words and a 100-byte `DrawSequence`) was built and A/B'd on the RTX 3090 Ti, with the
same rows and the camera turning, two 60 s runs each: colour 1.03 against 1.02 ms, shadow views 1.98 against 1.94 ms,
Z-prepass depth 0.286 against 0.277 ms. The constant buffer was no faster on any pass, so it was not kept.

## The scene list and view masks

Every view reads one list per publication: the installed payload's inputs part, laid out as `[entries | main frame
inputs | shadow frame inputs]` (`PayloadVersions::regionCount`, `mainInputsAt`, `shadowInputsAt`).

-   **One entry per object** (`ResidentRegion`, kept by the main build in `Impl::mainCache`). The main build writes its
    main part: the pipeline and rows words, the flags (`kInputDrawable`), the decal ordinal and the main view bits
    (`SetMainPart`). The shadow build then writes its words into the same entry: the caster key, each occlusion map's
    key, the shadow material row and the shadow view bits (`SetShadowPart`). An entry stays while either part holds it
    (`sideOf`). The frame inputs after the entries are what each build keeps out of them: decals without a slot, face
    shapes and other second-stream positions.
-   **The view mask** (`DrawInput::ViewWords::mask`, the `kView*` bits in `GpuLayouts.h`): what the object takes part in.
    Main candidates carry `kViewMainCull`, members `kViewMain`, decals `kViewDecal` alone, LOD members
    `kViewReflection`; a shadow entry carries the views of the modes that hold it (`ModeViewBits`).
-   **A view is its bits.** Each view's latch names its view bits (`BuildDrawsLatch::viewBits`), and `BuildDrawsCS` drops
    an input whose mask has none of them before reading anything else. The Z-prepass reads `kDepthViewBits`, the colour
    pass `kColourViewBits`, a shadow view its mode's bit (the sun's and the spot lights' cascades apart), the occlusion
    maps and the reflection theirs. A shadow view also names the key word it reads (`KeyWordOf`, cull flags bits 16-17):
    the caster key or its occlusion map's key, with the shadow row for its material.
-   **Main segments.** One build makes both segments' payloads (the colour segment's first, the Z-prepass's a copy with
    its own inputs). A pair is drawable only when both segments' shader variants are given what they read, so a member
    is drawn by both or by neither (`<- SEGMENT SPLIT` otherwise).
-   **Every view scans the whole list.** Measured against the per-view lists (2026-10-08): no frame-rate cost.

## Ownership: how the engine stops drawing DCLF's objects

An object is withheld from the engine only while the installed publication claims it, and the publication's payloads
draw every claim. The set of phases DCLF claims for is a capability (the toggles and what DCLF has set up), not the views
a frame happens to have.

-   **Main pass.** `PassCapture` hooks `BSBatchRenderer::RegisterPass`. A claimed geometry's pass is recorded (the
    accumulate phase reads it) but never reaches the batch renderer, so the native loop has nothing to draw. A claim is
    the whole object: it joins the set in every phase it takes part in (`SetParticipation`) once everything every one
    of them draws it with is ready (`MainReady`, `PhaseReady`), and leaves all of them together. One that is not ready
    waits, the engine's in every view. An occlusion map with such an occluder is registered by the engine too
    (`SetLacking`).
-   **Shadow views.** The same, per shadow render mode, from the shadow build's claim sets. This includes the direct
    group insertions the registration makes without `RegisterPass`.
-   **The sun's registrations.** `SunAccumulation` skips the registration of a claimed caster in the sun's
    `Accumulate`, reproducing its mask write, so no pass is built only to be withheld.
-   **The sun's culls.** The sun entry exclusion removes entries whose every object is DCLF's from the cascade culls'
    object arrays. The GPU applies the same entry rule to DCLF's inputs (`kCullSunEntry`).
-   **The main camera's cull.** `PrimaryCull` stands in for DCLF's references inside the engine's list jobs. It keeps
    the per-object state the cull maintains (fades, LOD, the tree clock's bit), so the engine never traverses those
    references. The cut it stands in with is kept per candidate entry, planned again only for the entries that moved.

The menu's toggles (`Runtime/Toggles`) change all of this live: a toggle withdraws the set until a publication committed
under the new toggles is installed. Switching DCLF off clears every claim and bypasses the capture.

## Where the code is

The module tree, the threads, a frame's data flow and the diagnostics contract are in the **Code map** of
[drawcall-limit-fix.md](./drawcall-limit-fix.md#code-map). In short: `src/Features/DrawcallLimitFix.{h,cpp}` is the feature
(its hooks, lifecycle and menu); under `src/Features/DrawcallLimitFix/`, `Engine/{Access,Capture,Cull,Views}` touch the
engine, `Scene/{Model,Ports,Coordinator,FrameView}` and `Scene/Channels.h` keep the scene, `Draws/` draws it
(`FrameDriver`, `IndirectDraws`, `Gpu/`, `Pipelines/`), `Runtime/` holds the building blocks, and `Diagnostics/` every
parity check, probe and report.

Inside `Scene/Coordinator/` and `Draws/IndirectDraws/`, `Internal.h` is shared by that folder's files only. The layouts the
shaders read are in `Draws/IndirectDraws/Core/GpuLayouts.h`, and must match `BuildDrawsCS.hlsl` and `Lighting.hlsl`. The
scene's set of growable buffers is `Draws/IndirectDraws/Core/SceneBufferSet.{h,cpp}`, and what a revision requires of it is
`Draws/IndirectDraws/Revision/Requirements.cpp`. The payload's pooled versions are `Draws/IndirectDraws/Core/PayloadVersions.{h,cpp}`,
DCLF's ties to the pools `Draws/Gpu/PooledBackings.{h,cpp}`, and its retirement clock `Runtime/FrameRetirement.{h,cpp}`.

Includes within a folder use bare names. Includes across folders are rooted at `src/`
(`Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h`).

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
    undersized buffer is impossible by construction: versions move only at a revision's selection, and a pooled backing
    is reused only once the frames that bound it have retired on the GPU.

**The flags** a parity run reports, each with its count and first instance:

| Flag | Switch | What it means |
| --- | --- | --- |
| `<- PHASES`, `<- HOLES` | always | The capability changed outside a toggle, a load or `failed`; a view not drawn while casters were withheld from it |
| `<- LATCH` | always | Values clamped to the revision's latch: the latch grew after the revision was made |
| `<- UNREVISED`, `<- UNBUILT` | always | An epoch without the revision's recording; a publication without a payload for the resources |
| `<- CANDIDATES` | `CS_DCLF_PERSISTENT_PARITY` | The candidate tables against a fresh build |
| `<- CUT` | `CS_DCLF_PERSISTENT_PARITY` | The stand-in cut against a whole plan |
| `<- COMMIT` | `CS_DCLF_SET_PARITY` | The set's incremental commit against a full evaluation |
| `<- DETACH` | `CS_DCLF_PERSISTENT_PARITY` | The tables' slots against the tracked set (`CheckObjectSlots`, `ValidateSlice`) |
| `<- SUN EXCLUSION` | `CS_DCLF_PERSISTENT_PARITY` | A caster the cascades skipped under an excluded entry that DCLF does not draw |

## Switches

Every switch is a row of the registry in `Runtime/Switches.cpp`, with its kind and a one-line description:

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
| `CS_DCLF_RESIDENT_PARITY` | Resident records against their passes built again |
| `CS_DCLF_BUILD_PARITY` | The GPU's draw sequences against the CPU's templates |
| `CS_DCLF_SKYLIGHT_PARITY` | DCLF's Skylighting occlusion map against the engine's |
| `CS_DCLF_CAPTURE_PARITY` (with `CS_DCLF_PARITY_BOTH=1`) | The tables against the engine's own lighting draws: descriptors, transforms, every constant group, textures |
| `CS_DCLF_SET_PARITY`, `CS_DCLF_CAPTURE_POINT_PARITY`, `CS_DCLF_FADE_PARITY`, `CS_DCLF_MIRROR_PARITY`, `CS_DCLF_ACCUMULATED_PARITY`, `CS_DCLF_FOLIAGE_PARITY` | Narrower checks of one mechanism each (see their registry rows) |

The standard validation is two 60-second runs on the same save:

1.  The first six rows together.
2.  Capture parity with ownership off.

The CPU tests in `tests/DrawcallLimitFixCPU` cover the kept state and the publication foundation.
