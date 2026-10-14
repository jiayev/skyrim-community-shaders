# DCLF pool follow-ups

What should probably move into the pooled versions next, after the payload and FrameValues moved there (2026-10-14). The
design is in [dclf-architecture.md](./dclf-architecture.md), "The payload" and "Pooled versions and the retirement clock".
The migration is recorded in [dclf-async-publication.md](./dclf-async-publication.md), "The payload and frame values as
pooled versions".

Each item has three parts:
-   **Why**: what the data costs where it is now.
-   **Where**: the code involved, at the line numbers of 2026-10-14.
-   **Sketch**: a direction, not a design.

Status (2026-10-14, after the scene-lane pass, dclf-async-publication.md, "The scene lane as bookkeeping"): §1 is done for
`TexcoordOffset` (a sixth FrameValues pool and a GPU patch of the rows), not for the effect materials' values; §2 is partly
done (per-publication latch bytes held per frame slot); §4 and §5 are not started, and wait on their readers; the rest is not
started. Measure before moving anything:
-   the payload pools' report lines (catch-up per part; `Diagnostics/Reports/DrawsProducer.cpp`);
-   `frame data through the render thread`, which counts the latched and staged bytes by target
    (`Diagnostics/Reports/DrawsFrameValues.cpp`, `FrameData`).

## 1. Animated material values: a per-frame FrameValues pool by material slot

**Why.** Every value an engine controller animates went through the coordinator and the payload:
-   effect materials: `FalloffData` and `BaseColor` (still so);
-   Lighting and effect materials: `TexcoordOffset` (no longer: below).

Each change is a new material record (`Tables::materialFrameVersion`), a rewritten material row and a publication. The
four-part journal (`kMaterialRowParts`) already limits each catch-up to the part that moved: V3 measured the VS part.
Even so, material catch-up is still about 300 KB a publication while such materials are in view. The values also reach the
GPU a publication late, while FrameValues' shading reaches it in the same frame. Since motion stopped publishing
(2026-10-14), these items are why publications are still about 1.7 a frame.

**Done for `TexcoordOffset` (2026-10-14), another way.** The sketch below adds a row the draws read; that changes
`package/Shaders`. Instead the texture transforms are patched into the material rows themselves
(dclf-architecture.md, "Texture transforms"):
-   the main build lists every packed row carrying one (`TextureTransformList`, `Core/Gpu/TextureTransforms.h`), and the
    payload carries it;
-   FrameValues' sixth pool (`texture-transforms`) samples each entry's current buffer every frame and sends the rows that
    moved;
-   `cs.dclf.z.texture-transforms` (`TextureTransformsCS.hlsl`) stores them into the installed payload's rows, first in the
    Z-prepass epoch.

The record keeps its capture-time value, `RefreshMaterialTransforms` and the transform watch are gone, and the controllers
no longer capture a transform-only write. The cost is a pool read of 16 bytes per carrying row per frame and a dispatch over
them (0.30 ms a frame on the pool in the motion run). Left: the effect materials' `FalloffData` and `BaseColor` still go
through the records, and the epochs that read the rows before the Z-prepass epoch read last frame's transform
(dclf-open-defects.md, "The scene lane and the snapshot builder: work left").

**Next: compute the scroll on the GPU, no per-frame update at all.** The engine's float controller advances `TexcoordOffset`
from its own state (the interpolator's key, the controller's frequency, phase and time) once a frame. With some reverse
engineering of `BSLightingShaderPropertyFloatController::Update` (0x14150DDE0) and the effect controllers' updates, the
patch pass could evaluate the scroll itself from per-material constants captured once (the controller's parameters and the
start value) and the frame's time, the way the engine does. FrameValues' `texture-transforms` pool and its per-frame reads would
then go. To work out first:
-   what the update reads (a keyframe interpolator, a linear rate, or both) and whether every controller kind that writes
    the texture transforms can be expressed as a closed form of time;
-   what makes a controller stop or restart (activation, `kActive`, a cell unloading), so the captured constants change only
    on events;
