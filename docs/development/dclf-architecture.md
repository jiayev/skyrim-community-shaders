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

The render thread drives everything; one worker thread (`Common/AsyncWorker`) runs the builds; the GPU runs the epochs.

| Point in the frame | Render thread | Worker | GPU |
| --- | --- | --- | --- |
| `Main::Draw`, before the main cull (`BeginSceneFrame`) | Applies the scene events, then the **scene phase** (`SceneStore::BuildFrame(Scene)`): every tracked object's record, from the events since the last frame. A kept record's placement and palette are queued, and kicked to the worker at the end | **scene placement** | |
| The engine's cull jobs | The engine registers the passes it keeps. `PassCapture` records every registration and **withholds** DCLF's objects. `PrimaryCull` stands in for DCLF's references inside the list jobs. | | |
| `BeforeShadowMaps` | Joins the scene placement job (`SceneStore::JoinPlacements`), rebuilds the frame's shadow view list (`ShadowViews`) and kicks the shadow build | **shadow** build | |
| Each shadow view's `FinishAccumulating` | `CaptureShadowView`: where the engine drew, and with which constants | | The engine's own shadow draws |
| `AfterShadowMaps` | Joins the shadow build and runs the **shadow epoch** | | Every captured view's casters, culled per view |
| Skylighting's `RenderOcclusion` | `CaptureSkyOcclusion`, then the **sky epoch** | | The occlusion map |
| `EarlyPrepass` | The **accumulate phase** (`BuildFrame(Accumulate)`): patches each record with what the main camera's registrations decided. Then the pipeline lookups, the hand-back of what DCLF cannot draw, and the Z-prepass kick. | **zprepass** build | |
| `Main_RenderDepth`, world drawn | `CaptureDepthPass`: the **Z-prepass epoch** | | DCLF's depth, the HZB, two-phase occlusion culling |
| `Prepass` | Latches the accumulator, runs `RefreshFrameConstants` (the camera-dependent constants), kicks the colour build | **colour** build | |
| The main pass's opaque batches start (`BeforeOpaquePass`) | `CaptureMainPass`: what the pass binds | | |
| The opaque batches end (`AfterOpaquePass`) | `ExecuteColour`: the **colour epoch**, then the claims for the next frame | | The drawn set into the G-buffer, depth-tested EQUAL, then decals |
| `Present` (`Reset`) | Joins what is left (`EndFrame`), applies the scene events, handles the menu toggle, writes the report | **primary feedback** decode | |

The scene placement job takes the kept records' placements and bone palettes (the engine's palette update is
thread-safe: it locks the skin instance and runs once a frame). From its kick to its join the render thread writes none
of those columns and no table grows, and the engine's work in that window (the main cull, the water reflections)
moves no transform. A late join waits for it, or takes its items inline when it had not started; a walk-parity frame
takes them inline before the parity reads the tables. Under `probe` the join takes every item again and counts those
that moved inside the window.

A build runs on the worker between its kick and its join. At the join the epoch checks that the job was built for
exactly its own inputs (`SameInputs`, `SameShadowInputs`). If it was not, or the job is late, the render thread builds
the payload itself. `CS_DCLF_ASYNC=off` builds everything inline; `probe` builds both and compares them byte for byte.

## The data

```
engine writers ──events──▶ SceneStore tables ──change log──▶ kept stores ──▶ payload ──commit──▶ epoch (GPU)
 (hooks)                    (Scene/)                         (Draws/)        (worker)  (render thread)
```

1.  **Events.** Hooks on the engine's writers push events from whatever thread makes the change:
    -   a subtree attached or detached (`SceneTracker`);
    -   a fade stepped, or a property's flags or material set;
    -   a controller added, or Havok moving a node;
    -   a switch node's index changed;
    -   a material's value changed (`MaterialSources`).

    The render thread drains them (`EventQueue`). Nothing about an object is re-read on a schedule: a change the
    events miss is a defect, and the walk parity check finds it.
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
    -   the constant blocks and binding records per (material, pipeline) pair (`PersistentBindings`, `BuildCache`);
    -   the resident objects' draw inputs (`ResidentRegion`);
    -   the drawn set (`DrawnMarks`);
    -   the shadow inputs and records per render mode (`ShadowKept`).
5.  **Payload.** A build is a pure function from the tables and lookups to a payload: the build's constant arena,
    binding records, draw inputs and the kept stores' changes. `BuildMainPayload` (`Draws/IndirectDraws/MainBuild.cpp`)
    and `BuildShadowPayload` are the two builders. On the worker, the payload is also staged into an upload batch.
6.  **Commit and epoch.** On the render thread, the commit adds what only it has: the frame's constant buffers
    (`ConstantMirror`), the per-frame textures and the latch. Then one render-graph epoch runs the GPU work:
    -   `BuildDrawsCS` culls the inputs (frustum, and in the Z-prepass two-phase HZB occlusion) and writes the
        indirect draw sequences;
    -   the draw passes execute them;
    -   the colour epoch draws exactly the set the Z-prepass decided (the per-object visibility words).

