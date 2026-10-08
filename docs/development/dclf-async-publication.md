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
about 0.05 ms a frame. (Step 5 moved them to the coordinator, with the releases handed back, and found the crash's cause:
"Step 5: ingestion".)

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

**Phase 6b, R3c (b): the scene revision, assembled, recorded and selected** (2026-10-05, r3cb1, r3cb2). The epochs still
draw the commits' shapes; the revision is made and recorded beside them, counts only.

- **Passes prepare for a revision.** Every DCLF pass reads its shape through its preparation (`CurrentFrame`,
  `CurrentShadowFrame`, `CurrentReflectionFrame`, and the index pool's latch, all with the `PassPrepareContext`). A
  preparation whose host data carries `RevisionShapes` gets the revision's shape; a live ticket gets the published one.
- **Versions.** Every versioned buffer registers itself (`VersionRegistry`), and an adoption counts as a change. A
  revision's `VersionSet` snapshots the current versions only when the count moved, and is the recordings'
  `IResourceVersions`.
- **Assembly** (`SceneRevision.cpp`, `AssembleRevision`, at the scene work's join). It uses an `org::async::RevisionAssembler`
  with eleven slots: the versions, a shape per epoch (Z-prepass, colour, shadow variants, occlusion variants, reflection),
  and a recording per epoch.
  - A shape fragment is kept while `SameShape` holds.
  - A recording fragment requires exactly its epoch's shape and the versions. While neither changes, the next revision
    inherits it; otherwise it is requested on ORG's host thread, one recording per shape (each shadow variant has its own),
    with `EpochRevisionData` (the versions and the epoch's shape) as host data.
  - A recording that fails for some inputs is dropped from the revision and not requested again until they change.
  - ORG: `RequestEpochRecording` now takes live async epochs as well, recorded only and never admitted (persistent-epochs.md).
- **Selection.** `BeginSceneFrame` collects and selects the newest complete revision (`SelectRevision`).
- **Result.** Startup: about 25–30 recordings per epoch over the first 300 frames, all made, none failed: every DCLF epoch's
  recording for a revision is replayable. In steady play a recording is requested about once per 300–1,000 frames: a
  Z-prepass bucket call after a lookup resolves, or a shadow variant. Each revision is sealed, published and selected one
  frame after its join. The live epochs were unaffected: 0 ticket waits after startup (5–6 in the first interval, as
  before), nothing prepared again, and the members, draws and warnings as in r3ca4.

**Phase 6b, R3c (c), part 1: the revision's recordings drawn where it covers the frame** (2026-10-05, r3cc3).
`CS_DCLF_REVISIONS` (on unless 0) lets each epoch's commit, its shape made, submit the selected revision's recording
instead of its own ticket's preparation (`ChooseRevisionRecording`; ORG's `UseEpochRecording`). The revision covers the
epoch when:
- its version set is still current (`VersionSet::changes` against `VersionRegistry`), so every buffer its recording reads
  is the one the commit wrote;
- it has the epoch's recording;
- its shape for the epoch, or one of its shadow variants, is the commit's shape (`SameShape`), so the latch blocks,
  layouts and addresses the commit wrote are the ones the recording reads.

Otherwise the ticket's own preparation draws, as before, and the commit counts why.

- **Result** (small tables, r3cc3). Startup interval: about 90% of every epoch's submissions are the revision's. The rest
  are "no revision" for the first 3–5 frames, and "shape differs" (22–27) while pipelines arrive faster than recordings
  follow. Steady play: 100% for colour, shadow, occlusion and reflection; the Z-prepass is 296–300 of 300 (a lookup
  resolving between the join and the epoch). The live tickets: 0 waits, nothing prepared again. Members, draws, the
  parity reports and the image are as in r3cb2.
- **Found and fixed on the way.**
  - The first run crashed on ORG's host thread, in a live preparation's poll of a slot's backing. The slot named a
    versioned buffer's old version that nothing held any more: the index pool had grown six times in one commit, and
    each growth dropped the version before it. A latent R3a race, fixed in ORG: slots own their group members.
  - The index pool now grows once per batch of acquires (`reserveFor`): a cell's load used to double it once per
    acquire that did not fit.
**Phase 6b, R3c (c), part 2a: commits write into the revision's shape** (2026-10-05, r3cc4–r3cc6). A commit no longer needs
to make the same shape as the revision. When the selected revision covers the frame, the commit writes its values into the
revision's shape:
- its latch block, through the layout the shape now carries (`PassFrame::latchLayout`, `ShadowFrame::latchLayout`,
  `ReflectionFrame::latchLayout`);
- its bucket plan and map (`PassFrame::zPlan`, `ShadowFrame::rows`, `ReflectionFrame::map`);
- its latched copies, through `LatchedUploads`' layout mode. A value the list lacks is staged, which is still correct, and
  a copy the frame did not write copies zeros.

Then it submits the revision's recording; its own shape is still made and published for the live path. "Covers" is now
decided from what the values must fit, at the start of the commit:
- **Main:** the heaps and pipeline layout; the viewport and cull mode; the max counts holding the frame's draws; the latch
  holding the cascades, shadow volumes and bucket slots.
- **Reflection:** the targets, tree LOD and pipeline slots.
- **Shadows and occlusion:** the variant of the frame's view layout, with the same map rows, push addresses and slot
  buffers; a latch holding the states, key slots and sun processes; capacities holding the payload's draws, per view and
  per bucket.
- **An older pipeline set is accepted when it holds the frame's claims** (`RevisionHoldsClaims`). Sets only append (the
  versions of `DrawPipelines`), and the frame's claims were committed at the join before the revision was made, so a
  revision made at that join or later has every pipeline they draw with.
- A pipeline slot the tables gained since the revision maps to no bucket (none of its draws is the frame's).
- **Result** (r3cc6). Startup misses: Z-prepass, colour and reflection 3 each (from 22–27); shadow and occlusion 25 (their
  rows change while shadow pipelines arrive). Startup re-preparations: 136 → 81. Steady play: every main and reflection
  frame, and shadows 294–300 of 300. The shadow misses are a bucket outgrowing the revision's when the caster set crosses a
  power of two (the revision's payload sizes them), and a layout the revision has not seen when a retained view expires.
  No value was staged for lack of a copy; the image, members and draws are as before.
- **Next (part 2).** Strict revision-driven epochs. The commit writes its values into the revision's shape instead of
  making its own. Growths are adopted when their revision is selected. Structural stamps revoke a changed member's claim.
  What the revision does not cover is decided before the engine draws. Then the epochs' own preparations go.

**Phase 6b, R3c (c), part 2b: shadow capacities from the scene, claims that follow the revision** (2026-10-05, r3cd1–r3cd4).
- **Shadow capacities from a scene bound.** `DrawBoundStore` keeps, beside the draw bound, every draw an object of the
  scene can cast per shadow or occlusion mode and caster key (`ShadowShare`, `modeKeyDraws`), from the same change log
  (an object's share is taken out and put back only when it changed). `ShadowBoundsOf` turns it into per-mode and per-key-slot
  draws through the lookups' key slots, and `MakeShadowShape` sizes a view's max count and buckets from it (and the payload,
  which stays within it). The capacities therefore change with the scene's objects, not with the frame's casters. Parity
  against a scan (`CS_DCLF_PERSISTENT_PARITY`, r3cd2): 79 checks, 0 differ.
- **Claims gated on the selected revision.** `BeginSceneFrame` selects the revision first. The last commit's set becomes the
  frame's claims (`ApplySet`) only once the selected revision was made at or after that commit
  (`IndirectDraws::SetApplicable`); until then the last claims stand, and the commits in between merge into one application
  (an entry's geometry is the latest commit's). The frame's claims are therefore always within the selected revision's set,
  so its pipelines hold them (`RevisionHoldsClaims` compares the claims' commit with the revision). Needed with it:
  - `ApplySet` derives the joined and left main claims from the claims it changes (merged join and leave lists were
    ambiguous across held commits);
  - the reflection's one-frame lag reads the claims in effect (`Tables::setPhases`), not the last commit's.
  Without a revision ever sealed (no resources yet) the set applies as before.
- **Shadow coverage by the key slots drawn.** The revision's map rows must route every key slot the frame's payload draws,
  not equal the frame's rows: a state's row only gains pipelines while shadow pipelines arrive, and the commit writes the
  revision's rows.
- **Result** (r3cd4, small tables). Startup misses: Z-prepass, colour and reflection 7 (no revision 3, versions moved 4,
  no "pipelines moved"); shadow and occlusion 7 (from 25). Startup re-preparations: 81 → 34. Steady play: every epoch 300 of 300,
  but one interval with 4 shadow frames of a view layout seen for the first time. Up to 19 frames at startup and 2–4 an
  interval kept the last claims while their commit's revision was recorded. The image is unchanged.
- **Findings for the rest of part 2.**
  - *Coverage cannot be released at the join for the main phase.* The frame's list filter is installed at frame begin; a
    member released at the join whose root the filter left out is drawn by nobody that frame (`PrimaryCull`'s "lost while
    out"). The main phase's coverage must be settled at frame begin, which the claims gate does; at the join only phases whose
    engine registration comes later (shadow, occlusion, reflection) can still be released.
  - *Deferred adoption needs every consumer bounded by the version it targets.* A growth made at the join is needed because
    the frame's tables outgrew the current versions; deferring its adoption to the revision's selection leaves the frame's
    tables larger than the buffers its uploads and per-object dispatches write. Each such writer (record, bone, geometry,
    tree and fade rows, tree LOD tables, face positions, per-object visibility and inputs) must then write and dispatch within
    the current version only, and the revision's shapes must take the pending versions' addresses and capacities. The
    alternative is R4's: the revision's buffers are filled when the revision is made, not by the frame's commits.
  - The remaining misses: "versions moved" (a growth, startup and cell loads only), "no revision" (the first frames), and a
    shadow view layout seen for the first time.

**Phase 6b, R3c (c), growth as graph work, G1: the shared groundwork** (2026-10-06, r3cg1). Decided: growth is handled as in
SARP/BasicRenderer (`BuildVersionedGpuBuffer`): a graph producer makes the next version on a worker and fills it through a
dedicated uploader for non-patch graph work; the version is ready once those copies complete, and its owner adopts it when the
revision naming it is selected. The shared pieces:
- **DXVK** (interop version 4): `DxvkOrgInteropDeviceInfo::uploadQueue`, a transfer-capable queue DXVK and the compute queue never
  use (the next queue of the transfer-only family, else of the compute family). `enableQueue` keeps a family's largest count.
- **BasicRHI**: `AdoptedVulkanDeviceInfo::spareQueues`, queues the host created and never uses; `Device::CreateQueue` hands them
  out on an adopted device (one whose family supports the kind; offered again when destroyed), and their families join every
  Concurrent resource's sharing list.
- **CS**: the upload queue is offered as a spare queue, not as BasicRHI's copy queue, so the graph's own copy passes stay in
  DXVK's stream. Its submissions come from the uploader's thread and go to the queue directly under DXVK's submission lock: what
  they write is a version no queue has used, current only once its copies completed. The DXVK DLLs are staged with
  `tools/stage-dxvk-dlls.ps1` (the CS build does not).
- **ORG**: `UploadManager` gives `CopyQueueUploadService` a queue of its own (`CreateQueue(Copy)`, else the primary copy queue;
  `IUploadService::HasDedicatedStreamingQueue`); `VersionedBuffer::Make*` may run on any thread (versions made without an ECS
  entity, as pooled backings are); `WorkerOwnedDestination::PendingVersion`; `PersistentGraphHost::RetainUploads`. Test
  (`PersistentVulkanHostTests revision`): a version made on another thread, filled through the uploader on the spare queue,
  holds its contents once its ticket completes.
- **ORGModuleServices**: `VersionedBufferGrowth.h`, `GrowVersion` (make, fill, readiness token) and `TokenForTickets`.
- In game (r3cg1): `[ORG] Queues: graphics 0:0, compute 2:0, upload 1:1`; the graph's copy queue stays 0:0; every epoch from the
  revision in steady play, no errors.

**G2, first slice: the main rows' growth as graph work** (2026-10-06, r3cg4). The main material and pipeline rows
(`GrowableRows`, `ReserveMainSequences`) grow on the scene graph in revision mode; every other growth still adopts at once.
- **The producer**: `SceneArtifact::BufferVersion`, registered by `Published::SceneGraph` on the preparation pool.
  `SceneGraph::PostGrowth` posts an exact request (its own address, fingerprinted) and an `AwaitExact(GpuReady)` on the
  coordinator lane, both lock-free. The producer calls `GrowVersion`, whose new `contentsOf` lays the rows out for the made
  version's own address (`CapturedMainRows`, the patch `EmitMainRows` applies). `TokenForTickets` marks its token
  authoritative: the uploader notifies every ticket transition, so the graph needs no recovery polling.
- **`Growths`** (render thread): the outstanding growths. `Settle` (at the join, before the shapes) takes in what the graph
  finished. A ready growth's version is what a revision made then names: `VersionSet::Snapshot` overlays it, taking a
  version-registry value of its own (`VersionRegistry::next`), and `GrowableRows::RevisionAddress` gives the shapes its address.
  While one is pending, no revision is sealed (`growthPending`), so the commit's claims wait (`SetApplicable`).
  `SelectRevision` adopts the growths the selected revision names (`AdoptNamed`): the version becomes current, the table's
  capacity, address and generation move, and it holds the captured rows' version (`held`), so only rows changed since are sent.
  When the selection makes exactly the set's versions current, the registry takes the set's value: no "versions moved" miss.
- **Rows past the current version wait**: the build skips a pair past either table (`Skip::Capacity`), and the emission stops
  at the tables' capacity without advancing `held`, until the adoption. The resident region's cached verdicts include the
  capacities (`PipelineWitness`), so they resolve again after a growth. The commit's hard failure on rows past the tables is
  gone.
- In game (r3cg4, small tables): both growths requested, filled on the upload queue and adopted at selection (`growths as graph
  work: 2 requested, 2 adopted`). No join had to wait. Steady play is 300/300 on every epoch except one interval with 4 shadow
  frames (a first-sighting layout, as before). No capacity skips; image normal. Startup still has 3–5 "versions moved" per
  epoch from the growths that still adopt at once.
- Next: the other growth sites (scene tables, sequences, latches, index pool, shadow rows and buckets) as requests. A fill comes
  from a kept capture, or there is none for buffers rewritten whole; their writers and per-object dispatches are bounded by the
  current version (G3).

**G2/G3: the main sequences, the scene tables and the object buffers as graph work** (2026-10-06). A change is an owner's
sizing (`MainSizing`, `SceneSizing`, `ShadowSizing`) and the buffers that grow with it (`Growths::Change`), adopted with the
revision that names it; revision shapes take the revision's sizing (`RevisionShapes::scene`, `SceneSizingOf`). The writers
and per-object work are bounded by the current version (`SceneFit`, `ObjectFits`; bounded emission whose `held` waits), and
passes size dispatches and copies by the sizing their recording was made for, never the live one.

*The device loss this exposed, and its cause (ORG).* Deferred, the main sequences lost the device within ~200 ms of their
first growth, every run (and the scene tables, intermittently, through the bone rows); every change adopted at once never did.
Not the deletion queue (8x its retirement depth still lost it) and not where versions are made. A probe of the main pass found
the Z-prepass recording of a revision binding the **old** 64-draw sequences while drawing by the **new** layout (16384 draws,
512 per decal group): an indirect fetch ~190x past the buffer, with no shader active. ORG lowers a pass's resolvers as groups,
re-resolved by every preparation under its capture context (a revision's versions), or as direct entries, bound to the set
resolved when the pass was lowered. A pass declaring one resolver two ways (the Z-prepass's sequences: indirect arguments and
an address read) failed the patch recipe (its two declared templates merge into one requirement) and was lowered direct, so a
revision's recording took the revision's layout with the live version. Fixed in ORG:
- `PrepareIncrementalResolverPatchRecipe`: the members' merged requirements are the recipe when the declared templates merged,
  so such a pass is a group (z.depth, depth-phase2, shadow.view, sky.view and reflection.faces were the direct ones).
- Fail loudly: `PersistentGraph` records the resolvers each pass binds directly with the set they resolved to, and every
  preparation checks them under its own context; one that resolves another set throws ("binds a resolver's resources
  directly ... this preparation resolves another set"), never a GPU fault. With the fix disabled it fired on the first
  recording after the growth (r3cv1) and the graph stopped cleanly.
- Test (`PersistentVulkanHostTests revision`): a pass declaring a versioned buffer two ways, recorded for a revision naming a
  pending (unadopted) version, copies that version. Without the fix the test fails at the check.
- In game, every growth deferred (main rows, sequences, scene tables, object buffers): 0 device losses and 0 check failures in
  r3cf1-5 and r3cz1-3 (before the fix: 3 of 3 lost with the sequences deferred); 6-7 growths requested and adopted per run;
  steady play submits every epoch by the revision (300/300 Z-prepass, colour, shadow, occlusion, reflection).
- Versions are made by the producer again (`GrowVersions` on the preparation pool; render-thread making was a guess at this
  loss): r3cw1-2 clean, 7 growths per run. `VersionGrowthRequest::version` and `Growths::Post`'s `a_deferrable` are gone.
- What still grows at once in revision mode is reported by buffer ("adopted at once: ..." on the growth line). At startup (r3cg4b):
  the shadow sequences, bucket counts, material rows and view blocks, the reflection sequences, counts and tree-visible lists,
  and the index pool (indices, firsts, copies) - once each, the cause of the 2-3 "versions moved" per epoch left at startup.

**G4: every growth a request; per-view coverage** (2026-10-06). In revision mode nothing grows at once any more ("adopted at
once" is empty; small tables r3cp3-4: 0 "versions moved" at startup, every epoch 300/300 in steady play, screenshots normal).
- *Shadow* (`ShadowSizing`): the slots' sequences (one size for every slot; a slot added while a growth is pending is made at the
  size asked for), the bucket counts (a shape's rows are trimmed to the sizing's words, keeping every bucket the payload's own
  draws need: `TrimmedRow`; a claimed draw past them is logged as a defect), the material rows (deferred with nothing to fill: a
  new version is sent whole after its adoption; a worker-staged batch is used only if the rows' generation has not moved), and
  the view blocks (`viewSlots`). Live shapes size by the current sizing, revision shapes by the revision's (`ShadowShapeInputs::
  sizing`, `materialRows`, `viewBlocks`); the scene bound only sizes ahead within the slot.
- *Per-view coverage*: `ShadowViews::Rebuild` marks DCLF's views (`covered`): its candidates in the engine's order within the slots
  its buffers hold now (`IndirectDraws::ShadowViewCapacity`, which also asks for the slots the last frame's candidates need). Only
  they are withheld and captured; the rest the engine draws whole (`uncovered`, counted: 30 at startup with small tables, 0 after).
- *Reflection* (`ReflectionSizing`): sequences, bucket counts and tree lists, asked for at every join from the main sizing's
  newest change, so both are adopted with the same revision; the faces' buffers are made at the plan's size; a commit past them
  is a logged skip. The graph rebuild after new tree lists stays in the reflection epoch (`rebuildPending`).
- *Index pool* (`PoolSizing`): room for every geometry the tables have (a bound kept from the geometry log at the join), a first
  index per slot and a copy per range; adoption lays every range out again in the new version (`relayout`). A range that does not
  fit until then waits (`waiting`; unclaimed geometry: the claims' was asked room for); a fragmented pool asks for a relayout.
- *Graph builds* (ORG): a recording is of the build it was made on. `PersistentGraphHost::BuildGeneration` moves with every build,
  and a revision's version set (and so its recordings) is made again when it moves; buffer creation no longer counts as a version
  change (no revision before names the buffer). `PersistentGraphHost::CanUseEpochRecording` (RenderGraph::
  CanBindPersistentTicketRecording: the bind's rules, the ticket unchanged), asked inside the commit before it writes into a
  revision's shape: a recording the ticket would not take is a miss ("not its ticket's"), never SubmitEpoch's throw. Found when the
  reflection cube's import rebuilt the graph between a shadow epoch's choice and its submission. Test: `PersistentVulkanHostTests
  revision` (a live epoch's recording refused after its graph is built again).
- Left at startup: frames before the first revision, "not its ticket's" after the startup builds (new view slots, the reflection
  extension), and with default tables one "versions moved" from growths made before the async epochs start.

**R3b structural stamps; strict epochs: a frame's claims always within a revision of the graph that runs** (2026-10-06; small
tables r3se2-3, default r3sd1: no losses, steady play 300/300 by the revision, every report "<- OK", screenshots normal).
- *Structural stamps.* A new change cause, `kChangeStructure`, noted (by `Tables::CausesBetween`, and by the writers that change
  those columns directly) when what a revision's shapes are made from changes: the record's and the draw's pipelines, the geometry
  half and partitions, the shadow and occlusion techniques and rejection. Not flags, materials or the fade distance (bindings
  counted those: 14-35 needless revocations an interval). `RevokeUndrawnClaims` takes back, at the join, every claim whose slot
  had one since the selected revision's join: read off the revoke cursor before `ApplySet`'s own notes (`NoteStructureChanges`)
  and at the join. The engine draws it until a revision made after the change is applied. 25 at startup, 0-9 an interval after.
- *Explicit builds* (ORG). `PersistentGraphHost::SetExplicitBuilds`: an extension added or removed waits for the caller's build
  point, `BuildIfRequested`; until then every submission runs the graph as built (its passes resolve only what they declared, so
  the state an extension grew meanwhile - new view slots, a new import - is unused). `EpochRecording::buildGeneration` and
  `EpochRecordingCurrent`. DCLF's build point is the first thing `BeginSceneFrame` does, every frame (`IndirectDraws::BuildPoint`).
  Mid-frame no build can invalidate the frame's recordings. The one exception is new main resources (resize, Light Limit Fix
  toggled), built at once by the colour epoch, the frame's last, as before.
- *Frame coverage.* `IndirectDraws::DecideCoverage`, after `SelectRevision`: the selected revision covers an epoch when it has its
  recordings, all of the graph as built now, and its versions are current. Without the main epochs' the frame has no claims:
  `SceneStore::WithdrawSet` takes every claim back as if every member left (records, `PassCapture`'s set with nothing drawn,
  `PrimaryCull`'s admission, the sun exclusion), and tree LOD is the engine's. The next covered `ApplySet` applies the whole set
  again (`setGeometryNext`: the geometry each slot was last decided for). So frames before the first revision and after a build
  are the engine's: 9-10 at startup over 6 builds, none after. Their epochs still commit (counted "a frame without claims"): the
  revision shapes are still made from the commits' observations (viewport, block sizes, heaps, the shadow layouts drawn), so the
  epochs cannot be skipped until the shapes come from revision inputs alone (R4).
- *Shadow, occlusion, reflection.* Decided before the engine draws them: no shadow views (`ShadowViewCapacity` 0) and no occlusion
  map while a build is pending or the frame has no claims; the shadow maps are imported at that point, not at the view's capture.
  The faces are withheld only when the claims stand and the cube is imported into the graph as built
  (`PassCapture::SetReflectionCovered`); else the epoch skips ("the engine's").
- *Recordings for an epoch not submitted* (ORG). A live epoch's recording waits for the slot its ready, unsubmitted ticket holds;
  an epoch the caller stopped submitting (the faces while not covered, no water in view) held it for good, and no revision
  published. Strict epochs turned this latent stall into a deadlock: no revision, so no coverage, so no reflection epoch.
  `PersistentGraphHost::ReleaseEpochTicket` hands that ticket back (abandoned, re-prepared); the build point releases the
  tickets of epochs the last frame did not submit (`RenderGraphRuntime::TakeSubmittedSegments`) while a revision waits. Tests:
  `PersistentVulkanHostTests revision` (explicit builds; a recording completing for an epoch not submitted).
- Left: the first sighting of a shadow or occlusion layout and the reflection's first shape (own preparation, 2-4 at startup,
  one 4-frame shadow layout per run); the own-preparation path itself goes with R4's shapes from revision inputs.

**R4, first step: the main epochs' shapes from revision inputs; a growth's fill on its own queue's family** (2026-10-06; small
tables r4v1-3 with a forced mid-run growth and spawned NPCs, default r4d1: no losses, steady play 300/300 by the revision).
- *Main shape inputs without a commit.* What a revision's Z-prepass and colour shapes take from the frame's capture - the viewport
  (the main pass's depth range) and the frame blocks' sizes - is recorded in `RunEpoch` before anything is submitted. Block sizes
  are the largest seen, so a revision's latched copies hold every block a frame binds (a smaller or absent block is zeroed past its
  bytes, never staged). The heaps are the graph's own (`PersistentGraphHost::Descriptors`, new), not a commit's. The main latch
  holds a cascade and a local shadow volume for each of the last Rebuild's shadow view candidates from the join on, not from a
  colour commit's growth. So a frame without claims submits no main epoch (it has nothing to draw), and the main epochs have no
  own preparation left: the first report's Z-prepass and colour are all "by the revision", the withdrawn frames not submitted.
- *Reflection.* Its faces draw from the frame before's main commits, whose inputs name the main rows by address. They are covered
  only when those commits exist, are of the current scene buffers, and no growth of the rows was adopted since
  (`Committed::rowsGeneration`, `Resources::MainRowsGeneration`); else the engine renders them, decided before it does.
- *Device loss on a mid-run growth of the main rows* (found here, older than this step: the committed state lost the device too).
  A spawned NPC's pipelines grew the pipeline rows 128 -> 256 as graph work; the GPU faulted at VA 0 about 10 ms after the request,
  before any adoption, and only with the growth's fill (no fill, no loss; revisions off, no loss). The fill's copy runs on the
  dedicated upload queue, a spare queue of DXVK's transfer family, but `CopyQueueUploadService` made its command pools for
  `QueueKind::Copy`, which BasicRHI resolves to the device's primary copy queue: the graphics family. Startup fills had always been
  empty, so the path had never copied. Fixed in BasicRHI: `Device::CreateCommandAllocator(const Queue&)` (the queue's own family;
  D3D12 by kind), used by the copy service; and a Vulkan submission now rejects a command list whose pool is of another family than
  the queue, loudly, instead of losing the device. `CS_DCLF_TEST_ROWS_GROWTH=<frame>` grows both main row tables at that frame, to
  exercise the fill under claims on demand.
- Left at startup: one Z-prepass commit stages the bucket counts' values (frame ~31: the count buffers exist before the graph that
  declares them is built; an undeclared target is staged by design), and the shadow, occlusion and reflection epochs' first
  shapes still come from their commits (layouts drawn, `known`): their own preparation at a first sighting, next.

**R4, second step: shadow, occlusion and reflection coverage from revision inputs** (2026-10-06; small tables r4f1 with spawned
NPCs, r4f2 with a forced growth, default r4f3: no losses, every report "<- OK", every steady epoch by the revision).
- *Shadow views by predicted layout.* `CaptureShadowView` records what every view draws with - mode, target, slice, viewport,
  rasterizer state - whether DCLF draws it or not (`Impl::observedViews`). A view is keyed by its accumulator, descriptor, render
  mode and occurrence: a sun cascade's volumetric copy is a view of its own that the engine draws first with the same accumulator,
  so the captures count per key within the frame and `ShadowViews::All()`'s order counts the same way. `DecideShadowCoverage`,
  right after `Rebuild` and before the engine draws a view, predicts the frame's layout (the covered views in order, then the
  retained slots as `ExecuteShadowFrame` lays them out). The views stay DCLF's only when the selected revision has that shape,
  recorded on the graph as built; else `ShadowViews::UncoverAll` leaves every view to the engine that frame. The predicted
  layouts, newest first, are what the revisions make the shadow epoch's shapes for, not the commits' shapes. A layout first
  seen mid-run (a light coming or going) is now 1-3 frames of the engine's views, where it was 4 frames of own preparation;
  mispredictions (views not drawn as predicted) are counted and logged: none.
- *Occlusion maps* likewise: their layout from the maps' last captures (`PredictedOcclusion`, taken whether DCLF draws a map or
  not), the revisions' occlusion shapes from those, and `OcclusionReady` only with the selected revision's shape for it.
- *Reflection*: its revision shape no longer waits for a reflection commit (the heaps are the graph's), and the faces are DCLF's
  only when the selected revision has their recordings.
- Left at startup, once per run: one shadow and one occlusion commit whose push data differs from the revision's (the payload's
  shared and feature data addresses: the first revision's shapes are made before any shadow commit had a payload), and the Z-
  prepass's staged bucket counts. Next: those addresses from resources, not the payload; then the own-preparation path goes
  (R6: a revision that does not cover an epoch is decided before the engine draws, so nothing is left for it).

**R4, third step: the shadow arena's blocks and latch from resources** (2026-10-06; small tables r4g4 with spawned NPCs, r4g5 with
a forced growth, default r4g6: no losses, 0 epochs on own preparation in the whole run, startup included, 0 mispredictions).
- *Fixed blocks.* The zero block, CS's SharedData and FeatureData sit at fixed offsets in the shadow constants
  (`ShadowArenaBlocksOf`: after the frame record, at places CS's block sizes give; FeatureData's size is fixed for the session,
  `State::featureDataBytes`). A view's push data and the latched layout (`ShadowLatchedLayout(resources)`) come from them, not
  from a payload, so the first revision's shapes match the first commit's. A commit's copy of another size is a loud failure.
  The shadow constants are published as the latched target when made, not when the graph first declares the pass.
- *The shadow latch reserved at the join* (`ReserveShadowLatch` in the shape producer: views, key slots over every mode, states
  registered, sun processes), as the main latch already was: the first commit's latch growth had left the revision's latch behind
  ("values past its latch").
- Left: the Z-prepass bucket counts staged once at startup (969 values, benign); the reflection epoch's first 8 recordings fail at
  startup as "not replayable" (pre-existing, also in r4f1-3; the faces stay the engine's those frames). Next: R6, the
  own-preparation path deleted (nothing reaches it now), then R5 and R7.

**Parallel graph work, step 1: per-frame engine values out of the versions** (2026-10-06; motion runs m1 -> m14: carried 40
units/frame through the worldspace on a slow turn, Tracy 35 s; bridge run fog1 with the persistent parity).
- *Found* (m1): two values that drift with the time of day almost every frame re-versioned scene state every frame. IBLParams
  (a material frame component) re-versioned ~1,030 material slots a frame; the vertex fog (FogParam, FogNearColor, FogFarColor)
  and the pixel stage's FogColor re-versioned every technique row, so every pipeline's witness moved and every pair resolved
  again (~800 of ~1,100 a build), and, refreshed at Prepass, they re-kicked the early colour build every frame.
- *Changed:* IBLParams and FogColor are read by no Lighting stage: dropped from the material frame components, and kept at a
  technique row's first value (KeepTechniqueFog). The vertex fog is the frame's: under DCLF_BINDLESS the vertex stage reads it
  from its own frame block (DCLFFrameFog, VS b13, which no shader declared), latched by every main commit from
  SceneStore::Tables::frameFog (MergeFrameFog over the frame's technique evaluations).
  The main frame mask names VS b13 as supplied although the pass's block there is emptied (PackFrameBlocks): the mask is the
  constants check's contract, and without the bit every pair of a pipeline reading DCLFFrameFog was rejected (constants skip,
  0 sequences; m10-m14 and fog1 drew almost nothing). A constants skip now warns in the report.
- *Measured* (m1 -> m14, confirmed by m15 with the draws restored: same worker and join figures, median 8.8 ms; bridge fogfix1:
  set parity clean, resident draw parity 0 differ, 7,450 sequences): main builds 131 -> 35 ms/s of worker time (2.7 -> 2.0 a frame, resolve ~400 -> ~50 us a build); the
  worker 25-34% -> 12-19% busy; render-thread joins 1.5-2.0 -> 0.5-0.8 ms/frame, now mostly the scene join; median frame
  9.4 -> 8.5 ms. Early colour rekicks: a few per 300 frames (lookups). Technique parity 0 of 80 differ; fog unchanged on screen.
- *Left:* the scene join (Scene.Evaluate, up to ~10 ms at cell loads) is steps 2-4's; latched copies staged during motion
  (~3 a frame, also before this step); one 243 ms render-thread "scene tables" frame in m14, not in m13 (same values work).

**Scene revisions on the graph** (2026-10-06; supersedes parallel graph work steps 2-4). The goal is render-thread waits of
effectively 0, by BasicRenderer's model: the render thread ingests (drains the engine's queues and posts the batch), the graph
prepares a scene revision (SceneState in batch order on the coordinator, payload producers on the pool, shapes, recordings,
assembly) and publishes it, and BeginSceneFrame selects the newest complete one. Per-frame values are FrameValues(N), written
per frame and waited for by the GPU. No scene work is time-sliced or resumed on the render thread.
- *Measured* (m15, motion): waits 1.0-1.5 ms/frame, nearly all the scene join (median 0.05 ms, p90 1.8, p99 8.1, max 10.2;
  kick-to-join window ~0.2 ms). The engine evaluations inside Scene.Evaluate cost ~0.004 ms/frame; the rest is DCLF's own
  (geometry resolve 0.35-0.5, records 0.15-0.2, loop tail 0.2, sun candidates 0.24, placements 0.25-0.3).
- *Why the joins exist:* the frame reads and writes the tables the walk mutates. Inventory, in frame order, with where each
  goes (P: the publication, read immutable; FV: FrameValues(N); I: an intent posted to SceneState; E: the engine boundary):

| Hook | Reads or writes today | Goes to |
|---|---|---|
| BeginSceneFrame | JoinSceneTask, InvalidateVerdicts (toggles) | deleted; I |
| | SelectRevision, DecideCoverage, ApplySet/WithdrawSet (claims, `setPhases`) | P (pointer swaps) |
| | ProcessEvents (walks attached subtrees, writes `tracked` and tables; last references' destructors) | I (drain and post); E (releases handed back) |
| | DecideTreeLod (mirror, camera) | FV |
| | PrepareReflection, PublishListFilter, RefreshMainRenderers | P |
| | the scene task kick | SceneState producer |
| AfterFullFrustum | join; PrimaryCull's hidden keys and lost members; RevokeUndrawnClaims; MakeRevisionShapes; fade write-back and early shadow kicks | P (notes and claims); producers; E (fade stores) |
| BeforeShadowMaps | ExecuteReflection (commit); placement join (writes records); KickSceneStreams (stages records); ShadowViews::Rebuild; coverage; KickShadowBuild (sun planes, modes) | P + FV; FV; producer (structure) + FV (placements); capture; P; producer (camera parts to the GPU or FV) |
| AfterShadowMaps | ExecuteShadowFrame | P + FV |
| EarlyPrepass | placement join; BuildFrame(Accumulate) (membership binding from the registrations); pipeline and program requests; pipeline lookups (`MutableLookups`); shadow program requests; RefreshLodTechniqueRanges; KickZPrepassBuild | FV; I (the registrations captured); Lookups producer; FV (frame block); producer |
| Occlusion hooks | set, SetLacking | P |
| Prepass | LatchAccumulator; RefreshFrameConstants (technique rows, frame lighting, material frame components); KickSceneStreams; KickColourBuild; reports | capture; FV (frame blocks); producer; coordinator task |
| Z-prepass, colour, reflection, occlusion, shadow epochs | commits: payloads, `GetTables` (frame fog and lighting, LOD fade, face streams), lookups | P + FV |
| Engine hooks | PassCapture (claims), PrimaryCull (filter, notes), SunAccumulation, LocalLightCull, SceneLists | P |

  The main payload's inputs are already camera-independent (MainInputs: the eye only for parity; frame masks, addresses,
  lookups and tables generations). The shadow build's are not yet (the sun's culling processes, modes, CS's shared and feature
  data). Each step below removes rows; the join moves later as rows go, and is deleted with the last.
- *Steps:* 2 FrameValues(N); 3 frame constants and captures out of the tables; 4 the accumulate phase and lookups as an intent
  and a producer; 5 ingestion split (drain and post, releases handed back); 6 the chain and the publication, every join and
  AsyncWorker deleted; 7 payload producers in parallel, shadow views from a capture, stress runs.

**Scene revisions on the graph, first increments** (2026-10-06; motion runs m16, m17; bridge par1 with set, resident-draw,
persistent and walk parity).
- *ParallelFor* on `PublishedSceneExecutor` (BasicRenderer's TaskSchedulerManager::ParallelFor): the caller and up to every
  preparation worker take chunks from one atomic counter; the caller waits only for chunks a worker is running. CPU tests:
  every index once across threads, a throwing chunk reaches the caller, late helpers never touch the body.
- *Placements* from the scene task: items, then roots, across the pool, each under its own read lease; a per-item taken bit
  replaces the resume index (the join takes what a refused lease left); a root's changed slots go to its own list.
- *Sun and light candidates:* the dirty entries' verdicts across the pool, applied in order.
- *Batched buffer resolve:* `dxvkGetInteropResourceInfos` (DXVK fork, export @145) pins every buffer that needs a stable
  address in one command-stream chunk (full chunks go ahead unsynchronized) with one synchronization;
  `RenderGraphRuntime::DescribeResources`; `GpuResources::Prefetch` (results until BeginFrame, Acquire takes them); the walk
  names the round's buffers before evaluating (not per-frame entries, not entries left without a record). Resolve time over
  the motion run 3.19 s (m15) -> 1.35 s (m17); Scene.Evaluate max 10 -> 3.8 ms.
- *Measured* (m17 against m15): waits 1.0-1.5 -> 0.5-1.3 ms/frame, still the scene join. Inside the task, parallel chunks
  this small gain little: placements' takes are ~55 us of work over 15 threads, but the wall time stays ~115 us (44 us of it
  the serial queueing; the helpers' wake-up costs about what they save); the candidates' cost is the snapshot rebuild
  (~1.2 ms, ~200 times in 35 s), not the verdicts. The join must go, not shrink: the next steps are structural.
- *Parity* (par1): 7,458 sequences, set parity clean, resident draws and records 0 differ. Walk parity (first run with it in
  a while) reports 10 records and ~660 stale verdicts, all non-per-frame entries (hidden LOD land chunks kept eligible; one
  lever's record): event coverage, not the placements; to look at separately.
- *Next* (step 2 proper): the per-frame columns leave the tables one at a time for a FrameValues store with its own GPU path
  (placements, sun entries, LOD fade and palettes first), so the frame stops writing what the walk owns; then the structural
  tables' frame readers move into the chain.

**FrameValues, step 2a: placements leave the object records** (2026-10-06; bridge pl1 with set, resident-draw, persistent and
walk parity).
- *Split.* `BindlessObject` keeps the object's structure (shading, room, alpha, tree, skin offsets, wetness, and `lodFades`: the
  fades its pipeline allows), 256 -> 128 B. The placement is a row of its own, `BindlessPlacement` (144 B: world, previous world,
  bound, sun entry, fade node with its LOD type and held flag in w), in its own buffer by object slot (`SceneBuffers::placements`,
  grown with the records; t123 in both stages, from the frame record like t124-t127). The draw's LOD fade word is made in the
  shader (`DCLFLodFadeFlagsOf`: the record's fades while the row's node has them apply, with its type), exactly as
  `BuildObjectRecord` made it. BuildDrawsCS and FadeStateCS read only placement rows (`placementsIndex`).
- *Kept.* `PlacementStore` keeps the rows from the change log's `kChangePlacement` (kPlacementRowCauses) and the streams upload
  its runs beside the records'; `kChangePlacement` no longer rewrites a record.
- *Measured* (pl1): 7,457 sequences, no constants skips, set parity clean, resident draws 0 differ; records 0 differ (8.8
  rewritten an update), placement rows 0 differ (84.4 rewritten an update: the movers, now 144 B each instead of 256 B records);
  walk parity as in par1 (the 665 stale verdicts and 10 records noted there).

**FrameValues, step 2b: the placements made on the pool, waited for by the GPU** (2026-10-06; bridge fv2 with set, resident-draw
and persistent parity; motion m18 with persistent parity, m19 without).
- *ORG.*
  - `CopyQueueUploadService::QueueSignal` (`IUploadService::QueueStreamingSignal`, only from a dedicated queue): a signal entry
    that FIFO-follows the producer's copies; the batch carrying it signals the external timeline (empty batches too).
  - A host frame-wait timeline (`PersistentGraphHost::SetFrameWaitTimeline`, set before the graph is built: every packet binds
    it as `RenderGraph::kFrameWaitTimelineIdentity`) and a submit-time value (`SetFrameWaitValue`): `SubmitPersistentTicket`
    adds it to every batch's waits, the synchronous path to the admission's incoming waits. A value fixed at ticket preparation
    could not work: an epoch's next submission is not known to be the next frame's.
  - `PersistentGraphHost::SubmittedPoint` (a `GpuPoint` any thread can test or wait on; the queue registry's fences are shared
    so a waiter outlives a rebuild).
  - Tests: CopyQueueUploadServiceTests (a consumer submitted before the copies waits for the signal), PersistentVulkanHostTests
    (sync, async, reuse: a frame waits on the GPU until its value is signalled from the dedicated uploader). The hardware run of
    CopyQueueUploadServiceTests fails in its first test before and after these changes (WARP, what ctest runs, passes).
- *DCLF* (`Draws/FrameValues`).
  - The walk publishes a `PlacementPlan` (per-frame movers, gated or not; the roots it takes; the slots it wrote in full),
    holding its engine objects, taken at the next frame's start and released on the render thread once no producer reads it.
  - `BeginSceneFrame` kicks one producer a frame on the preparation pool and sets the frame wait to its sequence number; the
    producers chain in order. Each samples the plan across the pool under read leases (the written slots once, plans kept until
    sampled: a startup's first frames precede the upload queue), waits on the worker for the GPU point of the frame that last
    read its ring buffer, sends that buffer the journal's runs it lacks, then always signals.
  - Four ring buffers of every row (`kRing`); frame n reads buffer n % 4 through the frame record (t123) and the culling's latch
    (`BuildDrawsLatch::placementsIndex`). The 2a store and scene buffer are gone. Placements stay in the tables for now (the
    placement job also takes palettes): the persistent parity compares the rows with them after the placement join.
- *Measured.*
  - Bridge (fv2): set parity clean, resident draws 0 differ, 7,470 sequences. Frame values about 85 us sampling and 45 us
    sending a frame on the pool, 0 buffer waits, 0 refused leases. Parity 2 rows of ~8,170 differ, the RuinsLever the walk
    parity already finds stale in the tables.
  - Motion (m18): 0 differ in most reports, 52-65 when a moved static (`PotOpen`) is written in full by the walk: its row is the
    next frame's. Event-fed sampling (amendment A/B) removes that lag.
  - Motion (m19): render-thread waits 0.3-1.4 ms/frame, nearly all the scene join (m17 0.5-1.3): unchanged, as expected. Epochs
    cheaper: shadow views 76-84 us (m17 112-126), Z-prepass 142 (181).
- *Next.* Palettes (bones) into FrameValues the same way, then the placement half of the job, the tables' placement columns and
  the placement join go (2c). After that, the scene join's remaining readers (steps 3-6).

**FrameValues, step 2c: palettes made on the pool; no placement in the tables, no placement job, no join** (2026-10-06; bridge
fv3 with set, resident-draw and persistent parity; motion m20 with persistent parity, m21 without).
- *Palettes.* A second ring of buffers (`FrameValues::PaletteRow`, t122 `DCLFPalettes`, vertex stage, from the frame record).
  A skin's block (`Tables::boneOffset`, `boneRows`; placement only, `PlaceBones`) holds its current palette at twice its offset
  and the previous one after it (`PaletteRowsOf`), so the layout never moves: no bone region, nothing rebuilt when the blocks'
  capacity grows. The producer runs the engine's palette update (thread-safe: the skin's critical section and frame counter) for
  every plan item with a block, then copies both palettes into the kept rows; a size that is not the block's is counted (the walk
  writes it again: `KeepSkin`). An older plan's item naming a block a newer one names again leaves it to the newer.
- *The extras buffer.* t126 holds the extras alone (`ExtrasStore`, `UpdateExtras`, `kInitialExtraRows`); the records address
  them directly.
- *Deleted.* The scene placement job (`TakePlacement`, `TakeRoot`, `RunPlacements`, `ApplyPlacements`, `JoinPlacements`, the probe
  and the movers' witness), its joins at BeforeShadowMaps, EarlyPrepass, ProcessEvents and the walk, and
  `IndirectDraws::BeforePlacementJoin` (which waited for the early shadow build because the join wrote the tables). The tables'
  placement and palette columns (`ObjectRecord` is 16 B: geometry, material, pipeline, flags; `sunEntry`, `lodFade`, `bones`,
  `previousBones`) and their change causes (`kChangePlacement`, `kChangePalette`). The walk lists every recorded mover for the
  frame values (no gating: event-fed sampling is amendment A/B) and publishes the plan (`PublishPlacementPlan`: the roots, the
  palette capacity).
- *Kept as structure.* `Tables::sunEntryNode` (the node; its bound is the row's) and `hasFadeNode` (the fade-out test's input;
  a `kChangeBindings` cause). Diagnostics that read bounds read the engine's now (render thread) or the frame's rows.
- *Parity.* `FrameValues::CheckParity` compares every slot's row with the engine's now (`SampleSlot`), and every skin's palettes
  with the skin's after the frame's update; the bindless record parity takes a made-up placement on both sides (it checks the
  layout); capture parity compares the native draw's palettes with the frame's.
- *Measured.*
  - Bridge (fv3): set parity clean, resident draws 0 differ, 7,454 sequences. Frame values ~100 us sampling (638 items, 254
    skins) and ~60 us sending, 0 buffer waits, 0 refused leases, 0 size defects. Parity against the engine: 40,795 rows and
    1,270 palettes a check, 0 differ.
  - Motion (m20, persistent parity): 0 differ in nearly every report; 26-65 rows when the walk writes moved statics
    (`FireSptiCookingBase`, `PotOpen`, `RuinsLever`: their previous world), the known one-frame lag; palettes always 0 differ.
    Note: persistent parity runs the scene work inline (`SceneWorkInline`), so its waits are not the async path's.
  - Motion (m21): render-thread waits 0.25-1.25 ms/frame (m19 0.3-1.4), still the scene join; frame values 80-130 us sampling,
    46-374 us sending a frame (up to ~300 KB of palettes while many actors are in view).
- *Next.* Step 3: frame constants and captures out of the tables; then the accumulate phase, the lookups and ingestion (4-5) and
  the chain (6), which remove the scene join. Event-fed sampling (amendment A/B) removes the moved statics' lag.

**Step 3, first part: the frame data through the render thread traced; the extras rows' frame parts out of the tables**
(2026-10-06; motion m22 before, m23 after; bridge x3 with set, resident-draw and persistent parity).
- *Frame data through the render thread* (`Draws/FrameData`): every byte a commit writes for the GPU, counted at the three
  ways it can: a latched copy (`LatchedUploads`, "latched: <buffer>", the frame-constants buffer by slot and part), a staged
  upload (`CommitUploads`, "staged: <buffer>") and a latch block's write (`LatchWrite`, "latch: <what>"). What a worker or the
  pool sends (the streams job, FrameValues) is not counted. Reported every 300 frames, largest first. Motion (m22/m23):
  | Where | KB a frame |
  |---|---|
  | staged `cs.dclf.fade-roots` (the whole static table, re-sent when its version moves, every ~3 frames) | 98-112 |
  | staged `cs.dclf.fade-visibility` (PrimaryCull's visibility blocks, whole, every frame) | 67 |
  | staged `cs.dclf.fade-animated`, `fade-root-lists` | 9-16, 6-7 |
  | latch: shadow per-frame view data, bucket tables, culling latches; shadow constants | ~4, ~2, ~1.5, ~1.9 |
  | latched frame constants: PS b6 2.5, zeroed light block 2.4 (constant zeros, every commit), frame record 1.6, VS/PS b12 1.4 each, PS b5 1.3; lighting, LOD fades, fog, extras frame < 0.3 each | ~11 |
  | latch: reflection (culling latches, face per-frame data, tree rows, buckets) | ~4.4 |
  | everything else (main latch, counters, tree LOD, records and extras the commit sends itself) | ~5 |
  | **total** | **~175-232** |
  About 85% is fade: its static table should be sent by the journal's runs (or published with the revision), and its visibility
  by changed blocks. The extras were never the render thread's bytes (the streams job stages them on the worker): their cost was
  the watch's CPU.
- *Zones* in `RefreshFrameConstants` (m22): 86 us a frame: shading and extras 63 (the watch re-deriving every watched slot's
  extras), pipelines and techniques 14, material frame components 9, texture transforms 2, wetness 0.5.
- *Extras.* An object's extras rows hold only its static parts (`SceneStore::WriteObjectExtras`: the land blend's material
  offset; how TextureProj is made, in row 0's x for ProjectedUV objects - World x projection, the projection alone for Envmap, a
  multi-index shape's own rows; the ProjectedUV parameters), written with the record's accumulate patch and on the shading
  events. The frame's parts are an `ExtrasFrame` each main commit latches: VS b13 c3-c7 after the fog (the land blend position, the
  ProjectedUV projection from the engine's own routine: 80 B), PS b13 c16 (the tilings and the projected-normals switch: 16 B).
  The shader completes them from the placement row (`DCLFLandBlendParamsOf`, `DCLFTextureProjOf`; CPU `CompleteExtras`). The
  per-frame watch (`shadingWatch`, `watched`, `kWatchExtras`) is gone. Reflection reads the colour commit's blocks, as before.
- *Parity* (persistent): the completion against the engine's routines (`ReferenceExtras`) for every object with extras, and the
  static rows against a write now. x3: 5,045 objects, 0 differ, largest difference 0.0078 (TextureProj's translation: the engine
  subtracts the eye and adds it back, an ulp of the world position), 0 static rows stale with no event. Set parity clean, resident
  draws 0 differ, 7,459 sequences, frame values 0 differ.
- *Measured* (m23): `RefreshFrameConstants` 86 -> 25 us a frame (shading and extras 63 -> 2.4). Prepass 0.09-0.26 ms (m22
  0.16-0.18; it also carries the colour build's kick and the reports).
- *Found on the way:* a full shader recompile crashed twice inside Mod Organizer's usvfs (`hook_MoveFileExW`), from
  ORGModuleServices' shader cache publishing on many compile threads at once, with one temporary name per key (two compiles of a
  key wrote and renamed the same file). `ShaderCompiler::Store` now writes a temporary of its own and publishes one rename at a
  time.
- *Left in step 3:* the shading resample and wetness (now ~3 us) as FrameValues rows, which moves their sampling to the frame's
  start (a frame's flicker off native on candle emissives, against capture parity's 0.1%: a decision); frame lighting and fog
  out of `Tables` into a frame capture; technique rows and material frame components are lookup inputs (step 4). The fade data
  above is the largest render-thread payload and is not in step 3's list.

**Step 3, second part: the shading as FrameValues rows; the frame's globals a capture** (2026-10-06/07; bridge x4 with set,
resident-draw and persistent parity; motion m25).
- *The shading row* (`BindlessShading`, 64 B, PS t121 `DCLFShading`, `kShadingBufferRegister`): MaterialData, EmitColor with
  SSRParams.w, the wetness (SkinPerGeometry), Linear Lighting's emissive multiplier, and a mask of the components the pass writes
  (for the CPU's comparisons with the engine's constants, which keep the unwritten sentinel). A third FrameValues ring beside the
  placements and the palettes. The record (`BindlessObject`) loses all of it: 128 -> 64 B (room index, record flags, alpha test,
  LOD fades; tree; palette and extras offsets). `kChangeShading` is gone (`kChangeCauseCount` 9), and with it the tables'
  `shading`, `emissiveMult` and `skinWetness` columns and the accumulate patch's shading.
- *Which slots, when:* the walk names a slot (`SceneStore::ShadingItem`: its property held, its pass, member, actor) for its
  shading events (`NameShadingEvents`: LOD fades, emittance, the controllers' MaterialSources writes; every slot the first time),
  and writes its extras' static rows on the same events; the accumulate phase names a patched record (`ApplyAccumulatePatch`).
  `BeginSceneFrame` hands them to the frame's producer (`TakeShadingItems`), which samples them across the pool
  (`SampleShading`, under leases; each slot once, the newest naming wins). The render thread does no shading work.
- *The wetness* stays a render-thread capture: `Skin::GetWetness` advances an actor's fade on its first call a frame and its
  cache is not thread-safe (Skin's own SetupGeometry hook calls it on the render thread). `CaptureWetness` runs it at
  `BeginSceneFrame` (0.7 us), fanned out by `ActorValueIndex` to the meshes whose value changed or joined; the producer writes
  those after the items (a non-actor's item zeroes it).
- *Accepted difference:* a value written while the frame renders (a controller on the animation job, a cull's
  `GetRenderPasses`) is drawn from the next frame (dclf-open-defects.md, "Shading that changes during a frame is drawn from the
  next frame"). Capture parity compares such draws with the engine's value now and counts them on their own line.
- *Parity* (persistent, at Prepass, against the frame's rows): x4 39,810 slots a check, ~25 changed since the frame's start, all
  named for the next frame; 0 queued, 0 missed; wetness 1,070 meshes, 0 differ. Extras 5,045 objects 0 differ; set parity clean;
  resident draws 0 differ; 7,455 sequences; frame values rows and palettes 0 differ.
- *The frame's globals* (`SceneStore::FrameCapture`): the frame lighting, the fog and the character light's noise view are no
  longer columns of the tables; `RefreshFrameConstants` writes the capture, every commit latches it. The unread lighting
  versions (`frameLightingVersion`, `frameLightingUploaded`) are gone.
- *Measured* (m25, motion): `RefreshFrameConstants` 22.6 us (m23 24.6: shading and extras 2.4 -> 0); the walk's naming 3.0 us
  (coordinator); the producer's shading 3.0 us (38 items, 26 rows changed a frame); FrameValues sends 182 KB a frame on the
  upload queue. The render thread's own record writes halve (`cs.dclf.objects` 1.41 -> 0.69 KB a frame). Waits unchanged
  (the scene join, 0.9-1.2 ms a frame in motion).
- *Found on the way:* `constantsRefreshed` was set only by the old resample's first pass; without it every pipeline was
  evaluated in full every frame (m24: Pipelines 13 -> 60 us). Set after the pipelines' loop again.
- *Left:* technique rows and material frame components (step 4, with the lookups); the fade data (its own step: fade-roots'
  whole-table resends, fade-visibility whole every frame); the zeroed light block latched every commit.

**The fade data through the render thread: what changed, not whole tables** (2026-10-07; bridge x5/x7 with set, persistent and
fade parity; motion m26/m27). The depth commit's fade uploads were ~85% of what the render thread sent the GPU. Each is now the
part that changed:
- *Fade roots* (`FadeRootStatic`, 80 B a root, ~1,350-4,000 roots): every write marks its row in a journal
  (`Tables::fadeRootsJournal`, `NoteFadeRoot`; a table clear resyncs); the commit sends the runs since the version the buffer
  holds (all of them for a new backing) and trims the journal (`SceneStore::FadeRootsSent`). 98-138 KB a frame in motion (the
  whole table every ~3 frames) -> ~1.5 KB in ~14 runs.
- *Visibility blocks* (11,392 B each, one per list process): of each block, the header, the operators and plane sets its counts
  use and the view planes, each part only where it differs from what the buffer holds (`SceneBuffers::fadeVisibilitySent`).
  FadeStateCS reads nothing past the counts. 67 KB every frame -> 0 with the camera still, 8-24 KB moving.
- *Animated stamps* (`fadeAnimated`): the stamped roots' words in runs, gaps under 32 words merged (`SendWordRuns`), not the span
  from the lowest root to the highest. 15-16 KB -> 2-3.5 KB (~35 copies).
- *Root lists* (`fadeRootLists`): the words that differ from a mirror of the buffer, in runs. 6-10 KB every other frame -> out
  of the report's top.
- *The zeroed light block* (PS b3, 2.4 KB): constant, so sent once per frame-constants buffer by the first commit
  (`Resources::sharedLightZeroed`), and out of the latched layout.
- *Measured.* Bridge (x7): 61 KB a frame (x4 111). Motion (m27): 44-69 KB a frame (m23 175-232, m25 ~270). Fade parity: port,
  state (640 updates, 0 differ) and roots with engine-drawn parts all OK; set parity clean.
- *Seen, not from this* (dclf-open-defects.md, "Fade visibility parity differs in single windows"): fade visibility parity's CPU check (the engine's cull against the port's compound test on the sampled
  block, in the list jobs) differed in one window of x7 (4,815 of 34,040, "the engine culled, the port visible, compound accepted
  at op 63"). It reads no uploaded data; earlier sessions' runs show the same class (junk-landOwn*, junk-trav15).
- *What remains* in motion: fade-visibility (camera-dependent), the trees' rows (5 KB, whole on a version change), the shadow
  latches (~10 KB), the frame constants (~11 KB), face positions when faces animate in view (up to 78 KB at the bridge).

**Step 4 reordered behind steps 5-6; the accumulate phase's spikes removed in place** (2026-10-07; bridge x14/x15 with persistent,
resident-draw, set and fade parity; motion m28/m29).
- *Why not a posted task yet.* The accumulate phase (EarlyPrepass) cost a median 26 us but a p90 508 us in motion (m27), and 490
  us lie between EarlyPrepass and the Z-prepass. Posted to the coordinator it would run after the main cull's writes (properties'
  alpha and LOD fades from GetRenderPasses, currentFade), but the Z-prepass and colour builds (the same FIFO worker) and the
  Z-prepass commit read the live tables, and `KickZPrepassBuild` reads them on the render thread (material lookups, reserves,
  `PrepareMainInputs`, the witness). Ahead of the builds, the kick would join it; behind them, the Z hook would wait for both
  builds and the phase. It becomes possible once builds and commits read the publication (step 6). Not into the walk's task:
  that runs beside the main cull, and the walk is to be replaced by events (plan, "eliminate the scene walk").
- *Decal order kept* (`SceneStore::KeptDecalOrder`, DecalOrder.cpp). A decal's key (its child-slot path up the scene graph) is
  taken when it joins, is patched again or is written again by the walk (an actor's parts every frame: their 3D is
  re-parented without a patch), not for every decal on every change; the changed ones are merged into the kept order and the
  ordinals counted again in one pass. A BGSDecalNode's decals are keyed from the end of its decal array, which any added or
  removed decal (the engine's own too) shifts for all of them, so they are keyed again together. Parity
  (`CS_DCLF_PERSISTENT_PARITY`: the kept order against one made whole): 0 differ, 0 stale keys (x14, x15). The first two
  versions differed (20 stale keys a check: the decal node's shift, then the re-parented actor parts), which the parity's stale
  key report named.
- *Resident maintenance by changes* (Residents.cpp, `KeepResidentsAlive`). The used pipeline and material sets, the pipelines'
  template members, the trees' members and the fade roots' centres were recounted from every resident (~7,500) on any join or
  drop (a quarter of the frames in motion, 53 us); now a join, patch, drop or tree listing names its slot, and only those are
  counted out and in again (`SlotMembers`, `ResidentCounted`); the joins' marks of a failed join are cleared by count. Whole
  again only when every residency ends or the tables reset. The used-set parity (bound records against the sets): 0
  differences.
- *Measured* (m29 against m27): the phase 136 -> 70 us a frame mean, p90 508 -> 155 us; decal order mean 74 -> 17, p90 351 ->
  52; resident keeping mean 15 -> 6.5, p90 57 -> 10. What is left in its p99 (480 us) is the joins themselves on cell loads
  (classification, slots, the engine evaluations of new pipelines and materials).

**Step 5: ingestion** (2026-10-07; motion m32, bridge y2 with persistent, resident-draw, set and fade parity).
- *The split.* `ProcessEvents` ran twice a frame on the render thread (Present and the frame's start): it drained the
  engine's queues and applied them (the category refresh, the attached subtrees walked and evaluated, the detached entries
  erased, the validation slice, the structural events). Now:
  - `IngestEvents` (render thread, Present and the frame's start, the coordinator idle) only drains: the tracker's attach and
    detach events, fade snaps, fades, property and node events, switch events and object LOD's segment writes move into the
    pending `EventBatch`, in push order; tree LOD's mirror is drained there too (DecideTreeLod reads it next). The load-screen
    discard stays there.
  - `ApplyEvents` is the scene work's first part (`RunSceneWork`, on the coordinator), once a frame. What it drops of
    PrimaryCull's is held for the join like the walk's. A batch that no scene work took for a whole frame (menus, DCLF switched
    off) is applied by Present (`EventsUnapplied`), so the queues never wait longer.
- *Releases handed back.* The scene work no longer drops engine references: the erased entries' geometry, the replaced
  always-render roots, the applied batches (their tracker events hold subtrees), the walk's node events and applied switch
  events go to `SceneStore::HandBack` and are released at Present (`ReleaseHandedBack`), on the engine's main thread, with
  nothing reading them. The walk had been dropping node and switch references on the coordinator since phase 3 step 2.
- *The graveyard crash, explained and fixed* (crash-catalog.md). Moving the events behind the list filter reproduced it (m30:
  `PrimaryCull::EndFrame`, a freed root released from the graveyard, in the first cell loads). Cause:
  `PrimaryCull::RestoreSceneLists` (an occlusion map the engine draws, most frames in motion) took a reference to every root of
  the published filter, trusting "the snapshot is current, so its roots are alive". The filter's roots are raw pointers from the
  sun candidates; a root the update detached is freed before the frame starts, and a filter made before the frame's events
  still names it. The events-first order had only hidden it (the lost member took the root out of the filter first). Fixed in
  the shared contract: `SunCandidates` hold their entry nodes (`held`), so every reader keyed by them (the cut, the list filter,
  the exclusions) may dereference one; their last owner may be any thread, so the references go through
  `EngineReleases` (a lock-free queue released at Present). The restore leaves out roots no longer under the scene node: m32 left
  out 360-1,435 detached roots per 300 frames that it used to reference from freed memory, with no crash in 7,700 frames of
  cell loads.
- *Measured* (m32 against m29, motion): the render thread's event time 0.08-0.14 -> 0.03-0.10 ms a frame, its scene-frame hook
  0.13-0.23 -> 0.07-0.13 ms. The apply's own cost is unchanged (about 74 us a frame, now once on the coordinator rather than
  twice on the render thread), and it now lies inside the scene task: the scene join's wait is unchanged (0.27-1.33 ms a frame,
  m29 0.27-1.19). The join goes with step 6.
- *Accepted difference.* The frame-start readers before the kick (the point lights' category filter, the list filter) see the
  scene as the last apply left it, one ingestion behind: a category node that appeared this frame is judged next frame (fewer
  empty children cut for a frame), and a member lost to a detach is taken out of the list filter at the join instead of before
  it. Neither can reach freed memory now (the candidates hold their roots, the geometry is held until Present).

**Step 6: the tables as a publication** (2026-10-07; motion m32, m33).
- *Why not the frame's tail.* The tail after the colour epoch (its end to Present) is 0.64 ms (p50; p90 0.97) against the
  scene task's 0.27 ms (p50; p90 2.1, p99 4.1, m32): run there, the work's wait only moves to Present. The walk needs the whole
  read window (Main::Draw to Present) with the frame's readers on a copy that holds still.
- *Decided* (user: long-term scalability first, rewrites fine; the main thread is eventually to only accept a scene revision):
  BasicRenderer's journal model. The coordinator owns the working tables and is their only writer; at the end of its work it
  publishes an immutable, pooled snapshot, made from a pooled set no frame holds by replaying the changes since that set's
  version; BeginSceneFrame accepts the newest by pointer swap. Nothing is copied or replayed on the main thread, and readers keep
  contiguous columns. Copy-on-write chunked columns (O(1) publication) were rejected: they break contiguous uploads and every
  reader, for no gain on the accept. Stages: 6a the snapshot (a whole copy first); 6b the frame-side writers out of the tables
  (the accumulate phase as a captured intent, frame constants, lookups, fade-root bookkeeping); 6c the readers on the accepted
  snapshot and every in-frame join deleted; 6d the whole copy replaced by journal replay, family by family, each with a parity.
- *6a* (m33). `SceneStore::PublishTables` (the coordinator, after the set's commit) copies the tables into a pooled snapshot no
  one holds; `AcceptTables` (BeginSceneFrame) takes the newest. The tables copy as they are (logs, journals, slot tables, maps,
  import leases). Cost: 0.32 ms p50 (p90 0.40, max 0.90) a frame on the coordinator, pool of 2. Unread so far, and inside the
  joined scene task: the render thread's waits rise by that much (0.67 -> 0.98 ms a frame mean) until 6c removes the join, and
  6d replaces the whole copy.
- *6b, first part: the accumulate phase on the coordinator* (y3). The joins no longer read the engine's registrations (scene
  membership, `BindByMembership`), so the phase splits: `PrepareAccumulatePhase` (render thread, EarlyPrepass) drains the
  registrations (diagnostics, the frame's lighting pass), serves the material evaluations the last joins asked for
  (`ServeMaterialRequests`: SetupMaterial is the engine's) and runs the material tail (writer events, texture transforms, the
  validation slice); `RunAccumulateWork` (the coordinator, "accumulate") makes the joins, keeps the residents, orders the decals
  and publishes the tables (6a's snapshot now includes the frame's joins). At its join (`FinishAccumulateWork`) the render thread
  evaluates the new pipelines' PerGeometry blocks (SetupGeometry) and hands PrimaryCull the members the joins dropped. A join
  whose material record is not evaluated yet asks for it and waits a frame, native meanwhile (`bindRetry`). Still joined at
  EarlyPrepass: the Z-prepass kick and the lookups read the live tables. y3: set parity 0 in every window, 7,436-7,442
  sequences; steady membership as x15; the startup's first window bound 7,905 objects a frame (x15 7,946): 8,162 joins waited
  a frame for their material, all served.
- *6b, second part: the frame's engine values out of the tables* (y4, y6). Writing them into the frame's snapshot copy was
  rejected: the coordinator bumps the same version counters in its own tables, so versions would collide and the upload caches
  that compare them would miss changes. Instead `FrameTables` (Scene/FrameTables.h) holds them, written by the render thread only,
  versioned by its own counter, indexed by the tables' slots and keyed to them (a slot whose key, binding or record version moved
  is taken afresh): the pipelines' PerGeometry blocks; the technique rows' constants (the tables keep each row's key; the shadow
  mask is the key's low bit); the material records as the frame draws them (the coordinator's record with the writer events'
  re-evaluations, the frame-sourced components and the texture transforms on it), their versions and a material log the builds
  follow, with the per-signature application and the transform watch. Readers (the main build, the material lookups, the capture
  parity) read it. The joins no longer evaluate techniques (EvaluateTechnique was engine code on the coordinator); new pipelines'
  blocks and new technique rows are evaluated at EarlyPrepass (`RefreshNewPipelineConstants`). A written material's unreferenced
  slots are dropped by the coordinator (`DropWrittenMaterials`: the references are its), released at Present.
  Found on the way (y3-y5: 15 of ~500 frame lightings differed): the frame lighting's AmbientSpecularTintAndFresnelPower.w was
  only ever seeded by the first frame's full evaluations; pipelines made later never published theirs. With the joins waiting a
  frame for their materials no such pipeline existed at the first frame, so the component stayed 0. The new pipelines'
  evaluations are now merged where nothing else wrote a component (`lightingSeeds`). y6: 0 of ~490 frame lightings, 0 geometry
  variables, 0 technique blocks, 0 material frame components and transforms differ; set parity 0; 7,420-7,443 sequences.
- *6c, first part: the frame on its snapshot* (y7-y9). `GetTables()` is the accepted snapshot (`FrameView`); the coordinator's
  tables are `GetSceneTables()` (the revision's shapes read them, on the coordinator). The snapshot is the frame's alone, so the
  frame's start writes the set into both (`WriteBoth`: `ApplySet`, `WithdrawSet`, `RevokeUndrawnClaims`), and `AcceptTables`
  checks the two are equal there (log end, version counter, sizes): a difference is a defect, logged once and republished at once
  ("published again at the frame's start"). Revocation moved from the walk's join to the frame's start, after the set's
  application, from the change log's position at the commit (`setCommitCursor`). Found on the way: `setPhases` indexed past the
  snapshot (y7, `PhasesIn`); members re-bound by the joins kept the coordinator's applied phases (y8: set patched 15; the joins
  now clear them). y9: set parity 0, 7,448 sequences, every parity 0 differ.
- *6c, second part: the scene work spans the frame* (m34-m37, y10). The scene task runs on its own lane
  (`SceneScheduler::SceneLane`, one thread "CS DCLF scene", `AsyncWorker::SubmitScene`), so the frame's builds never queue
  behind it; it is joined only at Present and at the frame's start. The 12 in-frame joins are gone (PrimaryCull's full-frustum
  hook, the epochs, EarlyPrepass: the accumulate work is posted there and joined at Present). What passes between the frame and
  the coordinator is handed over at the frame's start with the task joined (`HandOverAtFrameStart`): sun and light candidates,
  switch changes, retired slots, shadow texture changes, retired imports, the lookups (the coordinator keeps its own copy,
  `lookupsView`); the frame's posts to the coordinator (fade roots sent and owned, reseeds, lookup resets) wait for the next
  task. Sun candidates carry their geometry's slot, so PrimaryCull resolves members without the coordinator's maps. The
  reports moved to Present, after its join; the fade write-back and the early shadow build are kicked after the set's
  application. `GuardFrameAccess` counts every frame read of coordinator state while the task is in flight ("FRAME ACCESS",
  an error line): 0 in m36, m37 and y10.
  Crashes on the way: m34 (the lane made with no preparation workers: the executor requires one), m36 (ORG's host thread,
  `ShadowViewPass::Prepare` read a target's depth views past their count after a shadow map import; the slice is bounded by
  both). Results:
  - m37 (motion, 7,719 frames): 0 crashes, 0 guard violations, 0 republishes; render-thread waits 0.10-0.38 ms/frame (m32
    0.67), left at Present (join accumulate 0.05-0.19) and the colour build's join (0.04-0.18). Outermost DCLF zones on the
    render thread (Tracy, 35 s): 0.65 -> 0.43 ms/frame (the walk's join 0.22 and the revision's shapes 0.03 gone; Present's
    hook 0.015 -> 0.067 with the join it now holds). Publication 0.35-0.55 ms on the coordinator (the whole copy, 6d's).
  - y10 (bridge, parities): set parity 0 in every window, 7,446 sequences, 0 constants skips; persistent records, extras,
    geometry, decal order, fade port/visibility, tree LOD, sun exclusion, material frame components, pipeline constants all
    0 differ.
- *6d: the publication by replay* (m38-m44, y11-y13). A pooled snapshot equals the tables as they stood at its change log's end
  (its last publication, plus the frame's start's writes if a frame accepted it: `WriteBoth` writes both alike), so
  `PublishTables` brings it up to date with what changed since, family by family, and copies the rest whole with those held out
  of the copy (swapped aside, put back by a scope guard):
  - *Material records* (2.3 KB each, most of the whole copy) by their versions: every write gives a session-unique version, so an
    equal version is an equal record.
  - *Per-object columns* the change log covers (`ColumnsOf`/`CausesBetween`: records, draws, lights, tree animation, skin, shadow,
    identity, bindings), by the slots the log names since the snapshot's end; new slots grown with `GrowObjects`' defaults.
  - *Geometry columns* (records, imports, slot and layer keys) by the geometry log, which now also names a cleared slot
    (`ClearGeometrySlot`); `geometryLastUsed` (the walk's, unlogged) stays in the whole copy.
  - *Extras rows* by a new block log (`extrasBlockLog`: every block written, allocated or freed). Copying only the logged slots'
    blocks left freed blocks stale (y11: the persistent extras parity, which compares every row, differed in all 18 windows; no
    drawn row differed: the per-slot parity was 0).
  - *The logs* (change, geometry, material, extras block) by appending what they gained and dropping the head the tables trimmed.
  - *Trees and fade roots* (rows, refs, free lists, nodes, index maps, the per-object slot columns) copied only when their write
    stamp moved (`treesStamp`, `fadeRootsStamp`; every write bumps them: a fade row's through `NoteFadeRoot`, the structural writes
    in List/Unlist past their early returns, `KeepResidentsAlive`'s member-list and centre writes). Stamps at function entry kept
    nothing (bumped every frame); the fade-root journal is the depth commit's hand-over every frame, so it is copied with the rest.
  - A snapshot just made, or one the logs no longer reach (trimmed past, invalidated by a clear), is copied whole.
  Parity (`CS_DCLF_PERSISTENT_PARITY`, every 60 publications): every material record, every object slot's columns, every geometry
  slot, the logs and the extras rows byte for byte, and a kept family against the tables'. 0 differ in every window of m39-m43
  and y12-y13.
  Cost (coordinator, per publication): 0.41 ms (m37, the whole copy) -> 0.20 (m38, records) -> 0.13 (m39, objects and logs) ->
  0.10 (m40, geometry) -> 0.06-0.10 (m44, families kept: trees ~75%, fade roots ~50% of publications in motion, all on the
  bridge). Per publication in motion: ~150 of 8,700 object slots, ~4 of 2,500 geometry slots, ~2 of 1,600 records written; the
  remaining whole copy 0.03-0.05 ms (small members and the index maps). y13: set parity 0, 7,444 sequences, 0 constants skips;
  m44: 7,730 sequences, waits as m37 (the publication is the coordinator's).
- *6e: the main payloads in the publication* (user: a render-thread wait comes out entirely, never made rarer; BasicRenderer's
  draws depend on material tables published asynchronously). Found (m45): the colour build was waited for twice a frame
  (BeforeFrameConstants, the epoch's join) because it baked the frame's values into the rows it wrote (WriteMaterialRow and
  WritePipelineRow read FrameTables), which RefreshFrameConstants writes at Prepass; and its staleness witness read the
  coordinator's counters (rekicks 10-14% -> 1-9% fixed). *Decided (user):* records, bindings, technique and pipeline constants are
  scene state the coordinator publishes (a change a frame late, as joins); the per-frame floats (frame components, texture
  transforms) either posted too (CS_DCLF_FRAME_FLOATS=published) or patched into the rows' upload by the render thread (patch);
  both built, the switch following publication latency.
  - *A, constants* (y14): Tables::pipelineConstants and techniqueConstants, posted by the render thread's evaluations (keyed by
    the slot's key and binding version), handed over at the frame's start, applied by the scene work (ApplyConstantsPosts),
    published (a stamped family). MainReady waits for them ("constants"); the builds and the lookups read them. 103 blocks, 16 rows,
    0 dropped; no member waited.
  - *B, material records, published mode* (y15): a writer event's re-evaluation and a slot's changed frame floats posted
    (MaterialPost), applied by the coordinator (records; frame floats onto the record under Tables::materialFrameVersion); the
    builds and lookups read the snapshot's records, versions and log. ~27 slots' frame floats a frame at the bridge (the river's
    transforms). The build witness is the snapshot's version alone: 0 rekicks.
  - *C, lookups at the frame's start* (y16-y17, m46): GpuTextures resolves bindings, the null view and the samplers outside an
    epoch (the graph's retained descriptor service and the host's cleanup queue, as its import thread); the Lighting programs and
    pipelines, the pipeline entries and the material and shared lookups are refreshed once, at the frame's start
    (DrawcallLimitFix::RefreshFrameLookups), and the coordinator's copy taken after; the kicks', the main epochs' and the shadow
    epochs' material refreshes are gone (the shadow epochs keep their own), and BeforeFrameConstants with them. y16 drew nothing:
    the samplers were still created only inside an epoch (fixed). y17: 7,440 sequences, set parity 0. m46: the early colour build
    kept 100%; waits 0.03-0.24 ms/frame, the colour join 0.01-0.10 (the build overrunning its window behind the Z-prepass
    build) and the Present join 0.01-0.13.
  - *E* (plan amendment "step E, the main payloads published as GPU versions", E1-E5): a payload ring filled on the upload queue
    from immutable captures, the builds made ahead on the scene lane, the joins deleted.
  - *E1: the stream views* (y18, m47). The kept stores' arrays are already immutable captures (`KeptView`: copy-on-write elements
    and a journal snapshot, replayable against any holder). The object records, the extras rows and the geometry slots' draws are
    now brought up to date once a frame, by the streams' job kicked at the frame's start once the set is written (nothing writes
    the frame's tables after it), as `StreamViews` holding the frame's snapshot; every build (Z-prepass, colour, shadow) takes them
    from it (`StreamsForBuild`: the job runs first on the same FIFO lane) instead of updating the geometry store itself, and the
    commits emit them. The shared stores are then written by one job alone. `KeptHolders` (the versions a ring of buffers holds,
    the journal trimmed to the oldest) is tested against whole copies (`TestRingHolders`). y18: 7,446 sequences, every parity 0
    (but the known fade-visibility windows), set parity 0. m47: streams 300 of 300 staged batches taken; waits 0.13 ms/frame
    (the colour join 0.04).
  - *Order changed*: the ring (E2) needs its fill on the upload queue (undeclared buffers have no barriers on the graphics queue), and
    the upload queue's producer needs the payload at the frame's start, so the builds move ahead first (E3), then E2 and E4 together.
  - *E3a: publications installed whole* (y19, y20, m48, m49). Builds made ahead need the set in the tables they read, so the set's
    application and revocation (ApplySet, RevokeUndrawnClaims) are the coordinator's, on its tables alone, at the end of its work
    (PublishScene: the tables published with the set applied, the claims, the main claims' changes since the publication before).
    The frame's start writes nothing (WriteBoth gone): it installs the newest publication the selected revision covers
    (SelectPublication, IndirectDraws::SetApplicable) - its tables, claims and PrimaryCull notes together - or keeps the installed
    one whole (13-29% of frames in motion: the revision's recordings a frame late). Withdrawal is the frame's alone.
    Keeping an older publication needs what it names to stay valid: BasicRenderer's rule, nothing logically freed while a version
    that names it lives. `RetirementChain` (Common/Retirement.h): each publication holds the chain node opened with it, a node
    holds the newer one, and a node's batch - object, tree, fade-root, extras, bone, face-stream slots, the SlotTables' slots, the
    material/pipeline lookups' retirements, the geometry buffers' owners, the material references, every engine reference handed
    back - returns to the coordinator's free lists (RecycleRetired) once no publication up to it lives. CPU test
    (TestRetirementChain). m48: ~10,000 slots retired and recycled a 300-frame window, balanced; m49: 32-87 frames of 300 kept the
    installed publication, 0 crashes, lost-while-out 0; y20: set parity 0, every parity 0. Left: face regions (faceRegionFree).
  - *E3b: the builds ahead* (y21-y23, m50-m55). The Z-prepass and colour payloads are built with their publication (BuildAhead,
    called by PublishScene): one task at a time on the preparation pool, in publication order, never joined; it makes the
    publication's stream views (the kept stores are its alone) and both payloads from the publication's tables, the coordinator's
    lookups (an immutable copy, made again only when they change) and what the frame's start posted (PostAheadContext: resources,
    the epochs' last frame slots), and stages them against the versions the buffers hold once the publication before is committed
    (predicted; a commit whose buffers hold others sends from the vectors: 10-20% of frames in motion, the kept ones). The frame's
    start installs a publication only once its draws are done (DrawsReady). The epochs commit the installed payload: no build, no
    join; the frame part of a commit (frame number, frame slots, the frame record's buffers and FrameValues' ring indices) is the
    frame's. A payload the epoch cannot commit (none built yet, other resources, other frame slots: the VS mask toggles b7 a few
    times a minute) is built at the epoch with rows of its own (counted; the tables then hold no journal version of them). The
    kicks (KickZPrepassBuild, KickColourBuild, KickMainJob) and the joins are gone.
    Found on the way: builds on the lane after the accumulate work overran Present (m50: Present join 1.2-1.9 ms/frame) - hence
    the pool; a kept publication walked a freed fade-root node (m51, m52: crash in FadeRootLists) - the tree and fade-root nodes
    are now owned while listed and the applied event batches (detached subtrees) go through the retirement chain; idle pooled and
    fallback payloads pinned old publications and with them the whole chain (m53, m54: slots stuck retiring) - released when idle.
    m55: waits 0.01-0.19 ms/frame (the Present join alone; m46: 0.03-0.24 with the colour join), 0 crashes, the chain recycling
    every frame. y23: 7,431 sequences, set parity 0, 600 of 600 staged batches taken, every parity 0 but the known
    fade-visibility windows.
  - *E2+E4: the payload ring on the upload queue* (y24, m56). The epochs read the installed payloads from a ring of 4 entries (object
    records, extras rows, geometry table, the rows' tables, each segment's inputs), named by values: BuildDrawsLatch grew a payload
    block (payloadValid, the inputs' and geometry table's SRVs, the rows' addresses; 288 bytes) that BuildDrawsCS takes over its
    push constants, and the frame record names the entry's objects and extras. The frame's start makes the entry hold the installed
    publication (PrepareFrameUploads: grown by replacement, the old buffer retired with the frame's imports) and hands FrameValues'
    job what fills it (FrameValues::Kick's FrameUploads): after its own rows, before the frame's signal, on the dedicated uploader,
    once the frame that last read the entry is done on the GPU - each buffer sent what changed since the version it holds, the rows'
    headers made absolute for the entry. The commits upload none of it (no staged batch, no streams, no rows, no inputs); a payload
    built at the epoch (the fallback) keeps today's buffers and uploads. The reflection reads the entry the last Z-prepass commit
    read (its depth inputs are the frame before's). y24: 7,426 sequences, set parity 0, 600 of 600 commits read the ring. m56: 0
    crashes, waits 0.03-0.17 ms/frame, the producer 0.7 ms a frame at most 1.8 (FrameValues' rows with it), 2.2-2.6 MB a frame.
    (The m56 attribution of those bytes to the segments' own inputs was wrong: see E5.)
  - *E5: the journals kept for the ring; cleanup* (y25, y26, m57-m60). The 2.2-2.6 MB a frame were whole resends: each publication's
    build trimmed its journals (ChangeJournal::BeginBuild) to the version the publication before predicted for today's buffers, while
    a ring entry, filled every fourth frame, holds an older one - below the floor, so sent everything (objects, extras, the regions).
    The ring's entries are now the journals' holders (RingHolders: a KeptHolders per journal, set by the producer as it queues an
    entry's uploads), and the builds trim to the oldest of them; the scene buffers (the shadow commits', a fallback's) are holders
    too, uncounted (a frame behind at most; one below the floor is sent everything). The segments' own inputs are 0 KB a frame (the
    region holds nearly every input). m57: 120-265 KB a frame, the producer's sending 270-370 us (m56 675-740); the report now
    breaks the ring's bytes down by buffer. Removed: the builds task's staging (StageMainPayload, its pools, the payloads' staged
    fields, the commit's staged branch), the main jobs (MainJob, DropMainJob, SameInputs, mainPayload, probePayload, colourEarly,
    BuildInputsWitness). The epochs' frame slots: a slot an epoch supplied before and lacks now is supplied from its last capture
    (counted, logged once per slot), so the builds ahead see the same slots every frame - the in-frame builds for frame slots went
    from 1-6 per 300 frames to 0 (VS b7: 16 bytes captured at startup, absent since on the bridge; no pipeline drawn reads it, 0
    constants skips before and after). The Z-prepass's carried vertex blocks are the colour epoch's replay too. Face regions
    (faceRegionFree) now retire through the chain (kRetiredFaceRegion): the reflection draws the frame before's streams. The
    resident region's resyncs, now most of the ring's bytes, were 1,209 of 1,211 decal-count changes (counted by reason): every
    ordinal OrderDecals moves is in the change log, so a count change re-takes only the decals between the old and the new count (a
    capped count moves those without a log entry). y26: 0 resyncs, resident-draw parity 0 differ (1.4M entries), set parity 0, 7,447
    sequences. m60 (motion): the regions 17-33 KB a frame (m57 80-220), material rows 15-45 KB the largest part left, waits 0-0.19 ms/frame
    (the accumulate join at Present), 0 in-frame builds past startup, 0 crashes. The SlotTable CPU test, failing since E3a's retirement, follows the contract (recycled before reuse).
    m59 crashed once on the coordinator (RefreshCategoryNodes walking a newly appeared category node's children during a cell load:
    a parent pointer read garbage) - the scene work reading engine memory the engine changes inside Main::Draw..Present (F); not
    reproduced in m60.
  - *S1: the shadow payload built ahead* (y27, m61). The shadow epoch was ~0.1 ms of render thread (the views' blocks 0.04, frame
    captures that stay) and its AsyncWorker join ~0.002 ms: the step is structural. The shadow lookups are refreshed at the frame's
    start (RefreshMainLookups) for the last epoch's views - modes (occlusion maps included), rasterizer states, target format
    (Impl::lastShadow) -, so the builds ahead read them in the lookups snapshot; the epoch refreshes them only for views those did not
    cover (counted: 0-1 a 300-frame window). The builds task builds the shadow payload after the main ones (RunAhead), from the
    publication's tables, stream views and lookups, the candidates the coordinator made with it (CoordinatorSunCandidates,
    CoordinatorLightCandidates) and the inputs posted at the frame's start (PrepareShadowInputs for the last views), with the next
    frame's exclusions (the exclusion caches are the task's alone). The payload drops its stream views after the build (the
    publication holds them), so the payload the epoch committed (committedShadow: the occlusion epoch and the revision shapes read it)
    pins no publication. The epoch commits the installed publication's shadow payload when it covers the frame's views
    (ShadowAheadUsable: resources, modes and states, CS block sizes), with the frame's full-frustum planes in its latch and the frame
    record's per-frame buffers (FrameValues' ring entries, the tree wind's slot) written over the build's (uploaded around it: copies
    in one batch are unordered); otherwise it builds its own (shadowFallback: no kept state, no exclusion cache), counted. The commit
    still uploads from the vectors (against what the buffers hold: S2 moves them to the ring). Deleted: the shadow job, its early and
    BeforeShadowMaps kicks, the witness (ShadowInputsWitness), SameShadowInputs, StageShadowPayload, JoinJob, TakeJob, the probe
    (ProbeWorkerBuild), and the async stats' job fields. y27: 300 of 300 shadow epochs committed the payload built ahead in steady
    play (fallbacks at startup and once when a view's mode and state first appeared, a frame late), persistent shadow parity 0 differ
    (105k inputs), cascade-culling and sun parity OK, set parity 0, 7,446 sequences. m61: 0 crashes, the render-thread waits' only
    site the accumulate join at Present (m60: shadow join and cancel in 23 windows), occlusion maps all drawn.
  - *S2: the shadow payload in the ring* (y28, m62). The ring entry holds the installed shadow payload's material rows and each used
    mode's inputs (occlusion maps' included), filled by the producer with the main parts; the geometry table is the entry's (the main
    and shadow payloads build it from the same stream views and face streams). The shadow journals keep what the oldest entry lacks
    (RingHolders::shadowRows, shadowInputs). The shadow and occlusion views' latches carry the payload block (ShadowRingLatch: the
    mode's inputs, the geometry table, the shadow rows' address; no pipeline rows) and the arena's frame record names the entry's
    objects and extras; the occlusion epoch reads the entry the shadow commit read (ringShadow, cleared at the frame's end). Shadow
    rows hold no addresses: no patching, no shader change. A shadow commit that reads the entry uploads only the arena. y28: 300 of
    300 shadow commits read the ring, persistent shadow parity 0 differ, set parity 0. (The bridge's main material rows, 27 KB a frame,
    are 13.5 materials a build whose frame floats move: so since y17; the main rows report now names the key part that moved.)
  - *S3: the streams job out* (y31, m63). Nothing that reads the ring reads the scene buffers' object records or extras (the reflection
    draws with the colour commit's frame record, the entry's), so only a commit that reads no entry - a fallback build's - sends them,
    from the stream views (StreamsNow: the installed publication's, or made at once while the coordinator and the builds task are
    idle; refused and counted otherwise). Deleted: the streams job (KickSceneStreams, its AsyncWorker staging, JoinSceneStreams,
    DropSceneStreams, StreamsForBuild, its report). The extras parity runs in the builds task, which owns the store.
  - *S4: the fade write-back a task* (y32, y33, m64, m65). The write-back of the stood-in roots' fade milestones onto their nodes is a
    task on DCLF's executor, posted at the frame's start when none runs, never joined: it drains the batches itself, writes under the
    read window's leases (what the window's close leaves goes to the next task), and holds the frame's tables snapshot - so its
    publication's retirement node, and the root nodes it names - while it runs. Its three joins (the frame's start, BeforeShadowMaps,
    the accumulate phase) are deleted; DrainAsync waits for it. m64: ~1,300 milestones written, 0 stale, 0 frames found a task running.
    On the way: ShadowAheadUsable no longer compares the capacities a payload was built within (a ring entry is sized from the
    payload, the buffers only grow: those were the remaining shadow fallbacks), AheadUsable compares the face positions' address the
    geometry rows name, and the builds ahead are made for every shadow mode and state drawn so far (lastShadow, sticky: a point
    light's paraboloids or an occlusion map that come and go find their inputs built). y33, m65: in-frame builds only in the first
    frames (none built yet), set parity 0, every parity 0 but FrameValues' known 2 rows, the render-thread waits' one site the
    accumulate join at Present (0.001-0.12 ms/frame). AsyncWorker now runs only the scene lane (F).
- **F: the scene lane reads no engine memory** (decided 2026-10-07). The scene lane is joined at Present (and at the frame's start)
  because it reads the engine's scene graph live, and runs engine code on it, while the engine's next update writes and frees that
  memory (the m59 crash in RefreshCategoryNodes; the hidden flips after the drain). The lane takes no read leases: every engine field
  it reads becomes a value captured by the hooks on the writer's thread and carried by the events into a mirror the lane applies in
  order (CS_DCLF_MIRROR_PARITY compares it with the live scene at the frame's start, naming the writers the hooks miss); the fade
  write-back's engine stores are the render thread's; FrameValues keeps its leases (its sampling is the frame's by nature). Steps:
  F1 engine code off the lane, F2 the frame capture (frame globals, category roots), F3 the mirror and its parity (lane still reads
  live), F4 the lane reads the mirror (a guard fails loudly on any engine access from it), F5 the joins out (an intent mailbox
  drained by a serialized pump on the executor's coordinator; outputs as publications and queues), F6 AsyncWorker deleted.
  - *F1: engine code off the lane* (y34, y35, m66). Inventory of what the lane ran: the skin palette update in WriteObject; the
    switch catch-up (FUN_140d29990 and the child's UpdateDownwardPass: transforms written) from ApplySwitchEvents and AddSubtree;
    NiPointers made from raw keys (the placement plan's roots, the candidates' held entries); the engine's
    DetermineUtilityShaderDecl; the membership witness sampled by both the render thread and the lane; LightLimitFix's room map read
    while the render thread swaps it. Now:
    - the record's palette rows are the skin data's bone count (the engine's update copies it into numMatrices; FrameValues runs that
      update before it samples, and counts a palette of another size);
    - the catch-ups are the render thread's at ingestion (SceneStore::CatchUpSwitches): every switch event whose selection changed,
      every switch under an attached subtree, each reached from Main::WorldRootNode (a subtree a loader still assembles is not
      walked), and every switch in the world on the first ingestion after a load (what the rescan's AddSubtree did);
    - the roots the scene work lists hold a reference taken when first listed (rootOwners); the plan's roots and entries and the
      candidates' snapshots copy it, and a root no list names is left out, logged;
    - DetermineUtilityShaderDecl (vtable 0x3D, 0x1414adf00) is ported: a function of the property's flags alone, never 0;
    - the membership witness is sampled once by the render thread at the frame's start (BeginFrame), the commit and the binds read
      that copy; the room map is copied by the render thread when its generation moves (PrepareAccumulatePhase);
    - the fade write-back task finds the stores (node, fade bits, fade) from the frame's tables and holds them with that snapshot;
      the render thread makes them at the next frame's start (m66: 16-301 milestones a window, 0.7-3.8 us a window), never under a
      lease.
    Left for F3/F4 (captured at the hooks): treeObjectOf (a virtual call), GetExtraData("BSX"), the extra list's emittance source,
    the actor's race and keywords, the property and material owners the accumulate phase takes. y34/y35: sequences 7,404-7,445, set
    parity 0, every parity as y33 (y34's three fade visibility windows are the intermittent seen in y18-y26; y35 0 in all); m66:
    waits at the accumulate join only (0.07-0.19 ms/frame), 0 errors.
  - *F2: the frame capture* (y36, y37, m67-m70). The engine globals the scene reads are one immutable capture the render thread takes
    at the frame's start (Scene/FrameGlobals: the loading screen, the interior test, decal bias, the [LightingShader] LOD fade
    settings, SetupTechniqueDescriptor's bytes, the shadow global, StaticShadowBits' accumulator byte and switches, the fade update's
    constants, the main camera for SampleLodFadeFrame, the tree wind, the cull-hidden roots, and the membership witness made from
    them). The scene work's tasks bind their frame's capture (FrameGlobals::Scope, KickSceneTask); a helper shared with the render
    thread reads the render thread's latest; any other thread reading it unbound is counted and logged (0 in every run). The
    Lighting shader is found by the render thread among the frame's registrations (DrainCapture), not by the walk.
    The category nodes and the portal graphs' parentless roots are a CategoryCapture the render thread makes at the frame's
    ingestion when CategorySignature moved, a detach was ingested (at Present too) or a load ended; it holds every node it names, and
    RefreshCategoryNodes diffs its set against the newest (the always-render roots are copies of its references). A capture taken at
    Present (the first version) is the update's to change before the scene work diffs it: m69 crashed in AddSubtree walking a new
    category's children on the lane (a dangling child, the m59 class F4 removes), so captures are taken at the frame's start only.
    Cost on the render thread: 6.9-8.1 us/frame for the globals (24 us before the INI settings were looked up once), 1.3-6.3 us/frame
    for the categories (8-116 captures in 300 frames, most while moving). Tracked sets as before (127 category nodes; m67 against m66
    at the same points); y37 every parity OK; m70 waits at the accumulate join only.
  - *F3a: the mirror and its parity* (y38-y41, m71; the scene work still reads live). Engine/SceneCapture: node, geometry, property
    and alpha records of what the classification reads and holds still between writers (per-frame values - transforms, bounds,
    currentFade, the LOD level, the flag bits the culls and fade updates write: kTopFadeNode, kIgnoreFade, kRenderUse, kAccumulated,
    27 - are FrameValues' and the events', not records). Records hold no references: keys are addresses. SceneTracker's attach hook
    captures, after the engine's call on the attaching thread, an attach that lands in the world (Main::WorldRootNode up the parent
    chain): the subtree and the ancestors; a subtree still being assembled is captured whole when it is attached to the world. A
    detach in the world lists its nodes too, and the mirror (Scene/SceneMirror, the scene work's) drops them; properties and alphas
    are counted by the geometries naming them. A load screen's attach and detach events are carried to the next batch for the mirror
    (the tracking still discards them), so the mirror continues across loads.
    CS_DCLF_MIRROR_PARITY: the render thread captures a slice of the tracked set (256 geometries and their ancestors) live at the
    frame's start; the scene work compares it with the mirror after the frame's events. A field that differs is evented (an event
    named the object since its capture: a hook without its value, F3b), late (named by the next batch) or missed (no event: a writer
    no hook sees, F3c); the report names the field and the first object.
    Measured: world attaches come from loader and job threads, never the main thread in these runs: at the bridge a few a window, in
    motion up to 2,700 a window at cell changes (43k records, 17 ms of capture on those threads in 300 frames, ~0.4 us a record). The
    mirror follows the tracked set through the cell changes (m71: 12.4k geometries against 11.9k tracked, 6.5k against 5.8k, ...);
    absent from it ~1,300 of ~320k records compared a window (properties and alphas a swap installed). Probe 0.22 ms/frame on the
    render thread and check 0.17 ms on the lane, parity only; applying the captures 0.06 ms/frame. Standard parities unchanged.
    Found (y41 bridge, m71 motion; probe observations, each stale object counted every pass):
    - evented, F3b: hidden bits, controllers and bodies (node events), fade near/far and the LOD type (fade events), the alpha
      property's flags, property flags, material and fade node (property events).
    - fade near/far: one setter, FUN_14147a9b0 (near clamped to a minimum, far to twice it), called by the model and reference attach
      (FUN_14021f200, from the node's radius) and an FX/projectile path (FUN_1407cfba0): hooked with values in F3b.
    - missed, F3c: geometry property and alpha pointer swaps (LOD terrain blocks: 'Block (3, 0)', 'objHD-LargeRef'), property flags
      and material changes with no SetFlags/SetMaterial, hidden bits on objects no patched store names (the water's 'CurrentPlane'),
      the LOD type on the sky's clouds, rigid body motion, dismember editorVisible, flag bits 11/12/20.
  - *F3b: the hooks carry values* (y42-y49, m72). An update (SceneCapture::Update) is the fields a writer changed, captured after
    the write on the writer's thread, and goes onto SceneTracker's stack as a third event type (Updated), in one order with the
    attaches and detaches: an address a detach let go and an attach took again never gets an earlier object's values. The mirror
    applies an update into the record it holds (SceneMirror::Update) and ignores one for an object out of the world (its attach
    captures it whole). Carried: the hidden bit (the 464 patched stores' stubs), property flags (and the glints they decide),
    material, material alpha, external emittance and controllers (the property setters' detours), node controllers
    (PrependController), the fade range (a new detour on its only setter, FUN_14147a9b0), the LOD type (FUN_14147aa00, its setter
    for the model and 3D paths), and the fade statics after the cell's placement of a reference (FUN_1402d1280, FUN_1402d5090: new
    detours; they write the LOD type inline, and one branch skips the fade snap). The parity names a field only by an update that
    carried it; the events without values (switches, fade snaps, LOD segments) still name the object.
    Found on the way: the stale fade fields (about 10,500 a window, every one captured during the load) were the capture's own: it
    read a fade node's statics only when netimmerse_cast<BSFadeNode*> passed, while the engine and the hooks reach fade nodes by the
    vtable's AsFadeNode, so the updates filled records the probe read as zero. The capture uses AsFadeNode now. +0x109's 0x40 is the
    cull's, every frame (a watchpoint showed it), and is not the record's. CS_DCLF_MIRROR_WATCH (Diagnostics/MirrorWatch): hardware
    watchpoints on a fade node's statics after its placement (1, or a node's name), the writer named by the instruction.
    Measured: about 50k updates a window at the bridge (nearly all hidden-bit flips by the culls' stores), 92k in motion (32k to
    objects out of the world). Mirror parity: late 0; hidden, property flags and material, fade range and type all 0 missed (y41:
    3.6k, 1k, 7k, 4.5k). Left for F3c (no hook): node flag bits 11/12/20, rigid body motion, renames, dismember, geometry property
    and alpha swaps (LOD blocks), the property's fade node; the geometry alpha the LOD segment events name (evented). Standard
    parities as y41/m71.

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