-   the clock: the engine's controller time against the frame's, so the GPU value matches `[cur]` on the same frame.

**Epoch order (check first).** The patch runs first in the Z-prepass epoch. Any epoch recorded before it that reads the main
material rows sees last frame's offset (or, right after a new payload is installed, the capture-time value). W10 named the
water reflection and Skylighting's occlusion views, but dclf-architecture.md's frame table puts the reflection epoch after the
depth commit, so confirm which epochs really read the rows first. Then either run the patch in the earliest of them
(`ViewEpoch.cpp`'s latch would carry `textureTransformsIndex`), or, with the GPU-computed scroll above, have each epoch's
pass compute its own value.

**Also left: the frame-sourced components.** `RefreshMaterialSignatures` writes the frame-sourced floats into every slot of a
signature whenever the frame's sources move (most frames), so `PublishTables` still replays hundreds of 2.4 KB material
records a publication (540-950 in the e6 run, before the texture transforms left). Two directions: a per-signature row the main
build packs with the record (a frame's move writes one row per signature), or copy-on-write chunks for
`SceneTables::materials`, so a publication shares the chunks nothing wrote.

**Where.**
-   The capture and refresh: `Scene/Ports/MaterialSources.{h,cpp}`; `Scene/Coordinator/FrameConstants.cpp`
    (`RefreshMaterialSignatures`); and `Scene/Ports/MaterialPortEffect.cpp` (`kFalloff` 0x38, `kBaseColor` 0x48).
-   The row: `Builders/MainBuild.cpp` (`WriteMaterialRow`) and `Builders/MainPayload.h` (`kMaterialRowParts`).
-   The pools: `Core/Gpu/FrameValues.{h,cpp}` (six pools, `kPools`).

**Sketch** (for the effect values; the texture transforms' patch is the other way, and needs no shader change).
-   **The pool.** Add a FrameValues row set, the material frame values: one row per material slot holding only the
    animated floats. The producer samples the effect materials whose controllers run at the frame's start, as it samples
    shading, and catches its backing up from a journal by material slot.
-   **The read.** The draws read the row through a frame record register, as they read t121-t123. The material slot is
    already known: it is the input's `y` (low 20 bits), or the material row's offset from the table base divided by
    `kMaterialRowBytes`.
-   **The record.** The material row keeps the evaluated values. The record stops changing for an animated value, so
    neither the material row nor the publication moves; material catch-up should fall to about 0.
-   **Cost.** The Lighting and Effect stages read the values from the new rows under `DCLF_BINDLESS`. That changes
    `package/Shaders`, which recompiles the shader cache.

## 2. Per-frame values in `org::LatchBlock` and the latched copies

**Why.** A latch block has a region per frame slot, and a commit writes every value into its slot's region every frame.
The epoch's latched-copies pass then copies them to their targets on the GPU, unchanged or not. Some of these values are
truly per frame; others are transport of data that changes rarely.

**Where.**
-   The main commit: `Core/Uploads.cpp` ~102-255 (`LatchedUploads`).
-   The shadow commit: `Epochs/ShadowEpochs.cpp` ~340-380.
-   The copy list: `Core/Latched.h` (`LatchedCopy`, `LatchedList`).
-   The latch layouts: `Core/GpuLayouts.h` (`BuildDrawsLatch`, 320 bytes per dispatch).

**What is there, by how often it changes.**
-   **Every frame (keep):** the BuildDrawsLatch's camera and culling fields (`viewProj`, cull planes, the frame's indices
    into FrameValues' backings); the sun cascades and local shadow volumes; the zeroed counters and bucket counts; the
    camera's frame-slot blocks.
-   **Per publication:** the BuildDrawsLatch's payload block (`inputsIndex`, `geometriesIndex`, the row addresses) and the
    frame record's object and extras registers. These are rewritten into every dispatch's latch every frame, but change only
    when a payload is installed.
-   **On a change, rarely:**
    -   the frame record's texture descriptors (resolved again only when a view changes);
    -   the captured D3D11 constant blocks the frame slots carry (VS and PS frame slots, the effect camera, light and blood
        blocks, the extras frame). Many hold the same bytes for long stretches; VS b7 was captured once at startup.
    -   the engine's structured frame buffers (`Resources::frameBuffers`, copied whole every colour commit from the
        `ConstantMirror`: `Core/Uploads.cpp` ~150-166).

**Sketch.**
-   **Per-publication values.** Install them with the payload: a small immutable block per `PayloadVersions`, pooled like
    a part, named once by value.
-   **Rare values.** Give each source a pooled version, published only when its bytes change (the `ConstantMirror` already
    knows when a buffer was written), with the latch carrying only its index or address.
-   **What stays.** The recordings are fixed per revision, so every pooled value must still reach the GPU by value through
    the latch, as the payload does; nothing can be bound in a recording. The latch shrinks to the truly per-frame part.

**Done (2026-10-14).** The per-publication bytes are written once per frame slot, and again only when their key moves (the
latch block, the source, the installed pipeline slots): `LatchRegionsHeld` (`Core/Latched.h`), used by the main commit
(the bucket map and tables) and the shadow commit (the shadow map rows). The shadow arena is staged only in the 256-byte
chunks that differ from what the buffer holds (`ShadowEpochs.cpp`, `kArenaChunk`). Staged shadow constants fell from 4.0 to
1.5 KB a frame, and the main commit's payload uploads from about 90-100 to 37 us an epoch. The per-publication block in
the payload and the pooled rare values are not done.

**Also on the commit, not done.**
-   `FrameView::InstallClaims` after a withdrawal still builds the main-claim vector from the claims' phases (O(claims),
    only after a withdrawal). The coordinator could carry it in the pooled claims snapshot (`publicationClaims`).
-   `RefreshFrameConstants` is not split: the lighting merge and technique fog read the same Prepass engine state as the
    camera-dependent part.
-   `FrameData::Note` builds a string and does a map lookup per staged or latched write (about 150-170 a frame); an
    interned key per target would remove the allocations.

## 3. LightLimitFix's light array in its latch

**Why.** `ORGLightCulling` copies the whole light array into its latch block every frame. The block is sized for
`maxLights` in every frame slot, whatever the frame's count. This is outside DCLF.

**Where.** `src/Features/LightLimitFix/ORGLightCulling.cpp`:
-   ~425: the latch is made at `kLatchLightsOffset + maxLights * lightStride` per frame slot.
-   ~474-483: `Execute` writes the `LLFLatch` and then `latch->Write(slot, kLatchLightsOffset, lights, bytes)`.

**Sketch.**
-   **Is the data per frame?** Most lights move or flicker, so the array is largely per-frame data. A journal pays only if
    LightLimitFix can give its lights stable indices, so that an unchanged light is not sent again.
-   **With stable indices:** a pooled backing per frame, as FrameValues does, caught up by light index, with the latch
    carrying the count and the backing's index.
-   **Without them:** the gain is only memory: a backing sized to the frame's count, against the latch's
    `maxLights`-sized region in every slot.
-   **The clock.** It would need a retirement clock outside DCLF's (`Core/Runtime/FrameRetirement.h` is DCLF's), for example
    one owned by `RenderGraphRuntime`, which already advances DCLF's.

