# DCLF pool follow-ups

What should probably move into the pooled versions next, after the payload and FrameValues moved there (2026-10-14). The
design is in [dclf-architecture.md](./dclf-architecture.md), "The payload" and "Pooled versions and the retirement clock".
The migration is recorded in [dclf-async-publication.md](./dclf-async-publication.md), "The payload and frame values as
pooled versions".

Each item has three parts:
-   **Why**: what the data costs where it is now.
-   **Where**: the code involved, at the line numbers of 2026-10-14.
-   **Sketch**: a direction, not a design.

None is started. Measure before moving anything:
-   the payload pools' report lines (catch-up per part; `Diagnostics/Reports/DrawsProducer.cpp`);
-   `frame data through the render thread`, which counts the latched and staged bytes by target
    (`Diagnostics/Reports/DrawsFrameValues.cpp`, `FrameData`).

## 1. Animated material values: a per-frame FrameValues pool by material slot

**Why.** Every value an engine controller animates goes through the coordinator and the payload:
-   effect materials: `FalloffData` and `BaseColor`;
-   Lighting and effect materials: `TexcoordOffset`.

Each change is a new material record (`Tables::materialFrameVersion`), a rewritten material row and a publication. The
four-part journal (`kMaterialRowParts`) already limits each catch-up to the part that moved: V3 measured the VS part.
Even so, material catch-up is still about 300 KB a publication while such materials are in view. The values also reach the
GPU a publication late, while FrameValues' shading reaches it in the same frame.

**Where.**
-   The capture and refresh: `Scene/Ports/MaterialSources.{h,cpp}` (TexcoordOffset's two buffers);
    `Scene/Coordinator/FrameConstants.cpp` (`RefreshMaterialTransforms` ~146, `RefreshMaterialSignatures`); and
    `Scene/Ports/MaterialPortEffect.cpp` (`kFalloff` 0x38, `kBaseColor` 0x48, ~103-109).
-   The row: `Builders/MainBuild.cpp` ~602 (`SetParts`) and `Builders/MainPayload.h` ~92-98 (`kMaterialRowParts`).
-   The pools: `Draws/Gpu/FrameValues.{h,cpp}` (five pools today, `kPools`).

**Sketch.**
-   **The pool.** Add a sixth FrameValues row set, the material frame values: one row per material slot holding only the
    animated floats. The producer samples the watched materials (`transformWatch` and the effect materials whose controllers
    run) at the frame's start, as it samples shading, and catches its backing up from a journal by material slot.
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
-   **The clock.** It would need a retirement clock outside DCLF's (`Runtime/FrameRetirement.h` is DCLF's), for example
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
