# Crash catalog

Crashes met while developing the Drawcall Limit Fix, from the crash logger's logs in
`Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`. AE 1.6.1170 addresses, with the Address Library
ID in brackets. Newest first.

## Open

### `PrimaryCull::EndFrame`: a freed root in the scene lists' graveyard (event order)

| | |
| --- | --- |
| Seen | 2026-10-04, every run of the phase 3 scene-task build whose list filter (`PrimaryCull::PublishListFilter`) ran before the frame's events (`SceneStore::ProcessEvents`). Fixed by the order. |
| Thread | The render thread, at Present (`DrawcallLimitFix::Reset`, `PrimaryCull::EndFrame`, `listGraveyard.clear()`). |
| Fault | Execute at 0x0 or 0x1: a `NiPointer` release whose object's memory was back on the heap's free list (its first word a free-list link, the rest zero). |
| When | The first 40 frames after the load, at the first frames that published a list filter (Rebuild with a filter, then without). |

**The bisect** (inline, `CS_DCLF_ASYNC=off`, so not a threading race):

| List filter published | Runs that crashed |
|---|---|
| before the events | 5 of 5 |
| after the events, before the walk | 0 of 1 |
| after the walk | 0 of 3 |

The scene lists' per-frame decisions were logged in both orders: Keep and Rebuild, the filter, the kept sizes, and an empty
graveyard at the job. They matched. Every root the instrumentation saw go into the graveyard (removals, attach events,
dropped events) had a reference count of 6.

**Attribution: unknown.** The freed root entered the graveyard by `BuryLists` (a whole list buried on a Rebuild), which
was logged only by count. The events drain the scene tracker's queue, whose events hold references to attached and
detached subtrees. That is the likeliest link, but it is not shown. The order (events first) is kept and documented in
`DrawcallLimitFix::BeginSceneFrame`.

### GPU device loss: a page fault at address 0 on the first DCLF frame