## 4. `CommitUploads`' staged batches

**Why.** The commits still stage some writes on the render thread (a memcpy into a `StagedUploadBatch`, submitted at the
commit's end), into the version the adopted set names:
-   the face positions (`UploadFaceStreams`);
-   the tree rows (`UploadTrees`);
-   the fade roots, their programs and animation stamps (`UploadFadeRoots`, `fadeAnimated`, `fadeRootPrograms`);
-   the index pool's copy list;
-   the readbacks' arm headers;
-   a few zeroings.

Most of this is the publication's content, which the snapshot builder could publish ahead, as it now does the payload. The
writes also mutate a version in place after it was named. This is safe, because copies are ordered on the queue, but it
breaks the rule that a version is immutable once named.

**Where.**
-   `Core/Uploads.h` ~31-66 (`CommitUploads`; `AcquireStagedBatch` ~11-28, a scan of a vector of batches by `use_count`).
-   The users: `Core/Uploads.cpp` ~59 (main commit: 259-260, 300-345, 497) and `Epochs/ShadowEpochs.cpp` ~346-376.

**Sketch.**
-   **Payload parts.** The face positions, tree rows and fade root rows become payload parts (or a second
    `JournaledBufferSet`), caught up from their tables' journals on the snapshot builder and read by value.
-   **The index pool's copy list** becomes the publication's too: the pool's update is a pure function of the geometry log.
-   **What stays.** Zeroings stay latched. Once no write is left, `CommitUploads` and its batch pool go.

**Not started (2026-10-14): the readers come first.** The recordings bind the versioned buffers these writes target
(`ShaderResource(*scene->fadeRoots)`, `VertexBuffer(*scene.facePositions)`, ...) in `Passes/Build.cpp`,
`Passes/SceneState.cpp`, `Epochs/ViewEpochRecord.cpp` and `Core/Resources.cpp`. A part needs those passes to read it by value
(an SRV index or address through the latch) and the draws' face stream addresses patched at emit time, as
`PatchRowAddresses` does for rows. Until then the builder cannot write them without writing into a named version a frame in
flight reads. The index pool is the same: its copy list is render-thread state against the adopted versions.

## 5. Tree wind values

**Why.** Tree wind takes its data two ways:
-   **The tree rows** (`SceneBuffers::trees`) are uploaded whole on the render thread whenever the tables' tree content
    epoch moves, with no journal.
-   **The frame row** (`TreeWindFrameRow`, 64 bytes) is staged every Z-prepass commit into one plain buffer
    (`treeFrameBuffer`, outside the set), which the next frame overwrites.

`fadeFrameBuffer` follows the same pattern.

**Where.** `Core/SceneBuffers.h`:
-   ~26-45 (the members);
-   `UploadTrees` ~275-300 (the whole upload ~281, the frame row ~297-299);
-   `UploadFadeRoots` ~220-266.

The buffers are made in `Core/Resources.cpp` (~525 tree frame, ~545 fade frame).

**Sketch.**
-   **The tree rows:** a journal by tree slot (`KeptArray`), published as a payload part (item 4).
-   **The frame rows** (tree and fade) are per frame and belong in the latch, through a latched copy, as the other frame
    values do; the plain buffers then go.
-   **What stays.** The GPU-only state (`treeClocks`, `treeWindRows`, `fadeStates`) stays in the set.

**Not started (2026-10-14).** The frame rows' latched copies need the depth shape's `MainLatchedLayout`
(`Revision/Producer.cpp`) to list a `LatchedCopy` for each buffer and `MainLatchedTargets` (`Passes/MainExtension.cpp`) to
declare both; the commit's two staged writes then become `latched(...)` writes (`LatchedUploads` already stages a write
whose copy is missing).

