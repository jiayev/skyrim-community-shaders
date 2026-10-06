# DCLF asynchronous publication: implementation status

The full migration is **not implemented or enabled**. Current DCLF still uses
same-frame AsyncWorker jobs, scene joins, mutable tables/lookups, and the existing
ORG epoch submission path. There is no `published` mode yet and no performance
improvement is claimed by this foundational change.

## The design (2026-10-04): an asynchronous scene, a submit-only render thread

DCLF took about 1.2 ms of the render thread a frame (Riverwood bridge, 86-93 fps). Its one worker (`AsyncWorker`) built
for about 0.4 ms, kicked and joined within the frame. The direction: everything but ORG submission moves to workers, and
the scene is maintained asynchronously, as BasicRenderer's is:
- posted intents;
- one serialized coordinator draining them;
- producers on a pool sized from the hardware;
- immutable publications the frame takes the newest of.

**The render thread** does three things a frame:
1. **Accept** the newest complete publication P at `BeginSceneFrame`, a `PublicationExchange` select. Installing it
   means pointer swaps for its claims, list filter and sun exclusion. It then opens the engine-read window W(N).
2. **Capture and submit** each epoch. The capture is a plain copy of what only the render thread can read: D3D11
   bindings, constant mirrors, a shadow view's state, the eye. The submission is a reserved ticket.
3. **The engine boundary**, bounded by changes:
   - closing W(N) at Present, which waits only for an item a worker is reading;
   - `ConstantEvaluator`'s stand-in calls of the engine's SetupMaterial/SetupGeometry;
   - engine writes the workers listed, such as the fade write-back.

`RenderThreadBudget` reports DCLF's render-thread time every 300 frames by those buckets. The rest, "other", is what is
still to move.

**A frame N.** The update's hooks only push events into the existing lock-free queues. At `Main::Draw` the render thread
accepts P (built in W(N-1)) and opens W(N). Then the workers do two things:
- the coordinator drains frame N's events into the scene state and builds P(N), which takes effect at frame N+1;
- the pool builds **FrameValues(N)**, uploaded on the copy queue, whose batch signals timeline value N.

Each epoch's GPU work waits on that value (`FrameProducerWait`). A late FrameValues costs GPU idle, never render-thread
time, and the frame is still exact. The wait is spec-safe because the copy submission makes the host writes visible.
The value is always signalled, with an empty batch on failure.

**What each carries.**
- FrameValues(N): every input that decides whether or where a claimed member is drawn this frame:
  - placements, palettes and root bounds;
  - per-frame shading samples;
  - faces and tree LOD;
  - visibility: detached this frame, hidden, unselected switch children.
- P(N): everything else (membership and claims, records, materials, pipelines, lookups, per-segment payloads and their
  ticket uploads), effective at N+1.
- Frame N draws exactly claims(N).
- An object joins DCLF a frame later than before; the engine draws it meanwhile.
- A leaving member is drawn from its retained record one more frame, and hidden by FrameValues if the engine stopped
  drawing it.
- A member's structural change (material, pipeline) shows a frame late.

**The engine-read window.** From `Main::Draw` to Present the engine reads its scene graph but does not write it; the
exceptions are known (`currentFade`, billboards, texture transforms). A worker reading engine memory holds a per-item
lease and checks that the window is open before each item. Present closes the window and waits only for the items in
flight; the rest resumes in the next window, by index, as `RunPlacements` resumes today.

**The phases.** Each phase builds, passes the CPU tests and an in-game run of 60 s or less, and is committed before the
next:

| Phase | Work |
|---|---|
| 0 | The `RenderThreadBudget` report; ORG's "other" epoch time attributed (native flush, completions, stream close, post, rebuild check) |
| 1 | `PublishedSceneExecutor` as DCLF's scheduler, with lock-free queues and no caps; the DCLF `AsyncStateGraph`; `EngineReadWindow`; the existing jobs moved onto it unchanged |
| 2 | ORG's render-thread overhead that needs no DCLF change: the host's wake after each submission, and the compute queue's submissions (done, below). Reserved submission, upload recording on the host's thread and the timeline wait need inputs that exist before the epoch, so they move to phases 4 and 6 |
| 3 | `SceneStore` split into coordinator-owned state and an immutable `ScenePublication`; events, the scene phase and the set commit moved to W(N) |
| 4 | FrameValues(N), fed by change-gated, slot-resolved writer events instead of per-frame polling (below): partitioned producers, copy-queue upload, the per-frame region the shaders read, and the epochs' GPU wait on its timeline (`FrameProducerWait`) |
| 5 | The accumulate phase, lookups, pipeline requests and frame constants moved to producers. `EvaluateTechnique` is a port reading engine globals, so a worker can run it; SetupMaterial/SetupGeometry stay at the boundary. The ProjectedUV and land-blend extras rows move to the shader; `ValidateSlice` becomes parity-only |
| 6 | Per-segment payloads built once per publication, in parallel; their uploads recorded into the tickets on the host's thread (`TryPostOwnedPreparation`) and the epochs submitted reserved (`TryReserveReadyEpochs`, `TrySubmitReservedEpoch`); shadow views from a capture; the scene lists built off the engine's job; `AsyncWorker` and every join deleted |
| 7 | Pool sizing against the engine's job threads; same-input parity; load, equipment and water stress; a starved-pool run for the GPU wait |

**Phase 0 baseline** (2026-10-04, p0-budget2: Riverwood bridge, turning camera, 75–93 fps, 50 s). DCLF's render
thread took 1.11–1.27 ms a frame (max 1.8–4 ms, one 11.8 ms load spike).

| Bucket | ms/frame |
|---|---|
| accept | 0 |
| capture | 0.016 |
| submit | 0.048 |
| ORG overhead | 0.20 |
| engine boundary | 0.019 |
| other | 0.83–0.99 |

| Hook | ms/frame |
|---|---|
| scene frame | 0.28–0.43 |
| Z-prepass | 0.16 |
| shadow epoch | 0.15 |
| EarlyPrepass | 0.11 |
| colour epoch | 0.095 |
| Prepass | 0.09 |
| occlusion (4 calls) | 0.077 |
| BeforeShadowMaps (the reflection epoch) | 0.075 |
| Present | 0.046 |
| shadow view capture | 0.027 |
| reflection faces | 0.007 |

ORG's per-epoch render-thread time is now attributed in full: the "other" phase is 0.

| Phase | µs per epoch |
|---|---|
| upload recording | 10–38 |
| the host's completion post (mailbox and wake) | 7–12 |
| Z-prepass submit (its five queue submissions) | 31 |
| native flush | 1 or less |
| completions | 1–3 |
| stream close | 1–3 |

Phase 2 takes the upload recording and the post off the render thread; with them the ORG overhead goes.

**Phase 1** (2026-10-04, p1-exec).

- **The executor.** `PublishedSceneExecutor` is DCLF's scheduler (`SceneScheduler`): one coordinator thread and a
  preparation pool of `hardware_concurrency` minus 2 threads (14 on the test machine; `CS_DCLF_WORKERS` sets the count).
  Its queues are unbounded `tbb::concurrent_queue`s (static TBB), so a dispatch is rejected only for an invalid scope,
  class or task.
- **`AsyncWorker`.** It keeps its API and job order, but its jobs now run on the coordinator lane. Its render-thread side
  takes no mutex: a job's state is one atomic, and its end is a semaphore.
- **The state graph.** `Published::SceneGraph` instantiates DCLF's `AsyncStateGraph` on the executor. It has the artifact
  kinds; `SceneState` alone does not coalesce. No producer is registered yet.
- **Build change.** The state graph now links CS's compiled spdlog (`ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET`, an
  ORGModuleServices change); the header-only copy collided with it once the graph's code was linked.
- **The read window.** `EngineReadWindow` opens at `BeginSceneFrame` and closes at Present. The scene placement job and
  the fade write-back take a lease for each item they handle on a worker.
- **The capture service.** Its admission limits default to unbounded, and its ingress is a growable queue.

The late-join cancel is not removed yet. Callers still `Cancel` a late job before building inline over its payload, and
`Cancel` waits for the running job, so a late join blocks. Removing it needs the inline builds gone (phase 6); the header
says so. No join was late in any run.

The 50 s run on the bridge matched Phase 0:
- 1.15–1.22 ms of render-thread time a frame;
- the same pacing;
- 0 render-thread waits and 0 member passes drawn natively;
- 300 window closes, none waiting on an item, and 0 leases refused.

**Phase 2** (2026-10-04, p2-wake2 to p2-wake4).

- **The host's wake.** After each submission the render thread signalled a Vulkan semaphore (`vkSignalSemaphore`) to wake
  the host's thread: 7–12 µs per epoch. The host's sleep already includes the GPU timelines of the frames in flight, so a
  submission now leaves that wake to the next GPU completion (`Async::wakesOnGpu`, a Dekker pair with the mailbox). A
  signal is still sent in three cases:
  - the host sleeps with nothing in flight;
  - a control message is posted;
  - the render thread is about to wait for a ticket.

  588 of 600 submissions needed no signal. The "post" phase fell to 0.1–0.3 µs.
- **The compute queue's submissions.** These go to a thread of their own (`ComputeSubmitter`, first in first out, an
  unbounded TBB queue), because the compute queue is ORG's alone. The stream's exit wait may precede the signal. The
  Z-prepass's submit fell from 31 µs to 13 µs.

**Why the rest of phase 2 moved.** An epoch's uploads exist only once DCLF's commit has run, in `beforeSubmit` on the
render thread. So recording them on the host's thread has to wait for payloads built ahead of the epoch (phase 6).
Likewise, a reserved ticket must not go stale, and today the commit grows resources (a stale reserved ticket throws).
The timeline wait belongs with the first consumer of the copy queue, FrameValues (phase 4).

**Results.**

| | Phase 0 baseline | Phase 2 |
|---|---|---|
| DCLF render-thread time, ms/frame (`CS_DCLF_PROFILE` on) | 1.11–1.27 | 0.92–1.08 |
| ORG overhead, ms/frame | 0.20 | 0.10–0.15 |

The profile itself costs about 0.3 ms: without it the total was 0.69–0.71 ms (p2-wake3 and p2-wake4).

**Fixed: a GPU device loss.** From the first Phase 2 run (p2-wake), some startups lost the device on the first DCLF frames:
a page fault at address 0, with no shader running. The cause was the compute submitter: its `vkQueueSubmit2` ran at the
same time as DXVK's submission thread, which something below DXVK does not tolerate. It now submits under DXVK's
submission lock, on its own thread. See crash-catalog.md, "GPU device loss: a page fault at address 0".

**Phase 3, step 1: the claims a frame late** (2026-10-04, p3-lag2).

- **The split.** `CommitSet` decides; `ApplySet` (at `BeginSceneFrame`, before the events) applies the last decision to
  the records and publishes it as the claims. So frame N draws, and the engine withholds, the set the walk of frame N−1
  decided. This is the membership half of "What each carries", still on the render thread.
- **The bug the parity check found.** A slot freed after `ApplySet` kept its applied phases until the next one. 69 freed
  slots a run were reported "in the set, drawn by nobody, record disagrees", and the shadow builds read those phases.
  `Tables::ResetObject` now clears them.
- **Validation.** With `CS_DCLF_SET_PARITY=1`, every 300-frame window was clean:
  - 0 objects in the set and drawn by nobody;
  - 0 drawn outside the set;
  - 0 records disagreeing;
  - 0 members' passes drawn natively.

  The one-frame "kept, GPU-culled, drawn again" gaps (about 1,400 a window at the bridge) are as before: the older runs
  show the same count, all with the culling's "rejected" verdict.

**The frame's order** (`CS_DCLF_FRAME_TRACE=1`, TEMP: each traced site's first call, from Main::Draw, one frame in 600). On
the render thread:

| µs | Step |
|---|---|
| 11–150 | the scene work (`ApplySet`, events, walk, `CommitSet`) |
| 166–470 | the water-reflection faces, whose culls run LocalLightCull's, the sun's and the pass capture's hooks |
| 478–540 | the sun's full-frustum cull, with PrimaryCull's per-frame setup (`PrepareFrame`) at 522 |
| 691 | `AfterListJobs` |
| 704 | BeforeShadowMaps |

Elsewhere, the scene lists' job reads the list filter at 150 µs, and the list jobs (the stand-ins) run from 488 µs.