| | |
| --- | --- |
| Seen | 2026-10-04 17:28 (run p2-wake, the first run of the phase 2 build of the threading rework). Not reproduced in the three runs of the same build that followed (p2-wake2 to p2-wake4). |
| Where | The Riverwood bridge save, frame 4 of the session: the first frames DCLF drew, while its pipelines were still being built on demand (the log's "on-demand pipeline build" lines) and its shadow sequence buffers grew (`shadow view slot 0 sequences: 64 draws grown to 16384`). |
| Fault | Aftermath (`gpu-crash-2026-10-04-17-28-55.nv-gpudmp`): page fault at VA `0x0`, fault type 0, access 0, engine 0, client 0; no active shader, no markers (`CS_GPU_CRASH_ANALYSIS` was off, so DXVK's checkpoints were not recorded). |
| Sequence | BasicRHI first saw ORG's timelines read `UINT64_MAX` ("poisoned timeline") on the host's thread; the async host stopped, and the Z-prepass's submission failed with "Async epoch host stopped before accepting its completion", which disabled the render graph. DXVK reported the device lost 10 s later, and the game hung (not responding) until it was killed. |
| Log | `build/dclf-profiles/20261004-p2-wake-device-loss/` in the SARP workspace: the game log and the Aftermath dump. |

**First reading (superseded: attributed below).** A read of address 0 is a buffer device address that was never filled in: a null BDA in
an indirect draw, a dispatch argument, or a table row read before its first upload. That points at something DCLF
draws in its first frames, not at a steady-state path.

The build had two new ORG changes (dclf-async-publication.md, "Phase 2"): the host's lazy wake, and compute-queue
submissions made on their own thread. Neither changes a GPU dependency. The wake only changes when the host's thread
reads its mailbox. A compute submission still waits on what ORG makes it wait on: the stream point, and the graphics
values of the epoch's uploads. It only reaches `vkQueueSubmit2` later.

**Second occurrence, and the bisect (2026-10-04).**
- p3-churn (a later build, the claims a frame late) faulted the same way on frame 4: VA 0, no shader, no markers
  (`gpu-crash-2026-10-04-18-36-35.nv-gpudmp`).
- The first frames of the crashed and the clean runs log the same growth, in the same order: the material rows 1024 to
  2048, the shadow view sequences 64 to 16384 on frames 3 and 4, then the shadow material rows 256 to 512. The fault
  follows the last of these.
- A 12-run startup bisect (`startup.sh`, 12 s in game each) alternated the compute submitter with
  `CS_ORG_COMPUTE_SUBMIT=inline`: 0 losses in either arm.

So the rate is about 2 in 24 runs of the phase 2+ builds, against none across the dozens of runs before phase 2. Still
not attributed, and not yet bisectable at that rate.

**What the later runs showed (2026-10-04, evening).** The loss kept recurring on short startup runs (`startup.sh`, 12 s
in game): about 1 in 8 to 1 in 4 startups, always on the first DCLF frames (4 to 6).

- **With crash analysis** (`CS_GPU_CRASH_ANALYSIS=1`, run hang2):
  - DXVK's fault report: "Memory address: 0x0 (type 2)", a GPU *write* to address 0;
  - Aftermath's last finished marker: "CS DCLF: Z-prepass inputs";
  - so the write is in the Z-prepass epoch's work, on its first executions.
- **Bisected, and not the cause:**

  | Change | Arm | Losses |
  |---|---|---|
  | The host's lazy wake (`CS_ORG_LAZY_WAKE`, TEMP, removed) | on | 2 of 8 |
  | | off | 2 of 8 |
  | The frame-slot ring's size (`CS_ORG_FRAMES_IN_FLIGHT`, TEMP, removed) | 12 slots | 1 of 8 |
  | | 24 slots | 1 of 8 |
  | The compute submitter | thread | 0 of 6 |
  | | inline | 0 of 6 |

  The scene task and the claims a frame late arrived after the first two occurrences.
- **Not yet done:** finding which Z-prepass pass writes through a null device address on its first executions. The
  candidates are those whose output address comes from a latch or push data the commit fills (build-draws' sequences and
  counts, the tree-LOD cull, the fade state). It probably predates this work, and only became visible with the many short
  startup runs.

**Attributed (2026-10-05): compute submissions made beside DXVK's.** A startup loop (`CS_DCLF_TEST_EXIT=200`, the game
relaunched at once, about 30 s a run) made the loss frequent enough to bisect. Each arm ran until its first loss:

| Arm | Losses |
|---|---|
| Compute queue off (`CS_ORG_COMPUTE_QUEUE=0`) | 0 of 29, and 0 of 20 earlier |
| Each compute batch after all earlier graphics work | lost at run 4 |
| Tree wind alone on the compute queue | lost at run 3 |
| Pass statistics (timestamps) off | lost at run 1 |
| Tree wind's dispatch skipped (the batch holds only ORG's barriers) | lost at run 14 |
| The batch submitted without command buffers (waits and signals only) | lost at run 1 |
| The same, submitted inline on the render thread | 0 of 28 |
| The submitter thread, under DXVK's submission lock | 0 of 39 |