## 6. Readback staging stays a frame-slot ring

**What.**
-   `HostEventRing` (`Core/Readbacks.h` ~14-100), with `FadeWriteBack` and `VisibilityReadback`: one host buffer per frame
    slot for each list capacity a sealed set names.
-   The tree LOD counts (`Core/Resources.cpp` ~609), the pass stats (~231) and the foliage diagnostics
    (`Diagnostics/Draws/Foliage.cpp` ~365), each with a readback buffer per frame slot.

**Why it is kept.**
-   **Direction.** The data goes from the GPU to the CPU, and each execution's recorded copy rewrites its slot's buffer
    whole, every frame.
-   **Retirement.** The depth commit harvests a slot when the slot comes round again; the host has already waited for that
    execution, so reusing the slot already implies retirement.
-   **No catch-up.** There is no CPU-side content to catch up and no version to prefer, so a pool would add only
    bookkeeping.

**The one gap.** A `StagingSet` is append-only: a set for a capacity no sealed set names any more is kept until the ring
goes, because a kept recording may still name it. Release it on the clock once no sealed set names its capacity and no
frame bound one that did. Low priority: it grows only with the event lists' growths.

## 7. Fresh allocations outside the set and the pools

**Why.** Every growth comes from a pool. What is still made directly is invisible to the pools' census and their leak
watch.