**Step 2, next: the window's readers on published data.** For the scene work to run on the coordinator from Main::Draw,
nothing in that ~700 µs may read the store the walk is changing:
- PrimaryCull's setup, which reads tracked geometries, object indices, set phases, sun candidates and switch changes, writes
  fade ownership back, and is called by the commit (`NoteSetChanges`). It moves into the scene task, which builds the cut
  as immutable data; the render thread keeps only the cull-time inputs (the list processes, the fade eye).
- LocalLightCull's category filter (category nodes, light entries).
- The sun's and the scene lists' candidate generations.
- `GetFrame` (an atomic).

**Phase 3, step 2: the walk on the coordinator** (2026-10-04, p3-task2).

**`BeginSceneFrame`** now runs in this order:
1. `BeginFrame`: the frame number, and the published sun-candidate generation the window's hooks read
   (`GetPublishedSunGeneration`).
2. `ApplySet`, which also hands the last commit's set changes to PrimaryCull.
3. The frame's events (`ProcessEvents`), on the render thread.
4. The point lights' filter, the main renderers, tree LOD's and the reflection's preparation, and the list filter.
5. The kick of the walk and the set's commit onto the coordinator (`SceneStore::KickSceneTask`). The placements run
   inside the task, under read leases.

**The join** comes at the first reader that needs the frame's walk: PrimaryCull's full-frustum hook (`JoinSceneTask`).
Every later DCLF hook joins too, a no-op once joined. At the join (`FinishSceneWork`):
- PrimaryCull receives what the work held for it: hidden keys and lost members;
- claims whose record stopped drawing are revoked (`RevokeUndrawnClaims`, from the change log since `ApplySet`), so the
  engine draws them that frame;
- the fade write-back and the early shadow build are kicked.

Capture, walk and persistent parity run the work inline.

**Why the events stay on the render thread.** With the list filter published before the frame's events, every run
crashed within its first 40 frames. Present released a root in the scene lists' graveyard that something had already
freed. The bisect, inline (`CS_DCLF_ASYNC=off`):
- the list filter before the events: crashed in 5 of 5 runs;
- after the events and before the walk, or after the walk: 0 of 4.

The list decisions logged per frame were the same in both orders, and no root the instrumentation saw buried had a
reference count below 6, so the mechanism is not identified (crash catalog). The events also walk newly attached subtrees
and drop the last reference to detached ones, which runs the engine's destructors: main-thread work either way. They cost
about 0.05 ms a frame.

**Results** (`CS_DCLF_PROFILE` on):

| | Phase 2 | Step 2 |
|---|---|---|
| Scene frame, ms/frame | 0.28–0.39 | 0.06 |
| DCLF render-thread total, ms/frame | 0.92–1.08 | 0.76–0.79 |

- The join waited on 0.24 frames in each, 0.006 ms a frame.
- Set parity was clean in every window.
- 0 members' passes were drawn natively, and 0 claims were revoked.

**What of the walk is synchronous** (amendment, 2026-10-04). Measured at the bridge (b-ring2; p3-trace2; p3-probe):

| | ms/frame | What |
|---|---|---|
| Lag-tolerant, P(N) | ~0.17 | structural events 0.056, validation 0.018, sweeps, sun and light candidates 0.010, shadow sets, the set commit. The events for all of it already exist; moving it to the coordinator is enough. |
| Synchronous by nature, FrameValues(N) | ~0.21 render thread, 0.2 worker | placements, palettes and root bounds of what moved; visibility; per-frame shading |

Most of the synchronous cost is polling:
- Graph animation and the update pass push about 1,500 move events a frame whether or not anything moved. 620 still roots
  a frame are re-read, and 193 of 243 reference-event placements do not change.
- About 650 per-frame entries are re-scheduled every frame (`perFrameSet`, `MoveReasonOf`, `QueueRoots`' scan).
- The Prepass extras watch re-derives about 1,009 ProjectedUV and land-blend rows from the eye and a clock.

**Where the writers run.** The update's writers run on the engine's job threads, but some run inside the render phase too.
In one traced frame:

| Writer | µs after Main::Draw |
|---|---|
| Havok's node transform | 135 |
| sky cell skin | 326 |
| graph animation | 1,038 |
| `Update3DPosition` | 1,045 |
| `AnimationGraphPlace` | 4,374 |

So the read window is not strictly quiescent. The placement probe found 0 of DCLF's placements moved between the sample
and the join in 300 joins (p3-probe), and stays as FrameValues' standing check.

**Adopted.**
- **Change-gated, slot-resolved writer events.** Each move writer's hook keeps a stack copy of the reference root's
  transform and bound, and compares after the engine's call. It pushes only for keys in the latest publication's
  immutable key index, tagged "root bound changed" or "subtree updated", with the resolved placement slots, into a
  per-thread buffer. Lock-free. The RE (Ghidra) of each writer must confirm that it finishes its subtree's world update,
  and name the writers that move children without the root (those are not compare-gated).
- **FrameValues samples exactly those slots**, partitioned across the pool at W(N)'s start. Polling remains only for
  entries with no writer event: switches without switch events, and `kMoveAlways`.
- **The extras rows go to the shader**: static parameters written with the record, plus the frame block.
- **Tree-wind skins** become evented, or GPU-animated like the owned trees.
- **`ValidateSlice` becomes a parity check** once the reports show the detach and parent-reason events complete.

**Rejected: capturing transforms or palettes in the handlers.** A writer cannot know it is its subtree's last of the frame
(the writers run in varying order across job threads, and an unhooked one is possible), so sampling at W(N)'s start stays.
The engine's face-morph publications remain the one capture.

The engine's own remaining work that DCLF's hooks time is out of scope; it belongs to the "native work first" direction:
- the sun's full-frustum cull and Accumulate, about 0.11 ms;
- the point lights' Accumulate, about 0.04 ms.

**Phase 6, step 1: the commits' uploads off the render thread** (2026-10-05, p6-base, ab-off/ab-on).

The baseline (bridge, turning camera): 0.79–0.92 ms a frame on the render thread. Each epoch's ORG time split into the
commit (the feature's inputs, about 130 µs a frame over all epochs), and **recording the commit's uploads, one copy
command per staged entry, about 90 µs**. A Tracy capture showed the recording as `RecordCopies`, and a count of the
commits' entries showed where they came from:

| Commit | Entries | Mostly |
|---|---|---|
| Shadow | ~115 | object records (70) and bone rows (28): the frame's first commit sends the scene streams |
| Z-prepass | ~110 | **single 4-byte words of `fade-animated`**, one per animated fade root |
| Colour | 25–38 | object records changed since |
| Reflection | 21 | per-face rows |

What changed:
- **The workers' batches are recorded on the workers** (ORG, persistent-epochs.md, "Recorded ahead by the producer").
  The main and shadow builds record their staged batch where they stage it, and the commit hands the list over with
  `SubmitWorkerBatch`.
- **The fade-animated words go up as one copy**: a CPU copy of the buffer (the GPU only reads it), and the span from the
  frame's lowest root to its highest.
- **The scene streams are staged on the worker** (`IndirectDraws::Impl::StreamsJob`). The job is kicked where nothing
  writes the tables until the next commit: after the placements' join at BeforeShadowMaps, and after
  RefreshFrameConstants at Prepass. It updates the object and bone stores, stages the changed runs, and records them.
  `CommitSceneStreams` submits its batch when the buffers still hold what the job started from, takes its versions as
  held, and sends only what changed since. Otherwise the batch is dropped, and the commit sends everything since the
  held versions, the job's changes included (the stores keep them). Off under the persistent parity, which checks what
  the render thread uploads.

Results:
- **Entries recorded on the render thread:** shadow 115 → 15, Z-prepass 110 → 18, colour 25–38 → 13. Every streams job was
  taken by its commit (600 of 600).
- **Render thread, alternating A/B** (the streams job off and on, two runs each, with the other two changes in both):

  | | Off | On |
  |---|---|---|
  | Total, ms/frame | 0.823, 0.833 | 0.809, 0.750 |
  | Shadow epoch | 0.139, 0.145 | 0.084, 0.078 |
  | Colour epoch | 0.088, 0.088 | 0.078, 0.079 |

  The two kicks cost about 6 µs each. The job runs about 70 µs on the worker, and is done by its join (0.5 µs).
- Single runs are not comparable: two runs of one build differed by more than this step's gain on hooks it does not touch.
  Compare alternating runs.

**Phase 6, step 2: per-frame work bounded by what changed** (2026-10-05, q-streams → q-p6b → q-p6c, Tracy medians).

Before moving the shadow commit, a capture split the shadow epoch's 103 µs. Three parts redid every frame what only
changes with the scene:

- **The scene's draw bound** (`SceneDrawBound`) scanned every object slot (8,300 at the bridge) before each epoch's sequence
  reserve: shadow, occlusion and main, about 10 µs each. It is now kept in `DrawBoundStore` (`Impl::drawBound`), from the
  tables' change log like the record stores: a slot the log names has its share (its draws, and the pipeline slot they count
  toward) taken out and put back. It is resynced by a scan only for new tables, a log it fell behind, or fewer slots. The
  scan stays as its parity under `CS_DCLF_PERSISTENT_PARITY` (0 differ, 0 resyncs at the bridge), and the report prints a
  "scene draw bound" line.
- **The shadow pipeline lookups** (`RefreshShadowLookups`) asked for every key under every raster state of every used
  mode, about 170 requests (12 µs). A refresh that found every pipeline records what it was made for: the pipeline set's
  generation, and each used mode's format, keys and states (`Lookups::shadowPipelinesResolvedFor`). The same again is
  skipped. A missing pipeline clears it, so the next refresh asks again.
- **The views' bucket rows** (`BucketsOfRow`: sort, partition and map the pipelines of a state's map row) were rebuilt for
  each state in each frame (11 µs in the shadow epoch, the same per occlusion view). They are kept per state
  (`Impl::ShadowRowBuckets`) while the lookups (`Lookups::instance`, new on a reset), their shadow generation (which every
  map-row change advances) and the published shadow pipeline version are the same.

| Render thread, µs (median) | Before | After |
|---|---|---|
| Shadow epoch | 103 | 58 |
| … its commit body | 46 | 24 |
| Occlusion epoch | 50 | 31 |
| Z-prepass epoch | 47 | 37 |
| Colour epoch | 36 | 27 |

Shadow views 1800 of 1800 drawn, 0 not ready; bridge screenshots unchanged.

**Phase 6, step 3: per-frame values through the latch, copied by the ticket** (2026-10-05, p6d, p6e).

After step 2, a temporary count of every commit's staged entries showed what the render thread still recorded as copies:
about 75 a frame, nearly all either a counter zeroed every frame or a per-frame block rewritten whole (shadow 15–21, each
occlusion view 4, Z-prepass 18, colour 13, reflection 20). Neither needs a copy recorded at the submission: the values can go
into the latch with a plain store, and the copies from the latch can be recorded with the ticket, on ORG's host thread, since
a latch region's place depends only on the frame slot (LatchBlock: "also valid ... as a copy source").

- **`ShadowLatchedCopiesPass`** (shadow and occlusion epochs) and **`ReflectionLatchedCopiesPass`**, the first pass of each
  epoch: per view (face), its blocks from the latch's region for it to the buffer the draws read, and its draw count and
  bucket counts from a zero source (a one-slot `LatchBlock` never written: `ShadowResources::zeros`,
  `ReflectionResources::zeros`). They declare their targets as copy destinations, so the graph orders them before the
  passes that read them; their revision is the frame shape's generation, which names every copy they make.