So nothing a compute pass reads or writes, and no missing GPU dependency: a submission of only semaphore operations
faulted. It faulted only when the compute submitter's `vkQueueSubmit2` could run at the same time as DXVK's submission
thread. Vulkan asks only that each queue be externally synchronized, so the conflict is below DXVK (a layer in front of
the driver, or the driver's timeline semaphores across queues); which one is not established.

**Fix.** Every queue of DXVK's device is used under DXVK's submission lock (`RenderGraphRuntime`'s `LockQueue`), the
compute submitter included; it takes the lock on its own thread, never the render thread's. Also fixed on the way: the
stream's wait at an epoch's end was on the graph's compute timeline, which a graph rebuild destroys while DXVK may still
hold that wait. It now waits on a semaphore of the runtime's own (`computeExit`), which every compute submission
signals and which lives as long as the device. The second change alone did not stop the loss (a build with it, and
without the lock, lost the device at run 11); it is a correctness fix of its own.

**Verified.** The final build (both changes, no switches): 0 losses in 40 startups. One of the 40 crashed on the CPU
instead, a separate race in ORG's resource tracking: see "`flecs::set` writes through null" below.

What the bisect also found: the Aftermath dumps carry no page-fault details or markers for this fault, Vulkan validation
reports nothing (GPU-assisted validation disables itself on DCLF's indirect-bindable pipelines; synchronization
validation does not see descriptor-heap accesses), and a startup run exits before DCLF draws if `CS_DCLF_TEST_EXIT` is
below about 200 (the counter includes the main menu's frames).

### `flecs::set` writes through null: ORG's resource tracking from two threads

| | |
| --- | --- |
| Seen | 2026-10-05 02:24, run final14 of the device-loss verification (1 of 40 startups), frame 7. |
| Thread | The render thread, in the shadow epoch's commit: `CommitSceneStreams`, then `StagedUploadBatch::Stage` creating a buffer, then `DeviceManager::CreateResourceTracked`, then `EntityComponentBundle::Set<ResourceID>`, then `flecs::set`. |
| Fault | `mov [r8], rax` with `r8 = 0`: flecs returned a null component pointer and ORG wrote the value through it. |

**Cause.** Every ORG resource has an entity in a flecs world (memory statistics). Creation changed the world under
`DeviceManager`'s creation mutex; destruction changed it with no lock at all, on whichever thread released the resource.
Since async epochs, retired backings are released on the graph host's thread (`message.retired`, the deletion queues), so
a destruction there and a creation on the render thread could change the world at once. flecs is not thread-safe.

**Fix (ORG, self-contained).** The world has one owner at a time (`ECSManager`): every change is posted to a wait-free
queue and applied by whoever drains it. Posters never wait, and only readers (the memory view) do. Tracking tokens are
created deferred. See ORG's README, "Resource tracking", and `tests/TrackingWorldTests.cpp`.

### `SkyrimSE+14F79AA`: a material virtual call during a model load

| | |
| --- | --- |
| Seen | 2026-09-22 23:32, 23:52, 23:58; 2026-09-23 00:12, 19:52. The variant below: 23:57 and 00:07. |
| Thread | An IO thread (`IOManager::DoOnPreRunTask` on the stack), loading a model: `BSResource::EntryDB<BSModelDB>`, `BSStream`, `BSResourceNiBinaryStream`, `CompressedArchiveStream`. |
| Fault | `call [rax+0x18]` in `FUN_1414f7850` (107719+0x21A) reads 0xFFFFFFFFFFFFFFFF: a virtual call through a vtable that is not one. |
| Objects | R14 a `BSLightingShaderMaterial`, and on the stack the `BSLightingShaderProperty` being loaded and its `NiAlphaProperty`. |
| Stack | `FUN_1414f7850` (107719) <- `FUN_141476cc0` (105544) <- `FUN_1414ac820` (106494) <- `SkyrimSE+0D21AE7` (70381, which the logger labels `NiPSysSphericalCollider::Func38`, a mislabel). |
| Variant | The same call jumps to address `0x6` ("tried to execute memory at 0x6"), returning into `SkyrimSE+14F79AD` (107719+0x21D) from `FUN_140d25d00` (70502). Same function, same vtable call, different garbage. |
| When | Loading cells: repeated `coc`, and the test harness's flights. Seen with `CS_DCLF_FADING=0` too, so it is not the fading work. |
| Models | The geometry on the stack when named: `Door:7` (23:58), `RoadCurve90R01:0` (19:52). |

**Attribution: unknown.** DCLF writes none of the engine's material or property memory. It keeps pointers
to materials as cache keys, rewrites only its own material records (`RefreshMaterialPatch`), and runs the
engine's material setup on the render thread (`ConstantEvaluator::EvaluateMaterial`). A material whose
vtable reads as garbage on the loading thread looks like a use after free, or an object being built while
something else reads it.

**What the faulting call is (decompiled, 2026-09-23).** The function starts at `1414f7790` (107719; the
logger names it by the block `1414f7850`). It is the engine's shared material cache: `BSShaderProperty::
SetMaterial` (`14147c033`, frame 1's `FUN_141476cc0` reaches it) calls it to swap a newly loaded material
for an identical cached one.

-   It takes the cache's spin lock (`1435ef070` owner thread, `1435ef074` count), hashes the incoming
    material (vfunc 4, `ComputeCRC32`), and walks the hash chain at `cache+0x30` (entries of `{hash,
    material, next}`, 0x18 bytes).
-   For each entry with an equal hash it calls the **cached** material's vfunc 3, `DoIsCopy(incoming)`, at
    `+0x18`. That is the faulting `call [rax+0x18]`.

So the object with the bad vtable is a material still linked into the cache, not the one being loaded. Its
vtable pointer is ordinary heap memory: `rax` is `0x1B67F66AAC0` at 19:52, and `[rax+0x18]` holds
0xFFFFFFFFFFFFFFFF or 6. A material freed while its cache entry survived, then its memory reused, fits
both. The dereference is the first touch of the stale entry, so the free happened earlier, on any thread.

**Pattern across the test runs.** Five of the seven crashes have a run log (`CS_DCLF_TEST_COMMANDS` runs;
00:07 and 00:12 have none):

-   Four came within about 1 s of the fourth `coc` of a four-`coc` run (23:32, 23:57, 23:58, 19:52).
-   One came 17 s after the second `coc` (23:52).

None of the 65 other logged runs with one or two `coc` crashed, and every four-`coc` run in the logs did.
Every crashing run had DCLF on, with static ownership and shadow views; `CS_DCLF_ASYNC` was set to probe,
set to on, or unset.

**Hooks near materials, for the record:**

-   DCLF hooks `CopyMembers`, `OnLoadTextureSet`, `ClearTextures` and `ReceiveValues` on 14 material
    vtables, and the two property controllers (`MaterialSources.cpp`). Each thunk calls the original and
    then only records the pointer (`NoteWritten`): no reference counting, no writes.
-   Several stack scans also hold `EffectShaderNoDecalsFix::BSTriShape_LinkObject::thunk`
    (`src/EngineFixes/EffectShaderNoDecalsFix.cpp:12`, `BSTriShape` vfunc 0x19). It sets `kNoDecals` on
    soft-effect geometry after the original. These are stack-scan hits ([S]), not probable frames, and
    likely stale return addresses from the same load.
-   The first `MaterialSources` commit is 2026-09-23 02:18 (`169489e2`), after the first crash (22 23:32).
    But the code was in the working tree before that, so this does not clear it.

**Next steps:**

-   Repeat a four-`coc` run (`CS_DCLF_TEST_COMMANDS=600:coc WhiterunDragonsreach;1200:coc Riverwood;1800:coc
    Whiterun;2400:coc Riverwood`) three times each with `CS_DCLF=0`, with DCLF on but ownership off, and
    with Community Shaders off. The crash reproduces reliably at four `coc`, so three clean runs are
    evidence.
-   If it follows DCLF: find who releases a material that the cache still holds. Hook the cache's removal
    and the material destructor (vfunc 0) under a switch, log pointer and thread, and match them against
    the faulting entry.

## Fixed

### `CommunityShaders.dll`: `NativeProbe::OnNativeLightingDraw` (2026-09-22 22:36)

An access violation reading `0x1FFDD`, inside `SetupGeometry`'s hook (`Hooks.cpp:230` -> `NativeProbe.cpp`).

-   **Cause:** the native probe read an `NiSwitchNode`'s `childRevID` array through CommonLib's members.
    CommonLib declares them after `NiNode`, whose declared size in a multi-runtime build is VR's, so the
    members read the wrong memory.
-   **Fix:** `SceneStore::ReadSwitch` reads them at the SE/AE offsets (engine notes, "Switch nodes").

## Earlier, not investigated here

From the logs of 2026-09-22 before the tree and actor work:

-   **01:06 and 01:10:** a C++ exception from `Util::` in `src\Utils\D3D.cpp:237`, called from
    `GrassOptimizations.cpp`.
-   **01:15:** an engine null read in `FUN_140e4c730` (77397), from `bhkConstraintChain::Func55` on the
    pathing and navmesh job.
-   **01:59:** a null read in `org::StatisticsManager::SetupQueryHeap`, from
    `org::runtime::CreateDefaultStatisticsService` during `RenderGraph::Update`.