**Where.**
-   **First versions.** Every versioned buffer's first version is a fresh allocation, made at setup and registered with the
    set: `MakeVersioned(CreateWords(...))` in `Core/Resources.cpp`, `Epochs/ShadowIndexPool.cpp` ~25-31 and
    `Main/FrameLayout.h` ~178-184. Only later versions come from `cs.dclf.scene-buffers`.
-   **Fixed plain buffers:**
    -   the draw count buffers (`cs.dclf.draw-count`, the shadow and view draw counts), the HZB counter, the sort's counts,
        offsets and block sums (`Main/FrameLayout.h` ~178-180);
    -   the tree LOD draw rows (`Core/Resources.cpp` ~597, ~753);
    -   the tree and fade frame buffers (item 5);
    -   the engine frame buffers' copies (`Core/Resources.cpp` ~396);
    -   the readback buffers (item 6).

**Sketch.**
-   Let `GrowableBufferSet::Register` make the first version through the registration's pool, so the census covers every
    version.
-   The fixed plain buffers are setup-time and constant-sized. Leave them unless one becomes per-frame data (the frame
    buffers do: item 5).

## 8. BasicRenderer: telemetry lost in the migration

**Why.** The pool moved from BasicRenderer to ORGModuleServices (W3). BasicRenderer's adapter keeps the per-event counters
(`EmitPoolTelemetry`: allocated, reused, reclaimed, exhausted, suspensions, wakes). The following were lost:
-   **Process-wide pooled-backing gauges:** `SARP.VersionedBuffer.PooledBackings` and `PooledBackingBytes`. The pool now
    emits per-pool `LiveBackings`, `LiveBytes` and `FreeBackings` gauges, which the adapter ignores.
-   **Per-build allocation telemetry:** `BytesReplayed`, `RequestedCapacityElements`, `BackingCapacityElements`, and the
    journal page records (`JournalPageAllocatedBytes`, `JournalPageClonedBytes`, `JournalDirtyBytes`,
    `JournalRetainedImageBytes`, `ImageMaterializedBytes`).
-   **The `SARP_GPU_LIFETIME_TRACE` `version_release` log line:** a version's release, with the remaining references on
    the resource and the backing.
-   **The exhaustion trace** into `resource_lifetimes.csv` (`TraceExhaustion`, under `BASIC_TELEMETRY_OUTPUT_DIR`, rate
    limited to one a second).

**Where.** BasicRenderer `src/Runtime/Resources/VersionedBuffers/VersionedGpuBufferArtifacts.cpp`:
-   now: `EmitPoolTelemetry` ~95-114, the hooks ~209;
-   before the migration (BasicRenderer `HEAD`): `EmitBackingPoolTelemetry` ~45, `TraceExhaustion` ~102-144, the
    lifetime trace ~314-323, the build's records ~1121-1129.

On the ORGModuleServices side: `BackingPoolEvent` and `BackingPoolHooks::telemetry` (`BackingPool.h` ~100-121).

**Sketch.**
-   **Gauges.** Sum the per-pool gauges into the process-wide ones in the adapter's hook: an atomic total per gauge,
    updated by each pool's delta, keyed by pool name.
-   **Exhaustion trace.** The `Exhausted` event carries the pool's name: write the CSV row from the hook, with the same
    rate limit.
-   **What needs ORGModuleServices.** The per-build records and the version release need new hooks there: a build-stats
    callback from `BuildVersionedBuffer` (requested and backing capacities, bytes replayed, journal pages), and a lease
    release event (with the backing's remaining leases).
-   **DCLF.** DCLF's pools set no telemetry hook; the report reads the census directly. DCLF could use the same hooks for
    Tracy plots if the report's windows prove too coarse.