## Ownership: how the engine stops drawing DCLF's objects

An object is withheld from the engine only after DCLF has drawn it. The claims are what the last epoch drew.

-   **Main pass.** `PassCapture` hooks `BSBatchRenderer::RegisterPass`. A claimed geometry's pass is recorded (the
    accumulate phase reads it) but never reaches the batch renderer, so the native loop has nothing to draw. A pass
    that is fading is not withheld, because the accumulate phase gives it no bindings. At `EarlyPrepass`, before the
    depth and main passes, a withheld pass DCLF cannot draw this frame (no bindings, or its pipeline not built) is
    handed back to the engine (`HandBackUndrawable`).
-   **Shadow views.** The same, per shadow render mode, from the shadow build's claim sets. This includes the direct
    group insertions the registration makes without `RegisterPass`.
-   **The sun's registrations.** `SunAccumulation` skips the registration of a claimed caster in the sun's
    `Accumulate`, reproducing its mask write, so no pass is built only to be withheld.
-   **The sun's culls.** The sun entry exclusion removes entries whose every object is DCLF's from the cascade culls'
    object arrays. The GPU applies the same entry rule to DCLF's inputs (`kCullSunEntry`).
-   **The main camera's cull.** `PrimaryCull` stands in for DCLF's references inside the engine's list jobs. It keeps
    the per-object state the cull maintains (fades, LOD, the tree clock's bit) and builds synthetic passes on the
    worker, so the engine never traverses those references.

The menu's toggles (`Common/Toggles`) change all of this live. Switching DCLF off drains the worker, clears every
claim and bypasses the capture.

## Where the code is

`src/Features/DrawcallLimitFix.{h,cpp}` is the feature: its hooks, the frame's order above and the menu. Everything
else is under `src/Features/DrawcallLimitFix/`:

| Folder | What is in it |
| --- | --- |
| `Common/` | The switch registry (`Switches`) and the live toggles (`Toggles`), the worker (`AsyncWorker`), the event queue, and the building blocks of the kept state: `KeptState`, `SlotTable`, `FrameRecordPatches` |
| `Engine/` | Everything that hooks or reads the engine directly: `PassCapture`, `PrimaryCull`, `SunAccumulation`, `ShadowViews`, `SceneTracker`, `FaceSnapshots`, `ConstantMirror`, `EngineStates`, and `EngineAccess.h` (the shared raw-access helpers) |
| `Scene/` | `SceneStore` (interface `SceneStore.h`, implementation `SceneStore/`), the record layouts (`Records.h`), the lookups, and what a record is derived from: `MaterialSources`, `ConstantEvaluator`, `LightingConstants`, `LightingDescriptors`, `VertexInput` |
| `Draws/` | `IndirectDraws` (interface `IndirectDraws.h`, statistics `IndirectDrawStats.h`, implementation `IndirectDraws/`), the pipelines (`DrawPipelines`), the SPIR-V programs (`ShaderPrograms`, `SpirvReflection`), and the game's buffers and textures as the graph sees them (`GpuResources`, `GpuTextures`) |
| `Published/` | The asynchronous publication foundation, deliberately not wired in yet ([dclf-async-publication.md](./dclf-async-publication.md)) |
| `Diagnostics/` | Capture parity, the periodic report (`Report.cpp`), the open defects' probes and the test harness |

Inside `SceneStore/` and `IndirectDraws/`, `Internal.h` is shared by that folder's files only:

-   `SceneStore/` is split by the tables' life cycle: tables, events, classification, the walk, the accumulate phase,
    the frame constants, residents, sun candidates, slots and parity.
-   `IndirectDraws/` is split by the path a frame takes: passes, resources, kept stores, the main and shadow builds,
    material lookups, the main and shadow epochs, uploads and diagnostics. The layouts the shaders read are in
    `GpuLayouts.h`, and must match `BuildDrawsCS.hlsl` and `Lighting.hlsl`.

Includes within a folder use bare names. Includes across folders are rooted at `src/`
(`Features/DrawcallLimitFix/Scene/SceneStore.h`).

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
| `CS_DCLF_ASYNC=probe` | Each worker build against an inline one |
| `CS_DCLF_CAPTURE_PARITY` (with `CS_DCLF_OWNERSHIP=off`) | The tables against the engine's own lighting draws: descriptors, transforms, every constant group, textures |
| `CS_DCLF_SET_PARITY`, `CS_DCLF_DEDUP_PARITY`, `CS_DCLF_BINDLESS_PARITY`, `CS_DCLF_PASS_PARITY`, `CS_DCLF_CAPTURE_POINT_PARITY` | Narrower checks of one mechanism each (see their registry rows) |

The standard validation is two 60-second runs on the same save:

1.  The first seven rows together.
2.  Capture parity with ownership off.

The CPU tests in `tests/DrawcallLimitFixCPU` cover the kept state and the publication foundation.