- **Layouts:** `ShadowLatchLayout::ViewBlockOffset(slot)` (a view slot's 1280 bytes), `ReflectionLatchLayout::FaceOffset(f)`
  (the face block, its tree LOD row and visible list header). The frames carry the regions' offset and the zeros, both in
  their `SameShape`.
- **The commits** write the values into the latch (`WriteViewBlocks`, the reflection's face loop) and stage none of them.
  The shadow worker no longer stages zeroed counters (`ShadowPayload::stagedSlots` is gone).
- An uncaptured reflection face draws nothing, so its block is copied as the latch holds it.

Results (bridge, Tracy medians, one capture each):
- Copies recorded per commit: shadow 15–21 → 3–5 (SharedData and FeatureData, the tree frame, face positions), occlusion
  4 → 0, reflection 20 → 0.
- Render thread: shadow epoch 58 → 53 µs, reflection epoch 62 → 48 µs; the occlusion epoch read 23 µs in one capture and
  28 µs in another, against 29 before.
- 1800 of 1800 shadow views and 600 of 600 reflection faces drawn; shadows and water reflections unchanged at the bridge.

**The main commits** (Z-prepass, colour) and the shadow commit's SharedData and FeatureData blocks go through one
mechanism, `LatchedUploads`, which a commit calls as it calls its staged uploads:
- A target the epoch's latched-copies pass declared (`Resources::latchedTargets`, published by `MainLatchedCopiesPass` from
  `MainLatchedTargets`; the shadow constants for `ShadowLatchedCopiesPass`) has its bytes written straight into the
  segment's latch block (`Resources::latchedBlocks`, one per segment: both main commits write in the same frame slot, and the
  first one's copies may not have run when the second writes; grown with what is written so far) and a copy in the frame's
  `LatchedList`, part of its shape. Anything else is staged as before.
- A copy is in the list only in a frame that writes it, so a target not written keeps what it holds, as with staging. The
  frame lighting, written only when it changed, is now written by every main commit: a copy written now and then would
  change the shape with it.
- The frame constants were read only through device addresses, ordered after the upload pass by its barrier. They are now a
  graph resource (`cs.dclf.frame-constants`), declared by their readers (both main draws, the reflection faces, the foliage
  parity pass), so the graph orders them after the latched copies.
- Left staged, as the compute queue reads them (a closed execution rejects a resource on two queues): the tree frame row (now
  the Z-prepass commit's alone: only its TreeWindPass reads it), the fade frame row, the fade events' header and the fade
  visibility. Also staged: what changes now and then (the tree and fade root tables, tree LOD's shape and mesh rows, the fade
  root lists), the fade-animated span, the sort's first zeroing, and an inline build's payload.

**ORG**: `IUploadService::RecordStagedUploads(list, slot, afterWork)` now records the barrier that orders the staged copies
after the work before them only when it has a copy. `RecordPendingUploads` recorded one before calling it in every epoch,
copies or not; it was the list's first command, about 2.4 µs. ORG's persistent, copy-queue and Vulkan host tests pass.

Results (bridge, Tracy medians; before step 3 → now):

| Render thread, µs | Before | After | Of it: recording the pending uploads |
|---|---|---|---|
| Shadow epoch | 48.0 | 40.4 | 11.5 → 2.7 |
| Z-prepass epoch | 83.5 | 79.8 | 13.4 → 11.0 |
| Colour epoch | 44.9 | 36.6 | 9.5 → 2.2 |
| Reflection epoch | 55.9 | 38.9 | 19.8 → 4.6 |
| Occlusion epoch | ~29 | 21.5 | |

About 44 µs a frame. Copies the commits record: shadow 0–1 (face positions), occlusion and reflection 0, colour 0, Z-prepass
5 (the fade rows, the tree frame row, fade-animated). Every view and face drawn, no ticket prepared again for a changed list,
screenshots unchanged.

**Phase 6, step 4 (started): what makes a ticket stale.** Reserved submission (`TryReserveReadyEpochs`,
`TrySubmitReservedEpoch`) never waits, and treats a ticket that turns stale after its reservation as a contract violation:
an epoch can be submitted reserved only when its commit leaves its shape as the ticket was prepared for. Today a changed
shape costs a re-preparation the render thread waits for: 300–400 µs each (up to 1.4 ms), 10–20 times in 15 s of a fast
turn. Logged per change, at the bridge, after the pipelines settle (about 5 s):
- **Shadow views that come and go** (a local light's paraboloid pair, 4 ↔ 6 views): two changes when the pair appears,
  one when it goes. **Fixed:** a slot the previous shape had past this frame's views stays in the shape with no work
  (a zero latch: no dispatch, no draw; counters zeroed by the latched copies; its render pass loads and stores), for
  `Impl::kRetainedViewFrames` (600) frames after it last drew. The report counts them ("slots kept with no work").
- **The engine alternating a view's depth range** (viewport max depth 1 ↔ 0.99997 or 0.999985). **Fixed:** the range is
  a value. The view block carries it (`DCLFDepthRange`, PerTechnique c3, written by `WriteViewBlocks`); the pulled shadow
  vertex stage applies it, z' = min·w + z·(max − min) under a [0, 1] viewport, which is the window depth the view's viewport
  gives, with two clip distances (z, w − z) keeping its clipping. The views' states clip depth and never clamp it
  (`ShadowRasterStateId` rejects the others), and depth bias applies after the transform either way, so it is exact.
- **The view's rasterizer state alternating** between cull siblings: DCLF takes a view's state from the engine's last pass
  in it, whose cull mode changes from frame to frame, and each state's pipelines are buckets of their own (50 ↔ 59). Tried
  and reverted: every view drawing the buckets of every state its mode has used (the inactive ones with a count of 0) kept
  the shape, but cost 0.2 ms of GPU a frame in the shadow views (0.574 → 0.770 ms), for a 0.35 ms wait every several
  seconds. Open: choosing the pipeline per draw (an indirect execution set, as the main pass does) would make the state a
  value too.
- The Z-prepass's bucket calls (rare).

Fast turn, 15 s: re-preparations 9 → 6, ticket waits 10 → 7 (one of them a ticket not yet ready). Shadow GPU time
unchanged.

**Recording reuse** (ORG, persistent-epochs.md, "Recording reuse"; `CS_ORG_REUSE_RECORDINGS`, default on). Rather than make
every shape fixed, an epoch's recordings are kept per frame slot and submitted again while what they were recorded for holds;
a ticket the commit made stale takes the kept recording of its new shape (a lock-free read on the render thread) instead of
being prepared again. DCLF's `PublishShape` republishes one of the last four shapes (`RecentShapes`) when the frame is the
same as it, generation and all, so a view state that comes back finds its recording again.
- Bridge, steady: every ticket takes a kept recording (600 of 600 per report), none prepared again; a change makes a few new
  recordings once (13, all kept).
- Fast turn, 15 s: re-preparations 6 → 4, ticket waits 7 → 4; what remains is a variant first met in a slot (the host has
  18 frame slots: 3 frames of 6 epochs).
- The host thread's ticket preparation: median 186 → 43 µs (no recording), 1.34 → 0.42 s of CPU per 15 s.
- Kept recordings have no Tracy GPU zones and no ORG pass timestamps (the first version resolved timestamps into readback
  buffers the statistics service had since recreated: a GPU page fault at startup); turn reuse off to profile on the GPU.
- **A regression the first validation missed, fixed.** With reuse the DCLF set fell from about 8,155 members (387 objects
  waiting) to 7,787 (3,457 waiting), and members waiting for a pipeline never joined. `DrawPipelines` kept three versions of
  each pipeline set and wrote new pipelines only into one nothing referenced. A frame shape holds the version it bound
  (`IndirectState::version`, `ShadowIndirectState::version`), and kept recordings hold their shapes, so every version stayed
  referenced and new pipelines waited forever. `Versions::owned` now grows: a version is made when none is free, and goes
  when the last recording holding it does. Membership is back to 8,152 with 387 waiting. Shape and recording counts alone
  did not show this; the set's report did (members, waiting).

**Phase 6b, step 1: shapes that only change with the scene** (2026-10-05, p6r–p6t, q-cull2).

- **A shadow view's cull state.** DCLF took the renderer's cull mode at the view's hook, which is its last pass's: the
  Utility shader sets it per pass, 0 for a two-sided property and 1 otherwise (skyrim-engine-notes.md, shadow maps). It
  changed from frame to frame with whatever drew last, and each change was a new shape. A view's state is now its table
  entry at cull 1, or at 0 where the table has none (the volumetric copy), as the occlusion views already did. A two-sided
  caster's key draws without culling (`kRasterTwoSided`) whatever the state, as the engine's pass does. This is also closer
  to the engine: before, a view whose last pass was two-sided drew every caster without culling. The cull siblings
  (`PendingView::cullStates`) are gone.
- **The report.** "[DCLF] epoch shapes": per epoch, the frames that kept the published shape, came back to a recent one,
  or made a new one.
- **Results.** Bridge and fast turn: after the first report window (startup, pipelines arriving: about 25–30 new shapes
  per epoch), no epoch's shape changed in any window, and nothing was prepared again. In the startup burst (285 recordings)
  the host thread's work delayed a few tickets: 8 waits, up to 1.5 ms. Moving recording off the critical path is step 2.
- **The viewport as a value** (planned) is not needed for now: with the cull fixed, no viewport change appeared.

**Phase 6b, R3a: immutable inputs (resource versions)** (2026-10-05, r3as2–r3as6, r3an). A scene revision's recordings must
name inputs that nothing changes in place, as BasicRenderer's published state does. Before this, every DCLF growth gave the
same `org::Buffer` a new backing (`ResizeStructured`/`ResizeBytes` in a `MutateBackings` scope): it bumped the host's one
backing version, which made every recording stale, and the mutation waited out the host thread's preparation.

- **Versioned buffers.** Every DCLF buffer that grows is an `org::VersionedBuffer` (`Versioned`, `Versioned.h`): about 45 of
  them, among them the scene tables, the per-object inputs and verdicts, the sequences, the bucket counts, the sort's staging
  and ranks, the index pool's three buffers, the shadow and reflection buffers, and both `GrowableRows` tables. A growth
  publishes a new version at the new size and leaves the old one as it was; DCLF's own "held" versions already sent a new
  backing everything, and reseeded the GPU-written state, so nothing else changed. `NewVersions` (the scope that replaced
  `MutateBackings`) tells the host (`NoteNewVersions`): tickets prepared before are stale, as before, but nothing waits.
- **Declared as resolvers.** Passes declare the versioned buffer itself (`*buffer`): a preparation resolves the current
  version, or the one its revision names (R3c). Writes go to the current version (`->Get()`, `Target`). Latched copies name
  a versioned target by its key, and copy into whichever version their preparation resolved. Versioned buffers are not
  registered under their identifiers: a registration would hold its version for the graph's life, and nothing looks the
  identifiers up.
- **ORG fixes found on the way.**
  - Once a program has resolver groups, its direct hazards were derived in registration order, not the epoch-ranked
    order, and a group phase edge could run against the epoch order: both made cycles. Fixed in `PersistentGraph.cpp`; a
    cycle now names its passes and edges.
  - A resolver use's resource token named the member it had at `Declare`, so after a membership change recording could
    not find it. It now resolves by position, like the use's views.
- **Results.** Tables started small (`CS_DCLF_TABLE_START=small`, r3as6): 36 growths at startup, none waited
  ("0 backing changes"); then 8,152 members, every shadow view and reflection face drawn, shapes stable, nothing prepared
  again, no errors, image normal. Default (r3an): 8,155 members, as before. The startup window still prepares about 137
  tickets again: the live path's reaction to new versions and shapes, which R3c removes.

**Phase 6b, R3b step 1: growths adopted through one point** (2026-10-05, r3bs). "The tables as of a revision" needs no copy of
the tables: the live tables, for the revision's members whose structure has not changed since it, are exactly that, and the
claim machinery (`ApplySet` a frame late, `MemberBindingStands`, `RevokeUndrawnClaims`) already keeps claims and what is drawn
together. What a revision does need is that writes go to the versions its recordings read. So a growth is now a `Growth` (the
next versions, made at the new sizes, `VersionedBuffer::MakeStructured`/`MakeBytes`) adopted through `Adopt`, which makes them
current and then runs the growth's consequences: the held versions reset, the addresses and descriptor indices taken again, the
index pool laid out and copied again. Live mode adopts at once, so nothing changed (small tables: the same 36 growths, members
and draws); R3c adopts a revision's growths when it is selected.

**Phase 6b, R3c (a): shape producers, main and reflection** (2026-10-05, r3ca2, r3ca3). A revision's shapes must be made from
its own inputs, before the frame that draws with them. The main and reflection shapes now come from producers (`Shapes.cpp`):
`MakeMainShape` from `MainShapeInputs`, with `PlanZBuckets` (the Z-prepass's buckets and calls) and `MainLatchedLayout` (the
latched copies, in the commit's order and packing), and `MakeReflectionShape` with `PlanReflectionBuckets`. The commits make
their shapes through them. At the scene work's join `MakeRevisionShapes` makes the same shapes again, from the revision's
inputs and what each segment's last commit captured: the viewport, the frame blocks' sizes, the descriptor heaps (read only
inside an epoch). The shape parity compares each commit with the revision made in its frame and the one made the frame
before (the one `BeginSceneFrame` would select).

- **Made revision-level.** The max counts come from the scene's draw bound, not the frame's draw count: a capacity never
  depends on what a frame drew. The colour pass's cascades and local shadow volumes reserve the latch before the shape is
  made. A frame buffer the mirror cannot fill is latched as zeros, so the latched list does not depend on it.
- **Result** (small tables, r3ca2/r3ca3). In steady play the commits' shapes equal both revisions', every frame, for the
  Z-prepass, colour and reflection. The predicted latched layout was never missed. The only differences are at startup:
  the pipeline set growing (pipelines arriving) and the Z-prepass's calls following the lookups as they resolve, plus about
  one frame in 3,000 where a pipeline resolves between the join and the Z-prepass. Both are R4's: a revision names its
  pipeline set and lookups. Members, draws, growths and warnings are as in r3bs.
- **Shadows and occlusion** (r3ca4). Their views come and go with the engine's: a local light's paraboloid pair, and the
  cascades' rasterizer states. So a revision holds a shape per view layout it has seen, not one shape. `ShadowViewLayout`
  is a view's slot, mode, target, slice, rectangle and rasterizer state. `MakeShadowShape` makes the views from it, from
  their map rows' buckets and from the payload: the modes' and key slots' draws, and the arena's blocks. At the join,
  each recent shape's layout is made again, and a commit is compared with the variant of its own layout. Retained views
  (the slots kept with no work) are made by the producer like the others; their capacities now follow the payload instead
  of staying as they were copied. A union of every state's buckets per view was rejected: each key slot would need a range
  per state, more than the slots' sequences hold (twice the draw bound). Result: steady play 100% for both lags. The
  startup differences are pipelines and buckets. During play, buckets differ 1–3 times in 300 frames, when the payload's
  key-slot draws grow between the join and the epoch (the revision's payload, built for it, removes that). A layout
  appears that no revision has seen only when retained views expire: the coverage case, whose views a revision's frame
  leaves to the engine.

## Implemented foundations

- `ORGModuleServices::AsyncPrimitives` is a backend-independent header-only target.
  It can be configured without the RHI/shader services through
  `ORGModuleServices/cmake/AsyncPrimitives.cmake`.
- BasicRenderer's `SerializedTaskPump` implementation lives in that shared
  package. The old BasicRenderer header remains a source-compatible facade.
- The shared `GraphScheduler` interface owns cancellation scopes and supports
  classified immediate/delayed dispatch and optional tracing. BasicRenderer's
  graph now consumes it through an adapter; the existing constructor preserves
  its scheduler, task-domain policy, and renderer hooks. Cancellation queries
  copied by continuations retain their scheduler state rather than borrowing a
  callback argument. Foreign scopes are rejected using lifetime-stable identities.
- Renderer payload trace interpretation, GPU-buffer retirement notification,
  and ORG upload-ticket conversion now live in `RendererGraphAdapters.cpp`, not
  the graph implementation. Coalescing, exact-content retention, lifetime trace
  classification, and producer admission policy are immutable host metadata; the
  core no longer switches on renderer artifact kinds.
- `ArtifactResources.h` shares the existing immutable payload, lease, readiness,
  and backend-opaque GPU submission contracts. BasicRenderer's public names are
  aliases, not separate implementations. `ArtifactKindPolicy` conservatively
  retains exact content by default; BasicRenderer explicitly installs its
  existing active-list retention policy.
- `ArtifactIdentity.h` and `ArtifactSnapshot.h` share addresses, immutable
  revision/incarnation identities, strong version handles, dependency recipes,
  alternative expressions, snapshots and typed payload handles. These are
  parameterized by the host's kind type; no renderer kind inventory is imported.
  BasicRenderer retains its concrete public names and helper signatures through
  aliases and forwarding wrappers. Requirements remain non-owning metadata;
  snapshots/handles retain explicit leases.
- `GraphSchedulingLayout` supplies queue dimensions, the graph-control class,
  and legacy acceptance class. The core allocates acceptance mailboxes once from
  a validated layout, rather than sizing/indexing them with renderer enums.
  Producer, acceptance, and waiter admission validate class bounds. Invalid
  acceptance classes become failed graph results without running their actions;
  invalid waiter registration returns an empty subscription. The renderer adapter
  installs its original domain/lane choices.
- Renderer trace recording/report generation lives in `RendererGraphTrace.cpp`
  behind an injected trace-session interface. The default renderer constructor
  retains the existing CSV, Chrome trace, and summary formats. Shared scheduling
  construction without a trace factory leaves tracing disabled.
- Fire-and-forget `PostIntents` now queues noncoalescible host kinds as ordered
  exact requests. Explicit latest-value APIs reject them. Previously that posting
  API marked every request coalescible, bypassing transaction protection.
- `ArtifactBuild.h` now shares build contexts/results, requests, suspension
  factories, observation/awaiter registrations, producer scheduling contracts,
  diagnostics and kind-count-parameterized statistics. BasicRenderer retains its
  public names through aliases and supplies the original enum defaults through
  `RendererArtifactScheduling`. Shared defaults use numeric lane/domain zero
  with no admission group. The suspension identity allocator now lives in the
  compiled `ORGModuleServices::AsyncRuntime` target, with the original renderer
  function forwarding to that implementation. It is not duplicated per kind type.
  As before with a statically linked service, cross-DLL users exchanging IDs
  must route through one runtime owner, not independent static copies.
  Registration reset still cancels exactly once and retains its snapshot lease
  until that snapshot is replaced or destroyed.
- Trace configuration, numeric payloads, diagnostic helpers, mutex phases/counts,
  and the trace-session interface now live in the shared package. The session is
  parameterized by host kind/event/report types. BasicRenderer retains its event
  values, renderer report fields, formatting and recorder implementation; no
  renderer-specific trace events were added to the shared headers.
- `ORGModuleServices::AsyncStateGraph` now compiles the existing dependency engine
  independently of BasicRenderer/RHI/ORG. `StateGraph<Binding>` contains the
  original algorithm; `StateGraphTypes` supplies kind, scheduling and trace types.
  The compiled default supports numeric kinds 0..63. Hosts can explicitly
  instantiate their own binding once; BasicRenderer does so behind its original
  `AsyncStateGraph` constructors and hook interfaces. Its destructor drains before
  destroying the wrapper so renderer callbacks cannot outlive that wrapper.
  There is no second renderer implementation of the algorithm.
  The shared target keeps TBB, spdlog and BasicTelemetry as CPU dependencies;
  existing SARP telemetry names remain unchanged for comparison continuity.
  Independent installed packages `ORGAsyncRuntime` and `ORGAsyncStateGraph` expose
  the CPU targets without pulling the GPU module package into consumers.
  This does not yet provide a nonblocking DCLF frame path.
- `DCLF::PublishedSceneExecutor` implements the shared scheduler with one serialized
  coordinator and two dedicated preparation workers, separate from shader and ORG
  recording services. Each domain has a preallocated bounded queue (256 entries
  by default, including delayed tasks), plus at most one/two running tasks.
  Dispatch rejects saturation instead of waiting for capacity or running inline.
  Queue admission uses a short mutex; this is not a wait-free API. Cancellation
  wakes delayed tasks and workers release their captures before scope completion.
  Exceptions are retained for teardown/test waits; scopes retain cancellation
  state after executor destruction. Wait/Shutdown reject calls from its workers;
  the executor owner must outlive callbacks. Shutdown drains only for teardown.
  Statistics expose queue/active/high-water counts and accepted/completed/cancelled/
  rejected/failed work. Optional per-task tracing is not implemented yet.
  The separately built CPU library is linked into CS but not instantiated by the
  live frame path. Task queue bounds do not implement the scene-byte budget or the
  one-building/one-ready/one-pending capture admission policy.
- `PublicationExchange<T>` implements a bounded single-coordinator/single-owner
  handoff with active, ready, and retired slots. Selection does not allocate,
  acquire a mutex, scan the scene, or release the exchange's old payload. A
  nonthrowing acceptance predicate must validate generation and reserve executable
  capacity; the exchange itself does not prove GPU readiness. Rejected candidates
  and replaced active references are reclaimed by the producer. `TryReplace` lets
  the producer take back a successor the owner has not selected yet (latest wins).
- `RevisionAssembler` / `RevisionFragment` (`RevisionAssembly.h`, 2026-10-05, plan R2)
  are the "same update" primitive. A revision names one exact fragment (artifact
  version) per slot; unchanged slots inherit the newest sealed revision's fragments,
  pending ones included, so a newer revision shares work in flight. `Seal` validates
  closure: a fragment's requirements must be the exact fragments in their slots, so
  an epoch's recording is never named beside a layout it was not recorded against.
  A revision is handed to the frame only once every fragment it names is ready, and
  selected whole through the exchange. The newest complete revision wins: older
  pending ones are abandoned then (never cancelled before, so steady change cannot
  starve publication), and older ones completing later are superseded. A failed
  fragment fails every revision naming it; the active one stays and `Collect`
  reports the failure. Fragments settle lock-free from any thread (one exchange of
  their waiter list); revisions count down atomically onto a completion stack and
  notify the coordinator, which collects and publishes. Tests: partial readiness,
  inheritance, closure, stale drafts, supersession, abandonment, latest-wins,
  failure, and a 4-resolver stress run (whole, in-order selections only).
- `LeasedArraySlots<T, N>` permits an external triple-buffer exchange to retain
  immutable payload storage after returning a slot. Writers replace backing only
  when a reader still holds its version. This helper does not synchronize slot
  ownership; the existing face snapshot atomic exchange does that.
- Face snapshots use leased whole-head storage. `ShapeView` and scene-table
  `FaceStream` carry the lease, and the existing main/shadow payload vector copies
  retain it. Head retirement and subsequent morph captures cannot invalidate a
  payload's positions. The usual three buffers are allocated during registration;
  only retained older versions require additional backing.

Shared-package changes are mirrored in the BasicRenderer and CS submodule working
trees. Both must be committed/pinned when delivering the migration.

## Required continuation, in order

The offline capture model now lives in `CapturedScene.h/.cpp`. Its factory copies
member transforms and evaluated component bytes into immutable group pages;
geometry/material are required for each member, with pose/face components required
when marked. Incomplete, duplicated or mismatched components are rejected. Pages
retain version-qualified resource owners and carry source-update, incarnation and
world-generation identities. Resource leases guarantee lifetime, not immutable GPU
contents; the adapter must supply versioned resources or ordered patch semantics.

`CapturedSceneReducer` applies ordered replacement/detach/reset events on the
coordinator. It retains detach tombstones, rejects stale revisions/incarnations and
old-world pages (even in a relabelled envelope), and exposes immutable roots sharing
unchanged group pages. Root-map copying is currently coordinator-side and linear;
this is not the final incremental scene table implementation or render-thread
selection. Lifecycle sequence numbers are global across resets. Allocation failure
leaves the prior root and tombstones intact. There is no coalescing in this reducer.

`CaptureAdmission` now bounds the coordinator's pending envelope by event count
(default 4096) and owned capture bytes (default 128 MiB). The caller supplies other
preparation/staging bytes on each admission. Accounting includes vector capacities,
but excludes allocator overhead and storage behind resource leases; event metadata
has its separate fixed count bound. This is not yet the full pipeline memory budget.
Pressure does not consume the event or its sequence: callers must retain and retry
in order, especially detach/reset/replacement events. One oversized whole group
may enter only with an empty pending envelope and zero other preparation bytes.

Only adjacent `RefreshGroup` events can coalesce. Refreshes must have identical
membership, geometry/material values and resource versions/owners; transforms,
pose and face values may change together. Components are canonicalized so this
comparison is linear in component count plus compared byte sizes. Structural
replacement, detach and reset events never merge. The reducer independently checks
refresh compatibility against its installed group, rejecting mislabeled structural
updates and refreshes without a live base. Coalescing/resource release happens only
on the coordinator, not the render thread. The ingress mailbox, one-building/one-ready
state machine, total resource-budget accounting and pressure-retention integration
remain integration work; the admission class is not yet connected to game hooks.

`CapturePreparation` now joins offline admission and reduction to an explicit
building/ready/acknowledgement state machine. It issues monotonically increasing
build tickets carrying immutable roots and an output-byte reservation. Wrong-ticket
completions cannot alter the current build. Failure, over-reservation results and
rejected ready results permit retry without requiring a new capture. Accepted resets
invalidate ready output immediately and logically cancel older builds; a running
build retains its reservation until completion. Ordered pending input is reduced
transactionally before issuing a successor. The conservative initial policy permits
one building OR one ready result plus pending captures, not simultaneous building
and ready successors. There are no waits in these coordinator methods.

The result is `PreparedCapture`, an opaque CPU artifact, not yet an executable
`PreparedScenePublication`. Its root is charged conservatively in full while
building/ready, plus reserved output bytes. Retained reducer roots, externally held
leases, allocator overhead, GPU resources and ingress storage still need unified
incremental accounting; this is not proof of the complete 128 MiB pipeline budget.
The graph producer/continuation adapters, completion/acknowledgement mailboxes and
frame lease selection remain unwired. Every method, including ready inspection and
acknowledgement, is coordinator-only; render-thread code must not call them directly.

`CaptureService` now hosts that offline state machine asynchronously. Engine-side
test callers post owning capture envelopes through a preallocated ring; a shared
serialized task pump drains them exclusively on the dedicated coordinator. An
injected CPU builder runs on a preparation worker and returns through a one-entry
completion mailbox. A separate ready slot and ticket-checked acknowledgement slot
hand results back without exposing mutable coordinator state. No public post/take/
acknowledgement operation waits for a build; short mailbox/scheduler mutexes remain.
Only shutdown drains workers. The direct builder remains a CPU test seam; the
optional graph backend described below supplies the dependency engine.

Ingress count and capture bytes are independently bounded (one isolated oversized
page permitted). They are not yet part of unified incremental memory accounting.
Rejected posts retain caller ownership for ordered retry. Invalid accepted input or
pump rejection marks the service faulted, rather than silently discarding lifecycle
events. Build failures pause preparation until new input or explicit Retry, avoiding
a tight failing-build loop. Resets retire undelivered old-world output on the
coordinator; already delivered leases still require consumer generation checks and
cleanup-lane release. These CPU mailboxes are not the lock-free frame-selection path
and confer no native draw ownership. Nothing instantiates this service in live CS.

`MakeGraphCaptureBackend` now connects the service to the shared graph through an
asynchronous backend interface, sharing the same dedicated executor. It posts a
typed immutable captured-root artifact, then a preparation artifact depending on
that exact root version. Nonzero build-ticket fingerprints qualify payload requests;
the graph rejects input-bearing requests without them. Preparation executes on the
preparation domain, while exact CPU-ready/terminal continuations report through the
coordinator mailbox. Graph addresses are released after completion, and graph
shutdown occurs before executor teardown. There is no graph wait or snapshot polling
in the normal service path. This currently has only the CPU root/preparation stages,
not shader, import, staging or executable-ticket producers.

`DclfCaptureGraph` is a separately linkable target configured by
`cmake/DclfCaptureGraph.cmake`; the CPU test build enables it with
`DCLF_TEST_SHARED_GRAPH`. It is now linked into `CommunityShaders.dll` through
the module-services CMake integration, but no live DCLF code instantiates the
service yet. Its source remains excluded from the plugin's legacy source glob.

The first concrete component adapter, `CaptureHeadFaceValues`, accepts a leased
`FaceSnapshots::HeadView` with complete shape membership, or the earlier
`ShapeView` span. It copies a versioned CPU header and float4 positions from the
retained owner, validates contiguous ranges and generation, and does not
dereference raw view pointers. The engine-side face morph publication already
captures all shapes of a head into one leased storage version. The live scene
walk now retains one `HeadView` per encountered head and associates it with its
face streams, using raw head pointers only as per-walk lookup keys. Existing GPU
uploads remain unchanged. This is not a complete actor capture or a published
rendering path; the worker adapter is not yet called by live hooks.

The live tracker now assigns monotonic member and actor-group identities through
`SceneIdentity`. Actor siblings discovered under the same `GetUserData()` actor
share a group ID; ordinary objects use their member ID as the group. Detach,
reattach, owner replacement, rescan and world reset allocate fresh IDs, while
table slot reuse changes every journal cause even if numeric draw values match.
The CPU suite tests these lifecycle transitions. These IDs are metadata only:
actor membership is not yet captured as a complete update, and no native
suppression uses them. The actor-key lookup still depends on scene-event order;
publication admission must conservatively reject any unverified replacement
ordering before this can grant ownership.

`GpuResources::Lease` now exposes an already imported D3D11 buffer's stable
Vulkan view, import generation, and owning COM lease without re-importing it.
Geometry slots retain the vertex/index leases and their generations, independent
of the cache's age-based eviction. A later publication can copy those leases
with its numeric geometry values. Import generations distinguish cache eviction
and pointer reuse; they are **not** versions of mutable buffer contents. The
published path still needs explicit ordered content patches, GPU-completion
retirement, and device-lifecycle checks before such a view is executable.

The persistent ORG host now accepts an owned preparation callback through
`TryPostOwnedPreparation` without waiting for mailbox capacity. The callback
runs on its graph-owning async thread with upload and descriptor services active,
outside an epoch callback. A full mailbox, absent async graph, or pending rebuild
leaves the callback with the caller for retry. The Vulkan host test checks
cross-thread service access and owned-input lifetime. DCLF does not yet submit
texture/import requests through this port; descriptor retirement and ticket
qualification remain to implement before ownership can use it.
`OpenRenderGraphPersistentVulkanHostTests` passed in both synchronous and async
modes after this change, including a blocked-consumer test that fills all 64
mailbox cells and verifies the overflow request stays with its caller. The
development CommunityShaders DLL also linked against the updated host. The
mailbox has not yet been exercised by live DCLF imports.

The cold Utility shader's skinned-normal branch used `Bones` even with
`DCLF_BINDLESS`, which deliberately omits that constant buffer. It now calls
`GetBoneRSMatrixBindless` with the captured object bone offset. DXC compiled
both the bindless and constant-buffer VS variants. The shader was backed up as
`build/dclf-profiles/Utility-before-bindless-normal.hlsl` before being deployed.
The first capped game run did not leave loading; a second 55.7-second run reached
534 Tracy frames and logged 33/33 Utility stages ready, zero failed, with no
`Bones` or Utility precompile errors. Its final 400 accumulator scopes measured
p50/p95/p99 614/925/1,119 microseconds (table derivation 519/783/944 microseconds). Those
steady-state values are similar to the previous stationary run and are not an
async pipeline speedup. The trace and CSV are in
`build/dclf-profiles/20260927-164940-utility-bindless-normal-warm/`.

Engine boundary inspection confirms `BuildScenePhase` guards loading-screen
renderer-data lifetime, and `WriteObject` still samples live geometry/property/skin
state. `JoinScenePhase` currently makes the legacy results safe to consume. Removing
those joins before replacing these reads would violate lifetime/thread-affinity
requirements. Remaining capture adapters must preserve that load guard, capture
complete actor membership and evaluated material/skin outputs, and lease GPU inputs.

This is an offline contract plus one engine-safe face capture boundary, not yet a
full-scene engine capture adapter or a parity path. Remaining component schemas,
hook-safe source sampling, actor membership discovery,
stable-ID assignment, graph producers, bounded capture admission and rendered motion
history are still required. The reducer neither proves GPU readiness nor grants
native ownership, and is not called by the live renderer.

The continuation is "The design (2026-10-04)" above, phase by phase; the steps that stood here are folded into its phases.

## Tests

Standalone shared tests build from `extern/ORGModuleServices/tests/Async` and
exercise retained storage through repeated slot recycling and owner destruction,
concurrent triple-buffer exchange, bounded publication admission, rejection,
whole-value selection, retained frame leases, producer-side reclamation, and task
pump notification/rejection/delayed-handoff behavior. They also exercise shared
payload typing/lifetimes, readiness milestones, and opaque upload prerequisites.
Shared identity tests cover unrelated host kind types, ABA generation mismatch,
non-owning recipes versus owning handles, alternative dependencies, and scheduler
layout bounds/index round trips.
Build-contract tests exercise all result/suspension factories, typed dependency
lookup, custom host scheduling enums/defaults, acceptance callbacks, request
status/handle conversion, and move/reset/destruction of observation and exact
wait registrations while retaining their snapshot leases.
The CPU-only runtime suite exercises a custom host trace event/report vocabulary,
numeric payload forwarding, scheduler/mutex metadata, configuration defaults,
diagnostic names and correlation IDs. It also checks 32,768 concurrent suspension
allocations across two translation units; the renderer regression checks that
legacy and shared allocation calls use the same counter.
These do **not** establish
actor grouping or GPU/native correctness in the game; that integration is pending.

Also run BasicRenderer's `TaskSchedulerManagerTests` and `AsyncStateGraphTests`
after changes to the shared pump facade or scheduler adapter, and the existing
DCLF CPU tests. Graph tests additionally exercise scheduler injection, retained
cancellation, delayed work, foreign-scope rejection, exclusive trace-sink
ownership, immutable host policy, and ordered noncoalescible posted revisions.
The graph suite also runs a one-lane/one-domain configuration, validates legacy
acceptance routing, and rejects invalid producers, acceptance actions, waiters,
and malformed startup layouts.
Trace registration now preserves an existing sink when a competing
registration is rejected; its event-emission path remains atomic/hazard-based.

Identity/scheduling extraction validation: `AsyncStateGraphTests` and
`TaskSchedulerManagerTests` each passed ten consecutive runs; the shared
primitive tests passed twenty consecutive runs. `BasicRendererPublicHeaderSmoke`
and `SARPRendererHostCore` built. Source/ABI audits passed and the renderer boundary
audit reported zero violations. The build-input
audit reports only the seven new, explicitly listed adapter/diagnostic files as untracked;
they must be included when committing. No game runtime/performance validation was
performed for this extraction step, and the deployed game DLL was not changed.
An initial invalid-waiter assertion used a test executable linked before the
final library rebuild; relinking against the final library resolved it, followed
by the ten-run graph regression pass above.

Build/request/continuation extraction validation: shared tests passed twenty
consecutive runs, including custom host scheduling traits and registration
lifetimes. Graph and scheduler tests each passed ten runs. Public-header smoke
and `SARPRendererHostCore` built successfully. Source/ABI/boundary audits passed;
the build-input audit still reports only the seven untracked adapter files above.
Both shared-package copies are byte-identical for this slice. No game launch,
deployment, performance capture, or DCLF runtime enablement was performed.

Trace/identity extraction validation: both CPU-only suites passed twenty runs;
graph/scheduler suites passed ten runs each, including report-format, bounded
trace, concurrent-stop, and legacy/shared identity namespace checks. Public-header
smoke and `SARPRendererHostCore` built. Source/ABI/boundary audits passed. The
build-input audit still flags the same seven untracked adapter files, not missing
source-list entries. Mirrored additions match byte-for-byte; the package's
tracked CMake/README contents match after newline normalization. No game run,
deployment, or performance claim accompanies this structural extraction.

Engine extraction validation: the independently linked numeric-kind graph tests
passed twenty runs, and an installed-package-only consumer of that same suite
also passed twenty runs. These build trees configure no BasicRenderer, RHI or ORG
targets. Coverage includes exact dependencies/retention, posted coalescing,
noncoalescible lifecycle requests, milestone continuations, failure propagation,
cancellation and shutdown. The existing renderer graph and scheduler suites
passed ten runs each against the shared implementation; public-header smoke and
host-core builds passed. Both primitive/runtime suites passed twenty runs each.
Source/ABI/boundary audits passed; the build-input audit continues to flag only
the seven previously untracked adapter files. GPU streaming-count and game
performance/visual validation remain outstanding; no game deployment was made.

Dedicated executor validation: `DclfKeptStateTests`, `DclfSceneExecutorTests`, and
`DclfSceneGraphTests` each passed fifty consecutive runs. Executor coverage includes
independent coordinator progress under two blocked preparation jobs, queue bounds,
delayed cancellation, foreign scopes, coordinator ordering, exception propagation,
worker-wait rejection, retained cancellation queries, and worker-side capture
destruction before shutdown completes. The installed-package graph integration
test verifies exact two-component dependency gating with reordered completions
while unrelated coordinator requests continue. This is not Skyrim actor-group
validation. The executor library also built through the actual CS Ninja build;
the generated plugin source list excludes its separately compiled translation
unit. No full plugin build, deployment, game run, or performance claim accompanies
this slice.

Offline capture validation: all four DCLF CPU suites passed thirty consecutive
runs. New tests cover all 24 geometry/material/pose/face completion permutations,
rejection of incomplete groups, deep-copy isolation from source mutations,
resource-owner retention, immutable old roots, detach/reattach ABA, stale source
updates, reset generation checks and coordinator-worker lifetime. These use
synthetic component bytes, not live Skyrim input; same-capture rendering parity
and game correctness remain unvalidated. The CPU library built in the actual CS
Ninja configuration. No full plugin build, deployment or game run was performed.

Admission validation: all four CPU suites passed thirty runs after adding tests
for pending-entry/byte pressure, retry without sequence loss, whole-group refresh
coalescing, ordered detach and structural replacement, isolated oversized admission,
external preparation-byte accounting and rejection of structural updates labelled
as refreshes. The CPU library also built through the actual CS Ninja configuration.
No live rendering or performance change is claimed by this slice.

Preparation-state validation: all four CPU suites passed thirty runs with the
state machine included. Tests cover late/wrong-ticket completion, immutable worker
inputs while newer captures coalesce, ready acknowledgement, failure/rejection retry,
reservation overrun, stale-world completion and ready invalidation. A real executor
worker is artificially delayed across a reset and its result is rejected without
starting overlapping preparation. The library built in the actual CS Ninja tree.
No game deployment, full plugin build or runtime performance validation occurred.

Mailbox-host validation: all five CPU suites passed thirty consecutive runs. The
new service suite covers preparation on a non-caller thread, reset during blocked
work, immutable retained output, stale/duplicate acknowledgements, explicit retry
after builder failure, visible invalid-input failure, post-shutdown rejection and
idempotent teardown. The actual CS Ninja configuration also built the CPU library.
No full plugin build, game run, deployment or performance claim accompanies this
offline integration.

Graph-backend/face-adapter validation: all six CPU suites passed thirty runs after
fixing the adapter's missing request fingerprint. The initial graph service test
failed before entering its builder; DebugMCP could not launch the C++ test, so the
cause was traced to the graph's explicit MissingFingerprint rejection contract and
verified by the passing graph-backed regression suite. Debugger breakpoints were
cleared. Face tests cover mixed generations/owners, truncated ranges, header/layout
contents and independence after source storage expires. The executor/service/face
library built through the CS Ninja configuration; the graph adapter built against
the independently installed shared graph. No game launch, deployment, full plugin
build or GPU publication validation occurred.

Plugin-link/whole-head validation: the shared graph target now links into the
actual `CommunityShaders.dll` build, which succeeded after the face-stream bridge
was added. `DclfCapturedSceneTests` checks complete-head copying and rejects
missing ranges; all six DCLF CPU suites passed once after this bridge. The build
has not been deployed or run in-game. The standalone 512-group synthetic capture
benchmark reported post-call p99 between 0.1 and 1.5 microseconds and completion
between 0.20 and 0.45 milliseconds across five direct/graph trials each. That is
only service overhead, not a same-save game A/B or evidence that rendering work
has moved off the render thread. An existing demanding-save Tracy baseline showed
`CS DCLF: accumulator tables and pipelines` at p50 620, p95 1404, p99 24280
microseconds over its last 400 frames; the live code still has that work.

Identity bridge validation: the six CPU suites passed after adding actor sibling,
detach/reattach, pointer-reuse, owner-change and world-reset cases. The live
scene table carries each member and group ID beside its persistent object slot;
the legacy renderer ignores these metadata columns. No new in-game timing or
visual result is claimed for this bridge.

Geometry-lease runtime validation: both the development Ninja and multi-target
Release plugin DLLs built after the import-lease change. One MO2 run reached
gameplay but its early Tracy recording ended during loading; a second run was
stopped at 42.9 seconds with a 12.1-second gameplay capture (947 Tracy frames,
943 accumulator scopes). DCLF logged 9,373 stable game buffers and zero rejected
imports, with main, depth and shadow epochs continuing. The final 400 accumulator
scopes measured p50/p95/p99 678/913/1,133 microseconds; table derivation was
572/779/970 microseconds and `BuildMainPayload` 529/649/739 microseconds. The
earlier demanding-save baseline had 619/1,404/24,280 microseconds for the
accumulator scope, but its camera moved while this capture's camera stayed fixed;
these are not a controlled A/B and do not prove an optimization. Shader
precompile logged several unsupported Utility VS `Bones` errors at startup,
while runtime reported 33 requested/ready shadow stages and no failed stage
requests. No visual or GPU debug-layer validation accompanied this capture.
The trace, per-zone steady-state CSV and game log are in
`build/dclf-profiles/20260927-163309-async-lease-gameplay/`. The deployed DLL's
SHA-256 matched the Release build after the run; the previous deployed DLL was
backed up as `build/dclf-profiles/CommunityShaders-before-async-lease.dll`.

ORG ticket-reservation foundation: `PersistentGraphHost` now offers
`TryReserveReadyEpochs` and `TrySubmitReservedEpoch`. A render-thread caller can
check that every required epoch has a current, complete ticket and retain those
exact tickets without a ticket wait, graph build, or inline reprepare. Submission
consumes each ticket once; an unexpected post-reservation revision change throws
and schedules async replacement instead of waiting or silently omitting a claimed
draw. The existing blocking `SubmitEpoch` path remains for legacy callers.
The synchronous and asynchronous Vulkan host tests pass, including bounded
owned-request admission, reservation, single consumption, resource refresh, and
rebuild refusal; the CS development DLL links. This is not yet a publication-
qualified reservation: DCLF does not use it to decide native ownership, and
ticket readiness alone is not proof that a scene publication can render.
Each reserved epoch now retains a control-mailbox credit. Owned preparation
cannot consume those credits, and the Vulkan test submits a reserved epoch while
the worker is blocked and every other mailbox cell is full. The legacy path
still may wait for mailbox space; a publication reservation cannot mix with it.
The reserved path also still records pending uploads on the render thread.
Worker-recorded bulk uploads and publication-qualified ownership remain separate
steps; neither is claimed by this test.

Live ownership boundary audit: `DrawcallLimitFix::BeginSceneFrame` runs from the
early `Main::Draw` hook before the primary cull and before native pass
registration. `PassCapture::Withhold` consults the currently published main and
shadow claim sets during registration, while `IndirectDraws::PublishClaims`
replaces main claims only after the colour epoch, for the following frame.
Consequently a published-mode frame lease must be selected at or before
`BeginSceneFrame` and install its compatible claim sets there; selecting it at
`EarlyPrepass` or `BeforeDeferredComposite` would be too late to make native
suppression and DCLF execution agree. The existing live flow still joins the
scene job in `AfterShadowMaps`/`EarlyPrepass` and builds accumulator tables
synchronously in `EarlyPrepass`.

Live claim-lease prerequisite: `PassCapture` now atomically installs one
immutable main+shadow claim bundle after `BeginSceneFrame` processes scene
events and before registration begins. The legacy claim producers still
publish for the following frame; the registration hooks and sun-shadow claim
query read the selected bundle throughout this frame. Feature-off and load
handling clear the bundle (loads also clear all shadow claims), handing
registration back to native rendering. This does not yet install claims from a
prepared scene publication or reserve its tickets.

The Release DLL built and a 45-second MO2/Tracy run reached the demanding save.
Its final 300-frame report had 2,514 passes withheld, 2,514 objects claimed,
zero claimed-but-undrawn objects, and zero ownership holes over 300 frames.
Colour, depth and shadow worker builds were used in all 300 reported frames,
but the legacy joins remained: average reported colour wait was 0.225 ms,
shadow 0.036 ms and depth 0.008 ms. The last 400 Tracy accumulator scopes
measured p50/p95/p99 648/826/992 microseconds, with 1,249 microseconds max.
This was not a controlled same-save baseline comparison and no async speedup is
claimed. Trace and log: `build/dclf-profiles/20260927-171924-frame-claims/`
under the SARP workspace. The active MO2 DLL was backed up to
`build/dclf-profiles/20260927-frame-claims/CommunityShaders-before-frame-claims.dll`
and replaced with the matching Release DLL.
An earlier deployment command in this run also overwrote the DLL in MO2's
disabled `Community Shaders` mod with the development build. Its intended backup
directory could not be created by that command, so the exact prior disabled-mod
DLL was not preserved. The active `Community Shaders DXVK ORG` mod was untouched
by that command and was separately backed up as noted above.

Accumulator derivation boundary: the live `BuildAccumulatePhase` now measures
its parts separately from `BuildScenePhase`. A profiled 300-frame interval in
the demanding save put about 0.23 ms in static classification, 0.15 ms in
frame classification, and 0.22 ms in the old combined record section. The
record section is now split into an engine-affine value capture and a
pointer-free `AccumulatePatch` applied to the scene table; the latter does not
dereference a geometry, shader property, material or pass. The same save's
profiled interval reported about 0.10 ms in patch capture and 0.17 ms in
value-only application. The application is **still synchronous on the render
thread**; this extraction establishes a capture/prepare boundary but neither
publishes immutable tables nor shortens the normal-frame critical path.

The extraction made the extras-watch bit follow the newly captured projected-UV
and land-blend flags, rather than the prior object flags. Residency joins are
included in the before/after membership journal. A 45.5-second unprofiled MO2
run after the extraction reached gameplay and logged zero ownership holes,
zero claimed-but-undrawn objects, and zero validation drops in its 300-frame
windows. Its last 400 accumulator scopes measured p50/p95/p99
646/886/1,093 microseconds versus 648/826/992 microseconds in the earlier
same-save, stationary-camera frame-claim trace. The tail is worse and this is
not an optimization result. The two new captures and logs are under
`build/dclf-profiles/20260927-173655-accumulator-patch-split/` and
`build/dclf-profiles/20260927-173844-accumulator-patch-unprofiled/` in SARP.
These runs did not exercise residency or visual correctness; that requires a
targeted save and remains open. The remaining expensive classification and
pipeline/material slot mutation still read live engine data and must move to
coordinator-owned, versioned scene state before the value-only patch can run
off-thread.

Event-driven capture work (September 27): the live scene store now keeps a
material-to-slot reverse index. Material write events visit dependent slots
instead of scanning the entire material table. Existing controller hooks now
detect actual changes to emissive multiplier/colour and texture-transform
buffers; the former wake property dependents at Prepass, and the latter wake
the short transform watch across the engine's two buffer phases. Actor wetness
is sampled once per stable actor group and fanned out to its mesh slots. The
shared Skin cache now resets a reused form ID when its actor instance changes
and prunes only idle entries instead of clearing every actor's fade together
at 1,024 entries.
LightLimitFix exposes the nearest room identity separately from its transient
room number; tracked objects cache the identity on attachment and refresh the
number when the light room map is rebuilt. The room map is still rebuilt by
the engine feature each frame.

Shadow texture and pipeline membership is maintained from per-slot dirty
contributions, with reference-count-equivalent slot sets and a full rebuild
when the index is invalidated. The same-table incremental/rebuild parity check
(`CS_DCLF_PERSISTENT_PARITY`) reported no mismatches in the final capped run;
the final three 300-frame windows also reported zero ownership holes. The
normal demanding-save run reported shadow-set maintenance at 7.5 us/frame
versus 50.9 us/frame in the preceding same-save build. Wetness was 63.2 versus
121.9 us/frame and material writes 8.3 versus 14.5 us/frame. These telemetry
intervals are similar but the save evolves during each capture, so they are
directional, not a controlled per-frame A/B. Texture lookup now skips the
per-register comparison when the material record version is unchanged;
volatile targets and import completion remain on the existing epoch path.
The final Tracy capture and game log are under
`build/dclf-profiles/20260927-183410-shadow-dirty-normal/`; parity capture is
`build/dclf-profiles/20260927-183312-shadow-dirty-parity/`.

After adding per-zone Tracy measurements and the material-record version
shortcut, another normal run (`20260927-183853-event-zones-final`) measured
p50/p95/p99 in microseconds: material writes 8.5/11.0/13.5, wetness
65.7/89.8/108.0, texture transforms 10.1/14.3/17.3, and shadow-set
maintenance 26.8/44.2/58.5 per invocation. The final 300-frame interval
reported 20.9 us/frame of shadow maintenance at 0.7 calls/frame; more scene
changes than the prior run explain why this is above its 7.5 us/frame. The
accumulator scope measured 816/1,182/1,437 us versus 847/1,436/1,796 us
in the earlier same-save normal run; scene motion and runtime variation mean
this is not an isolated effect size. The final parity run
(`20260927-184006-event-final-parity`) reported zero shadow-index mismatches,
zero material-frame or shading mismatches in five checks each, and zero
ownership holes. All game runs stayed below 48 seconds.

Remaining work for this event-driven plan: actor input/event coverage and
incarnation-keyed wetness retirement on lifecycle events; completion-driven texture/import leases;
static-classification writer coverage; and typed immutable capture records on
the publication coordinator. Actor shading, alpha and visibility still use
conservative current-frame sampling where writer coverage is incomplete.
The live pipeline remains legacy and still joins preparation in normal frames.

Event-owned lookup use (September 27 follow-up): pipeline and material use is
now emitted by the derivation and residency writers as exact per-frame slot
lists. The pre-epoch known-texture refresh consumes a material-record change
journal rather than scanning allocated slots. A change is retained until that
slot is first used, so writes before later admission are not lost. Epoch
material and pipeline lookup preparation and frame pipeline-constant sampling
also consume the use lists. The material-write path no longer scans the entire
material cache for orphan entries: a successful cache insertion is immediately
followed by slot allocation, and each slot-removal path erases its cache key.
On parity frames, the use lists are compared with the previous full-table
discovery. LightLimitFix now changes its room-map generation only when the
room-to-index mapping actually changes, avoiding unconditional DCLF room
rebinding. The comparison map is reused across frames.

The 45-second demanding-save parity run at
`build/dclf-profiles/20260927-191943-iteration-free-parity/` logged no use-list
disagreements, no material-frame or shading parity differences, and zero
ownership holes and claimed-but-undrawn objects in its reported 300-frame
windows. A 12-second Tracy capture (907 frames) measured
`CS.DCLF.RefreshKnownMaterialTextures` p50/p95/p99 at 1.33/2.54/3.42 us over
1,807 calls; the preceding same-save parity capture
(`20260927-190426-event-revert-final-parity`) measured 3.99/6.64/9.96 us over
1,816 calls. This removes discovery over unchanged material slots, not the
required epoch import work. The persistent-shadow input parity discrepancy
already tracked separately remains present; this run recorded no new
shadow-set index disagreement. Shader/material/actor writer coverage and
completion-driven descriptor leases are still required before all persistent
state discovery and age-based maintenance can be removed. Camera-dependent
inputs, active wetness fading and live pose/visibility capture still require
bounded current-frame work; they cannot be inferred from yesterday's events.

Descriptor retirement follow-up: ORG's descriptor service now exposes an
atomic slot-plus-owner retirement operation. DCLF places the imported image
and retained D3D11 SRV in that owner; both are released only after the same
submitted queue fences that permit slot reuse. This removes the 16-frame
image graveyard, whose fixed duration was not a GPU-completion guarantee.
The inactivity-triggered cache eviction is still age-based. It cannot become
reference-count-only until every publication and prepared execution ticket
retains its exact descriptor owners, and device/graph rebuild invalidates
cached indices. The new lease covers retirement of an evicted entry, not
all future publication/ticket ownership. The first capped demanding-save run
(`20260927-193949-descriptor-completion-parity`) reached a GPU device loss at
frame 581, before any >600-frame texture eviction could exercise the new
operation. Parity was clean up to that point except for the pre-existing
persistent-shadow membership discrepancy; the run is inconclusive for the
retirement change. Its Aftermath dump is
`gpu-crash-2026-09-27-19-40-29.nv-gpudmp` in the Skyrim SKSE log directory.
The periodic all-import sweep has also been replaced by one deadline per
cached view. `BeginFrame` processes only expired deadlines, and an entry still
in use is rescheduled when its deadline arrives. This removes the steady
whole-cache eviction scan without changing the conservative 600-frame
inactivity policy.
The second 45-second parity run
(`20260927-194439-descriptor-expiry-parity`) completed without device loss,
past 1,190 main epochs. Both reported 300-frame intervals had zero claimed
but undrawn main objects and zero validation drops; the final shadow interval
had zero not-ready views under ownership. The first interval had 24 such
shadow views during cold pipeline readiness, the same count seen in the
preceding `20260927-193208-iteration-free-bitmap-parity` baseline. Material
frame and shading parity checks found no difference; the pre-existing
persistent-shadow membership discrepancy remained. The four DCLF CPU tests
passed. A focused ORG fence-retirement test was added, but its D3D12 test
target is disabled in this Vulkan-only build. A separate test configuration
stopped before compilation because DirectX-Headers is not installed in that
configuration, so that test is not yet executed.

The 1.6.1170 float and colour material-controller hooks now compare the exact
destination field around the engine update and emit a material event only for
an actual write. Ghidra's controller updates at `0x14150DDE0` and
`0x14150EA50` establish the destination-index tables and their early exits;
other runtimes retain the conservative event. A second 44.5-second parity run
(`20260927-192504-iteration-free-controller-parity`) logged no material-frame
or shading mismatches, no use-journal disagreements, and zero ownership holes
in its reported 300-frame windows. It does not prove coverage of other
material writers or actor inputs.

A material-change journal was tested for the pre-epoch known-texture refresh.
It needed to retain changes to slots that became used later in the frame;
same-table parity exposed this and passed after a pending-slot fix. Its measured
refresh cost was about 4.1 us/call in one parity run; the version-aware scan
measured 4.5 us/call in the final parity run, with substantial run-to-run
variation. The journal added state and had no demonstrated net benefit, so it
was removed from the live path.

The final Release DLL (SHA-256
`7A58E9A6A565690507729CFD76FD1E9EFD8E1DC0CE6EB24244F6DDD26E2CC545`)
was deployed to the active `Community Shaders DXVK ORG` MO2 mod, with the
pre-change DLL backed up under `build/dclf-profiles/20260927-event-capture/`.
The final 44.5-second parity run is
`build/dclf-profiles/20260927-190426-event-revert-final-parity/`. It logged zero
shadow-index disagreements, zero material-frame or shading mismatches across
five checks each, zero claimed-but-undrawn objects, and zero ownership holes
in the final 300-frame interval. The four DCLF CPU tests passed. Neither these
tests nor the demanding-save run cover interior portal rotation, equipment
changes, water transitions, or GPU debug-layer validation.

After the residency journal correction, the final Release DLL passed another
44.5-second capped run: the last three 300-frame windows logged zero ownership
holes, zero claimed-but-undrawn objects and zero validation drops. One window
handed 15 withheld passes back to native. The scene changed substantially during
this capture (about 2,500 to 3,560 claimed objects), so its final 400-frame
accumulator p50/p95/p99 of 855/1,298/1,455 microseconds must not be compared
as a stationary performance A/B. Trace and log:
`build/dclf-profiles/20260927-174240-accumulator-resident-journal/`.

### Reference-driven material and descriptor retirement (2026-09-27)

The duplicate `SceneStore::materialCache` has been removed. Persistent material
slots already contain the evaluated records; `materialIndex` supplies reuse.
Each live slot now owns its engine material reference directly. Last-object
reference transitions are consumed after the complete reference-journal batch,
without the material slot's previous age grace period. Reacquisition within
the batch cancels retirement, and incarnation checks reject stale events.
Table reset releases these owners too; previously the duplicate map survived
`ResetSlotTables` without slots through which its references could retire.

A game-material destructor hook cannot replace this ownership rule: DCLF's
own retained reference can prevent that destructor from running. Ghidra's
SetMaterial (`0x14147BFF0`) and material-manager release (`0x1414F7A40`) paths
confirm replacement releases the old material through the engine reference
count. No new executable-address hook is installed for retirement.

ORG descriptor heaps no longer poll a retained-index list during allocation.
The final CPU lease callback returns a GPU-retired slot to the free list.
Lease generations prevent an older callback from reclaiming a newer lease.
`RetireDescriptorSlotWithOwner` now transfers backing ownership into the slot
when GPU completion precedes CPU lease release, closing the previous gap
where only the descriptor index, but not its imported backing, stayed alive.
Backing destruction happens outside the heap lock.

Four DCLF CPU tests pass, including same-batch reacquisition and slot reuse.
The fence/backing lifetime tests now run in both persistent Vulkan host test
modes (two passing tests); they are no longer limited to the unavailable
D3D12 test configuration. They cover multiple CPU consumers, GPU completion
before final lease release, immediate backing destruction on final release,
and descriptor-slot reuse without polling.

The legacy `CS_DCLF_MATERIAL_CACHE` switch now controls validation only:
`off` disables the diagnostic, `probe` compares every used material slot,
and default retains the bounded eight-record sampler. There is no second
material cache to disable. Geometry/pipeline age retirement and GpuTextures'
600-frame inactivity deadlines remain. Removing texture expiry safely still
requires explicit ownership for every publication, shadow packet, frame patch,
and executable ticket that retains descriptor indices. This change does not
claim that coverage or remove the remaining engine-affine samplers/joins.

The deployed Release DLL SHA-256 is
`4B7C92D8BB8C8A9FCE1EE65C890C38F7713BD66DEA5077E705FDB8E625AAFFD6`.
The final normal-sampler parity run (`20260927-200844-material-ownership-final-parity`)
lasted 44.99 seconds. It reported zero stale sampled materials, no material-frame
or shading mismatches, no main claimed-but-undrawn objects, and no device loss.
The pre-existing shadow-membership discrepancy and first-window 24 not-ready
shadow views remain; the later window had zero not-ready views. Material slot
retirement reached 1,325 while live slots fell from 587 to 438; the two logged
last-frame evaluation counts were 0 and 2, not an aggregate churn measurement.
Accumulator p50/p95/p99 were 824/1,225/1,623 us (845 samples), versus
899/1,308/1,590 us (808 samples) in `20260927-194439-descriptor-expiry-parity`.
These same-save rotating runs are not stationary A/B measurements; p99 did
not improve and no performance win is asserted. The intermediate
`20260927-200333-material-ownership-parity` run used an earlier build and is
not validation of the final duplicate-cache removal.
The separate all-used-material probe
(`20260927-200936-material-ownership-full-probe`, 44.56 seconds) reported
486/487 validated records in its logged frames, zero stale materials, zero
material-frame/transform/shading differences, and zero main claimed-but-undrawn
objects. The same existing shadow issues remained. Neither run constitutes
interior/equipment/cell-transition coverage or GPU debug-layer validation.

### Writer-maintained actor output membership (2026-09-27)

Wetness propagation now uses `ActorValueIndex`, owned by the scene tables.
Object writes attach/update incarnation-qualified member/group IDs; object
reset removes membership and the last member immediately retires the group.
Dense table resets clear the index, while retained-table frames keep it.
No per-frame actor-to-mesh hash map is reconstructed. Capture samples one
representative per actor at the existing safe boundary. Only a changed output
fans out over all members; unchanged actors visit only newly joined or
explicitly reinitialized mesh rows. Updates consume a complete scene-writer
batch and publish the same output to every affected part before payload build.

The index contains no engine pointers and copies independently for table parity.
Skin still owns shared native/DCLF once-per-frame fade advancement. Position,
water, death and stamina sampling, Skin's legacy cache retirement, and actor
shading sampling remain; this is not complete actor writer coverage. No new
engine-address hook is needed for membership because the existing scene writers
already observe the necessary object lifecycle transitions.

CPU tests cover unchanged-output elision, multipart fan-out, equipment join,
reinitialized rows, representative detach, actor/slot incarnation replacement,
last-member retirement, reset and independent snapshots. Runtime parity checks
membership and output against the original actor list using the same frame's
cached Skin outputs. Tracy plots expose sampled actors, changed actors and
visited mesh rows under `CS.DCLF.Wetness.*`.

Scene-record rewrites now retain the wetness row when both member and group
incarnations match. Previously every rewrite zeroed it, defeating changed-only
propagation. New/replaced identities still initialize their rows and are admitted
as pending members. Group/member arrays are dense; writer-only reverse lookup
and swap removal avoid node-based traversal in the capture loop. The tests also
compare 1,000 randomized event batches against full same-input derivation.

Intermediate validation: `20260927-201621-actor-index-parity` (45.14 s) passed
membership/value checks but still propagated almost every rewritten row.
After retaining those rows, `20260927-201935-actor-index-retained-parity`
(44.99 s) propagated 245–288 of about 1,450 mesh rows in later checks with zero
differences, but ended in a GPU device loss. Decoded Aftermath evidence in
that directory reports `Error_DMA_PageFault`, without a fault address or shader
attribution. The earlier 19:40 dump has the same error class, not proof of the
same root cause. The retained pre-change control
`20260927-202143-actor-index-control` (44.54 s) and candidate repeat
`20260927-202245-actor-index-retained-repeat` (44.50 s) both completed without
device loss. The repeat's wetness checks and main ownership checks passed.
The node-based candidate was slower (wetness 78/137/329 us p50/p95/p99 versus
57/82/124 us in that control), motivating dense storage before promotion.
The intermittent GPU fault remains unresolved; a clean repeat is not a fix.

The dense candidate (`20260927-202552-actor-index-dense-parity`, 44.54 s)
completed without device loss, membership/value mismatches or main ownership
holes. Later checks visited 225–320 of 1,448–1,462 mesh rows, sampling
177–179 actors. Wetness p50/p95/p99 were 51/89/312 us; this scope includes
the new periodic full wetness parity check, so its tail is not comparable to
the prior build without that check. Accumulator timings were 805/1,170/1,416 us.
Existing shadow-membership disagreement remains tracked separately.

Final normal-mode comparison (parity disabled for both, same demanding save):

| Zone (us p50/p95/p99) | Retained control | Dense actor index |
|---|---:|---:|
| Wetness | 50.5 / 70.4 / 106.8 | 49.7 / 72.6 / 85.2 |
| Accumulator | 645.6 / 948.3 / 1,132.4 | 712.6 / 1,028.1 / 1,186.5 |

Control: `20260927-202807-actor-index-control-normal` (44.28 s, 955 wetness
samples). Candidate: `20260927-202703-actor-index-dense-normal` (44.56 s,
934 samples). Both completed without device loss or main claimed-but-undrawn
objects. The candidate claimed about 2,622 objects versus 2,512 in the control;
these evolving-scene runs are not a workload-identical A/B. Median wetness cost
is effectively flat; the structural result is removal of the unconditional
per-mesh wetness pass, not a demonstrated overall frame-time win.

### Aftermath and texture-lease audit (2026-09-27)

The previous `DXVK_DEBUG=crashanalysis` default was not a mode recognized by
this DXVK build. `hang` is the mode that enables device faults and NVIDIA
checkpoints. A second precedence bug meant that the modlist's
`dxvk.enableDebugUtils=True` selected capture-only before `hang`; the explicit
hang mode now wins. DCLF imported textures receive distinct image/SRV
names. The Aftermath marker callback resolves DXVK's integer checkpoint IDs
to their saved command labels, without waiting for the checkpoint mutex if a
fault has stalled another thread.

The first corrected, 55.57-second-capped run
(`20260927-204751-aftermath-diagnostics-v2`) logged checkpoint support and
resource tracking enabled. It hit a GPU fault during loading, before the
first colour epoch. Its dump identifies a fragment shader instruction error
at GPU PC `0xB410`; the DXVK hang log brackets a native `DrawIndexed(8991,1)`
between completed checkpoint 22037 and started checkpoint 22040. That is a
much narrower lead, but not yet a proven root cause or proof that it is the
same fault as earlier DMA page faults. The checkpoint-string bridge was built
after this dump. A second capped run (`20260927-205429-aftermath-marker-bridge`,
55.65 seconds) reproduced the same fragment instruction fault during loading.
Its decoded dump contains readable `DrawIndexed (8991, 1)`, `EndRendering`,
and barrier checkpoints, with the bound native `vs.4cddedc` / `fs.e17aede`
IDs. Shader source mapping is still absent; the dump identifies the offending
GPU work, not the source expression or invalid resource. Neither run reached
DCLF's first colour epoch, so the evidence does not implicate a DCLF draw.

A control run with `DXVK_AFTERMATH_DIAGNOSTICS=0`
(`20260927-205750-aftermath-config-off-control`, 44.28 seconds) reached the
demanding save and then reproduced an `Error_DMA_PageFault` at GPU VA
`0x25068EB000`; the dump had no usable checkpoint for that submission. The
two diagnostics-enabled runs instead faulted during loading. This small
sample does not establish causality, but enabling the heavier driver features
by default would be unsafe without isolating their effect. The checked-in
default therefore kept `DXVK_DEBUG=hang` (device fault details and native
checkpoints; since 2026-09-30 opt-in with `CS_GPU_CRASH_ANALYSIS=1`, because the
hang mode serialises concurrent command recording) and leaves `VK_NV_device_diagnostics_config` resource tracking,
shader debug info and shader error reporting behind
`DXVK_AFTERMATH_DIAGNOSTICS=1`. Those features are probed before use.

The combined toggle has since been removed. The three independent opt-ins
are `DXVK_AFTERMATH_RESOURCE_TRACKING=1`,
`DXVK_AFTERMATH_SHADER_DEBUG_INFO=1`, and
`DXVK_AFTERMATH_SHADER_ERROR_REPORTING=1`; the last is off by default.
The diagnostic build must have `AFTERMATH=ON` (the `Dev-Fast` preset has it
off). ORG pass labels now also emit producer-qualified NVIDIA checkpoints
when the adopted device enabled the checkpoint extension. The CS resolver
keeps their immutable text in a bounded, non-reused array, and DXVK's hang
decoder recognizes those external IDs rather than indexing its own ring.
`CS_DCLF_DRAW_TRACE=1` and `DXVK_DRAW_INDEXED_TRACE=1` opt into a filter for
8,991-index native draws. It correlates the original CS-hook caller and up
to 16 stack addresses with DXVK's draw arguments, shader names, index/vertex
buffer bindings, targets, and a checkpoint carrying the batch ID. The
command-stream log maps each contributing draw ID to its index in that batch;
normal DrawIndexed batching remains enabled. The opt-in logging is still for
diagnosis, not performance measurements.

With `AFTERMATH=ON`, isolated 45-second resource-tracking and shader-debug-info
runs (`20260927-213116-aftermath-resource-only` and
`20260927-213354-aftermath-shader-debug-only`) both reached the demanding
save. Shader-error-reporting alone (`20260927-213220-aftermath-shader-error-only`)
reproduced the loading-time instruction fault before the first DCLF colour
epoch. Its DXVK checkpoint sequence has candidate ID 1 immediately before
the failing `DrawIndexed(8991, 1)` with `vs.4cddedc` / `fs.e17aede`;
the original caller is SkyrimSE.exe RVA `0xE465AD`, which Ghidra maps to
`FUN_140e464d0`. That function binds an R16 index buffer and vertex stream
then draws `triangleCount * 3` indices. The captured R16 buffer is 17,982
bytes, exactly the 8,991 indices requested, so a simple index-buffer bounds
overrun is not supported by this evidence. Candidate ID 1 is the exact CPU
API call associated with the checkpoint, but the fragment shader's source
fault remains to be found. These single runs do not establish that either
safe flag can be defaulted yet, nor connect this loading fault to the later
DMA page fault.

Texture-import expiry was *not* removed by this audit. `GpuTextures` retains
SRV/import references and releases them after 600 inactive frames when no
material lookup owner pins the entry. Main/shadow payloads, frame-texture
patches and ORG prepared tickets still store raw descriptor indices; no shared lease follows all of those
indices through ticket submission and GPU completion. Replacing the expiry
queue with immediate last-lookup retirement would permit slot reuse while an
older payload or ticket still names it. Keeping every import forever would
avoid that race but be unbounded. The required change is to attach exact
descriptor/backing leases to each immutable publication, its frame patches
and executable ticket, and return them through the completion-driven host
retirement lane. The current same-frame joins and mutable lookup table mean
that publication-wide owner does not yet exist. This remains a blocking
dependency of removing the old texture-expiry path; this entry does not claim
the full async migration or sampler removal.

The ORG host now has a completion-owned handoff for a reserved ticket's
immutable resource owner. After submission, its async worker observes the
accepted queue timelines and releases that owner and upload staging even when
no later frame reuses the slot. An uncertain completion retains them through
device teardown. The Vulkan host test covers release after GPU completion with
no following frame. DCLF does not yet attach texture binding blocks to that
handoff. Legacy async `SubmitEpoch` now accepts the same explicit resource owner
and retires it on GPU completion without subsequent frames; the Vulkan host
test exercises both paths. DCLF does not yet supply that owner, and synchronous
execution still needs the same contract. The 600-frame expiry therefore remains
in use. Material lookup slots now hold imported binding owners until replacement
or slot-retirement events, so their former 32-frame restamping rule was removed.
Projected textures and per-pipeline shadow masks also retain owned bindings;
pipeline slot expiry releases its mask owner. Lookup versions advance on owner
replacement even if the numeric descriptor index is reused. Unchanged,
already-owned shadow masks skip descriptor resolution on subsequent epochs.
Shadow diffuse lookup bindings now follow shadow-contribution membership
events. Removed textures release their lookup owners; new or unresolved
textures enter a bounded-to-actual-pending retry set. This replaces the
unconditional per-shadow-epoch scan of the full texture set. Frame patches,
payloads, and submitted executions still need exact ownership before the
expiry queue can be deleted.

The `20260927-222450-material-binding-versioned` game run crashed in the
NVIDIA Vulkan driver while `ORG.Upload.RecordStagedUploads` recorded a
`vkCmdCopyBuffer` for sky occlusion. There is no new `.nv-gpudmp` for this
CPU access violation; `CommunityShaders.dmp` captured it. The debugger
found source offset `0x300`, destination offset `0x5dc000`, and length
`0x64000`; both ranges are within their RHI buffer sizes, and the command
buffer was recording. The same build completed the next capped run through
frame 963, so the cause is not yet attributed to the material-lookup change.
BasicRHI now checks copy ranges and recording state before calling Vulkan;
these crash arguments pass the new guard, which is hardening rather than a
fix for this incident.

The MO2 `Default` profile enables `Community Shaders DXVK ORG` and disables
`Community Shaders`. Captures from `20260927-223308-copy-bounds-guard` through
`20260927-231747-shadow-binding-deployed` used the older DLL in the enabled
mod, despite successful local builds; they must not be used to validate or
time the newer ownership changes. The enabled mod now contains the matching
RelWithDebInfo DLL/PDB (DLL SHA-256
`0DC5E1FD2C5E43F7380AC59D1FBBB0F9079D5BC5293EE59B02B47DAEB5CA2B31`);
its previous DLL/PDB are backed up under
`build/dclf-profiles/active-mod-before-shadow-binding-20260927`.

The first verified active-mod capture,
`20260927-232053-shadow-binding-active-mod`, completed in 47.05 seconds.
`CS.DCLF.RefreshShadow.Textures` measured p50/p95/p99
0.63/0.98/1.16 microseconds over 670 zones. During the captured steady-state
window the event and pending-import plots were both zero and 263 shadow
texture owners remained live. The older enabled DLL had measured roughly
23 microseconds median in this zone. The matching parity run
`20260927-232450-shadow-binding-active-parity` reported no incremental
shadow-dependency reconstruction disagreement and zero missing shadow
textures. Its 80/96 persistent-shadow-input differences are the previously
tracked shadow-membership discrepancy, not a new texture-lookup parity result.

All four CPU tests pass. No equipment/water/cell-transition scenario or GPU
debug-layer run is claimed by these demanding-save captures. Remaining next
steps are actor-value notification coverage and lifecycle-owned shared Skin
state, resource leases for every descriptor consumer, static-classification
writers, and migration of captured derivation to the publication coordinator.

## Follow-up: camera-driven material-slot churn (open investigation, 2026-09-28)

Later binding-ownership work removed the 600-frame texture expiry queue. A
camera-moving demanding-save capture then showed 20,106 `t0`/`t1` lookup
"changes" in 12 seconds. Every old view was null, and all 1,440 observed
material/pass/register groups used one unchanged SRV each. The high-frequency
paths were ordinary static textures such as Whiterun windows, pine foliage,
landscape dirt/cliffs, bread, and cabbage. This is slot churn, not evidence
that those texture files or SRVs are changing. The same run reported 8,100
material slots retired on last reference over 300 frames while roughly 620
were alive at the reporting point.

`SceneStore::SweepSlots` calls `materialSlots.DrainUnreferenced` immediately;
the next appearance of a material/pass allocates a new slot and rebuilds its
lookup owners. The churn is seen when moving the camera. Investigate whether
frustum/cull-driven loss of object bindings is being interpreted as the
material leaving the *scene*. A zero current-frame reference count must not
by itself mean that a still-attached object's material or its immutable
binding block should be retired. Trace object binding transitions, material
slot references, and attach/detach events for the same object and material
under camera rotation before changing retirement policy. Preserve native
handback and bounded ownership; do not restore a frame-age expiry queue as a
shortcut. `CS_DCLF_TRACE_TEXTURE_PATHS=1` enables the detailed Tracy identity
and path probe for this investigation; it is off in normal runs.

The source-level path makes this hypothesis concrete: `LapseAccumulated`
resets a still-tracked object's material and pipeline indices when native
registration did not renew its patch; `UpdateSlotReferences` then counts only
objects without `kObjectNoBindings`; `SweepSlots` retires the material on its
last such reference. Native registration is visibility-dependent, whereas
material membership should follow persistent object/property identity. The
next probe should confirm that this chain accounts for the measured churn,
including whether the same material key is reacquired when the camera turns
back; it has not yet been established as the sole cause.
