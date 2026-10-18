# DCLF architecture

How Drawcall Limit Fix (DCLF) works: the order of a frame, the data from engine events to GPU draws, how it takes objects
away from the engine, every object class and view it draws, and the reason behind each standing decision. It describes the
present only.

-   [dclf-layout.md](./dclf-layout.md) is the code layout as it is, the extension points and contracts features build on, and
    the rules `tools/dclf-layout-check.py` enforces. This document names code by file and symbol; the tree is there.
-   [dclf-validation.md](./dclf-validation.md) has the switches, the parity checks and what each report line means, the run
    procedure, crash triage, and the bugs parity found in the engine and in other features.
-   [dclf-open-defects.md](./dclf-open-defects.md) lists the defects and limitations still open.
-   [dclf-roadmap.md](./dclf-roadmap.md) is the work not built yet.
-   The engine behaviour DCLF depends on is in [skyrim-engine-notes.md](./skyrim-engine-notes.md); the render graph on DXVK
    is [render-graph.md](./render-graph.md).

DCLF runs on Skyrim AE 1.6.1170 only (`DrawcallLimitFix.cpp`: it does not install on another runtime). Every engine address in
the code and below is that runtime's.

## Contents

-   [Overview](#overview)
-   [The frame](#the-frame)
-   [Threads](#threads)
-   [The scene mirror, events and membership](#the-scene-mirror-events-and-membership)
-   [The scene lane and publications](#the-scene-lane-and-publications)
-   [Payloads, FrameValues and pooled versions](#payloads-framevalues-and-pooled-versions)
-   [Epochs and the graph](#epochs-and-the-graph)
-   [GPU culling](#gpu-culling)
-   [Ownership and withholding](#ownership-and-withholding)
-   [Shading: materials, constants, rows and bindless](#shading-materials-constants-rows-and-bindless)
-   [Fades and LOD cross-fades](#fades-and-lod-cross-fades)
-   [Object classes](#object-classes)
    -   [Opaque Lighting objects](#opaque-lighting-objects), [Ordered nodes](#ordered-nodes),
        [The forward epoch](#the-forward-epoch), [Refraction](#refraction),
        [The Effect shader in the forward epoch](#the-effect-shader-in-the-forward-epoch), [Particles](#particles),
        [Billboards and aligned particles](#billboards-and-aligned-particles), [Decals and pass layers](#decals-and-pass-layers),
        [Skinned objects](#skinned-objects), [Trees and actors](#trees-and-actors),
        [Terrain and ProjectedUV](#terrain-and-projecteduv), [LOD](#lod)
-   [Views](#views)
    -   [Shadows](#shadows), [The occlusion maps, drawn by DCLF](#the-occlusion-maps-drawn-by-dclf),
        [Water reflections](#water-reflections), [First person as a view bit](#first-person-as-a-view-bit),
        [Portals](#portals)
-   [What the engine still draws](#what-the-engine-still-draws)
-   [Trusted and parity](#trusted-and-parity)

## Overview

### Standing policies

1.  **DCLF owns all culling, on the GPU.** No design may rely on the engine's culling or registration results. The places that
    still read them are transitional; nothing new reads them.
2.  **No per-view lists.** The scene is one persistent list. What an object takes part in is a per-object bitmask of view types,
    tested by each view's GPU cull; the CPU never builds a list or a cut per view.
3.  **The normal path trusts its producers.** Checks are parity-only observers. The build is incremental, with no caps and no
    fallbacks: a fixed capacity that overflows is fixed by making it grow, never by a limit or a hand-back.
4.  **Events, not polls.** State that mirrors the engine's follows the engine's writers, deterministically. A change the events
    miss is a defect the parities find, never something a periodic re-read hides.
5.  **Lock-free by construction.** Data moves between threads as immutable publications, messages and pooled versions. No
    pipeline waits on a mutex in the normal path, and none is tuned faster instead of restructured.

### The model

The game draws each object with its own draw calls after culling it on the CPU. DCLF keeps a persistent mirror of every object
it can draw, fed by hooks on the engine's writers; a scene lane off the render thread turns the events into tables and
publishes immutable snapshots of them; a snapshot builder turns each into GPU payloads and a scene revision (the graph epochs'
recordings, made ahead). Each frame adopts the newest complete snapshot, tells the engine not to draw what it claims, and
submits the revision's epochs on DXVK's Vulkan device through the render graph: a GPU cull over the one scene list per view,
then indirect draws. Each object is drawn once, by DCLF or by the engine, never both and never neither.

### What DCLF draws

-   **The main pass:** the Z-prepass (two-phase HZB occlusion), the deferred colour pass, decals and pass layers, and blended
    objects, refraction, effects, particles and billboards in a forward epoch.
-   **Every shadow view:** the sun's cascades and their volumetric lighting copy, spot lights, point lights' paraboloids and the
    focus shadows.
-   **Both occlusion maps** while Skylighting is loaded: Skylighting's sky occlusion map and the precipitation mask.
-   **The water reflection's** plain cube faces, and **the first-person** view.
-   **Object classes:** static and dynamic Lighting geometry, terrain, trees (with GPU tree wind), skins of any number of
    partitions, actors and NPC faces, effects, particles, billboards, decals, and object, terrain and tree LOD.

-   **With Community Shaders' features**, each through its integration (`Integrations/IntegrationList.cpp`): Advanced Skin, Cloud
    Shadows, Dynamic Cubemaps, Exponential Height Fog, Extended Translucency, IBL, Light Limit Fix, Linear Lighting, Skylighting,
    Subsurface Scattering, Terrain Blending, Terrain Shadows, TruePBR, Volumetric Shadows and Water Effects. An integration supplies
    the feature's frame textures, material feature textures, per-object values and held groups as data; a feature's shader setup is
    never called per DCLF draw. A hook on a shader setup function must not assume every call is a real draw.

What stays the engine's is listed in [What the engine still draws](#what-the-engine-still-draws). Every feature toggle is on by
default (no toggle sets `offByDefault`, `Api/Settings.h`); a run that turns one off logs `featureset: REDUCED by ...`.

## The frame

### A frame, by thread

The render thread captures what only it can read and submits the epochs; it builds nothing. The scene is kept on DCLF's scene
lane and pool, and every frame draws the newest complete snapshot of it.

| Point in the frame | Render thread | Scene lane and pool | GPU |
| --- | --- | --- | --- |
| `Main::Update`'s entry (main thread) | The frame's engine writes, at the one safe point: `kAccumulated` (`IndirectDraws::WriteAccumulated`), the fade write-back's stores, then the stand-in observers ("Main::Update's entry", below) | | |
| `Main::Draw`, before the main cull (`FrameDriver::BeginFrame`, the shell's `Main_Draw_Early` thunk) | The graph's build point (`IndirectDraws::BuildPoint`); opens the engine-read window; applies the toggles; captures the frame globals and posts the frame inputs; the camera cut; adopts the newest complete snapshot (`AdoptCurrentSnapshot`, installed by `FrameView::HandOverAtFrameStart`), waiting for one only after a graph build or new resources; installs the claims; serves the scene lane's requests (switch catch-ups, the category capture, mirror capture requests); decides the draw families (tree LOD); publishes the scene-list filter; kicks the frame values (`FrameValues::Kick`) | **Scene pass** (`Coordinator::RunSceneWork`, whenever its producers wake the pump): events applied, walk, joins, `CommitSet`; then, at the snapshot builder's pace, `PublishScene`. **Snapshot builder** (`IndirectDraws::PostSnapshotWork`): payloads built, revision made and sealed, payload published as pooled versions. **FrameValues' producer**: placements, palettes, shading | The frame values' uploads on the upload queue; the payload's catch-up |
| The engine's list jobs | The engine registers the passes it keeps. `PassCapture` withholds DCLF's objects; `PrimaryCull` stands in for DCLF's entries inside the list jobs; whole entries are out of the scene lists | | |
| Inside `CalculateAndDrawShadowCasterLights` | The frame's shadow views begun and their coverage decided (`BeforeShadowCasters`), each kept light noted (`ShadowCasterKept`), the views listed from DCLF's selection (`ShadowCastersSelected`) | | |
| `BeforeShadowMaps` | The shadow views' revision inputs and frame | | |
| Each shadow view's `FinishAccumulating` | `CaptureShadowView`: where the engine draws and with which constants | | The engine's remaining shadow draws |
| `AfterShadowMaps` | The **shadow epoch** (trigger `shadow-maps.after`) | | Every view's casters, culled per view |
| `EarlyPrepass` | Posts the accumulate inputs for the next scene pass; answers the last pass's captures | | |
| `Main::RenderDepth`, world drawn | `FrameDriver::DepthCommitted`: the **Z-prepass epoch**, then the **reflection epoch** (trigger `depth.committed`) | | Tree wind and fade state (compute queue), texture transforms, phase 1, HZB, phase 2, visibility events; the reflection's faces |
| RenderDepth's first-person draw | The **first-person depth epoch** | | |
| Skylighting's `RenderOcclusion` | The **occlusion epoch** | | The occlusion maps |
| `Prepass` | Latches the accumulator; refreshes the camera-dependent frame constants; Light Limit Fix's light culling epoch | | |
| The main pass's opaque batches | `BeforeOpaquePass`: `CaptureMainPass`. `AfterOpaquePass`: the **colour epoch** (trigger `opaque.after`) | | The drawn set into the G-buffer, depth EQUAL; decals; Terrain Blending's terrain |
| The engine's blended draw | The **forward epoch** (trigger `blended.draw`) | | Blended objects back to front; refraction normals |
| `Main::RenderFirstPersonView` | The **first-person colour epoch** | | |
| `Present` (`FrameDriver::EndFrame`) | Closes the engine-read window; binds what the frame read to its last submission (`BindAdopted`, `FrameValues::EndFrame`); writes the reports; releases what the scene work let go of (`EngineReleases`) | | |

An epoch's position matters to the work after it. Streamline's upscalers and frame generation must not run on the GPU directly
behind an epoch: see [render-graph.md](./render-graph.md), "Hazard: external Vulkan work directly after a graph epoch"
(`Upscaling::SeparateFromGraphEpochs` separates them). Check that before adding an epoch or moving one late in the frame.

### The schedule

The frame's epochs and their passes are one table, `Schedule/Frame.cpp` (`kTriggers`, `kEpochs`, `kFrameOrder`); the pass keys,
owners and rules are [dclf-layout.md](./dclf-layout.md), "The schedule".

| Epoch | Trigger | Segment | Passes |
| --- | --- | --- | --- |
| `shadow` | `shadow-maps.after` | `ShadowView` | latched copies, index pool, build draws, the views' draws, cull counters |
| `main.depth` | `depth.committed` | `ZPrepass` | latched copies; tree wind (next frame); fade state, fade events; texture transforms; phase 1 build, index pool, tree LOD cull, depth draw; HZB, phase 2 build, tree LOD retest, rescues' depth; tree LOD counts; visibility events and readback |
| `reflection` | `depth.committed` | `Reflection` | latched copies, build draws, tree LOD's face cull, the faces |
| `fp.depth` | `fp.depth-draw` | `FirstPersonDepth` | latched copies, build draws, tree LOD face cull, depth, forward sort |
| `occlusion` | `occlusion.draw` | `SkyOcclusion` | latched copies, build draws, the maps' draws |
| `light-culling` | (Light Limit Fix's, live) | `LightCulling` | Light Limit Fix's clusters; not a revision's |
| `main.colour` | `opaque.after` | `MainOpaque` | latched copies, build draws, sort scan and scatter, forward sort, colour draws |
| `forward` | `blended.draw` | `Forward` | blended draws, refraction normals (shares `main.colour`'s shape, gated by the forward class) |
| `fp.colour` | `fp.colour-render` | `FirstPerson` | colour, forward (shares `fp.depth`'s shape) |

`main.depth`, `main.colour` and, while its class is on, `forward` decide the frame's claims (`EpochDef::claims`): a frame whose
claims withhold an object must run the epoch that draws it.

### The frame start and the wait

DCLF is persistent: once a frame installs claims, no frame hands drawing back to the engine until the user switches DCLF off.

-   **No claims** (the engine draws everything) only before DCLF's first covered frame (`FrameView::NotDrawingYet`: the tables,
    the main resources and the first revision being made; the first installation applies the whole set), under a load screen
    (the set does not survive a load: `NoteLoadingScreen`), or when `failed`. Every other frame draws the installed publication's
    claims (`FrameView::InstallClaims`), and after the first installation no claim is ever withdrawn: later installations replay
    the publications' join and leave notes.
-   **The wait.** What a snapshot cannot know ahead - a graph build (an extension added or removed, `BuildPoint`), new main
    resources (a new render size or frame buffers, made at the colour pass's `Setup`), new targets or a new shadow format - makes
    the snapshots made before it stale. The frame then waits at its start for the builder's snapshot made for the graph as built
    (`AdoptCurrentSnapshot`): a one-off stall on a rare event, counted on the strict epochs line ("waits at the frame's start").
-   **Nothing the builder needs is posted after that point.** The revision inputs are the build point's; the context for new main
    resources is posted at the `Setup` that made them (`PostResourcesAhead`; a work item made for other resources builds from it,
    `RefreshWorkContext`); the publication gate's scene-pass half is open while the frame waits; the owner thread's requests
    (shadow view slots, a build, the tickets a recording waits for) are served in the wait.
-   **Five seconds without a snapshot**, or a frame its adopted snapshot does not cover after the wait, is a defect
    (`stl::report_and_fail` with what it waited for or the coverage state), never a fallback.
-   **The one uncoverable frame** is the one whose colour pass made new main resources: no revision it adopted at its start is
    for them, so its colour epoch is not drawn (logged as `main resources made at frame N's colour pass`); the next frame waits.
-   **Explicit builds.** While revisions draw, an extension added or removed waits for the build point
    (`PersistentGraphHost::SetExplicitBuilds`, `BuildIfRequested`), the first thing the frame start does, so the graph a frame's
    recordings were made on runs through the whole frame. The build point also serves the revision code's owner-thread requests
    (`ServeRevisionRequests`) and posts the revision inputs (`PostRevisionInputs`).
-   **Tickets for recordings.** A live epoch's recording waits for the slot its unsubmitted ticket holds. The build point gives
    back the tickets of epochs the last frame did not submit (no water in view) while recordings are outstanding
    (`ReleaseTicketsForRecordings`, `PersistentGraphHost::ReleaseEpochTicket`); the frame-start wait does the same every 16 ms.
-   **Revision-driven epochs are required.** DCLF draws only from scene revisions' recordings, which need ORG's async,
    revision-driven epochs (`CS_ORG_ASYNC_EPOCHS`, `CS_ORG_REUSE_RECORDINGS`). Without them `BuildPoint` sets `failed`
    (`Core/Render/Revision/Coverage.cpp`).
-   **A toggle withdraws nothing.** It takes effect for the frames only when a publication committed under it is installed
    ([Ownership and withholding](#ownership-and-withholding)).

### The render thread

Every DCLF hook is a `Hook` of the render-thread budget (`Core/Engine/RenderThreadBudget.h`), reported every 300 frames. Its
time is split into buckets: `Exchange` (snapshot adopted, publication installed), `SceneCaptures` (globals, categories, mirror,
switch catch-ups), `Capture`, `Submit`, `OrgOverhead`, `EngineBoundary`, and `Other`, which names work that belongs off the
render thread. Per-draw hooks and the engine hooks DCLF edits on job threads are not counted there.

DCLF's render-thread time is about 1.06-1.14 ms a frame in a motion run (measured at Riverwood with the profile on, which costs
about 0.3 ms itself).

### Frame data through the render thread

Every byte a commit writes for the GPU is counted where it goes (`Core/Gpu/FrameData.h`), at the three ways it can:
-   a latched copy (`LatchedUploads`, `latched: <target>`): the commit stores into the epoch's latch block and the epoch's first
    pass copies it to the buffers the draws read (`MakeLatchedCopiesPass`, `Core/Render/Passes/Common.cpp`; used by the main
    epochs, the views and the shadow and occlusion epochs). A counter is zeroed from a never-written zero block. The copies are
    recorded with the revision's recording, so the commit records no copy commands;
-   a staged upload (`CommitUploads`, `staged: <target>`);
-   a latch-block write (`LatchWrite`, `latch: <what>`).

Per-publication latch bytes (the main commit's bucket map and tables, the shadow commit's map rows) are written once per frame
slot and again only when their key moves (`Draws::LatchRegionsHeld`, `Core/Render/Payload/Latched.h`); the shadow arena is
staged only in the 256-byte chunks that differ. Pool and FrameValues uploads are not the render thread's and are not counted.

### Main::Update's entry

All the main thread's engine writes happen at `Main::Update`'s entry (`PrimaryCull::AtUpdateEntry`, from the call at
`Engine::kUpdateEntryCall`): the last frame's job cycle has been waited for and this one's has not started, so no list cull, cell
animation or animation job runs. In order:
1.  `IndirectDraws::WriteAccumulated` (the `kAccumulated` bits, [below](#kaccumulated-from-visibility-events));
2.  the stand-in observers' `updateEntry`: `RoomVisibility::SetLive`, then Features/Fades' fade writers last
    ([The fade write-back](#the-fade-write-back));
3.  `afterWrites`: Features/Actors' animation stamp (`UpdateActorRate`).

The hooks and order are `Api/EngineEvents.h`. `PrimaryCull::BeginFrameCamera` runs at the frame start under the frame's number
and reads `FadeEye`, the camera cut and `TreeHeightTest` from the main camera, so it does not depend on the engine's list
processes running.

### Load screens

The render thread posts a load marker (`Scene::Channels::loadMarkers`). While a load holds, a pass drains the engine's queues
into the mirror's carry (`Coordinator::loadingCarry`), and the coordinator drops what the load invalidates (`ApplyLoading`). The
category capture is rebuilt when a load ends. A load screen's frames are claims-less whatever their snapshot.

## Threads

### The design

DCLF has one executor (`SceneExecutor`, `Core/Runtime/SceneScheduler.h`), started on first use before the first frame dispatches
a job, and never torn down:
-   **The coordinator:** one serialized thread for the frame's jobs.
-   **The scene lane:** one serialized thread for the scene pump's passes (`SceneLaneScope`), so a long pass never holds up the
    coordinator.
-   **The preparation pool:** `hardware_concurrency - 2` threads (at least 1; `CS_DCLF_WORKERS`), below normal priority: the
    passes' shards, FrameValues' producer, the snapshot builder (one at a time), leased frame-start jobs (`FrameStartJob`), the
    visibility decode and the fade write-back's judging.
-   The coordinator and the scene lane run above normal priority. Lanes are `SerializedTaskPump`s (`MakeLanePump`), never
    destroyed.
-   **The state graph** (`Core/Runtime/SceneGraph.h`, `SceneArtifact`): DCLF's `org::async::StateGraph` on the executor. Only
    `BufferVersion` has a producer (growth, `SceneGraph::PostGrowth`); the other enumerated kinds have none.

The render thread runs the hooks, `FrameDriver`, `Scene::FrameView` (the render thread's half of the scene), the engine-side
views and each epoch's commit and submission. Engine threads (cull jobs, loaders, Havok, animation) run the hooked writers, which
push events, and the list jobs `PrimaryCull` stands in for.

### The engine-read window

When a worker may touch the engine's scene graph (`Core/Engine/EngineReadWindow.h`): open from `FrameDriver::BeginFrame`
(`Main::Draw`) to Present, both on the render thread.
-   A worker touching engine memory takes a `Lease` per item or per `ParallelFor` chunk. A refused lease means the window closed,
    and the rest waits for the next window. `Close` waits only for leases in flight. One atomic word holds the open bit and the
    lease count.
-   **The window is not quiescent.** The engine writes `currentFade`, billboards and texture transforms inside it, and some update
    writers (Havok node transforms, `Update3DPosition`, `AnimationGraphPlace`) run inside the render phase. So per-frame values
    are sampled by FrameValues and structural ones come from events into the mirror; nothing assumes a still scene graph.
-   **The scene lane reads no engine memory**: it reads the mirror. The engine's read paths `Touch` the window; on a scene-work
    thread without a lease the access is counted and named (`<- LANE ENGINE ACCESS`). The scene work's live reads are parity
    observers only, each under a lease (`LiveCheckLease`). Plain field reads of pinned objects are not caught.

### Engine references

-   **Pins.** An attach's capture (`CaptureAttached`, on the attaching thread) holds every object it records and their
    ancestors; the batch keeps them until the render thread releases it at Present. The scene work's engine references are
    copies of pins (`HoldProperties`, the root owners). A key the mirror names that nothing pins is refused: a key is never turned
    into a reference.
-   **Pins only in the world.** A pin is taken only on an object in the world (`SceneCapture::InWorld`), where its parent holds
    it. Out of the world an object may have no reference at all: the engine keeps a clone's root at refcount 0 while it builds it,
    and the projectile clone pool keeps its master clone by a raw pointer for its whole life (`FUN_14021c800`, destroyed directly
    by `FUN_14021d140`). A pin on such an object frees it at Present under its owner. Leaf updates (`PushLeafUpdate`) are dropped
    out of the world, as the mirror's records are; the subtree is captured whole at its world attach.
-   **Releases.** No engine object's last reference is dropped on a DCLF thread. The scene work hands references back
    (`EngineReleases`, a lock-free queue released at Present; `HandBack`); `CS_DCLF_RELEASE_GUARD` observes it. Why: engine
    destructors run Havok teardown and crash off the main thread.
-   **Anything keyed by an engine node pointer holds the node** (`NiPointer`) for as long as a snapshot names it: the sun
    candidates hold their entry nodes, the scene lists' restore re-inserts only roots its candidates hold (`EntrySpace::held`),
    and `LocalLightCull`'s `CategoryFilter` holds its node. Detached roots are freed before the frame starts, while a filter made
    earlier may still name them.
-   **A buffer address is never an identity.** DXVK reuses a freed address at once: index pool ranges are per geometry slot,
    and `GpuResources::Prefetch` holds a reference while a buffer is cached.
-   **Engine materials are released through the engine's manager** (`Coordinator::MaterialReference`, `FUN_1414f7a40`,
    `Engine::Address::kMaterialRelease`), never through a `BSTSmartPointer`, whose release deletes the material without removing
    it from the material database (a later load crashes).
-   **GPU resources by thread domain** (`Core/Gpu/GpuResources.h`): `GpuResources::Scene()` (the scene work's geometry buffers)
    and `GpuResources::Frame()` (the render thread's tree LOD meshes), each its maps' only user. Lookup and texture owners are
    cleanup-queue owners (`ResourceCleanupQueue::Make`, `ExecutionResourceLease`), so the last drop anywhere destroys nothing
    there.

### Messages, not shared state

What passes between the frame and the coordinator outside a publication is a message: a `LatestSlot` (latest wins,
`Core/Runtime/LatestSlot.h`), an `EventQueue`, or the publication log. PrimaryCull's held notes are an `EventQueue<PrimaryNote>`
(`DeliverPrimaryNotes`); the frame inputs, the ahead context (`TakeAheadContext`), the room map and the pipeline frame are posts
the pass takes at its start. Replaced publications return to the coordinator, which drops them (`retiredPublications`).

-   `EventQueue` (`Core/Runtime/EventQueue.h`) is a bounded Vyukov ring that spills to a lock-free stack when full. It never drops
    an event, because no consumer can rebuild what a lost event would have told it.
-   Lock-free publication by reader count and atomic pointer (`VisibleSetHold`, the particle snapshot table, the writer tables).
    Not `std::atomic<std::shared_ptr>`, because MSVC's takes a lock.

## The scene mirror, events and membership

```
engine writers ──events──▶ mirror ──scene pass──▶ tables ──publication──▶ snapshot builder ──▶ payload + revision ──▶ epochs
 (hooks, any thread)        (scene lane)            (scene lane)            (pool)                                 (render thread, GPU)
```

### What is tracked

Everything under attached cells' Static, Dynamic, MultiBound and Actor category nodes, plus:
-   **the portal graphs' parentless roots.** An interior portal graph's `alwaysRenderChildren` (`+0x58`) roots have no parent.
    `RefreshCategoryNodes` files each one under its graph's shared portal node (`alwaysRenderRoots`), and `FindCategoryNode`
    returns that node for anything whose walk up ends at such a root;
-   **`TES::lodLandRoot`**, listed as a category root while either LOD class is on (`Api::ObjectClassContribution::categoryRoot`,
    `RootListing`);
-   **the first-person root** (`player->Get3D(true)` when it is not the third-person root), a parentless category root filed
    under itself ([First person as a view bit](#first-person-as-a-view-bit)).

Markers and water are not tracked. The render thread makes the category capture (`Scene::Channels::CategoryCapture`; the scene
work reads no TES, cell or portal graph), and makes it again only when the category signature moves, a pass saw a detach, or a
load ended. `CategorySignature` folds in each portal graph's list, and the signature is recomputed only on a watched edit
(`SceneTracker::WatchCategoryNodes`, `CategoryEdits`). `Coordinator::RefreshCategoryNodes` diffs it. Tracked entries are indexed
by category node (`categoryMembers`), so a cell going away drops its members without a scan.

### The scene mirror

The scene lane reads no engine memory (`Core/Engine/Capture/SceneCapture.h`, `Core/Scene/Model/SceneMirror.h`). A hook captures
the changed value on the writer's thread; the event applies it, in order, to the mirror.
-   Records hold no references (keys are addresses), only in-world objects are captured, and a detach drops the records.
-   Only what holds still between writer events is a record. Per-frame values (transforms, bounds, `currentFade`, LOD level,
    palettes) are FrameValues' ([FrameValues](#framevalues)).
-   Every classification, record write, join and verdict reads the mirror (`SceneMirror::Leaf`, `SceneCapture::LeafView`).
-   `SceneMirror::Prepare` copies an attach capture's records by kind on the pool; `Apply` moves them into the maps. A capture's
    replays of the updates numbered after it walk an update log (`SceneMirror::Recent`), and a capture numbered after every kept
    update looks nothing up. One walk of the batch indexes it for the steps after the mirror (`BatchIndex`).
-   **Stale records.** An out-of-world attach naming an object the mirror holds means it left by no hooked detach
    (ShadowSceneNode's queued light removal): the record and subtree are evicted (`SceneMirror::Evict`).
-   **Fade nodes without a pin.** Clones point shared properties at the newest clone's fade node, and a property's fade node is
    set after its geometry's attach (`PropertiesSetFadeNode` pushes a leaf update with pins). `AcquireFadeNode` takes the pin,
    else the node's root owner, else asks the render thread (`FadeNodeRequest`), which reads the property as `GetRenderPasses`
    would and posts the reference back. Fade nodes are reached through `AsFadeNode` (vtable), which some
    `netimmerse_cast<BSFadeNode*>` uses miss.
-   **Frame globals** (`Core/Shading/FrameGlobals.h`): the engine globals the scene reads are one immutable capture the render
    thread takes at the frame start (loading screen, interior test, decal bias, the LOD fade settings,
    `SetupTechniqueDescriptor`'s bytes, the shadow global, the fade update's constants, the main camera, the tree wind,
    `cullHidden`, the technique inputs). A pass binds its frame's capture (`FrameInputs`).
-   **Render-thread captures for the pass**, at the frame start (`ServeFrameRequests`, `Core/Scene/FrameView/FrameRequests.cpp`):
    switch catch-ups, the category capture, the portal roots' captures, the mirror's capture requests and the fade-node answers.
    The pins walk and the fade-node answers run in a leased pool job (`FrameStartJob`).

### Events, not polls

Hooks on the engine's writers push events from whatever thread makes the change; the scene lane drains them in each scene pass,
which their pushes wake; the render thread drains none. Nothing about an object is re-read on a schedule. Measured at Riverwood:
about 2% of the tracked set moves a frame, always the same 2%, and property, material, buffer, alpha and hidden state changed 0
times in 1,200 steady frames.

-   **Structural events** (`Core/Engine/Capture/SceneEvents.h`, `SceneTracker`): a subtree attached or detached, pushed lock-free
    from any thread. Node classes that implement child edits themselves (`BSParticleSystemManager`, `BGSDecalNode`,
    `BSFaceGenNiNode`) are detoured, as is `NiAVObject::SetCollisionObject` (`SceneEvents.cpp`, `Features/Decals/DecalHooks.cpp`).
    So is the engine's remove-all (`FUN_140d1d630`, `SceneTracker`'s `RemoveAllChildren`), which lets every child go without the
    `DetachChild` slots: `Actor::DetachArrow` (the nocked arrow at the release; without it the arrow stayed drawn where it was
    loosed), `Actor::RemoveWeapon` and `BSTaskPool`'s queued removal.
-   **Moves are not exits.** The engine moves physics objects between a cell's dynamic and static nodes, each move a detach then an
    attach. A geometry back under a tracked category node by the end of the event batch keeps its entry (`detachMoves`; about
    3,000-10,000 moves per 5 s at Riverwood).
-   **Property events**: `BSShaderProperty::SetFlags`, `SetMaterial`, `NiObjectNET::PrependController` and
    `BSLightingShaderProperty::SetMaterialAlpha` (vtable `0x31`) take the property's dependents again (`propertyDependents`). A
    detour on vtable `0x2A` (`LodFadeRenderPasses`) pushes the property when `specularLODFade` or `envmapLODFade` moved. The
    `Sky::SetColor` detour pushes an external emittance colour that moved (`emittanceEvents`). `SetExternalEmittance`'s re-point of
    an effect property is a property event.
-   **Node events**: Havok node writes and controller additions. A mirror update of a node's body or controllers, or of a
    geometry's dismember or skin, reclassifies its dependents (`ApplyMotionUpdate`). A sun entry node's membership change takes its
    other dependents (`rootDependents`).
-   **Fade resets**: 11 inline stores of `currentFade` outside any hooked function (cell and sky placement among them) are patched
    (`InstallFadeResetStores`), so the mirror's `currentFade` misses none. A placement snap (`FUN_14147aa20`) is an event
    (`fadeSnapEvents`).
-   **Materials**: a write captures the material ([Material writes](#material-writes)).
-   Moves, hidden bits and switch selections, below.

What else follows its event rather than a poll:
-   **Frame values.** The producer samples only the kept set's movers and roots whose keys moved (particles every frame), and
    calls `UpdateSkin` only for those skins.
-   **Materials and residents.** `RefreshMaterialSignatures` evaluates every signature only when the frame's material witness
    (`FrameComponentsWitness`) changes; `KeepResidentsAlive` refreshes the geometry template only for the pipelines whose
    membership changed.
-   **Publication.** Each candidate snapshot carries its change log (`EntrySpaceChanges`; readers call `ChangedEntriesByLog`).
-   **Exclusions.** The exclusion build republishes the same sun exclusion while nothing it was judged from moved, and links each
    one forward to later candidates (`ForwardExclusion`).
-   **Visibility decode.** `PrepareWrites` visits only the slots an event, the expiry list, the change log, the fade-root journal
    or the candidate log names (`VisibilityDecode`).
-   **First-person hidden bits** follow `SubtreeEvents`, looked up in a published member set (`WatchSubtreeMembers`), so the
    hooks never walk parents.
-   **Cull bookkeeping.** The animated fade roots go through an `EventQueue` (`PrimaryCull::AnimatedFadeRoot`). The scene-list
    filter is applied as a delta (`ApplyFilterDelta`); fade and tree ownership take joined and left deltas
    (`TreeAnimation::UpdateOwned`); the cut's planning (`PlanEntry`) and the local lights' filter builds run on the pool.
-   **Commit.** `SyncPipelines` skips an unchanged `pipelinesStamp`.
-   **Stats.** The colour and shadow cull counters are read back asynchronously (`CounterReadback`), with no D3D11 `Map`.

Two things are sampled rather than evented, deliberately: Advanced Skin's wetness is a render-thread capture at the frame start,
because `Skin::GetWetness` advances an actor's fade on its first call each frame and is not thread-safe (an evented version
would cost the game threads more than its ~1 us a frame); and the per-frame values FrameValues samples. Measured when the polls
became events (60 s motion): p99.9 frame 63.4 to 30.0 ms; scene work evaluates 245-303 entries a frame instead of 620-820.

### Movers by event

A static reference never moves in place: a script or console move is `Disable` then `Enable`, a detach and an attach. So what
moves is actor-owned, controller-animated or Havok-simulated: the movable set (`Core/Scene/Coordinator/Walk.cpp`).

-   **Move writers push a key** after the write (`MoveEvents`, `SceneEvents.h`): actors through `FUN_14066afa0` (every animation
    path), `TESObjectREFR::Update3DPosition` and Features/Actors' reserved writers (`Actor::UpdateActor3DPosition`, the sky cell's
    skin job, the ragdoll job); other references through the cell's animated references' and dynamic node's
    `UpdateSelectedDownwardPass` patches and graph-animated references (`FUN_1402f75a0`); node events name the reference above the
    node; particle systems through Features/Particles' snapshot thunks. An entry's key is the one reference on its chain
    (`MoveKeyOf`). `PushMoved` feeds FrameValues' own queues (`FrameValueMoves`) as well.
-   **A per-frame entry is evaluated only when one of its keys moved this frame or the last** (`Tracked::moveKey`,
    `moveKeyOuter`; `Coordinator::MoverDue`). The per-frame set (`PerFrameTraitsOf`): face shapes, actor-owned entries, skins,
    entries under a switch, and movers of a geometry type that moves every frame (particles). Actors and faces take the light path:
    an actor's record is kept while its classification and frame verdict stand (`Coordinator::FrameVerdictKept`); only placement
    and palette move.
-   **Placement plans are deltas** of the mover and root set, emitted only when that set changes (`Placements.cpp`): `movers` and
    `roots` joined or changed, `moversLeft`, `rootsLeft`, `dependentsLeft`, and `reset` for a set made anew. FrameValues applies
    every plan, in order, to its kept set; a coalesced delivery concatenates plans, never keeps the newest alone. So motion alone
    publishes nothing. A root whose reference moved this frame or last is read again (`movedKeys`), and `TakeRoot` writes the
    root's bound into its dependents' sun entries.
-   Not capturing transforms or palettes in the writers' hooks, because a writer cannot know it is its subtree's last of the frame
    (the order varies across job threads): FrameValues samples at its kick.

### Hidden bits by event

Every store that can change `kHidden` (bit 0 of `NiAVObject::flags`, `+0xF4`) is an event (`Features/HiddenNodes/HiddenStores.cpp`).
-   About 464 sites are patched with a call to a stub: every `or` that may set bit 0, every `and` that may clear it, every `mov` to
    `[reg+0xf4]` (two RSP-based sites left out). The stub reads the dword, runs the store, pushes the base register as a key when
    bit 0 changed, and re-runs an idempotent `or`/`and`; a `mov` is never re-run. The stubs live just past the module image (a
    `call rel32` must reach them). A false positive is a key no entry lists, never dereferenced. There is no off switch.
-   Every entry lists its node chain once, at classification, from the leaf up to (not including) its category node
    (`ListHiddenChain`, `hiddenDependents`). A hidden-bit event on a listed node reclassifies the entries under it
    (`DrainHiddenEvents`); an event on a category node takes every entry under it. Measured in Whiterun: actor frame verdicts taken
    again fell from about 1,260 to 21-32 a frame.
-   **An actor entry's hidden event stays owed until a walk evaluates it** (`Tracked::hiddenEventOwed`, set by
    `DrainHiddenEvents`, cleared by `MergeEntry`), and an event on a listed node numbered after a pass's capture is announced again
    by the first pass whose capture is newer (`hiddenAhead`, `FrameGlobals::cullHiddenSequence`).
-   **Render-time hides are not scene changes.** `TESWaterReflections::Update` hides the player's 3D and water objects around the
    cube faces, `Main::Draw` hides the first-person skeleton, and `GetRenderPasses` leaves each camera's alpha on the property.
    These are captured with the frame (`FrameGlobals::cullHidden`, `Api::CullHiddenSource`), and their pushes do not wake a pass
    inside the read window (`TakeScenePassDeferred` counts them).
-   A flip that lands between the hidden drain and the walk's read is taken a frame late, not missed (the behaviour graph's
    visibility controller, `BShkVisibilityController`, toggles NPC `SHIELD` and `Symbol` nodes from behaviour jobs).
-   A hidden actor part, and a fading non-member, blocks its light and sun entry's exclusion (`Eligibility.h`): the behaviour graph
    shows and hides an actor's parts during `Main::Draw`. A hidden static's geometry is left out of its entry
    (`Eligibility::PrimaryVerdict`), and its unhiding is an event that reclassifies it.
-   The same stubs carry the LOD gates' show veto (`Api::ShowVeto`, [LOD gates](#lod-gates-the-scene-side)).
-   Not hidden members keeping their records with a per-object hidden bit: the bit would travel in the same publication as
    membership, so it could not be faster, and a hidden member keeping its claim would leave a shown object undrawn until the
    publication.

### Switch nodes

Leaves under an `NiSwitchNode` (trees' variants, harvestables) are DCLF's while every switch on their path selects their branch
(`Coordinator::SwitchSelects`; `Features/SwitchNodes/SwitchNodes.h`). Core keeps the selections whatever the toggle, because the
stand-in takes whole roots and so follows every switch below them. Selections follow events:
-   **The index stores** (`NiSwitchNode +0x12C`: `BSTreeManager`'s LOD selection, the local map's, harvesting, a harvestable's 3D
    setup) are patched with Xbyak stubs that call `SwitchIndexStore`: the stub stores the index and pushes an event when the value
    changed. `NiSwitchNode`'s own child edits (vtable slots `0x35`, `0x37`-`0x3C`) push a structural event
    (`PushSwitchStructural`). Every site's bytes are checked first; if one differs none is patched and DCLF fails to install
    ("without these hooks DCLF cannot run"). Measured at Riverwood: the per-frame set fell from 1,707 entries to 1,057.
-   **Only the render thread's writes make events** (`switchEventThread`). A loader thread's subtree is not in the scene yet, and
    its attach brings the switches up to date; holding a node a loader is still assembling is not safe.
-   **Catch-up.** `FrameView::CatchUpSwitches` runs `NiSwitchNode::OnVisible`'s catch-up outside the cull: when the selected child
    is out of date it calls `childRevID.SetAt(index, revID)` (`FUN_140d29990`) and the child's `UpdateDownwardPass` (vtable `0x2C`)
    with `{ savedTime +0x130, flags bit 1 }`, for every switch an event names and every switch under an attached root.
    `Coordinator::ApplySwitchEvents` then reclassifies every tracked entry under a changed switch, so a selected child is never out
    of date when the scene work classifies it.
-   The stand-in's per-member bit `Cut::memberLive` (every switch on the member's path selects it) is read in full for a new
    snapshot or a resync, otherwise only for the entries a switch event names (`FrameView::TakeSwitchChanges`).
-   **Read the switch fields at AE's offsets** (`Engine::ReadSwitch`, `Engine::SwitchSelects`): CommonLib's `NiSwitchNode` members,
    and any declared after a runtime-sized base, sit at the wrong offsets in a multi-runtime build (once, every tree selected
    nothing).
-   **Child indices run to `_freeIdx`** (`NiTArray::free_idx()`), null entries included, as the engine's loops do; CommonLib's
    `size()` counts only non-null entries. Every DCLF walk over children follows this: the scene lists, `SceneTracker`'s detach by
    index, the stand-in's switch children, the category lookup and the decal order's child index.

### Material writes

A write captures the material (`MaterialPort::PushCapture`), and the scene lane evaluates its records again from the capture
(`Core/Shading/MaterialWrites.h`, `MaterialSources.h`). The hooks: `BSLightingShaderPropertyFloatController` and
`ColorController::Update`, the engine material vtables' `CopyMembers`, `OnLoadTextureSet`, `ClearTextures` and
`ReceiveValuesFromRootMaterial`, the effect controllers (`Features/Effects/EffectWrites.cpp`), and CS's PBR writers through
`NoteMaterialWritten` / `MaterialWriteScope`. A capture holds a reference and goes through a lock-free queue; the lane keeps the
newest per material. A material the lane has never seen is asked of the render thread (`FrameView::ServeMaterialRequests`, at
EarlyPrepass), whose answer wakes a pass. A capture or controller write confined to the frame-owned bytes (the texture transforms
and a port's declared frame spans, `Extensions::DifferOutsideFrameSpans`) captures nothing and rewrites no record.

**Report, never repair.** A record found stale points to a write the events do not cover; repairing it silently is what hid the
missing events before.

### Eligibility and classification

-   **Verdicts** are `Ineligible` (`Core/Shading/LightingDescriptors.h`, names in `kIneligibleNames`); the pure verdicts are
    `Core/Scene/Model/Eligibility.h`. Parent reasons combine by strength: a billboard (whatever its leaf) outranks a switch
    (decided per frame), which outranks an ordered node (decided by the leaf).
-   **The pass descriptor is derived from the property**, not taken from registration (`DeriveLightingDescriptors`, following
    `GetRenderPasses` and `SetupTechnique` with TruePBR's changes). The runtime bits (`kRuntimePassBits`): ShadowDir (13) and
    DefShadow (14) are set wherever the sun may apply and dropped on the GPU on a cascade miss
    ([the colour pass's sun test](#the-colour-passs-sun-test)); DoAlphaTest (20) is always added in batch lists 1, 3 and 4
    (`DrawnPassDescriptor`), because the engine's bit comes and goes with the early-Z global, blending and the last build's fade
    while the pixels do not change.
-   **The batch list** (`subPass`) is derived too (`SubPassOf`): the engine's classifier (`FUN_1414f4790`) reads only the property
    flags and the alpha property: bit 54 means list 4; otherwise bit 36 counts 2, plus 1 for alpha testing.
-   **The derivation cache** (`Tracked::Derived`): an object's positive derivation (descriptors, static flags, pipeline key,
    geometry, pipeline and material slots), valid while its witnesses hold (TriShape, property and material pointers, fade state,
    interior, alpha below one, technique, sub-pass, hint, decal bias, the slot tables' generation). A slot is checked against its
    key before it is served. `Tracked::StaticVerdict` caches negative verdicts against the same witnesses (rendererData included:
    a stale negative would leave an object native for good). A classification stands until an event takes it again; a full
    evaluation (`BuildFullOrder`) runs only after a reset, a load, a rescan or a live toggle that enters the classification.
-   **Two phases.** The scene phase (`BuildFrame(Phase::Scene)`) writes the records; the accumulate phase
    (`BuildFrame(Phase::Accumulate)`) joins them to their membership passes. The scene phase decides nothing that needs the pass:
    `Eligibility::DeferredToAccumulate` defers `Ineligible::Decal` to the join (rejecting decals early dropped 452 accumulated
    objects).
-   **Membership passes.** The engine never registers a member. DCLF builds its pass from the mirror (`BindByMembership`,
    `PrimaryCull::MembershipPass`, `MembershipLayerPass`, cached per tracked entry), never from the engine's registrations, which
    only parity observers read. A member written again keeps its slot and membership unless what its binding reads changed
    (`MemberBindingStands`).
-   **Shadow-only objects.** A walked object that fails main eligibility only for a reason the shadow views ignore becomes a table
    object with `kObjectShadowOnly`, when the caster rule accepts it (`Eligibility::ShadowOnlyReason`): `Technique` (a technique
    the main pass does not take, such as landscape with its toggle off), `UnsupportedParent` (a blended leaf under a `BSOrderedNode` the forward epoch
    does not take) and `MultiIndex`. Billboards are never shadow-only: `NiBillboardNode` turns to the culling camera, which for a
    shadow view is the light.
-   **The engine's empty fade node is no root** (`Engine::EmptyFadeNode`; `CS_DCLF_EMPTY_FADE_NODE`). `BSShaderProperty`'s
    constructor points `fadeNode` at a static `BSFadeNode` (the pointer at `0x14332a2a0`) that the engine reads as null; terrain,
    object LOD and some references keep it. Such geometry takes its plain entry root instead, and every reader compares the value,
    not the global's address.
-   **Every value in a persistent slot is fixed or refreshed on purpose.** Frame-sourced values never sit in a slot as if constant
    (learned from the technique constants, drifting floats, and a pointer into a map whose storage moved).

### The DCLF set

An object in the DCLF set is drawn by DCLF in every phase it is a member of, and the engine never draws it in a view of that
phase (`Core/Scene/Model/SceneSet.h`, `Core/Scene/Coordinator/SceneSet.cpp`).

-   **Phases** (`SetPhase`): `kSetMain` (the Z-prepass and colour pass), `kSetCaster` (plain and clamped shadow views: sun
    cascades, spot lights), `kSetCasterPoint` (paraboloids), the occlusion maps' two bits (`kSetOccluderSky`,
    `kSetOccluderPrecipitation`, Features/OcclusionMaps'), and the feature views' (`kSetReflection`, `kSetFirstPerson`).
    `kCoreSetPhases = kSetMain | kSetCaster | kSetCasterPoint`; a feature's bit is checked against Core's by `Features/Manifest.cpp`.
-   **Participation** (`Coordinator::SetParticipation`): main unless `kObjectShadowOnly`; casters unless `kObjectNoShadow`; the
    reflection's only for LOD lighting techniques; a first-person slot its view alone, and only if opaque, alpha-tested or forward.
    The phases DCLF can draw are `Coordinator::SetCapability`.
-   **Membership is decided once per scene pass**, after the joins (`Coordinator::CommitSet`). An object is a member when it is
    eligible, takes part in the phase (main: bound; shadow and occlusion: casts or occludes) and is ready (pipeline compiled,
    textures imported, shadow mask and shared lookups resolved, geometry and decal slot; for the shadow and occlusion phases
    `IndirectDraws::PhaseReady`). Every eligible object is a member: fading objects, skins whose LOD row selects no partition
    (`kNoPartitions`) and decals too.
-   **A claim is the whole object**: it joins the set in every phase it takes part in once everything every one of them draws it
    with is ready (`MainReady`, `PhaseReady`), and leaves all of them together.
-   **Readiness is judged by events and readiness sources, never a scan.** A member being rebound leaves first (`LeaveSet`,
    `MarkSetApply`) and keeps its claim until the publication that carries its new record.
-   **`ApplySet`** applies the commits inside `PublishScene` into `Tables::setPhases`, the record's `kObjectMember` and the lacking
    counts; a slot freed or reassigned since takes no phase. Records and `setPhases` change only there.
-   **Structural changes.** `kChangeStructure` is noted when what a revision's shapes are made from changes (the record's or draw's
    pipeline, the geometry half and partitions, the shadow and occlusion techniques). `RevokeUndrawnClaims` takes a slot's claim
    back whole when it changed structurally since the last publication, or when the work stopped drawing a claimed phase; the next
    publication claims it again.
-   **The claims** (`SetSnapshot`, published to the hooks through `PassCapture::PublishSet` and to DCLF's readers as
    `FrameView::FrameClaims`) are installed with the revision from the same publication, so a frame draws exactly the set its
    claims name.
-   **A member DCLF cannot draw this frame is the engine's.** The stand-in hands the registration any bound member not in the
    frame's set (`PrimaryCull::MemberDrawable`), so a pipeline still compiling delays a draw and never makes a hole.
-   **A claim follows what the CPU submitted, not what the GPU rasterised.** An occluded member stays a member, and the engine
    still does not draw it.

The set is checked by `CS_DCLF_SET_PARITY` and the `<- LEAK` line ([dclf-validation.md](./dclf-validation.md)).

## The scene lane and publications

### One scene pass

The scene pass (`Coordinator::RunSceneWork`, `Core/Scene/Coordinator/SceneSet.cpp`) runs on the scene lane, in order: take the
ahead context, drop returned publications, `RecycleRetired`, the coordinator's and the joins' inputs (`TakeCoordinatorInputs`,
`TakeAccumulateInputs`), the category capture, the components' queues, `CollectEvents` and `ApplyEvents`, the constants posts,
`RefreshTechniqueRows`, `RefreshMaterialRecords`, the walk (`BuildFrame(Phase::Scene)`), the joins (`BuildFrame(Phase::Accumulate)`,
`MakeNewPipelineConstants`), the bindings, `ResolveLookups`, `CommitSet`, `PublishScene`. A record bound in a pass is committed in
it. While DCLF does not run, a pass applies events only.

-   **Per-frame work once a frame.** The per-frame entries are walked only by the first pass after a frame input; later passes of
    the same frame take what their events schedule (`DeltaWalk`).
-   **A pass that changed nothing publishes nothing** (`PublicationNeeded`, `PublishKey` in `Core/Scene/Model/Publication.h`).
-   **Parities sample by pass**: the coordinator's parity checks run every 60th pass (`ParityDue(passSerial, k)`), so a burst does
    not repeat them and a frame with no pass does not skip them.
-   Under `Policy::ScenePassInline` (the parities that compare against the render thread's state) the pass runs inline at the frame
    start, and a Present with no frame since the last applies the events itself (`ApplyEventsInline`).

### The scene pump

A `SerializedTaskPump` (`Core/Scene/Coordinator/ScenePump.cpp`) runs a pass on the scene lane whenever a producer wakes it, any
number of times a frame or none. It is level-triggered and lock-free: a wake during a pass queues exactly one more. The frame
neither kicks nor joins it.

**Wakes by source** (`Core/Scene/SceneWake.h`, `WakeScenePass`): `FrameInput` once a frame (the frame start folds its posts into
one wake, `ScenePassWakeBatch`); otherwise only inputs wanted before the frame's own pass: an attach in the world, a detach, a
hidden bit, a leaf's property or alpha swap, property and alpha values, a property event, a switch, a material answer, a texture
reply, a new catalog, load markers and drains, the render thread's captures, an engine channel (the LOD gates). Per-frame queues,
LOD segment writes, value updates, attaches out of the world and material writers' captures never wake. The hidden, leaf, value
and property sources wake only outside the engine-read window, because inside it the engine sets and undoes them for its own
views; the event is still pushed, only the wake waits.

### Prepare on the pool, apply on the lane

The lane does O(changed) bookkeeping: it drains events, applies what the pool prepared, and publishes. Each phase is a pure
prepare over frozen inputs (the mirror, `Tracked` read only, the pass's snapshot: `ShardScope`) that writes only its chunk's
results, then a serial apply, in chunk order, that stores slots, logs and counters. Journals stay single-threaded: the chunks
collect their marks and the apply makes them. Counters go to one `EvalCounters` per chunk, merged in chunk order.

-   **Fan-out** (`CS_DCLF_FANOUT`, default 2: the pool from 128 entries; `FanoutMode`): the walk's rounds (`EvaluateEntry`, then
    `MergeEntry` in index order), the joins (`EvaluateJoin`) and `AddSubtree` (`PrepareSubtree`, `MergeSubtree`). The walk's
    merges stream (chunk k merged as soon as it is done); the joins' do not (a join merge writes what a layer's evaluation reads).
    `Policy::SerialFanout()` runs every one on the lane for the parities.
-   **Attach merge.** `PrepareGeometry` (pool) takes everything a geometry's merge needs (`GeometryPrep`: pins, held property
    keys, the sun entry, the gate token, LOD segments); `AddGeometry` only inserts, appends and attaches its identity.
-   **Walk scheduling.** The delta walk is the only walk. Its dirty roots are scheduled light (`ScheduleRoot(root, false)`): the
    root's motion is read again and its dependents reclassified only if that verdict moved. The dependents lists are indexed
    (`*DependentsAt`, `List`/`Unlist` in `Internal.h`). `FinishDeltaWalk` keeps kept slots' geometry slots current.
-   **Materials.** A material captured several times in a pass is held once (`HeldMaterial::listed`); its evaluation
    (`EvaluateMaterial`, `PrepareMaterials`, `PrepareJoinMaterials`) runs on the pool. A material lookup holds its texture and
    feature owners and its ORG lease in one immutable owner block (`Lookups::MaterialOwners`), whose identity moves exactly when an
    owner does. `ResolvePipelineLookups` is witness-driven. The held snapshots age once a frame.
-   **Shading.** `NameShadingKeys` is prepared by chunks of keys on the pool and applied in key order.
-   **Candidates.** The candidate tables' pooled snapshots (`EntrySpaceTable::pool`) are spares a pool job catches up from the
    published snapshot's log; the lane runs one itself only when it finds it unstarted.
-   **Shadow sets.** `RefreshShadowSets` applies the journaled slots. The index is rebuilt only when forced or when
    `shadowIndexNeedsRebuild` is set (a table reset, a full walk).
-   **Buffer descriptions.** `GpuResources::Prefetch` returns at once: DXVK's batched describe (`dxvkGetInteropResourceInfos`,
    callable from any thread) runs on the pool. The first `Acquire` that needs an answer runs the batch if no worker started it,
    or waits for it. Answers last through one more `BeginFrame`.

**The executor.** `ParallelFor`'s chunks are fixed (`[k * grain, min(count, (k + 1) * grain))`; callers index results by
`a_begin / grain`). A helper running an above-normal caller's chunk runs at the caller's priority and is restored after
(`SceneExecutor::ThreadPriority`), so the engine's normal-priority threads do not preempt work the lane waits on
(`CS_DCLF_ASYNC_PRIORITY=noboost` turns it off). `GetParallelForStatistics` gives, per name, the time callers were blocked on
chunks a helper had claimed. Measured (motion, Tracy): the scene lane busy 1.74 ms a frame mean, the snapshot builder 1.61 ms;
frame time is unchanged by either, since the worst frames are outside the lane.

### Persistent draw state

Every structure built from the tables follows what changed, by log, and sends GPU holders only the runs changed since what they
hold (`Core/Runtime/KeptState.h`). One implementation each:

-   **Slot tables** (`Core/Runtime/SlotTable.h`). The shared tables (geometries, pipelines, materials) keep slots with generations
    and reference counts. Liveness is by reference, never by idle time: `DrainUnreferenced` frees a slot after a complete batch of
    reference changes, in the batch where the last reference went. A freed slot is retired and returns to the free list only once
    no publication names it. References: an object's geometry slot while it has a record, its pipeline and material slots while it
    is bound, a skin partition by the partition before it (`UpdateSlotReferences`, from the logs).
-   **Object slots.** Every tracked eligible geometry keeps one object slot for as long as it has a record (`AcquireObjectSlot`,
    `FreeObjectRecord`: a free slot is `kObjectFree | kObjectNoBindings | kObjectNoShadow` with a null geometry). Every per-object
    table is slot-indexed; loops skip free slots by their flags. The per-object index lives in the tracked entry
    (`Tracked::objectStamp`, `objectId`).
-   **The change log** (`Tables::changeLog`, a `SharedEventLog<Change>` in `Core/Scene/Model/Tables.h`). Each entry is a slot and its
    `ChangeCause` bits: `kChangeBindings`, `kChangeAncestorValue` (Light Limit Fix's room index), `kChangeSkin`, `kChangeExtras`,
    `kChangeShadow`, `kChangeGeometry`, `kChangeMembership`, `kChangeStructure`. There is no placement, palette or shading cause:
    those are FrameValues', never a column. `Tables::NoteChange` notes only non-empty causes; writers in full compare columns
    before and after (`ColumnsOf`, `CausesBetween`, `NoteWrite`). `InvalidateChangeLog` makes every reader resync.
-   **Other logs**: `geometryLog` (geometry slot writes, which `GeometryStore` repacks by), `materialLog` (material version moves),
    `publicationLog` (published columns no change-log consumer reads), `extrasBlockLog`.
-   **`EventLog` / `SharedEventLog`**: append-only logs with absolute positions. `SharedEventLog` keeps its entries in 4,096-entry
    chunks every published snapshot shares, so copying and trimming cost O(chunks).
-   **`LogCursor`**: a reader's position and tables generation. A reader behind the base, or on a new generation, rebuilds from the
    tables.
-   **`ChangeJournal`**: `(contentEpoch, range)` entries after a floor. A holder (a GPU buffer at some content epoch) is sent the
    coalesced runs changed since its epoch, or everything when it is below the floor or new. There can be several holders.
-   **Kept arrays** (`KeptArray`, `KeptView`, `KeptRegion`): elements in copy-on-write chunks of about 16 KB; the first write to a
    chunk a view still holds copies that chunk alone. `KeptWholeArray` keeps one vector, for FrameValues' arrays, which need
    contiguity and are written from many threads after one `Mutable()`.
-   **Stable offsets.** Each skin has its own palette block while its slot keeps a palette of that size (`Tables::PlaceBones`,
    grown in `kBoneGrowRows` steps), and each record its own extras block (`AllocateExtras`), so one skin appearing moves no other
    record.
-   **Version-gated constants.** `Tables::pipelineConstantsVersion`, `pipelineBindingVersion`, `materialFrameVersion`, and
    `Lookups::NextVersion` per lookup entry: a pipeline or pair whose versions are unchanged is not looked at.

The kept stores built on these are the object records (`ObjectRecordStore`, `Core/Render/Payload/KeptStores.h`: a record is
rebuilt only for a log entry naming a cause it is built from, and kept only when its bytes differ), `ExtrasStore`,
`GeometryStore` (repacked from `geometryLog`), `DrawBoundStore` (the scene's draw bound, `SceneDrawBound`), the resident region
([Persistent resident draws](#persistent-resident-draws)), the main rows (`MainRows`) and the shadow epoch's state (`ShadowKept`).
Measured at Riverwood: slots made the walk 0.89-1.00 ms against 1.44-1.55 ms for a dense walk.

### Publication

**The publication** (`ScenePublication`, `Core/Scene/Model/Publication.h`) is immutable. It carries the tables, claims, lacking
counts, lookups and the catalog they were resolved from, the sun and light candidates, the category nodes (copied only when they
change), the toggles, a `RevisionRequest`, the oldest event it is the first publication of (`eventNs`, `eventFrame`), and a
`sequence`. `Scene::Coordinator` owns the working tables and is their only writer.

-   **A revision is its publication's alone** (tables, lookups, catalog); it never reads what the frame installed. Not a revision
    made from a publication's tables with the frame's installed lookups: it lags a publication and starts a self-sustaining loop of
    superseded revisions.
-   **The publication log** (`Core/Runtime/PublicationLog.h`, forward-linked, one producer, one consumer; nodes are
    `PublicationDeltas`): each publication's claims changes (joined, left) and the frame inputs its passes made: placement plans,
    shading names, seed requests, switches applied, retired imports, class membership changes, light-entry changes, component
    deltas (gate flips). The frame start walks the frame inputs up to the newest publication taken and the claims changes up to the
    installed one, so a publication the builder skipped loses none of them. Light entries travel as changes, never copies.
-   **Lookups** (`Lookups`, `SharedChunks`: 64 entries a chunk, copy-on-write by a stamp): a publication shares every chunk not
    written since the last; unchanged lookups (`Lookups::ChangeKey`) republish the last copy. What a build needs from the render
    thread's services is resolved ahead (the pipeline set indices and shader constant tables; the texture and sampler descriptor
    indices), so a build never calls the engine, D3D11 or ORG.
-   **Pacing.** A pass with something to publish publishes only when it runs inline, took new frame inputs (the frame's backbone
    pass), finds the snapshot builder has taken the previous publication (`IndirectDraws::SnapshotWorkTaken()`), or nothing was
    published yet. Otherwise it defers: `CommitSet` runs every pass, and `ApplySet`, the claims, the deltas, `PublishTables` and
    `PublishLookups` run in the next publishing pass. The claims snapshot (`publicationClaims`) is pooled and replayed from the
    phases log (`setPhaseLog`).
-   **`PublishTables`** brings the pooled snapshot nearest the tables up to date by replaying the change, geometry, material and
    extras-block logs by slot, and copies the rest whole; the pipeline family only when `pipelinesStamp` moves. The replay is
    collected once on the lane and copied as `ParallelFor` jobs over disjoint columns and record ranges, by contiguous runs. A
    column a change cause covers (lights, tree animation, extras, skin, shadow) is copied for a slot only when a note of the slot
    carries that cause, so every writer of such a column notes its cause. The geometry columns stay on the lane (an import lease
    the snapshot drops is let go there).
-   Not copy-on-write chunked table columns for O(1) publication, because they break contiguous uploads and every reader for no
    gain: publication replays logs into a pooled snapshot.

### The retirement chain

Nothing is logically freed while a publication that names it lives (`Core/Runtime/Retirement.h`, `RetirementChain`). Each
publication holds the chain node opened with it. A node's batch (object, tree, fade-root, extras, bone and face-stream slots, the
slot tables' slots, lookup retirements, geometry buffer owners, material references, engine references) returns to the
coordinator's free lists (`RecycleRetired`, first in each pass) once no publication up to it lives. GPU memory has its own,
exact clock ([Pooled versions and the retirement clock](#pooled-versions-and-the-retirement-clock)).

## Payloads, FrameValues and pooled versions

### The snapshot builder

The snapshot builder (`SnapshotBuilder`, a `SerializedTaskPump` on the pool; `Core/Render/Snapshot/Snapshot.cpp`) takes the newest
publication (`IndirectDraws::PostSnapshotWork`), makes the revision first, builds the draws ahead with every versioned address from
the sealed set (`AddressesOf`), publishes the payload as pooled versions, waits for its recordings without a thread, and posts a
`SceneSnapshot` (publication, draws, revision lease, generations). Every epoch's frame owners hold the adopted snapshot, released
on ORG's host thread after the slot completes, through the cleanup queue.

A build is a pure function from the tables and lookups to a payload: the rows it wrote, the draw inputs and the kept stores'
changes. `BuildMainPayloads` (`Core/Render/Payload/MainBuild.cpp`, both main segments in one build) and `BuildShadowPayload` are
the two builders, the main build first. `MainBuildStages` splits the main build so the stream views are made beside it
(`RunAhead`); the shadow build's exclusions and `CountModeDraws` run on the pool.

### Persistent resident draws

Each main segment's `ResidentRegion` (`Core/Render/Payload/MainBuild.h`, one per segment in its `BuildCache`, built by
`MainBuild::BuildResidentRegion`) keeps the whole scene's draw inputs across builds: every object the loop would draw as one input
with its pair's record, the depth segment's cull-only candidates, decals (the colour segment's, with their ordinal; a drawable
decal is `kRegionDecal`, its sequence in its group's range) and face shapes with their position streams.
-   It changes only through the change log (its own `LogCursor`), a pipeline's set index changing, or a pair's record failing or
    recovering.
-   The per-frame loop (`loopList`) holds only what the region cannot: a decal with no ordinal or with several partitions, and a
    shape whose stream is missing.
-   The payloads share the inputs copy-on-write, and `PayloadBuilder` catches a backing up from its content epoch.
-   The shadow build writes its words into the same region's entries (`sharedRegion`, `sideOf`).
-   It is off only under build parity (`Policy::ReuseResidentRegion`), whose whole builds are compared against BuildDraws' output.

**Pairs by witness and by event** (`MainBuild::UpdateRegionPairs`, `ResidentRegion::Pair`). Each (material, pipeline) pair keeps a
witness of what its resolution read: the pipeline's half (`PipelineWitness`), the material's versions (`Lookups::materialVersions`),
and the region's frame-slot masks and segment. Only the pairs of the material slots `Tables::materialLog` or `Lookups::materialLog`
names are checked, plus new pairs and those of a pipeline whose witness moved (`pipelinePairs`); every pair is checked when the
frame witness moves or a log cannot be read on. Each pair knows its objects, so a moved verdict rewrites only their entries.
`Pairs.Resolve` runs on the pool (`KeptArray::PrepareWrites`, `WriteParts` in the chunks, `MarkParts` in order). The binding
owners' bundle is kept by count (`ownerCounts`, `frameRegisterCounts`) and made again only when an owner joins or leaves; a
pipeline's constant-table hash is kept per lookup entry version (`tablesHashes`). Whether a pair can draw - a texture, sampler or
constant block the pipeline reads that the row lacks - is checked per build from what each row holds (`MainBuild::AssembleRecord`).

### The payload

The scene payload of a snapshot is one immutable `PayloadVersions` (`Core/Render/Payload/PayloadVersions.{h,cpp}`): seven parts
(`PayloadPart`), each a backing leased from its own pool, with its SRV index, device address (the row parts) and units: objects,
extras, geometries, material rows, pipeline rows, inputs (the scene list) and shadow rows. No recording binds it, and the draws
embed none of it: the epochs take its parts by value.

1.  **Publication.** `PayloadBuilder` (one ORGModuleServices `JournaledBufferSet`; the snapshot builder is its one acquirer)
    publishes a payload once the snapshot's draws are built. Each part leases the free backing nearest its journal (the highest
    `contentEpoch` not past it) and is sent the journal's runs since that backing's `contentEpoch`. A row is patched with the
    address of the backing it goes to (`PatchRowAddresses`), and the inputs' frame tails are written whole. A material row is
    journalled in four 256-byte parts (`kMaterialRowParts`: the VS blocks, the PS blocks' two halves, the header), so an animated
    material moves only the part holding its values. `ready` is built from the upload tickets; nothing waits on the GPU.
2.  **The gate.** Only a snapshot the frame will adopt is published. The frame must have taken the last posted snapshot, and that
    frame's scene pass must have ended (`IndirectDraws::NoteScenePassDone`), since a publication it makes replaces the one waiting.
    A sealed but unpublished build is replaced by newer work and sends nothing. A kept publication (the same draws made into a new
    snapshot) keeps its payload and uploads nothing. The snapshot is posted only once the payload's uploads completed; their
    completion wakes the builder.
3.  **Adoption.** The payload is installed with the draws (`InstalledPayload()`), and a covered frame always has a ready one. The
    epochs take its parts by value: the BuildDraws latch's payload block (`inputsIndex`, `geometriesIndex`, the rows' addresses),
    the shadow and occlusion latches, and the frame record's object and extras registers. `BindAdopted` binds its leases with the
    adopted set's.
4.  **Trimming.** A part's journals keep their changes back to the oldest `contentEpoch` a backing of its pool holds
    (`PayloadBuilder::OldestContentEpoch`); with none holding anything, everything may go, and the next backing is written whole.

Payload patches only append past the sealed mark; a rewrite leases a fresh backing, and there are no byte-range leases
(`CS_DCLF_PAYLOAD_APPEND`, off, appends into the last backing when every change lies past the mark). Measured, the pooled payload
against the ring it replaced: 315 against 566 KB a frame uploaded.

### FrameValues

FrameValues(N) are the values a frame's draws need to be its own (`Core/Gpu/FrameValues.h`): the placements, palettes and shading,
made on the pool while the frame runs, uploaded on the dedicated upload queue into pooled backings, and waited for by the GPU. The
scene tables hold none of them.

-   **Pools.** Three built-in row sets (`kBuiltInPools`: placements, `BindlessPlacement`, 160 B; palettes, `float4` rows; shading,
    `BindlessShading`, 96 B), then one pool per feature registration (`Api::FrameValuePool`: texture transforms, tree seeds,
    Features/Fades' seeds last). `FrameValues::Kick` leases the frame's backings on the render thread; the producer catches each up
    from its journal on the pool, without a GPU wait; `FrameValues::EndFrame` binds them.
-   **The producer** applies the publication log's placement plans in order onto its kept set, samples movers and roots whose keys
    moved this frame or the last, a skin's palette after the engine's palette update (`FUN_140e4ff90`, per skin), and the slots
    named for shading; it settles a written slot by sampling again until its transforms agree.
-   **The GPU waits, the render thread never does.** The producer signals the host's frame-wait timeline to the frame's sequence
    number after its uploads, always, also after a failure; every batch of the frame waits for it
    (`PersistentGraphHost::SetFrameWaitValue`). A late producer costs GPU idle, never render-thread time.
-   **What the render thread samples itself** at `Kick`: the fade nodes' seeds (before the frame's culls update the nodes,
    [Fades on the GPU](#fades-on-the-gpu)), the tree seeds (the near-list test reads a cull result, `TreeStaticOfNode`), the
    claimed first-person rows and palettes (before `Main::Draw` moves the skeleton into its root's space,
    `FrameValues::SetRootViewClaims`), and the shading fields only it may read (Advanced Skin's wetness).
-   **The ordered-node interleave key** (`BindlessPlacement::forwardSort`) is sampled here too ([Ordered nodes](#ordered-nodes)).

### Texture transforms and frame variables

A material's `TexcoordOffset` (offset and scale) is not its record's: the record keeps the capture-time value
(`Features/TextureTransforms/`, the first feature on the extension points).
-   The feature's payload product (`Api::PayloadProduct`) lists, at the main build's `Finish`, every packed material row that
    carries one (`TextureTransformList`: the material, the row's byte offset, the component mask). The payload carries the list,
    and adoption installs it with the feature.
-   Each frame the feature's frame-value pool reads every entry's current buffer (`textureTransformBuffer`, the one the frame's
    shaders read) under a lease, sending only the rows that moved, and its pass `texture-transforms.apply`
    (`cs.dclf.z.texture-transforms`, `TextureTransformsCS`), first in the Z-prepass epoch before anything of the frame draws,
    stores them into the installed payload's material rows. The pool's descriptor reaches the shader through the depth segment's
    `BuildDrawsLatch::stageWord` (the pass's `latchWord`). Every such row is written every frame, so a pooled backing holds the
    frame's value whatever its journal sent it.
-   **Frame variables.** The same list, pool and pass carry a port's declared frame variables (`MaterialPortContribution::frameVariables`,
    `Api::MaterialFrameVariable`: the Effect port's `FalloffData` and `BaseColor`, which the effect controllers move every frame).
    The main build records where a row placed each (`MainRows::MaterialState::frameVariables`, `PlaceFrameVariables`); the list
    holds an entry per placed variable with its port's read for the row's pass (a source float, optionally times a scale float),
    and the pool reads it from the live material.

### Pooled versions and the retirement clock

The payload's parts, the frame values' row sets and the scene set's growth all lease their backings from ORGModuleServices
`BackingPool`s, tied to DCLF by `Core/Gpu/PooledBackings.{h,cpp}`. All of DCLF's growable buffers are pooled versions; the
snapshot is the only source of them (no rings, no duplicate copies).

-   **The clock.** DCLF has one `RetirementClock` (`Core/Runtime/FrameRetirement.h`), advanced from the graph host's
    completed-frame callback: the exact GPU completion of each host frame, not a frames-in-flight window. A frame binds what it
    read after its last submission (`SubmittedRetirementPoint()`); `BackingLease::Bind` keeps the latest point.
-   **Reuse.** A backing is reused once the clock has reached its bound point and no lease holds it. The acquirer prefers the free
    backing whose contents are nearest what it needs, so a catch-up is about one publication's delta. No pool has a cap. Each keeps
    one warm spare per game frame in flight, less one (`PooledBackings::PerFrameSpares`). The trim floor counts free spares: older
    spares rotate in, and narrowing it to the leased and newest free would make every catch-up a whole re-send.
-   **Release.** A released backing is destroyed on the graph host's resource cleanup lane (`PooledBackings::Retire`), never on
    the thread that let it go (the render thread is the frame values' pools' acquirer). GPU resource destruction never runs on the
    render thread.
-   **The readback staging is not pooled**: it is GPU-to-host and rewritten whole every frame, one host buffer per frame slot for
    each list capacity a sealed set names (`HostEventRing::AddSet`).

### The scene's set: requirement, batch, sealed set

No GPU table DCLF keeps has a compile-time capacity: every one grows, and nothing past a capacity is dropped or falls back. Every
versioned buffer DCLF makes is a buffer of one set (`SceneBufferSet`, `Core/Render/Resources/SceneBufferSet.{h,cpp}`, over
ORGModuleServices' `GrowableBufferSet`), and so is every latch block. No recording binds a version its snapshot's set does not
name. The scene payload is not in the set: it travels as pooled versions of its own.

1.  **Requirement.** Each revision make (`MakeRevision`, on the snapshot builder) starts a build and requires what the
    publication's tables, the frame's posted counts and the builds ahead need (`Core/Render/Revision/Requirements.cpp`):
    `RequireSceneTables`, `RequireMain` (which updates the one `MainLayout`), `RequireIndexPool`, `RequireShadowSequences`,
    `ReserveShadowLatch`, each view's `RequireView`, the latched copies' blocks, and last `RequireLatches`. Buffers indexed alike
    are one `UnitGroup`: one requirement fans out to every member.
2.  **One batch, as state-graph work.** `Seal` names the newest ready versions when they meet every requirement. When one does
    not, it posts one growth batch for every unmet buffer on DCLF's state graph (`SceneGraph::PostGrowth`, the `BufferVersion`
    artifact), and the make waits; the batch's completion wakes the builder, which makes the revision again. Each new version is
    made into a backing leased from the set's pool (`cs.dclf.scene-buffers`), and owns its lease.
3.  **Sealed set.** A sealed set (`NamedVersions`) is immutable: every buffer's version, units, bytes, device address and SRV
    index, and every latch block. While nothing changed, `Seal` returns the very same set (its `Serial`), so every recording made
    against it stands; the shapes key (`RevisionShapesKey`) mixes that serial and the layouts.
4.  **Adoption.** The frame adopts the snapshot it selects (`AdoptSnapshot`: `NamedVersions::AdoptAll` and the revision). Commits
    and latch writes read the adopted set (`Impl::Adopted()`). After the frame's last submission, `BindAdopted` binds the adopted
    set's backings (`NamedVersions::Bind(SubmittedRetirementPoint())`). Nothing flows from the frame back to the producer.

Naming: `sealed` is a set the producer sealed, `adopted` the one the frame installed. A set's `serial` is its identity, a buffer's
`version` an `org::BufferVersion`, and a `contentEpoch` a journal position.

-   **Contents.** A new version holds what its registration says (`GrowableContents`): Fresh (nothing; its writers write it whole),
    Zero (zero-filled through the uploader before it turns ready: the fade states, the reported words, the tree wind) or Capture
    (laid out from immutable state; no DCLF buffer uses it now).
-   **Writers fill what they did not fill.** Every writer of a growable buffer keeps a `FilledVersion` or `FilledContents` for it
    and, on a mismatch with the version the set names, writes whole. No adoption callback resets anything. The index pool lays its
    indices out again, and sends its first indices whole, into any version the update did not fill (`IndexPool::indicesFilled`,
    `firstsFilled`).
-   **Capacities come from the named units**, never an owner's field: the shadow slots' sequences and bucket counts, the view
    blocks, the views' sequences and the index pool. Every view's plan is a requirement on the very entry its recording resolves
    (`RequireView`, `PlanViewBuckets` from the one `MainLayout`), and `Seal` refuses a set that does not hold its requirements, so
    no shape indexes past its buffer.
-   **No fixed draw limit.** Every sequence buffer is required to hold every draw the scene's tracked objects can produce
    (`SceneDrawBound`). A bound past the device's `maxIndirectSequenceCount` or the sort's 2^20 ranks is a hard failure
    (`CheckSequenceLimits`), never a silent drop.
-   **The index pool's room** is twice the geometry indices. A pool the frame found fragmented publishes that version's serial
    (`IndexPool::fragmentedAt`), and the next make requires a version newer than it (`RequireNewer`).
-   **Latch blocks** are the set's (`SceneBufferSet::RegisterLatch`, once): the main latch, the main segments' latched-copies blocks,
    the shadow latch, zeros and latched-copies block, and each view's latch and zeros. The producer keeps only their layouts
    (`RevisionLatchLayouts`); the reserves grow them (`ReserveMainLatch`, `ReserveShadowLatch`, `RequireView`), and
    `RequireLatches` requires each block at its layout. Another layout is a new block, so a frame in flight never reads one laid
    out anew.
-   `CS_DCLF_TABLE_START=small` starts the tables, slots and latch layouts small, to exercise every growth.

## Epochs and the graph

### Revisions and recordings

Each epoch is submitted from the selected scene revision's recording (ORG's revision-driven epochs). The commit writes the frame's
values into the revision's shape - its latch layout, bucket plan and map, latched copies - and hands the epoch the installed
payload's parts by value. A commit builds nothing, prepares nothing and compares nothing.

-   **Revision inputs** (`RevisionInputs`, one immutable post, latest wins): the main, shadow, reflection and scene resources, the
    graph's build and heaps, the targets and shadow-format generations, raster state count, shadow candidates, occlusion layouts
    (`PredictedOcclusion`), the main epochs' viewport and block sizes. `PostRevisionInputs` posts it when anything moved;
    `RevisionShapesKey` keys on it. The revision code (`RevisionProducer`, `Core/Render/Revision/Producer.cpp`) reads only its
    request and this post.
-   **Shapes from revision inputs** (`Core/Render/Main/Resources.h`): a segment's shape (`PassFrame`) is a function of what the
    revision names (versions, draw bound, pipeline set, lookups) and engine setup (viewport, frame block sizes), never of a frame's
    values. Max counts come from the scene's draw bound, never a frame's draws; shadow capacities from `ShadowBoundsOf`. Shadow and
    occlusion shapes are made per predicted view layout (`ShadowViewLayout`: slot, mode, target, slice, rectangle, raster state),
    which `DecideShadowCoverage` predicts before the engine draws.
-   **The revision assembler** (`AssembleRevision`, ORG's `RevisionAssembler`): one fragment per slot (the versions, a shape per
    epoch and shadow or occlusion variant, a recording per epoch). An unchanged fragment is inherited, so an unchanged revision is
    cheap. A failed or dropped recording (dropped by a graph build) makes the revision again.
-   **Recording reuse** (ORG, `CS_ORG_REUSE_RECORDINGS`): an epoch's recordings are kept per frame slot and submitted again while
    their shape holds; no code in a pass's `Record` runs per frame. Measured: host ticket preparation median 186 to 43 us. Kept
    recordings carry no Tracy GPU zones or ORG pass timestamps, so DCLF's GPU time is measured by frame time and Tracy.
-   **The ticket rule.** A pass's prepared invocation holds only what its recording depends on; everything a commit knows goes into
    a buffer the commit uploads (`TreeWindFrameRow`, `FadeFrame`). The passes dispatch every slot the buffers hold and the shaders
    stop at the row's count. A pass's revision is its buffers' named-version serials plus a shape, never a per-frame counter. A
    breach cost about 2.6 ms a frame of re-prepared tickets (`CS_ORG_EPOCH_STATS=1` reports "prepared again after the feature's
    inputs changed").
-   **Epochs in one compile.** The persistent program compiles once over the whole frame, epochs in frame order, with an executable
    each (`CS_ORG_EPOCHS`). Authored order is reassigned by epoch rank, then registration order (registration order alone closed a
    dependency cycle across epochs).

### One epoch for every shadow view

Anything per shadow view is a dispatch or a pass inside the one shadow epoch, never an epoch of its own: one epoch per view cost
4.4-6.0 ms a frame (3.5-5.1 ms of it ORG's compile, prepare and record), against 1.65 ms for the same four views in one epoch. A
shadow view slot the shape holds with no view this frame gets a zero latch (no dispatch, no draw), counted as `slots kept with no
work` (`ShadowEpochs.cpp`). A shadow view's pipeline-map offset carries its latch slot's base (`LatchBlock::Offset(slot) + row`),
as the dispatch's `LatchOffset` does.

### Queues and submission

-   **Async compute.** DXVK creates a compute-only queue and never submits to it; the interop exports it
    (`DxvkOrgInteropDeviceInfo::computeQueue`), and the host adopts it as BasicRHI's compute queue (`src/RenderGraph/RenderGraphRuntime.cpp`).
    Before an epoch's first compute submission, DXVK's stream signals a host timeline that submission waits for
    (`EnqueueStreamPoint`); at the epoch's end the stream waits for the compute queue (`CloseComputeEpoch`). `CS_ORG_COMPUTE_QUEUE=0`
    aliases it to graphics.
-   ORG lets a pass leave the graphics queue only when every resource it touches is `QueueSharing::Concurrent`; every graph buffer
    is. Uses of a buffer are ordered after its last write, and a write after every use since (`OrderUse`). A texture's uses stay
    serialized.
-   **Two passes run on the compute queue, a frame ahead**: fade state (`fades.state`, `cs.dclf.z.fade-state`) and tree wind
    (`tree-wind.next-frame`, `cs.dclf.z.tree-wind`), both in the Z-prepass epoch. Each writes one of two buffers by frame parity
    and the frame's draws read the other, the frame before's. Not in-epoch compute with its consumer right behind it, because
    that makes two cross-queue hops: fade state inside the Z-prepass chain raised the depth segment from 0.986 ms to 1.110 ms.
-   **Every queue of DXVK's device is used under DXVK's submission lock** (`RenderGraphRuntime`'s `LockQueue`), the compute submitter
    included. The stream's wait at an epoch's end is on `computeExit`, a semaphore every compute submission signals that lives as
    long as the device (a graph rebuild destroys the graph's own timelines). Why: a compute submission of only semaphore operations
    overlapping DXVK's submission thread faulted the GPU at VA 0; under the lock, 0 losses in 40 startups.
-   **`CS_ORG_EARLY_FLUSH`** (on): `ExecuteEpoch` flushes DXVK's immediate context before the epoch's first submission, so the
    D3D11 work issued just before an epoch does not wait on the CPU for ORG's preparation. DCLF-attributed GPU idle fell from
    1.23-2.17 to 0.56-1.08 ms a frame.
-   ORG's resource tracking world (flecs) has one owner at a time (`ECSManager`): every change is posted to a wait-free queue and
    applied by whoever drains it.

### Explicit DGC preprocessing and pipeline set versions

DCLF's colour draws are device-generated commands (`vkCmdExecuteGeneratedCommandsEXT`); the Z-prepass, the shadow views, the views'
depth and the draw families use plain indirect draws.
-   **Explicit preprocessing.** The main colour signature (pipeline index, push constants, two vertex buffers, index buffer,
    DrawIndexed; `DrawPipelines::Impl::CreateSignature`, `Core/Gpu/Pipelines/MainPipelines.cpp`) is created with
    `explicitPreprocess`. Each recording site keeps a never-submitted state list per frame slot (`PreprocessStates`,
    `Core/Render/Main/FrameLayout.h`) holding the same BeginPass, heaps, layout, topology and push data as the execution, because
    `vkCmdPreprocessGeneratedCommandsEXT` is recorded outside a render pass but snapshots its state. Sites: the main segment, the
    forward epoch, each view's forward part. Why: implicit preprocessing cost NVIDIA about 170 us of idle GPU per call (colour pass
    1.54 against 2.10 ms).
-   **Pipeline set versions** (`SetVersion`, `ShadowSetVersion`, `Core/Gpu/Pipelines/DrawPipelinesImpl.h`): a frame shape holds the
    version its handles belong to; the pipeline lane writes new pipelines only into a version nothing references, at indices that
    version never held, then publishes an immutable `PipelineCatalog`. The frame start takes the newest and binds its set version
    for the whole frame. No recording reads or sizes preprocess memory against a set being written; when every version is held,
    admissions wait a frame.
-   **Profiling.** Every pass is a `VK_EXT_debug_utils` region named after it (`cs.dclf.*`). Nsight bills DGC draws' time to the
    barrier before them; use frame-level numbers.
-   **The draws are not slower than the native batches they replace** (measured, Save5): colour 1132 against about 1206 us,
    Z-prepass 364 against 387 us, shadow maps 2.02 against 2.08 ms. The remaining GPU gap is compute and fixed costs.

## GPU culling

### The scene list and view masks

Every view reads one list per publication: the installed payload's inputs part, laid out as `[entries | main frame inputs | shadow
frame inputs]` (`PayloadVersions::regionCount`, `mainInputsAt`, `shadowInputsAt`). A `DrawInput` is 64 bytes (`GpuLayouts.h`).

-   **One entry per object** (`ResidentRegion`). The main build writes its main part: the pipeline and rows words, the flags
    (`kInputDrawable`), the decal ordinal and the main view bits (`SetMainPart`). The shadow build writes its words into the same
    entry: the caster key, each occlusion map's key, the shadow material row and the shadow view bits (`SetShadowPart`). An entry
    stays while either part holds it (`sideOf`). The frame inputs after the entries are what each build keeps out of them: decals
    without a slot, face shapes and other second-stream positions.
-   **The view mask** (`DrawInput::ViewWords::mask`, the `kView*` bits in `Core/Render/Resources/GpuLayouts.h`):

    | Bit | Name | Meaning |
    | --- | --- | --- |
    | 0 | `kViewMainCull` | a main-camera visibility candidate (the depth segment's inputs) |
    | 1 | `kViewMain` | drawn by the main camera (the set's main phase) |
    | 2, 3, 4 | `kViewSunCaster`, `kViewSpotCaster`, `kViewPointCaster` | the sun's cascades, the other clamped views, the paraboloids |
    | 5, 6 | `kViewSkyOccluder`, `kViewPrecipOccluder` | the occlusion maps (Features/OcclusionMaps) |
    | 7 | `kViewReflection` | the water reflection's faces (Features/Reflection) |
    | 8, 9, 10 | `kViewDecal`, `kViewForward`, `kViewRefraction` | the colour segment's decal slots, forward range, refraction range |
    | 11 | `kViewFirstPerson` | the first-person view (Features/FirstPerson) |

    A shadow entry carries the views of the modes that hold it (`ModeViewBits`). A feature's bit is checked against Core's
    (`Extensions::ResolveViewBits`, `Features/Manifest.cpp`).
-   **A view is its bits.** Each view's latch names its view bits (`BuildDrawsLatch::viewBits`), and `BuildDrawsCS` drops an
    input whose mask has none of them before reading anything else. The Z-prepass reads `kDepthViewBits` (`kViewMainCull`), the
    colour pass `kColourViewBits` (`kViewMain | kViewDecal | kViewForward | kViewRefraction`), a shadow view its mode's bit (the
    sun's and the spot lights' cascades apart). A shadow view also names the key word it reads (`KeyWordOf`): the caster key or
    its occlusion map's key, with the shadow row for its material.
-   **Main segments.** One build makes both segments' payloads. A pair is drawable only when both segments' shader variants are
    given what they read, so a member is drawn by both or by neither (`<- SEGMENT SPLIT` otherwise).
-   **Every view scans the whole list.** Measured against per-view lists: no frame-rate cost (median 102.4 against 103.9-106.3 fps).

### BuildDraws

`BuildDrawsCS.hlsl` culls the inputs and writes the indirect draw sequences (`Culled`, `Occluded`, `MainFrustumRejected`). The
candidates are the whole tracked active set; the engine's verdict is no input. `CS_DCLF_CULL=off|frustum|occlusion` (default
occlusion) seeds the live toggle.
-   **Frustum.** The bound's world AABB is projected corner by corner and rejected only when all eight corners are outside one clip
    plane. The tests are on homogeneous coordinates before any divide, so corners behind the eye count, and an object wholly behind
    the camera fails the near plane. A view's world-space cull planes (`CullPlaneMask`) are tested first; `MainFrustumRejected`
    then applies the portal program the root is culled through ([Portals](#portals)).
-   **DCLF projects with the engine's jittered ViewProj, bit for bit** (identical to the native main pass's b12).
-   **Final verdicts in phase 1.** Past its fade-out distance and not in view last frame, an object's draw is dropped
    (`FadeDistanceOf`, `kObjectFadeTest`; frustum-stamp bit 31, `kFrustumFadeHidden`, keeps it dropped while it stays past the
    distance); a tree above the frame's height limit draws nothing (`kObjectHeightTest`, as `BSTreeNode::OnVisible`); a member
    whose owned fade root's latest serviced `OnVisible` stopped is dropped (`kFadeVerdictServiced` without `kFadeVerdictDrawn`).
-   **The visibility stamp.** Depth phase 1 writes the frame's stamp for every candidate inside the frustum into
    `cs.dclf.frustum` (the frustum alone, never the HZB). Decal, forward and refraction inputs, which the colour segment culls,
    are stamped too (`kStampOnlyViews`: the stamp only, no draw, no visibility word, no counter). The buffer is never cleared: a
    stale stamp is not the frame's. The stamps feed [the visibility events](#kaccumulated-from-visibility-events).
-   **The engine is an oracle for the frustum test only, not for occlusion.** Its occlusion (planes, boxes, rooms and portals)
    keeps hidden geometry, so "engine kept, HZB rejected" is the expected win.

### Two-phase occlusion and the HZB

Occlusion is two-phase, in the depth segment's tail (`Core/Render/Passes/CorePasses.cpp`):
1.  `core.z.build-draws` (phase 1) tests against last frame's HZB;
2.  the depth draw (`main-view.z.depth`);
3.  the HZB is rebuilt (`gpu-occlusion.hzb`);
4.  `core.build-draws-phase2` and the features' phase-2 passes (`tree-lod.cull-phase2`) retest phase 1's rejects against the new
    HZB;
5.  `main-view.depth-phase2` draws the rescues.

Phase 2 runs only while the HZB exists (`PassContext::PhaseTwo`). The colour segment draws exactly the set the Z-prepass decided
(the per-object visibility words), depth-tested EQUAL.

**The HZB** (Features/GpuOcclusion: `HzbPass.cpp`, `Features/GpuOcclusion/HzbCS.hlsl`; the test is Core's `HzbTest.hlsli`, shared by
BuildDrawsCS and TreeLodCullCS):
-   Raw NDC depth, max reduction (the farthest surface). Mip 0 is half the next power of two of the depth. Texels past the rendered
    area read far (1.0); outside the domain or a level read 0 (the reduction's identity). UVs are scaled for the padding
    (`HzbView::UvScale`).
-   Built in one dispatch by AMD's FidelityFX SPD (`ffx_spd.h`, vendored unchanged): one group per 64x64 tile writes mips 0-5, and
    the last group (counter `cs.dclf.hzb-counter`) carries on to the end; a domain wider than 4096 texels splits into dispatches of
    six levels. 44 us against 97-98 us for per-mip dispatches.
-   The test projects the bound's cube, picks a mip whose four taps cover it, and compares with `kDepthMargin = 1e-4`, because the
    HZB holds stored depth under the viewport range while the bound is in raw clip space. `HzbScreenExtent` keeps any bound with a
    corner at w <= 1e-4.
-   The HZB leaves the first-person model and the rooms' stencil draws out (fewer occluders, never more).
-   Without the feature, or when its program fails, culling is frustum-only.

### Draw outputs

-   **The colour segment** writes DGC sequences (`DrawSequence`, 92 bytes, `Core/Scene/Model/Records.h`: the pipeline index, the
    five-word Constant argument, vertex buffer slots 0 and 1 - slot 1 a face shape's positions - the index buffer and
    DrawIndexed). The main buffer's ranges (phase 1 and colour, phase 2, one per decal group) travel in `BuildDrawsConstants`
    (`phaseTwoBase`, `decalBase`, `decalStride`). Each indirect draw's max count is its epoch's own draws, grown as a power of two
    so the recording settles.
-   **The sort by pipeline** is a counting sort (`DrawSort`, `SortSequencesCS.hlsl`, `src/RenderGraph/PrefixSum.cpp`): BuildDraws
    stages each sequence and counts it per pipeline; the prefix sum turns the counts into each pipeline's first slot; the scatter
    writes each sequence at its pipeline's first slot plus its rank (the rank word: pipeline in the top 12 bits, rank in the low 20;
    `kSortRankLimit = 1 << 20`). It covers phase 1 and the colour segment, not phase 2's rescues. Not for the shadow views:
    sorting them gained no measurable draw time and the scan costs more than it saves.
-   **The Z-prepass** draws with plain `DrawIndexedIndirectCount` per bucket; the buckets are the depth pipelines' groups
    (`ZPipelineKey`: the pulled stages' SPIR-V and the depth variant's cull, depth write, compare and bias; the pixel stage is left
    out when it cannot defer the depth test, `FragmentDefersDepth`). The slots-to-buckets map is in the latch
    (`BuildDrawsLatch::bucketMapOffset`); a slot without a published pipeline maps to no bucket. Decals and layers get no
    Z-prepass capacity, and the Z-prepass has no sort. Plain draws took the depth segment from 0.953 to 0.906 ms. Not the Z-prepass
    split into opaque and alpha-tested calls: it only added a call (0.46-0.47 against 0.35 ms).
-   **The index pool** (`Draws::IndexPool`, `Core/Render/Resources/IndexPool.h`, `IndexPoolCS.hlsl`) copies every geometry slot's
    16-bit indices into one buffer, so plain draws bind a single index buffer; the Z-prepass and the shadow views share it.
    `UpdateIndexPool` follows the geometry log with its own cursor, and the frame's first drawing commit reassigns the logged
    slots' ranges before its draws. Ranges are per slot, never shared by buffer address (DXVK reuses a freed address at once:
    sharing drew another mesh's indices after cell loads), and every logged slot gives back its range before any new range is
    handed out.
-   **Depth range.** DCLF draws both main epochs with the main pass's viewport depth range (`[0, 0.999998]`,
    `Core/Render/Main/FrameLayout.h`), not the engine's depth pass's (`[0, 0.999968]`), so their depths agree.
-   **Winding.** BasicRHI's `frontCCW` has D3D's meaning on both backends; DCLF passes the engine's own winding
    (`DrawPipelines::Impl::EngineFrontCCW`, and the view's state for shadow pipelines). Cross-backend gaps are closed in the RHI.

## Ownership and withholding

An object is withheld from the engine only while the installed publication claims it, and the publication's payloads draw every
claim. The phases DCLF claims for are a capability (the toggles and what DCLF has set up, `Coordinator::SetCapability`; the shadow
state catalog is enumerated at setup), not the views a frame happens to have.

### Toggles

The menu's toggles (`Core/Settings/Toggles`) change all of this live, applied at adoption.
-   The frame start applies the requested set (`Toggles::BeginFrame`), and the frame inputs carry it to the scene side
    (`FrameInputs::toggles`): the whole scene pass evaluates, commits and publishes under that set (a `ToggleSnapshotScope` over
    `RunSceneWork`), and the publication carries it (`ScenePublication::toggles`), its revision made for it
    (`RevisionProducer::toggles`).
-   The frames keep the installed publication's set (`FrameToggles`, `Toggles::Adopt` at `HandOverAtFrameStart`): until one
    committed under the new toggles is installed they draw, withhold and cover exactly as the installed one's toggles say, and
    nothing is withdrawn.
-   `ActiveToggles()` is the pass's set inside a scene pass and the frame's everywhere else; the scene side's producers that run
    outside a pass read the newest (`SceneToggles`).
-   A toggle that enters the classification, or a classify input (`Api/ClassifyInputs.h`: a CS feature switched on or off that
    holds objects back), drops the cached verdicts at the next pass.
-   Switching DCLF off clears every claim and bypasses the capture.

### Pass capture

`PassCapture` (`Core/Handoff/PassCapture.h`) hooks `BSBatchRenderer::RegisterPass` (vfunc `0x02`): a claimed geometry's pass never
reaches the batch renderer, so the native loop has nothing to draw. Not calling the original is the withholding.
-   It hooks registration, not `GetRenderPasses` (vfunc `0x2A`), because `technique` (with DoAlphaTest added) and `subPass` exist
    only there.
-   **Direct group insertions** bypass `RegisterPass` and are thunked too: the main modes' (`FUN_1414b2330`), the shadow modes'
    (`FUN_1414b2a60`: hints 11, 7, 3 and the volumetric group 15 / hint 8; `PassCapture::VolumetricGroupHook`), and the occlusion
    maps' (`FUN_1414b2c20`, group 14).
-   **The engine's depth pass** registers `BSUtilityShader` passes (`GetRenderDepthPass`) into renderers that are not the main
    camera's; they are withheld through the Z-prepass accumulator's (render mode 0xC) group insertions (hints 3, 7, 8, 11), by set
    phase.
-   **Which renderers.** Withholding covers the main views, an `InMainRenderers` view's accumulators (first person), an
    `OwnRenderers` view's renderers (the reflection faces), the occlusion maps and the shadow views (`SetViewCovered`,
    `SetViewRenderers`, `SetViewWithholdActive`, `SetViewFamilyOwned`, `SetFamilyOwned`).
-   **`WithholdMain`** (at `MainListHook`) withholds a member's main passes whatever its shader, the blended (hint 1), refraction
    (hint 4), cross-fade (hint 9 and 10) copies included, so DCLF draws the object once where the engine drew it twice during a
    cross-fade. It also withholds tree LOD's passes while the `TreeInstances` family is DCLF's.
-   Registration is concurrent (the scene lists build on `gJobList_SceneListAccumCulling`); captured entries go into a buffer with
    an atomic cursor, drained once a frame by the render thread, for observers only: DCLF draws nothing from registrations.
-   The engine draws a member for one frame when DCLF first draws it, because the claim is published after the colour epoch: the
    same pixels twice, not a hole.

The shadow views' claims are the same, per shadow render mode, from the shadow build's claim sets; the sun's registrations and
culls are below ([The sun's cascades without DCLF's objects](#the-suns-cascades-without-dclfs-objects)).

### The main camera's stand-in

`PrimaryCull` (`Core/Handoff/PrimaryCull.{h,cpp}`; toggle "exclude primary entries", `CS_DCLF_PRIMARY_EXCLUDE`) takes DCLF's objects
out of the main camera's cull. The engine's list jobs still run, and the render thread still waits at their `JobList::Finish`
(`CS.DCLF.ListJobsWait`); DCLF stands in inside them.
-   It hooks the list processes' `Process1` (vtable slot `0x16`, `0x140e28390`), filtered by process, on the job's thread. Entry 0
    of each list goes through `Process2` and stays the engine's.
-   **Admission by readiness alone.** An eligible entry (`PlanOf`) is admitted once every DCLF member it shows is in the set
    (`RunAdmission`, `Cut::pendingAdmission`). Only events check it: a new snapshot's entries, an entry a member joined, and,
    retried each frame, an entry with a member not tracked yet. Admission carried across snapshots is keyed by the root's member set
    (`MemberSignature`). No engine cull verdict is read.
-   **For an admitted entry in view**, the stand-in leaves the root's `OnVisible` to FadeStateCS, tests a tree's height, skips the
    geometries in the frame's set, and hands every other geometry to the registration in traversal order (`AppendVirtual`):
    decals, effects, blended objects and members not in the set. **Leaf exclusion** (`PrimaryCull::Owned`): wherever the engine
    still culls, an owned geometry is not handed to the registration.
-   **Per-entry walks by event** (`Cut::walk`, `RefreshWalk`). The stand-in walks only an entry with engine-drawn members or a
    member shown but not bound; every other admitted entry is one lookup in the list job. The walk flag is kept by events: a new
    snapshot, `NoteMemberLost`, a member newly drawn, `NoteAllMembersLost`.
-   **`PlanOf` judges nodes and geometry by their `OnVisible` slot** (vtable `0x34`, read at install: `PrimaryCull::WideOnVisible`).
    Accepted: `NiNode`'s (and subclasses that keep it), `BSMultiBoundNode`, `NiSwitchNode` (non-root, followed by event),
    `NiBillboardNode` (DCLF turns billboards on the GPU), `BSOrderedNode` (DCLF orders forward members itself), a fade, leaf or tree
    node at the root only; geometry: `BSGeometry`, `BSDynamicTriShape`, `BSSubIndexTriShape`, `BSSegmentedTriShape`. Anything else
    is rejected and the report names the class.
-   The cut it stands in with is kept per candidate entry (`PrimaryCull::Cut`), planned again only for the entries that moved. The
    render thread reads only the adopted publication and published tables, never the coordinator's live maps; PrimaryCull's
    liveness questions answer from the cut's candidate snapshot (`cut.candidates`).

**The culls off per entry.** Some geometry is still the engine's (water, sky, grass, precipitation, the particle manager's
systems, untracked temporary effects, first-person refraction, whatever the census lists), so the engine's culls cannot be turned
off globally. The switch is per entry: an entry whose every geometry DCLF draws ("whole") is skipped by every engine cull; an
entry with any engine-drawn geometry ("mixed", `Cut::mixed`) keeps the engine's culls whole (its `Process1` runs, and the
`AppendVirtual` hook keeps DCLF's members out of the registration).

| View | How a whole entry is skipped |
| --- | --- |
| Main camera's list jobs | Out of the scene lists (exteriors), else `Process1` returns at once (the stand-in, interiors) |
| Sun's full-frustum cull | Out of the scene lists it reads, and vetoed at insertion (`AppendNonAccum`) |
| Sun's cascades | The sun entry exclusion |
| Point lights' paraboloids | The light candidates' exclusion and `LocalLightCull`'s category filter |
| Occlusion maps | Out of the scene lists `Precipitation::SetupMask` culls |

Whole plain entries are stood in (`standInRoots`): no engine cull writes their bit. What stays with the engine: mixed entries
(campfires, waterfall bodies, splash emitters, weapons with blood, torches with `HeatRefraction`), rejected entries, entries that
are not sun candidates or wait for admission, and the views outside the switch (reflection faces' and first person's own culls,
grass, sky, the particle manager). `DrawWorld_BuildSceneLists` keeps running, because the lists still carry mixed, rejected and
non-candidate entries, the portal graph's rooms and occlusion planes, and water. Measured: about 3,350 of 3,480 candidate entries
whole in motion at Riverwood, 1,682 of 1,692 in an inn; the render thread's list-job wait fell 0.444 to 0.269 ms, the sun's
full-frustum cull 0.151 to 0.059 ms and the cascades 0.186 to 0.096 ms.

The light receivers need no DCLF chain for whole entries: residue lives in mixed entries, which the engine culls itself. Neither the
sun exclusion nor local shadow lights are preconditions of the stand-in (`PrimaryCull::PrepareFrame`).

### The scene lists without DCLF's roots

Roots that are admitted, not mixed, with `Cut::walk` clear, and removed from the cascades by the frame's sun exclusion are left out
of the six scene lists while DCLF draws both occlusion maps (`PrimaryCull::PublishListFilter`, `Core/Handoff/SceneLists.cpp`).
-   `ListReason` records why a root stays: not eligible, not admitted, mixed, walked, or not sun-excluded.
-   A detour on `DrawWorld_BuildSceneLists` (`0x14064bc20`) applies the filter on the build's own job. Each list keeps an entry,
    because its job sets the frustum up from the first.
-   **List modes** (`PrimaryCull::ListMode`): **Keep** (the object root's part stays as the last frame left it), **Rebuild** (DCLF
    builds that part itself, `EnumerateSceneLists`) and **Engine** (the engine's build, filtered: interiors and parity frames,
    `Policy::FilterSceneLists`). In Keep and Rebuild the engine's build still runs for portal rooms, occlusion planes and the extra
    list; its `AsNode` call on the object root (`0x14064be5a`) is patched to skip the walk. `PortalViews::VisibleUnbound()` is
    the lists' branch (`ListsKeepable`).
-   **Kept lists hold hidden roots.** The list-cull detour (`CullList`, `FUN_140e28f70`) sets the skip-hidden flag, and each list
    starts with an empty node that is never hidden.
-   **What changes kept lists**: structure changes (`NoteListStructure`) rebuild them; root attaches and detaches under a category
    node are queued (`listEvents`, applied in order by `ApplyListEvents`); a hidden bit on a listed node, a new filter, or the
    build's globals also rebuild.
-   `RestoreSceneLists` puts the roots back before an engine `SetupMask`, since only the occlusion maps read the lists between the
    list jobs and `ClearLists`. It re-inserts only roots its candidates hold and leaves out a root no longer under the scene node.
-   **Invariants:** roots the build's job releases go to `listGraveyard` and are released at Present; after a kept frame the
    engine's build must clear the lists first (missing it once put every root in twice and hung the render thread).
-   Measured at Riverwood: about 8,500 of 11,100 entries left out a frame; the sun's `objectArray` from 2,163 to 539 entries.

### kAccumulated from visibility events

The engine's culls do not run for entries DCLF draws whole, so their `kAccumulated` bit (NiAVObject flag bit 26) is DCLF's to write,
from the GPU's verdicts: an observer of DCLF's own culling, read back as change events.

#### What kAccumulated means

The bit DCLF writes means what the engine's main-camera cull means: in the main camera's view (frustum, plus the portals and
occlusion planes PortalViews reproduces), not app-culled, and for a geometry, not under a root whose `OnVisible` stops (faded
out). DCLF's HZB occlusion is not part of it, because the engine's cull has none. Its readers are the animated references' update
gate (`FUN_1402cff60`), the tree clock (`BSLeafAnimNode`), and the dynamic lights' receivers (every level down to the geometry).
-   A root's bit keeps the engine's meaning: the engine sets it on a faded-out root too; readers test the fade themselves
    (`FUN_1402cff60`: `currentFade <= 0`). "Not faded out" applies only to the geometry.
-   A root without a portal program follows `BSCullingProcess::Process1` (`FadeStateCS.hlsl`, `FrustumInView`, `EngineInView`):
    preprocessed roots (bit 12) pass unless `kNotVisible`, always-draw roots (bit 11) pass untested. A preprocessed root is tested
    against the main camera's frustum alone (the actor pre-cull's test, `BSFadeNodeCuller`), never a room program.

#### The visibility events

`VisibilityEventsCS.hlsl` (pass `core.z.visibility-events`, after both phases; `Core/Render/Passes/VisibilityEvents.cpp`) turns the
verdicts into change events. A root's verdict is FadeStateCS's `kFadeVerdictInView`; an object's is depth phase 1's frustum stamp,
which carries the fade drop (`kVisibilityBitFadeHidden`). One thread per root slot, then per object slot, compares the verdict
with its last reported word and appends only changes after a header (count, scene frame). A root's word is keyed by its listing's
generation; an object's by its slot. A change that does not fit stays unreported and is appended again next frame; the overflow
count grows the list. Nothing is dropped or capped.

**The readback** (`Core/Render/Readback/Readbacks.h`, `HostEventRing`, `VisibilityReadback`; `Visibility.cpp`): the readback pass
copies the whole list into the host buffer of its frame slot (`RecordingContext::FrameSlot()`), in the staging set of the
revision's list capacity. Staging sets are append-only and kept (a kept recording may name any of them). The depth commit harvests
the slot's last execution first (the host has waited for it; DCLF waits for nothing), then arms it with its frame and tables.
Batches are heap copies, never dropped, decoded in frame order by one task on DCLF's executor, never joined. A frame whose epoch did
not run is counted missed; the GPU appends its changes again. Only the render thread lets go of a batch's tables. The same
`HostEventRing` scheme carries the fade write-back's events. The decoder keeps `RootVisibility` per root slot and `ObjectVisibility`
per object slot, applying each batch against its own frame's tables.

#### kAccumulated written by DCLF

-   **The write rule** (`PrepareWrites`, from the newest batch's tables). For every listed root, every geometry under one (layers
    excepted) and every plain entry root (an entry with no fade root: a reference's root or a terrain block's multibound node), the
    bit is set when the slot was in view in either of the last two decoded frames, when a root's listing has no verdict yet, or
    when a camera cut has no batch of its frame or later decoded yet; otherwise it is cleared. Only changes from the last write are
    written; a full write follows a reset, a cut's first batch, or a refresh request. A plain entry root is visible when any of its
    geometries is. A geometry DCLF does not draw (no main input) is not written, so the engine's cull keeps that bit.
-   **The camera cut** (`PrimaryCull.cpp`, `kCutDistance` 256 units, `kCutTurnCosine` 30 degrees; `IndirectDraws::NoteCameraCut`):
    until a batch of that frame or later is decoded, every DCLF-written root and its geometry is set.
-   **The write** (`IndirectDraws::WriteAccumulated`, at [Main::Update's entry](#mainupdates-entry)): an atomic read-modify-write on
    `+0xF4`, skipped when already right. For each geometry written visible, the nodes between it and its root are set too
    (`SetChain`). `CellRoots`: every non-hidden child of the object root is set at the entry. Coming into view the write lags the
    GPU verdict by the readback's frame slots plus one frame; going out of view is covered by the two-frame window.
-   `IndirectDraws::AccumulatedVerdict` keeps each listed root's last-written verdict (`rootVerdicts`), for the tree manager's list
    loop.

### Room visibility, the tree list, the actor stamp

The engine's other readers of cull results are answered by DCLF, so none depends on a cull that no longer runs:
-   **`Main::IsRoomVisible`** (`Features/Portals/RoomVisibility.*`): its three calls (managed nodes, `Actor::UpdateRefLight`,
    `FUN_140771520`) are detoured. While DCLF runs and a set is published, the answer is `RoomVisibility::DclfAnswer` from the
    newest `PortalViews::VisibleSet` (the walk's rooms, the shared nodes of crossed portals, and the shared node of every portal of
    every popped room; immutable once published, lock-free by `VisibleSetHold`). With DCLF off the engine answers.
-   **The tree manager's list loop** ([Owned trees off the tree manager's animation list](#owned-trees-off-the-tree-managers-animation-list)).
-   **The actors' animation stamp** (`Features/Actors/ActorRate.cpp`, `UpdateActorRate`, at `afterWrites`): for each high actor whose
    3D's world bound is inside any of the last scene frame's shadow views (the sun's full-frustum planes and cascade culls, a spot
    light's planes, a point light's sphere), it stamps the skin property's `lastAccumulatedFrameCount` (`+0x80`, via
    MiddleHighProcessData `+0x178`) with this frame's `gFrameCounter`. No cull result is read. Why: only the depth and shadow
    registrations stamp it, so it is lost once the culls stop, and the sun's entry exclusion keeps off-screen actors out of the
    cascade culls.

### Accepted differences

-   **Lag.** An object joins DCLF a frame or more after it appears; the engine draws it meanwhile. A leaving member is drawn from its
    retained record until the publication without it is installed. A structural change (material, pipeline) shows when its
    publication is installed.
-   **Shading is sampled at the frame start**, so a value written while the frame renders (a controller on the animation job, a
    cull's `GetRenderPasses`) is drawn from the next frame. Capture parity counts those draws on their own line
    (`shadingSinceStart`).
-   **Fade verdicts lag one frame** (fade state runs a frame ahead), and an engine-drawn part under a written root reads the node's
    fade at the write-back's milestones, not every step.
-   **Tree clocks always advance**, while the engine advances only visible trees within its budget, so out-of-view trees' phase
    differs (measured: the engine's clock runs at 0-1x the GPU's, by fraction in view).
-   **Equal-depth pixels across compilers.** The native shaders (FXC through DXVK, invariant positions, fused `precise` mads) and
    DCLF's (DXC) differ by a few D24 units on near objects. No correctness gate may depend on the two compilers agreeing bit for
    bit; DCLF owns the depth of what it draws. HLSL shared by both must not depend on a degenerate division, which the compilers
    round differently (`GetTerrainShadow` divides only where the penumbra exceeds 1e-3); input parity cannot catch such a
    difference.

## Shading: materials, constants, rows and bindless

### The material port

Material records are ported, not evaluated by the engine (`Core/Shading/MaterialPort.h`, `MaterialPortVanilla`, `MaterialSources`,
`GeometryPort`): a record is `MaterialPort::Evaluate(MaterialSnapshot, MaterialFrame)`, `SetupMaterial` reconstructed as a pure
function that runs on any thread. The scene lane keeps the newest capture per material and evaluates records itself
(`RefreshMaterialRecords`, `RewriteCapturedMaterials`). The engine's evaluation runs only as parity (`<- MATERIAL PORT`).

Where each material input comes from (`Core/Shading/MaterialSources.h`):
-   the material's own fields: the write captures;
-   `TexcoordOffset` (VS 11): the frame's, patched on the GPU ([Texture transforms](#texture-transforms-and-frame-variables));
-   the shader object and globals (PS 6, `SnowRimLightParameters`, `CharacterLightParams`, `LODTexParams.z`,
    `LandscapeTexture5to6IsSnow.zw`): one evaluation per signature (`MaterialSources::Signature`, `ApplyFrameComponents`),
    written only when `FrameComponentsWitness` moves (`RefreshMaterialSignatures`);
-   `IBLParams` (PS 29) and `FogColor`: read by no Lighting stage and drifting with the time of day, so a record keeps its first
    value (`KeepUnreadFloats`, `KeepTechniqueFog`); re-versioning them re-resolved every pair every frame. `IBLParams` comes from
    the `BSLightingShader` object, not the material;
-   the character light's t11: a frame register (below); the record keeps only its sampler modes. `CopyFrameComponents` copies t11
    whole, because its index is -1 while a cell loads.

**Feature textures.** Every register a feature's material setup binds is a material-record feature texture (TruePBR's t80-t91,
Advanced Skin's t71/t74; `MaterialRecord::featureTextures`, `Extensions::FeatureTextureRegister`); an unlisted register falls
through to the frame record, which is empty there (TruePBR terrain was drawn black in shadow). Advanced Skin's are
`Skin::MaterialTexturesOf`, one rule for its hook and for DCLF; Extended Translucency's material model is
`ExtendedTranslucency::MaterialModelOf`, likewise.

**Supported techniques.** Parallax occlusion (7) and multilayer parallax (11) are supported (`IsSupportedTechnique`);
`SetupTechniqueDescriptor` ports the remap (7 draws as 0 unless the global at `0x142035500` is set, 18 as 9 unless the one at
`0x142032fdb` is). The Lighting shader instance is the engine's (`kLightingShader`); a registered Lighting pass naming another
shader is `<- LIGHTING SHADER`.

### Frame components and the geometry port

-   **PerGeometry blocks are ported** (`FrameView::RefreshFrameConstants`, `Core/Scene/FrameView/FrameConstants.cpp`):
    `GeometryPort::PipelineGeometryConstants` makes a pipeline's block from the frame's sample without running engine code. It is made
    whole when the slot is new, at the first refresh, or when the pipeline inputs move; otherwise
    `GeometryPort::RefreshFrameValues` writes the per-frame values in place (the sun, the ambient, the ambient specular, and
    `EyePosition` by the port's rule). `MakeNewPipelineConstants` makes a new pipeline's block on the coordinator, so it is in the
    publication that makes the slot drawable. SSE Engine Fixes' BSLightingAmbientSpecular patch (PS 6 written in SetupGeometry) is
    detected by its instruction and ported.
-   **The tables copy the frame's eye into every pipeline whose `SetupGeometry` writes `EyePosition`** (`WritesEyePosition`).
-   **Technique rows** are the coordinator's (`TechniqueRowFor`, `RefreshTechniqueRows`, `Tables::techniqueConstants`);
    `EvaluateTechnique` reads only `TechniqueKey`, pure over `FrameGlobals::technique`, once a frame per key.
-   **Constant tables come from SPIR-V reflection** (`ConstantTableOf`, `SpirvReflection`) of DCLF's own modules; ShaderCache's
    tables are the parity (`<- CONSTANT TABLE`).
-   **Constant blocks are compared bit for bit** (`ConstantBlock::SameBits`): components the engine does not write hold
    `kUnwrittenBits`, a NaN, and upload as zero.

### Frame constants

-   **`ConstantMirror`** (`Core/Engine/Capture/ConstantMirror`) keeps CPU copies of the per-frame D3D11 constant buffers DCLF watches,
    fed by the immediate context's Map/Unmap and UpdateSubresource, and re-reads mapped buffers from their last mapping (the engine
    writes `VPOSOffset` after unmapping PerTechnique).
-   **The frame lighting** (`DCLFFrameLighting`, PS b13, frame slot `kFrameSlotLighting`): `DirLightDirection`, `DirLightColor`,
    `DirectionalAmbient`, `AmbientSpecularTintAndFresnelPower`, merged per component from the frame's evaluations, because
    techniques differ in what they write.
-   **Vertex fog** is the frame's: `DCLFFrameFog` (VS b13), latched by every main commit from `FrameCapture::fog` (`MergeFrameFog`).
-   **Extras rows** hold only static parts (LandBlendParams, TextureProj, ProjectedUVParams); the frame parts are an `ExtrasFrame`
    each main commit latches, and the shader completes them from the placement row (`DCLFTextureProjOf`; CPU `CompleteExtras`).
-   **The character light's noise** is the frame register t125 (`DCLFCharacterLightNoise`, `kCharacterLightRegister`), from a
    character-light signature's live sample (`MaterialSources::CharacterLightView`). Not a second pipeline layout, because an
    indirect pipeline set needs one layout for all its pipelines (BasicRHI `IndirectPipelineSetDesc`).
-   **The sampler table** is a copy of the renderer's own D3D11 sampler states, read at its AE address (`GpuTextures.cpp`).

### Rows and bindings

**Per-object rows** (`Core/Shading/LightingConstants.h`, read by `package/Shaders/Common/DCLFObjects.hlsli` as structured buffers by
the draw's object index):

| Register | Row | Size | What |
| --- | --- | --- | --- |
| t127 | `BindlessObject` (`DCLFObjectRecord`) | 64 B | structure only: ancestor value (Light Limit Fix's room index), record flags (bits 8-31 the fade root slot + 1), alpha test reference, LOD fade flags, an 8-word feature block (tree wind's slot and generation), bone palette offsets and rows, extras offset |
| t126 | extras | 16 B rows | the record's static extras, at its extras offset |
| t123 | `BindlessPlacement` | 160 B | FrameValues': World and PreviousWorld as absolute 3x4, bound, sun entry, LOD fade node, forward sort |
| t122 | palettes | `float4` rows | FrameValues': a skin's block holds its current then its previous palette (`PaletteRowsOf`), and particles' corners |
| t121 | `BindlessShading` | 96 B | FrameValues': MaterialData, EmitColor (w = SSRParams.w), Advanced Skin's wetness, Linear Lighting's emissive multiplier, a written mask, a feature block (tree shading) |
| t124 | tree wind | 3 `float4` an entry | [Tree wind on the GPU](#tree-wind-on-the-gpu) |
| t120, t119 | fade states, fade roots | 48 B states | [Fades on the GPU](#fades-on-the-gpu) |
| t118 | Effect soft depth | | `kPOST_ZPREPASS_COPY`'s view, for soft effects |

Records are absolute: `Lighting.hlsl` and `Utility.hlsl` subtract the drawing view's `CameraPosAdjust` (c40) and
`CameraPreviousPosAdjust` (c41) as `BonesPivot`/`PreviousBonesPivot`, `precise`. That is why one set of rows serves every view, and
why no build reads the eye. Palettes are eye-relative like World, so the shader pivot is zero.

**Main pass** (`Core/Gpu/Pipelines/DrawPipelines.h`, `kDrawPush*`):
-   **per draw:** six push words (`kDrawPushWords`; the Constant argument is five, `kDrawPushArgumentWords`): the pipeline row's
    address (words 0-1), the material row's (2-3) and the object word (4), then a pad. `BuildDrawsCS` computes both addresses from
    the input's `y` (`RowsOf`: the pipeline slot in the high 12 bits, the material slot in the low 20), the rows' strides in its
    constants and the tables' bases in its latch (the installed payload's `materialRowsLo`, `pipelineRowsLo`). The object word holds
    the object index (bits 0-25), the local shadow lights BuildDrawsCS selected (26-29, Light Limit Fix's `ShadowBitMask`) and
    `kObjectSunMiss` (bit 31).
-   **per material slot** (`MaterialRow`, `kMaterialRowBytes` 1024): the vertex and pixel `PerMaterial` blocks (b1), then a header
    with t0-t15, s0-s15, the projected textures and the feature textures. A material slot is keyed by (material, pass descriptor),
    and the pack tables depend only on the technique, so the row is packed once for every pipeline and both segments.
-   **per pipeline slot** (`PipelineRow`, `kPipelineRowBytes` 2048): the technique blocks (b0), the PerGeometry template (b2), the
    permutation (b4), the shadow mask (t14, s14) and the vertex layout.
-   **per pass:** the frame push (binding 191, `kFramePushVS`, `kFramePushPS`): the frame slots' addresses (PS b3 shared lights, PS
    b13 frame lighting, ...) and the frame record (`DrawBindings`, 800 bytes: every other texture and sampler, and the payload's
    object and extras tables).

Row blocks sit at 256-byte offsets, so a row table is an array of constant buffers; each row's header holds its blocks' offsets
until the publication adds the address of the row in the backing it goes to (`EmitMainRowParts`, `PatchRowAddresses`). The rows
are kept across frames (`MainRows`, a chunked `KeptArray` each): a row is written again only when its key changes (a material row
by part). Under `DCLF_BINDLESS` the PerGeometry block is per pipeline; b3 (Light Limit Fix's room index and shadow mask), b11 (the
alpha reference) and b8 (Linear Lighting's emissive multiplier) come from the object and shading rows. The permutation block keys
on the pipeline alone, because `SuppressExternalEmittance` is read only by `Effect.hlsl`. The registers' namespaced constant
buffers go through `Common/NamespacedCBuffer.hlsli`, because DXC scopes cbuffers declared in namespaces differently from FXC; its
`NSCB_ALIAS` statics redirect `RoomIndex`/`ShadowBitMask` to the object record without touching Effect, Particle, Water or RunGrass.

**Shadow views** (`kShadowPushWords`): a draw's words name its material row (`ShadowMaterialRow`, one 256-byte block: the Utility
vertex shader's `PerMaterial` b1 and the diffuse's index for t0) and the object word; pushed once per view: the view's
`PerTechnique` (b0) and `VS_PerFrame` (b12) blocks, the zero block, `SharedData` (b5), `FeatureData` (b6) and the frame record.
Shadow material rows live in one growable table every view reads (the payload's shadow rows part, `GrowableRows`); the view's
blocks go in push data. Nothing caps the materials.

**Push-data tokens.** BasicRHI's Vulkan backend splits push data into tokens of at most 16 bytes (`kMaxPushDataTokenBytes`), so the
six-word draw push is two tokens: a 20-byte token lost the device on NVIDIA GA102.

**Scene tables.** One set of object records, extras rows and geometry rows - the installed payload's parts - and one face positions
buffer (`SceneBuffers::facePositions`, the set's) serve every epoch. The set requires the face positions at the highest face region,
and the per-object buffers - the visibility and frustum words, the reported verdicts - as one unit group at the tables' objects
(`RequireSceneTables`), so a segment or a mode never has more inputs than its buffers hold.

### Object rows

The object row (`BindlessObject`) is 64 bytes, structure only; what moves per frame is in the placement and shading rows. The shaders
read it as the structured buffer at t127, indexed by the draw's object word. Reading it as a constant buffer at its address instead
(a per-draw push address written by `BuildDrawsCS`) was built and A/B'd on the RTX 3090 Ti with the camera turning: colour 1.03
against 1.02 ms, shadow views 1.98 against 1.94 ms, Z-prepass depth 0.286 against 0.277 ms. The constant buffer was no faster on any
pass, so it was not kept.

### Vertex pulling and identical positions

-   **Pulled stages** (`DCLF_PULLED`; `PulledLightingSource` rewrites Lighting's source): the Z-prepass and the shadow views' vertex
    stages find their sequence by `SV_InstanceID` and decode the attributes from the pipeline row's vertex layout
    (`PipelineRowHeader::vertexLayout`); the Z-prepass and shadow views draw with plain indirect draws and fetch their own indices,
    vertices and per-draw words. The PerTechnique, PerMaterial and PerGeometry blocks become statics assigned from the rows, and the
    permutation block `Permutation.hlsli`'s statics (`DCLF_PULLED_ROWS`). `CheckPulledBindings` rejects any row binding.
-   **The Z-prepass and the colour pass compute bit-identical positions**, because the colour pass tests EQUAL: `viewPos` is
    `precise` (`DCLF_PRECISE`) in both. 8-bit UNORM attributes decode through `DCLFUnorm4` (the product by 1/255, corrected by its
    fused remainder), exact for all 256 values; `x / 255.0` is an ulp off on 126 inputs and striped foliage. The shadow views'
    pulled Utility stages use the same decode.
-   `DCLF_DEPTH_ONLY` (the Z-prepass pixel stage) returns right after the alpha test, so it reads only what the discard needs.

### Shader programs and pipelines

-   **DXC for DCLF's programs** (`Core/Gpu/Pipelines/ShaderPrograms.cpp`): SPIR-V, HLSL 2018, `vs_6_6`/`ps_6_6`, binding shifts,
    CS's `ShaderCache::GetCompileDefines` plus `DCLF_ORG`, `DCLF_BINDLESS`, `DCLF_BINDLESS_DRAW` (always), `DCLF_PULLED`,
    `DCLF_PULLED_ROWS`, `DCLF_DEPTH_ONLY` and the families' own. CS's game shaders stay FXC.
-   **Precompile.** CS's initial compilation workers also request DCLF's SPIR-V stages through ORGModuleServices; requests made
    before ORG setup are retained until `SetupResources`, and stage futures are shared by descriptor, stage and source family. An
    on-demand compile or pipeline build later is a warning naming what the precompile missed ("on-demand SPIR-V compile",
    "on-demand pipeline build" with `NearestKey`'s difference). `CS_DCLF_SHADER_DEBUG` (on) embeds debug info and copies the HLSL
    beside the log; it also makes identical code distinct modules, so the default build has more Z buckets.
-   **Shadow pipelines** are the pipeline lane's (`PipelineCatalog::shadowEntries`, `shadowSetVersion`); `ShadowRasterStateId` stays
    on the render thread (the capture uses an id at once) and posts the append-only list. A commit's readiness reads the modes and
    states its lookups resolved, never the render thread's.
-   **Opaque pipelines take their blend state from the engine's state for their write mode** (`DrawPipelines::Impl::ReadEngineState`,
    write modes 1 and 10), never the default blend state. Decal and forward state comes from the engine's own state objects,
    captured inside the deferred pass (`DrawPipelines::CaptureEngineStates`), because Community Shaders swaps the blend table for its
    deferred variants there. An attachment the pixel stage has no output for gets write mask 0 (`SpirvReflection::outputLocations`),
    as DXVK builds the engine's pipeline.
-   **Textures** (`Core/Gpu/GpuTextures`): one import per SRV, made stable in DXVK and imported into BasicRHI without ownership, with
    simultaneous access (DXVK keeps images in GENERAL), with a view in ORG's shader-visible heap; a null SRV reads zero through a
    null descriptor. `Request` (any thread) never imports on the caller: the import thread answers, and a draw whose texture is
    pending is deferred (it stays the engine's), because marking an image stable is a synchronous DXVK CS chunk and view creation
    blocks in the driver (up to 20 ms measured while streaming). `ResolveBinding` imports inline only for the frame's textures. A
    failed sampler creation is logged once and returns no index, so the draws that need it are skipped (`GpuTextures::Sampler`).
-   **Buffers**: `dxvkGetInteropResourceInfo` (`src/RenderGraph/DxvkOrgInterop.h`) returns the Vulkan handle and device address and
    marks the resource stable. Buffers the game can map are rejected (`Ineligible::UnstableBuffer`). A cleared slot's import leases
    go to the next execution committed and are held until the GPU retires it: DCLF draws game buffers by device address, which DXVK
    does not see (a released chunk's memory once went to the next streamed buffer while a submitted pass still drew it).
-   **RenderDoc**: with `renderdoc.dll` loaded the device has no `VK_EXT_descriptor_heap`, so the render graph cannot start and DCLF
    is off for the session, with the reason logged.

## Fades and LOD cross-fades

### Fades on the GPU

Fades are DCLF's: every pass of a member is withheld and drawn by DCLF, a fading member's included, with the alpha taken from its
root's fade (Features/Fades; `Core/Scene/Model/Records.h`, `FadeRootStatic`; `FadeStateCS.hlsl`).

-   **The fade roots.** Each fade node a member draws under gets a slot (`FadeRootStatic`, written by Features/Fades' coordinator
    component, `Features/Fades/FadeRoots.*`; every record lists its root, `ListFadeRoot` / `UnlistFadeRoot`). The coordinator's half
    is the mirror's: the radius, `fadeAmount`, near and far, `+0x109`, and the root's plan (`kFadeRootFade`, `kFadeRootLeaf`,
    `kFadeRootTree`, `kFadeRootOther`). The LOD distance scale (`lodScale`) is `powf`/`logf` computed on the CPU with the engine's
    CRT, because the GPU does not round them the same way. Every listing and every refresh of an input takes a new `generation`; a
    tree's LOD switch selecting past child 0 is an input (`kFadeRootTreeLod`), kept current by switch events; a placement snap
    reseeds the root (`ReseedFadeRoot`).
-   **Seeds at the frame start.** The node's own state and values are a seed the render thread samples at `FrameValues::Kick`,
    before the frame's culls update the node (`FadeState::StaticOf`, `FadeSeedItem`; a root sampled later in the frame is a step
    ahead). Each slot has two seed rows (`kFadeRootSeedOdd`, toggled by every listing, reseed and ownership change), so a new seed
    never writes the row the installed generation reads. FadeStateCS merges the seed of its generation (`MergeFadeSeed`) and skips
    a root whose seed has not arrived. The seeds are Features/Fades' registered FrameValues pool.
-   **The pass** (`fades.state`, `cs.dclf.z.fade-state`) runs on the compute queue in the Z-prepass epoch, one thread per root.
    Once a frame, a root the main camera's cull would visit gets its class's `OnVisible`: the tree height test, the leaf LOD step
    (`FUN_14147b110`, `FUN_14147a430`), the fade update and the tree LOD fix-up. Its state rows (`FadeNodeState`, 48 bytes,
    BSFadeNode's own fields) are its own, reseeded when the generation differs. The verdict bits are `kFadeVerdictInView`,
    `AboveLimit`, `Serviced`, `Drawn` and `Animated`. The frame inputs come from `FadeFrame`, a one-row buffer the depth commit
    writes.
-   **A frame ahead.** It publishes every root's state into one of two outputs by scene-frame parity (`FadeScene::fadeStatesOut`);
    the builds read the frame before's (`FadeScene::FadeStatesRead`, through `BuildDrawsLatch::fadeStatesIndex`). A zeroed output
    reads as the root's listed state. Verdicts therefore lag one frame.
-   **The engine's cull test, repeated** (`EngineInView`): Process1's node flags first (always-draw bit 11, preprocessed bit 12,
    bit 20: `kFadeRootAlwaysDraw`, `kFadeRootPreprocessed`, `kFadeRootPreprocessHidden`), then the root's room portal program
    ([Portals](#portals)), or the frustum alone (`kPortalNoProgram`). Without the compound-frustum test the GPU stepped the fade of
    roots behind occlusion planes, which the engine leaves alone: two copies of one state machine stay equal only on equal inputs.
-   **The C++ port** (`Features/Fades/FadeState.cpp`) is the same state machine, instruction for instruction: operation order,
    `MINSS`/`MAXSS`, and the unordered cases of the comparisons. `FUN_14147a430` takes the LOD output as a float in XMM1.
-   **Animated references' fade updates.** `UpdateAnimationJob` runs `FUN_1402cff60` -> `FUN_14147a160` for an animated reference the
    cull did not reach. `Hooks::AnimatedFade` records the nodes and inputs; the batch is stamped into `fadeAnimated`, and FadeStateCS
    applies that many updates before its cull test (`kFadeVerdictAnimated`). The CPU port does the same (`FadeState::AnimatedUpdate`).
-   **Who follows it.** FadeStateCS is authoritative for every root DCLF draws under. Phase 1 drops an input by its root's serviced
    verdict; where the published state did not service the root, the fade-out distance test applies (`kObjectFadeTest`). Shadow
    views drop a caster while its root fades (`RootFading`); `ShadowCasterReject` takes the material's alpha alone and reads no
    node.
-   **Every draw's alpha takes its root's fade** (`DCLFAlphaFade`, `DCLFObjects.hlsli`). The member's shading row holds the
    material's alpha; the object record names the root (`recordFlags` bits 8-31 = root slot + 1); the frame record binds the states
    at t120 and the static rows at t119. A generation-0 row means no fade. The Z-prepass reads the same row, so it tests the same
    alpha.
-   **Blended fades** (hint 9) are drawn in their opaque group with the faded alpha: DCLF has no blended pass for them.

### DCLF as the single fade writer

Every listed root is `kFadeRootOwned` (its members follow FadeStateCS) and `kFadeRootWritten` (DCLF is its one fade writer);
`kFadeRootStoodIn` marks the roots the cut stands in for, which no engine cull reaches (`Features/Fades/FadeRoots.cpp`).
-   `Features::Fades::FadeWrittenByDclf(node)` is the one predicate, reading the tables published at `Main::Update`'s entry
    (lock-free, immutable, held until the next entry; `Features/Fades/FadeWriters.h`).
-   **The engine's other writers skip written roots.** The animation gate's fade update (`Hooks::AnimatedFade`, the call of
    `FUN_14147a160`) still notes the update for FadeStateCS's animated batch but does not call the engine. `BSFadeNode`,
    `BSLeafAnimNode` and `BSTreeNode::OnVisible` are detoured at their entries (`FadeOnVisible`, `LeafOnVisible`, `TreeOnVisible`)
    and for a written root keep the engine's control flow (children, height test) without any store. Detours, not vtable slots,
    because subclass tables and `FadeState::PlanOf` compare the slot with the engine's addresses.
-   An owned root with engine-drawn parts is culled by the engine; FadeStateCS runs the same update for its DCLF members
    (`CS_DCLF_FADE_PARITY` compares).

### The fade write-back

The engine still reads the nodes DCLF writes: the tree LOD's instance hiding (`BGSDistantTreeBlock::UpdateBlockVisibility`), the
other views' culls and the occlusion maps' engine cull, `GetRenderPasses`, the animation gate and the next listing. So FadeStateCS's
milestones are written back onto them (`Features/Fades/FadeRecords.h`, `FadeScene.h`, `FadeWriteBack.cpp`).
-   For each root DCLF writes, FadeStateCS appends a `FadeEvent` to `cs.dclf.fade-events` when its milestone changes from the one in
    `cs.dclf.fade-reported`. The event carries root, generation, flags, `currentFade`, `levels` (`+0x152 | +0x153 << 8`) and `blend`
    (`+0x14C`). A milestone is faded out, fading or faded in, combined with the leaf LOD level (`+0x152 & 0xF`) and transition
    (`+0x153 & 0x70`), so a level step and its settling are events; blend steps are not.
-   The `fades.events` copy pass takes the list to the host on the `HostEventRing` scheme (one host buffer per frame slot, harvest
    then arm, staging sets kept, growth by `wanted`); the depth commit writes the list header whether or not anything is appended
    (`FadeCommit.cpp`).
-   `KickFadeWriteBack` (a task on DCLF's executor posted at the frame start when none runs, never joined) judges the batches in
    frame order against the frame's immutable tables, dropping stale generations, roots no longer written, and anything older than
    the last event taken.
-   `WriteFadeStores` makes the stores on the main thread at `Main::Update`'s entry: the fade, the flags' fade bits
    (`kFadeFlagMask`, atomically), and for leaf and tree roots the blend and levels (`+0x14C`, `+0x152`, `+0x153`). It pushes a
    mirror update, not a fade event. With DCLF off the stores are dropped.

### LOD cross-fades

While a root crosses LOD levels (`+0x153 & 0x70` is not `0x20`), the main views also draw the engine's hint-10 copy, DCLF's way
(`BuildDrawsCS.hlsl`; `PrimaryCull.cpp` counts crossing roots).
-   Crossing roots stay stood in. FadeStateCS steps the level and the blend; BuildDraws adds the partitions of row L+1 less row L
    to a main view's draws, marked `kObjectCrossfadeCopy` (object word bit 30), with the cross-fade factor (`+0x14C`) as alpha.
-   The engine's stencil test (mode 0xB, ref `int(blend * 31)` GREATER than the stencil) is taken as the exterior's (stencil 0),
    so the copy draws while `int(blend * 31) > 0`. DCLF draws it in both the Z-prepass and colour; the engine draws it in the
    G-buffer only.
-   The member's hint-10 passes are withheld with its others (`PassCapture::WithholdMain`).
-   SE's settings never start a crossing; the path exists for load orders that do.

### Skin LOD partitions

A skin is one object whose draw writes one sequence per partition the engine would draw (`Core/Engine/SkinPartitions.h`,
`Tables::skinPartitions`). `Tables::skinLodPartitions` holds the skin's mask per level 0-3 (the table at `0x14202a030`, indexed by
the pass's LOD row and the partition's LOD byte; cumulative, so row 3 is every level's), and each drawing dispatch picks the byte by
its root's FadeStateCS level (`DrawInput::lodPartitions`, `LodPartitions` in BuildDrawsCS). A dispatch without the fade buffers
draws row 3. A `BSDismemberSkinInstance` first skips partitions whose flag is clear. A mask of 0 (the current level draws none) is
still a member with no draw. Each partition is a `GeometryDraw` link (`nextPartition`); an object stays one record, one visibility
word and one cull verdict, and only its draws multiply. `LodRowOf` gives a pass's row: its index, plus 4 for `0x80`.

## Object classes

Each class is a feature (`Features/<Name>/`) on Core's contracts ([dclf-layout.md](./dclf-layout.md), "The extension points"),
gated by its live toggle, all on by default.

### Opaque Lighting objects

The base class: `BSLightingShaderProperty` geometry under a tracked category node, eligible by `Eligibility.h`, drawn opaque or
alpha-tested in the Z-prepass and the colour pass, and cast into every shadow view the caster rule admits. Static and dynamic
references, multibound content, harvestables and switch-node leaves are all this class; skins, trees, actors, faces and LOD are
the same path with what their classes add.

-   **IsBeastRace** is per object (Integrations/SubsurfaceScattering: `kObjectBeastRace`, `kRecordBeastRace`), modelled as
    Subsurface Scattering sets it today, for faces only.
-   **Hair with ProjectedUV** is drawn without the projection (`HairProjection`, `DrawnPassDescriptor`): `SetupGeometry` writes the
    projected-UV constants for every technique but Hair, so natively they are stale for Hair.
-   **Terrain's vertex stride** is the vertex description's stride nibble times four (CommonLib's `GetSize` leaves landscape data
    out).
-   **Multi-index shapes** (`BSMultiIndexTriShape`, type 7) are classified as tri-shapes; their second pass is a pass layer
    ([Decals and pass layers](#decals-and-pass-layers)).

### Ordered nodes

A `BSOrderedNode` orders only its leaves' blended passes. The engine registers its opaque leaves in its normal opaque path
(`OnVisible` `0x141503970` opens a culling group; `ProcessJob` `0x140e28af0` gives the group to hint 1 passes alone), so DCLF draws
them in the main phase like any leaf. A blended leaf under one (the alpha property blends or material alpha < 1; `OrderedLeaf`) is
DCLF's with the forward toggle and `UnsupportedParent` (shadow-only) without it; waterfalls' `OrderedRenderingNode` and hay are the
common cases.

**The ordered-node interleave.** The sort key is `BindlessPlacement::forwardSort` (placement row 9): `FrameValues::SampleRow`
(`StoreForwardSort`, `Core/Gpu/FrameValues.cpp`), in the engine-read window, walks a blended leaf's parents to the nearest
`BSOrderedNode` (exact type) and stores its live world bound centre, with w the leaf's ordinal (1 + its index among the node's
geometries, depth first in child order); 0 otherwise. BuildDrawsCS sorts such a draw at its node's centre and stages a group hash
of the centre's bits and the ordinal in the unexecuted `instanceCount` and `vertexOffset` words. A billboard under an ordered node
is a billboard.

### The forward epoch

Blended Lighting objects are drawn by DCLF's forward epoch (Features/Forward: feature, hooks, passes, epoch; `CS_DCLF_FORWARD`).
Core keeps the ranges, the `kRasterForward` bits, `ForwardSortCS`, the shared epoch's submission, refraction's pipeline and
`DeriveForward`.

-   **The epoch** (`forward`, `Segment::Forward`, trigger `blended.draw`) shares the colour epoch's shape and counts for the frame's
    claims while the class is on. The trigger is `Accumulator_BlendedDraw<0/1>` (`Features/Forward/ForwardHooks.cpp`): thunks at both
    call sites of the engine's blended draw `FUN_1414b39d0` (under and after water), main accumulator only. `SubmitForwardEpoch`
    submits through `IndirectDraws::SubmitSharedEpoch`, only after this frame's colour epoch.
-   **Classification** (`Core/Shading/LightingDescriptors.cpp`): a translucent Lighting property (blending, or material alpha below
    one) that is not a decal, a layer or Terrain Blending's gets `forward`, its blend mode (`AlphaBlendModeOf`), depth flags from
    `kZBufferTest`/`kZBufferWrite`, and the forward pixel descriptor (no Deferred). Object flag `kObjectForward` (bit 31); raster
    flags `kRasterForward`, `kRasterForwardDepthTest`, `kRasterForwardDepthWrite` with write mode 11; `AdditiveLighting` in the
    permutation for a destination-one pair. The synthetic membership pass is hint 1. View mask `kViewForward` alone: never a
    depth segment input (the engine's blended draws write no depth before the opaque ones).
-   **Pipelines.** A forward key is a main `PipelineKey` executed through the colour signature: the engine's blend on the first
    attachment, the others masked off; depth LESS_EQUAL when tested, written with `kZBufferWrite`.
-   **The range** has two halves (`MainLayout::forwardCapacity` each, every draw the forward slots can produce, a power of two):
    staging and sorted. BuildDrawsCS culls each forward input once in the colour segment (frustum, then the rebuilt HZB) and appends
    each visible draw to the staging half via count word 32.
-   **The sort** (`ForwardSortCS.hlsl`, `core.forward-sort`, in the colour epoch after the pipeline sort): one thread per staged draw
    computes its rank - farther first; at one depth a plain draw before an ordered group's; then group, ordinal under the node,
    object index, first index (stable whatever the atomic append gave) - and writes the sorted half with `instanceCount` 1 restored.
    O(n^2) in the visible forward draws, keys read through groupshared memory a tile of 64 at a time.
-   **The draw**: `forward.draw` draws the sorted half through the colour signature, with explicit preprocessing on its own state
    list (`Resources::preprocessForward`), on the colour pass's attachments all loaded (the first is kMAIN; the G-buffer attachments
    masked off as the engine has them unbound).
-   **The ordering contract.** DCLF's blended objects draw after the water, effect groups 10/11/12/7 and everything opaque, sorted
    back to front among themselves, and before every blended pass the engine still draws. Exact against additive engine passes in
    front; wrong by an engine pass's contribution where it lies behind a DCLF surface. Not after the engine's, because an alpha-over
    DCLF surface would then cover an effect in front of it. DCLF owns blended Lighting, the Effect port's meshes, particles and
    billboards in one sort.
-   **Views.** Main camera and first person's forward part. Shadow views draw no blended Lighting: the caster rule rejects
    alpha-blended properties.

### Refraction

Lighting geometry with `kRefraction` or `kTempRefraction` (heat haze, streams, invisibility; `CS_DCLF_REFRACTION`, needs forward).
-   **What the engine does.** Refraction has no colour pass: `GetRenderPasses` builds one Utility `RenderNormal` pass (hint 4, group
    5) after the refraction LOD fade (`fRefractionLODFadeStart`/`End`, result in the property's `+0x104`). `FUN_1414b4860` draws group 5
    after the world into `kREFRACTION_NORMALS` (RT 13), cleared to (0.5, 0.5, 0, 0), depth tested not written, no blending (write
    mode 2). The warp is the image-space `ISRefraction.hlsl`, which samples the image after the world is drawn.
-   **DCLF draws it in the forward epoch, after the blended draws** (`forward.refraction`, `cs.dclf.refraction`,
    `Features/Forward/ForwardPasses.cpp`): clears the imported normals to (0.5, 0.5, 0, 0) and draws the refraction range with one
    plain indexed indirect draw that tests the main depth. The epoch then sets the engine's begun and drawn flags (`0x1435ef130`,
    `0x1435ef131`). Main::Draw's own refraction point (`Main_Draw_RefractionCheck`) draws the remaining engine refraction on top
    without clearing; when DCLF drew the normals that frame, the hook clears group 6 first.
-   **Classification.** A refraction property (not a decal, layer or Terrain Blending's terrain; material alpha above 0) gets
    `refraction` and `forward` (blend 0, depth tested, not written), `kRasterRefraction` and its falloff and clamp bits; membership
    pass hint 4; view mask `kViewRefraction`. A refraction object is also dropped under a fade root FadeStateCS found stopped.
-   **Record and range.** `DCLFRecordFlags` bits 2-7 (`kRecordRefraction` and its falloff, clamp, no-normals, vertex-colour and
    two-sided variants, `RefractionRecordFlagsOf`); the shading row holds the property's `+0x104` in `MaterialData.x` and
    `refractionPower` in `MaterialData.w`. The range follows the forward range's halves (`MainLayout::refractionCapacity`).
-   **Pipeline** (`DrawPipelines::RequestRefraction`, `BuildRefraction`): one pipeline from the Utility technique `0x20A` (`TEXTURE
    NORMALS RENDER_NORMAL`, pulled), the shadow layout and plain draw signature; one attachment in the normals' format with the
    engine's write mask for write mode 2; depth LESS_EQUAL without writes; no culling. A refraction object joins the main phase only
    once the catalog its lookups resolved against has it (`MainReady`).
-   **Stages** (`package/Shaders/Utility.hlsl`, `DCLF_REFRACTION`): the vertex stage reads the pipeline row's vertex layout, the
    material row's `TexcoordOffset` and t0 (the refraction normal map), skins with the record's palette, computes the refraction LOD
    fade from the frame's `LodFadeFrame` and emits nothing past its end; `RefractionPower` is folded into `TexCoord0.w`. The pixel
    stage is the engine's; it discards a one-sided object's back faces.

### The Effect shader in the forward epoch

Features/Effects ports `BSEffectShaderProperty` on Core's contracts (`CS_DCLF_EFFECTS`, needs forward): the material port
(`EffectMaterial.cpp`, `EffectProperty.cpp`, `kMaterialKindEffect`), descriptors and variants (`EffectDescriptors.cpp`,
`DeriveEffectDescriptors`, `EffectVariantsOf`), the program family (`EffectPrograms.cpp`: `Effect.hlsl` on Core's Effect key slot),
the frame rows (`EffectFrame.cpp`) and the controllers' write hooks (`EffectWrites.cpp`). Core keeps the verdicts (`EffectOpaque`,
`EffectVariant`), the key bit (`kRasterEffect`), the particle item class and the billboard capture.

-   **What DCLF draws**: blended static meshes (hint 1) in the forward sort; decals (hints 2/3) in the decal groups (`DEFERRED` pixel
    stage); skinned effects through the palettes; opaque effects (below); the Lighting variant (`kEffectLighting`); weapon blood.
-   **Still variants** (`EffectVariantBit`, `Ineligible::EffectVariant`), the engine's: a shape that is not a `BSTriShape` or
    particle system, a skin mismatch, a decal that registers no pass, sky objects (bit 45), wireframe (bit 48), cloud lighting
    (`kEffectLighting` with `kCloudLOD`), Lighting without Light Limit Fix, projected UV, an unrecognised blend pair, and the
    particle causes.
-   **Membership.** The synthetic pass is derived from the property every time (`PrimaryCull::MembershipPass`); `PassCapture` keeps no
    effect registration.
-   **Mirror.** An effect property's record holds its `emittanceColor` and whether its material's two texture paths are set
    (`kEffectHasSourcePath`, `kEffectHasGreyscalePath`).
-   **Material port.** SetupMaterial's non-membrane half and SetupTechnique's filter modes as a pure function of the material snapshot,
    in the effect shader's variable space (`Features/Effects/EffectConstants.h`). `LightingInfluence.y` holds `softFalloffDepth`. The
    port declares `FalloffData` and `BaseColor` as frame variables, so the controllers' per-frame writes to them move no record
    ([Texture transforms and frame variables](#texture-transforms-and-frame-variables)).
-   **Bindless reads** (`package/Shaders/Effect.hlsl` under `DCLF_BINDLESS`): World/PreviousWorld from placement rows; fog from
    `DCLFFrameFog`; PropertyColor from the shading row (alpha = material alpha times the object's fade, `DCLFAlphaFade`);
    `CameraDataEffect` from the frame lighting block; `DLightColor` (`SampleEffectLight`, the sun light read as SetupGeometry does, on
    the render thread at each main commit); the blood globals (`SampleEffectBlood`); the soft depth from t118; `EffectRadius` = the
    placement bound radius; External Emittance's suppress bit per object (`kRecordEffectSuppressEmittance`).
-   **Pipelines.** An effect key has no Z-prepass pipeline (no Z bucket); its constant tables are the effect variables'. A forward
    key's pair is not held to its depth variant at all, and a decal key's depth variant is drawn only by the decal depth pass with
    the colour frame record.
-   **The Lighting variant** is ported unless `kCloudLOD` (light 0 is the cloud light) or Light Limit Fix is not loaded (point lights
    would be the pass's own). Point lights are Light Limit Fix's clusters; a register the capture lacks leaves the pair not drawable
    (the engine draws it), never drawn wrong.
-   **Effect decals**: decal group 1 (hint 2) or 2 (hint 3), blend mode from the pair, write mode 11 (10 for group 1 with
    MotionVectorsNormals), `kObjectDecal`. The member decal's chain key (`decalKey`) is ordered by the pass enum, so a group's
    effect decals precede its Lighting ones, as the engine draws them.
-   **Skinned effects** read `DCLFPalettes` at the record's bone offsets (`GetBoneTransformMatrixBindless`). A skinned blended effect
    is a forward object; a skinned decal a decal.
-   **Weapon blood**: the descriptor's Blood bit; the record's alpha test reference = threshold / 255 (`AlphaTestRefOf`); the two
    globals `BSTempEffectWeaponBlood` writes each update are a frame value.

#### Opaque effects

A hint 0 effect (no blending alpha property, no Falloff, depth tested and written, not a particle system) draws through decal group
1 with `opaqueEffect`: Deferred, blend mode 0, write mode 1, no depth bias, membership hint 0 (`DeriveEffectDescriptors`;
`CS_DCLF_OPAQUE_EFFECTS`, needs decals). It is an occludee culled once in the colour segment, and the decal depth pass writes its
depth before the colour pass. A property alpha below one would make the engine's pass blended; DCLF keeps it opaque.

### Particles

Effect-shaded `NiParticleSystem`s are drawn in the forward sort (Features/Particles: `ParticleSnapshots.*`, `ParticleRows.cpp`;
`CS_DCLF_PARTICLES`, needs effects).
-   **Taken** (`ParticleRejectionOf`): exactly an `NiParticleSystem` with particle data, a blending alpha property, no decal flags and
    none of the unported effect variants; aligned systems only with `NiPSysData` velocities; local and world space alike.
    Rejections are census causes (`particle: toggle off | class | no data | aligned | unblended | decal`).
-   **Writer side.** Thunks after the update slots `0x2C`/`0x2D` copy what the quads need (positions, size, colour, rotation,
    subtexture rect, velocity and aspect parameters; system world, world bound, model bound centre, fade distances) into the system's
    triple-buffered record, exchanged into `latest`. Only followed records are written (one lookup in an immutable table the producer
    republishes). Writers count themselves in; replaced tables and pruned records are freed only once that count was seen at zero.
    No lock, no wait, no engine reference held. A capture copies nothing when the system's `lastUpdate` and world are those of its
    last capture (the engine's two update passes a frame). A new snapshot pushes the system's move key.
-   **Reader side.** FrameValues' producer expands the newest snapshot faced to the frame's main camera exactly as `FUN_140e51320`
    (plain, rotated and aligned paths, the engine's quicksort, distance fade, colour packing) into the member's block of the frame's
    palettes: five rows a particle (four corners, subtexture rect). The placement row takes the snapshot's world and world bound.
-   **Geometry slot**: the renderer's static quad list and texture-coordinate stream, one slot per capacity bucket
    (`kSlotVertexDesc`: the texcoord stream alone; NiParticles' own puts position on stream 1, which DCLF reserves for face streams).
-   **Draw**: `Effect.hlsl` `PARTICLES` under `DCLF_PARTICLE_PULL`: no vertex input but `SV_VertexID`, corners pulled
    (`DCLFParticleInput`). A forward object culled with the snapshot's bound and sorted by its centre.
-   First-person particle members are expanded faced to the first-person eye.

### Billboards and aligned particles

Effect meshes under one billboard node are turned per draw on the GPU (Features/Billboards; `Core/Gpu/PlacementRows/BillboardRow.h`;
`CS_DCLF_BILLBOARDS`, needs effects; `CS_DCLF_BILLBOARD_PARTICLES` for particle systems under billboard nodes).
-   **Classification**: an effect `BSTriShape` without a skin with exactly one billboard node between it and its category node,
    first-person entries included. The face mode is read at AE's `+0x128` (`PlacementRows::BillboardFaceMode`), not CommonLib's
    `NiBillboardNode::userFlags` (the wrong offset in this build). The key holds `mode + 1` in raster bits 21-23
    (`kRasterEffectBillboardShift`); the record gets `kRecordEffectBillboard` with the mode.
-   **Row** (`PlacementRows::SampleBillboardRow`): World is the leaf's unturned world (node base = parent world x local, times locals
    down to the leaf); PreviousWorld's rotation is R0 and `[0].w` the leaf's world bound radius; Bound is the pivot with a radius
    covering every orientation. It reads nothing a cull writes.
-   **Turned on the GPU from the drawing view's camera** (`DCLFEffectWorld`, `DCLFBillboardWorld`, `DCLFBillboardRotation` in
    `DCLFObjects.hlsli`): camera axes from VS_PerFrame c0-c2, eye c40; a port of `0x140d3e650` (all six modes). One row, one
    record, every view; no per-view CPU work. PreviousWorld gets the same turn. Measured: 0 of about 11,000 leaves differing from the
    engine's turn (largest rotation 3e-7).
-   **A particle system under a billboard** needs no turn: the engine draws the world and particles of its last update, which the
    snapshot copies. A billboard DCLF draws whole is no longer turned by any cull, so such a system keeps the turn its last update
    left.

### Decals and pass layers

Decals (accumulation hints 2 and 3) and multi-index shapes' layers (hint 12) are drawn in DCLF's decal groups after the opaque draws,
each group in the engine's order (Features/Decals; `CS_DCLF_DECALS`, `CS_DCLF_LAYERS`).
-   **Occludees only.** A decal never occludes anything its host does not: never in the Z-prepass or the two-phase replay, culled once
    in the colour segment against the rebuilt HZB (`kViewDecal` alone).
-   **Groups** (`kDecalGroups` = 3: opaque, blended, layers). A decal's sequence goes to a fixed slot by its ordinal
    (`SceneTables::rangeOrdinal`), so overlapping decals keep the engine's order without flicker; a culled or unbuildable decal
    writes a zero-count draw into its slot. Each group's draw reads its own count word. Capacities grow (`decalCapacity`). A
    multi-partition decal (skinned blood decals) takes a slot per partition (`DecalSpanOf`).
-   **Decal order** (`Features/Decals/DecalOrder.cpp`, `OrderDecals`, a coordinator component). The engine draws a decal chain in
    reverse registration order; DCLF reconstructs that order from the mirror's parent chains: an ordered (`BSOrderedNode`) group, then
    the position from the scene's top, then the child path, with a `BGSDecalNode`'s decals by reverse index. It re-sorts only when the
    member decals change. Not the engine's per-frame order, because that order is unstable: `BuildSceneLists` deals roots round robin
    and skips hidden ones, so showing an earlier root reorders the decal roots after it (14-24% of frames while turning).
-   **The colour passes** (`Features/MainView/MainViewPasses.cpp`): the decal depth pass (opaque decals' depth with bias, LESS_EQUAL,
    written, alpha test in the depth variant); then the main draws and draw families' colour; then the second pass: opaque decals and
    layers, Terrain Blending's held terrain and no-blend meshes, then blended decals (the `Api/MainRanges.h` order). Why the decal
    depth pass: every host fragment under an opaque decal texel then fails the colour pass's EQUAL test before shading.
-   **State** comes from the engine's own state objects (`CaptureEngineStates`), bias from `DecalDepthBiasMode` (interiors get the
    no-bias modes). Additive blended decals take AdditiveLighting from the alpha property, by CS's rule.
-   **Pass layers.** A `BSMultiIndexTriShape` with an additional property has two main passes, and the main camera's claims are by
    geometry, so DCLF takes both or neither. The second pass is a pass layer: an object of its own held by the base's entry
    (`Tracked::layerSlot`, `Tables::layerBase` / `layerOf`), classified by `LayerPropertyOf`, written by `WriteLayer` with its own
    geometry slot for the second index list (`ResolveLayerGeometrySlot`) and no shadow (`ShadowReject::Layer`). A base and its layer
    are members together. A shape whose layer DCLF cannot draw stays `Ineligible::MultiIndex`: shadow-only, both main passes the
    engine's. The layer draws in the layers group, LESS_EQUAL, writing depth from its colour draw.

### Skinned objects

Any `NiSkinInstance`'s geometry - creatures, effects, LOD trees, the first-person arms - is DCLF's under Features/Skins
(`CS_DCLF_SKINNED`, `CS_DCLF_SKIN_PARTITIONS`); Core keeps the path.
-   **DCLF runs no skeleton.** The engine's `NiSkinInstance::boneMatrices` palette (three float4 rows a bone, world space, refreshed
    once a frame by `FUN_140e4ff90`, the previous palette beside it) is sampled by FrameValues into the frame's palettes (t122,
    `Core/Engine/SkinPalettes.h`), eye-relative. The record carries `boneOffset` / `previousBoneOffset` / `boneRows`
    (`kObjectPaletteBlock`).
-   A skinned draw binds the partition's own TriShape (`partition->buffData`), not `geometry->rendererData`.
-   Partitions by LOD level: [Skin LOD partitions](#skin-lod-partitions).

### Trees and actors

Trees and actors are the skinned path with their classes' rules: `CS_DCLF_TREES`, `CS_DCLF_SWITCH_NODES`, `CS_DCLF_SKINNED`,
`CS_DCLF_SKIN_PARTITIONS` and `CS_DCLF_ACTORS`. Every tree hangs under an `NiSwitchNode` (`BSTreeNode` > `BSMultiBoundNode` >
`NiSwitchNode` > shapes; [Switch nodes](#switch-nodes)), and most trees are skins of two or three partitions, one per LOD level.

#### Tree wind on the GPU

Technique 12 (TreeAnim) is ported by `Features/TreeWind/TreeWindObjects.cpp` (`kObjectTreeAnim`). The engine's tree manager
(`FUN_1404381e0`) advances each `BSTreeNode`'s clock and takes its gust from it; `SetupGeometry` case 0xc turns the node into
TreeParams and WindTimers. DCLF's members draw with what one pass makes of the same inputs, so nothing reads the tree nodes per
frame.
-   **Rows.** Each tree node a member draws under has a `TreeStatic` row (48 bytes: position, leaf frequency, model amplitude, its
    clock at listing, generation, animated, seedOdd), written by the feature's coordinator component as members join and leave
    (`TreeWindTables`) and seeded from the node at the frame start (`TreeWindSeeds`, a FrameValues pool).
-   **The pass** (`TreeWindCS.hlsl`, `tree-wind.next-frame`, `cs.dclf.z.tree-wind`) runs on the compute queue in the Z-prepass epoch,
    one thread per entry: entry 0 the nodeless trees', entry s + 1 tree slot s. It keeps a GPU-only `TreeClock` per slot, reseeded on
    a new generation and otherwise advanced once a frame, and writes three rows an entry (TreeParams, WindTimers, the listing's
    generation) into one of two wind buffers by frame parity. Every draw reads the other buffer, the frame before's, at t124
    (`DCLFTreeWind`), so the Z-prepass and the colour pass read the same values, as their EQUAL test needs, and nothing waits for the
    pass. The frame's values come from `TreeWindFrameRow`, a buffer, because the pass is prepared ahead of the commit that knows them.
-   **A record carries its tree slot and that listing's generation** (the object row's feature block, `WindTimers.zw`) and draws the
    entry only when the generation matches; otherwise it uses what it joined with (the shading row's feature block). Records take no
    GPU writes.
-   **Per-object TreeParams and WindTimers are required**: under the per-pipeline PerGeometry block a template tree's wind would be
    handed to every tree on its pipeline.
-   **Which trees are gusted.** A tree is animated (its clock advances and its gust is scaled by model `+0xB0`) only when its model
    has bones (`+0xB8`) and the engine's near-list check selects it (`FUN_14147d640`, evaluated when the tree is listed). Never read
    model `+0xB0` for other nodes: the engine never reads it there, and it can hold garbage (2.29e36 turned the terrain black).
-   **The distance** uses the engine's `0x5f3759df` fast inverse square root with one Newton step and its arithmetic shift, bit for
    bit (`FastSqrt` in `TreeWindCS.hlsl`, `LightingDescriptors.cpp`); with `std::sqrt`, 0.7% of trees disagree. Never call the
    engine's `SetupGeometry` per tree: case 0xc mutates the node (`+0x168 = +0x164`).
-   The engine's tree manager is stood down for stood-in trees (below).

#### Owned trees off the tree manager's animation list

`Features/TreeWind/TreeAnimation.{h,cpp}`:
-   Roots DCLF stands in for are taken off `BSTreeManager`'s animation list (`+0x50`) while owned (`TreeAnimation::SetOwned`,
    `UpdateOwned`, from joined/left deltas), under the manager's lock (`+0x18`). The engine's add (`FUN_1404376f0`), remove
    (`FUN_140437840`) and clear (`FUN_140437ae0`) are detoured, so the list's membership stays exact.
-   **The tree list loop** (`Features/TreeWind/TreeList.*`, `TreeAnimation::BeforeListLoop` / `AfterListLoop`): stood-in `BSTreeNode`s
    stay on the tree list (`+0x20`). Around `FUN_140437e50`'s call (`0x140437df1`, always detoured), under the tree list's lock, each
    one's multibound shape's `+0x10` is written as 1 or 2 from `IndirectDraws::AccumulatedVerdict`; the engine's values are
    remembered and the trees held by reference, and after the call the values are put back. Why put them back:
    `BSMultiBoundNode::OnVisible` reuses the cached result for cull modes 0 and 2 while `lastAccumTime` matches.
-   Not taking the trees off the tree list, nor porting the loop, because DCLF follows the loop's stores for these trees: the LOD
    switch (`+0x180`) and the near list's bones (`FUN_140438ed0`), which DCLF's skinned draws read. `CS_DCLF_TREE_LIST_ALL` (off)
    extends the verdict to every `BSTreeNode` DCLF lists.
-   Measured at Riverwood: the manager's update from 0.13-0.24 to 0.05-0.07 ms (2,173 of 2,562 roots taken off).

#### Actors

Bodies, armour, heads and the actor techniques (FacegenRGBTint, Facegen, Hair, Eye) are DCLF's under `CS_DCLF_ACTORS`
(`Features/Actors/Actors.h`). Whatever the toggle, the feature also owns:
-   the actor-owned entry class (`Api::EntryClass::ActorOwned`, resolved once per tracked geometry: `Tracked::owner`,
    `Extensions::OwnerClassOf`), per frame, its own motion;
-   the actors' engine events: `Actor::SetAlpha`'s fadeAmount writes, and the move writers (`Actor::UpdateActor3DPosition`, the sky
    cell's skin job, the ragdoll job) on reserved move-writer slots;
-   the actors' animation stamp from DCLF's shadow views ([Room visibility, the tree list, the actor stamp](#room-visibility-the-tree-list-the-actor-stamp)).

Advanced Skin's `SkinPerGeometry` (b7) is the owning actor's wetness, captured on the render thread and fanned into the shading rows
(`Integrations/AdvancedSkin`).

#### NPC faces: positions published by the engine's own writer

NPC face shapes (a `BSDynamicTriShape` under a `BSFaceGenNiNode`: head, mouth, hair, brows) are DCLF's in every view, and DCLF never
reads their positions from the shape (Features/Faces).
-   **Snapshots.** A thunk on each head's morph call (`0x1404334f3`) copies the head's face shapes on the job's thread; a thunk after
    the stage's `JobList::Finish` (`0x1406d36fd`) captures heads that want a first snapshot. Each head has a lock-free triple buffer,
    so every shape of a head is read from one job run. DCLF never reads `dynamicData` or takes its lock.
-   **An entry class, not a verdict** (`Api::EntryClass::FaceShape`): classified like any shape, every frame, its record given the
    positions stream row (`SceneTables::streamRows`). Face shapes follow their heads' publications (`ApplyFacePublications`).
-   **One buffer.** One face positions buffer (`SceneBuffers::facePositions`, `cs.dclf.face-positions`) serves every epoch, through
    every `DrawSequence`'s second vertex-buffer view (slot 1).
-   **A face shape is never drawn without its positions.** Without a snapshot or region it gets no record and the engine keeps it; an
    input whose layout reads its position from stream 1 is drawn only with a stream row (otherwise UVs and weights are drawn as
    positions).
-   **NPC face parts in the main pass** take hints 0, 3, 7 and 11; the claims cover the direct group insertions those make.

### Terrain and ProjectedUV

-   **Landscape** (Features/Landscape, `CS_DCLF_MTLAND`): the terrain's MTLand and MTLandLODBlend techniques (8, 19). Core keeps the
    MTLand material handling, the landscape lights (`kObjectLandscapeLights`) and the land-blend extras (`kObjectLandBlend`). TruePBR
    landscape layers' displacement (t80-t85) and RMAOS (t86-t91) are material feature textures.
-   **Under Terrain Blending** (`Integrations/TerrainBlending`, `CS_DCLF_TB_TERRAIN`, needs `CS_DCLF_MTLAND`), DCLF draws the terrain
    as that feature does. Terrain Blending draws near terrain blocks twice into depth (its terrain depth, and the main depth with
    Utility's `OFFSET_DEPTH`), then `BlendPrepassDepths`; in colour it holds the main pass's terrain and replays it after the opaque
    groups and before the blended decals (`LESS_EQUAL` with writes, its terrain depth as t55), then the
    `kNoTransparencyMultiSample` meshes (`EQUAL`).
    -   DCLF classifies terrain group 1 (`kMultiTextureLandscape`) or 2 (`kNoTransparencyMultiSample`) in object flag bits 20-21 and
        raster key bits 24-25. A group 1 Z bucket has a twin (`kBucketTerrainPair`): the near test is per draw on the GPU against
        the latch's eye and distance (`terrainNear`), the twin drawn into the main depth with `kZDrawPushOffsetDepth` (a push word the
        pulled vertex stage adds to clip z) and again into Terrain Blending's terrain depth (`Resources::terrainDepth`), which
        `BlendPrepassDepths` reads after the epoch.
    -   In colour each group appends to its own range (`MainLayout::terrainCapacity`), drawn after the opaque decals and layers,
        before the blended decals. t55 is named in the frame record.
    -   Terrain Blending itself is unchanged: DCLF withholds the members' passes at registration, so its replay draws only what is
        left native. Its write mode 1 leaking into the blended decals after its hook is accepted (RT0 alpha only, never read); DCLF
        derives write mode 11.
-   **ProjectedUV** (`CS_DCLF_PROJECTED_UV`): TextureProj and ProjectedUVParams are extras rows, completed in the shader from the
    placement row.

### LOD

Object and terrain LOD are Features/Lod (`Api::ObjectClass::LodObjects`, `LodTerrain`; `CS_DCLF_LOD_OBJECTS`, `CS_DCLF_LOD_TERRAIN`);
tree LOD is Features/TreeLod (`Api::ObjectClass::TreeLod`, draw family `Api::DrawFamily::TreeInstances`; `CS_DCLF_LOD_TREES`). The
terrain manager's LOD blocks hang under `TES::lodLandRoot`, a category root while either Features/Lod class is on; a class whose
toggle is off classifies as `Ineligible::Lod`. Classification keys on the Lighting property's LOD flags (`IsLodObject`, `IsLodLand`),
never on names: LOD generators vary (3D tree LOD is object LOD with a tree atlas, grass LOD alpha-tested object LOD).

-   **PreviousWorld** for every LOD technique (9, 13, 15, 18) is the current transform with the previous camera, as the engine draws it
    (`DrawnPreviousWorld`).
-   **Culling.** Object and terrain LOD are records of the DCLF set, tested by BuildDrawsCS in both phases with their record's bound.
    An object LOD shape is tested whole (its block's bound), not per visible range.
-   The main camera still culls `lodLandRoot` and the tree LOD root and runs segment `OnVisible`; DCLF withholds the passes.

#### The census

What each LOD class registers, which the design rests on:
-   **No LOD class registers into any shadow mode**: object and terrain LOD never cast (`ShadowReject::Lod`, `CastsNoShadow`).
-   **The water reflection's faces draw** tree LOD, object LOD (the LOD object techniques), HD object LOD, terrain LOD (LODLandNoise)
    and the sky: the technique set `LodLightingTechnique` (9, 13, 15, 18) names for the reflection's membership.
-   Terrain LOD is no occluder of the occlusion maps: `Precipitation::SetupMask` culls the scene lists, which hold no LOD root.

#### Object LOD

A `BSSubIndexTriShape` with the LOD object flags, drawn with LODObjects (21) and LODObjectHD (23) and CS's `LODOBJECTS` /
`LODOBJECTSHD` permutations, write mode 1, alpha-tested or not, as the engine (write mode 10 wrote the shape's alpha into the G-buffer
alphas). The engine's passes into the main camera's views are withheld at registration. Measured: capture parity 0 mismatched of
5,935 LODObjects and 3,708 LODObjectHD draws.

#### Object LOD: segments

Segment writes are events (`Features/Lod/LodSegments.h`, `LodSegmentEvents.h`, `LodSegmentHooks.cpp`, `LodSegmentChannel.cpp`): the
nine call sites of the three segment writers (show, hide, show all) are patched (`InstallSegmentHooks`); each makes the write and
pushes the shape onto the feature's engine channel (`Api::EngineChannel`), drained after the switch events. The component takes the
shape's drawn ranges again (`DrawnRanges`: the runs when clean, the enabled segments when dirty, the whole shape when not segmented)
into `SetEntryRanges`, and a change re-queues the shape in the same frame's walk.

A ranged geometry type (`Api::GeometryTypeContribution::ranges`) keeps its drawn ranges in the coordinator (`entryRanges`): whole is
the TriShape's own slot; none is `kNoPartitions` (a member that draws nothing); otherwise a chain of range slots
(`kPartitionChain | count`, linked by `nextPartition`, each with its own `firstIndex`), resolved again at each ranges event
(`Coordinator::EvaluateRecordInputs`). BuildDrawsCS, `ForEachDrawnGeometry` and capture parity walk the chain with no cap.

#### Terrain LOD

A land block is a `chunk` node with one `Land` `BSTriShape` under `TES::lodLandRoot` (`IsLodLand`: `kLODLandscape`), drawn with
LODLand and LODLandNoise (18), write mode 1; it never casts.
-   The engine registers its main pass by hint 6 straight into geometry group 0 (`FUN_1414f5090`, not `RegisterPass`) and draws group 0
    last through the group draw (`RenderPassImmediately`, `Engine::Address::kGeometryGroupDraw`). DCLF thunks that call
    (`BSBatchRenderer_RenderPassImmediately<3>`, `src/Features/DrawcallLimitFix.cpp`) only to count a native draw of a member; the
    members' passes are withheld at registration.
-   **`HighDetailRange`** (VS PerTechnique): the loaded grid's centre, absolute (DCLF's vertex shader takes the draw's eye off it), and
    half extents less 15 (`ConstantEvaluator.h`, `LodHighDetailRange`). It moves vertices, so the Z-prepass and colour pass draw one
    frame's value: sampled once at the frame's start (`FrameGlobals::technique`), written into the LOD land's technique rows when it
    moves (`RefreshTechniqueRows`), read by both passes from one publication (each pass reading the globals failed EQUAL on about
    600,000 pixels in one frame).
-   BasicRHI has `B8G8R8X8` formats (Vulkan: B8G8R8A8 with alpha swizzled to one) for BGRX terrain diffuse maps.

#### Tree LOD: what the engine does

Each `BGSDistantTreeBlock`'s tree groups attach one `BSMultiStreamInstanceTriShape` per group. The refill packs at most 75 instances
into 32-byte records (position, scale - or 0 when hidden and `allVisible` is clear - cos and sin of rotZ, alpha, 1.0, zeros).
`AddGroup` and `RemoveGroup` are vfuncs `0x1E0` and `0x1E8`, run on the terrain manager's threads; `OnVisible` tests each group's
AABB. The engine's draws use the worldspace's atlas, sampler address mode 0 and filter 2, and alpha reference 128/255.

#### Tree instance hiding

An instance's hidden byte's bit 0 is hidden (bits 1 and 2 are the reference cache; Engine Fixes' `bTreeLodReferenceCaching` writes it
as a bool); the block's `allVisible` ignores it. `UpdateBlockVisibility` sets the instance alpha to `1 - currentFade` of the full
tree's fade node under `bEnableStippleFade`, so LOD cross-fades with the full tree by the main cull's fade, which the fade write-back
keeps on the node. One hidden without a visible full tree leaves a hole.

#### Tree LOD: the draws

-   **The mirror** (`Features/TreeLod/TreeLodMirror.{h,cpp}`, `TreeLodHooks.cpp`; changes nothing the engine does): the refill's two
    calls (in the block attach `FUN_140503630` and its visibility update `FUN_140503f70`) name the block and group per thread;
    `AddGroup` inside a refill pushes the exact 32-byte records, and `RemoveGroup` on a `BSDistantTreeShaderProperty` shape pushes a
    removal; the block update's call in the node update (`FUN_140510730`) repacks every group by the rule with the block's current
    `allVisible` (`PackGroup`) and pushes it as an intended event, applied only where it changes the records. Events go through a
    lock-free queue; `Drain` applies them in push order on the render thread.
-   **Tables** (`TreeLodTables.h`, layout `TreeInstancesLayout.h`): versioned shape rows (translation, mesh slot, record count, type
    extent), 75 instance records per shape slot, mesh rows (vertex and index device addresses leased through `GpuResources`, stride,
    index count, UV offset), a draw row, and the visible list. They grow by requirements posted with the revision's inputs; the depth
    commit uploads only changed slots and meshes (`UploadTreeLod`).
-   **Cull** (`TreeLodCullCS.hlsl`): phase 1 one thread per record (skipped past the count or at scale 0; frustum by the sphere holding
    the instance's padded box; then the last frame's HZB); occluded records go on the retest list; phase 2 retests them against the
    rebuilt HZB. `TreeLod::VisibleHeader` holds three draws' arguments (phase 1 depth, phase 2 depth, colour counting both).
-   **Draws**: one non-indexed instanced draw per main pass, pulled under the Z-prepass's layout (push data: the draw row's address
    and the phase word); `package/Shaders/DistantTree.hlsl` under `DCLF_PULLED` keeps the engine's instance math `precise`. Z-prepass:
    the depth technique (a 4x4 stipple of the instance alpha, then the alpha test), LESS with writes; colour: the deferred technique,
    EQUAL, write mode 1; both two-sided. Atlas, sampler and alpha reference come from the draw row (`TreeLodTextureBinding`).
    Programs: `ShaderPrograms::FindDrawFamily` (descriptors `0x10100` deferred and `0x10001` depth), built by
    `DrawPipelines::RequestFamily` from `Api::DrawFamilyProgram`.
-   **Ownership**: the `TreeInstances` family's decision (`IndirectDraws::DecideDrawFamilies`) at the frame's start, before the main
    camera's cull: the toggle, programs, pipelines, tables and atlas (`TreeLodAtlasReady`). On it, `PassCapture::WithholdMain`
    withholds every tree LOD pass into the main camera's views; otherwise the draw row names no slot and both passes draw nothing.
    DCLF draws every instance the mirror holds this frame.
-   Measured: mirror parity 0 missing, stale or differing with up to 597 shapes and 10,139 instances; 6-25% of instances in view
    occluded.

#### LOD gates: the scene side

The engine's object and terrain LOD swaps (Upgrade, Downgrade, InitialLoad `Func2`s) are held until DCLF's claims cover the incoming
level (`Features/Lod/LodGates.h`, `LodGateComponent.{h,cpp}`, `LodGateAdoption.cpp`; `CS_DCLF_LOD_GATES`). Otherwise a swap shows
the incoming blocks for the frames before DCLF claims them, and the engine draws them meanwhile.
-   **The engine side** hides a swap's incoming blocks (app-cull) before their attach and keeps them hidden by vetoing the engine's
    show stores (the hidden-store stubs' `Api::ShowVeto`); outgoing blocks are stolen from their quadtree nodes inside
    `FUN_140510a10`, so they stay attached and shown. `LodGates::CaptureGate` is a stacked event on the gates' `SceneStack` channel,
    ordered after the incoming attaches, pinning the incoming roots.
-   **The scene side.** The mirror marks each incoming root gated (`SceneMirror::SetGated`), and `HiddenForWalk` ignores a gated node's
    own hidden bit, so the incoming are classified, bound and joined while hidden. An incoming geometry carries `kVetoObserve` (its
    phases recorded, none applied); the outgoing get `kVetoHold` after the flip. A gate flips when no incoming geometry is pending,
    none is unmirrored, and it has an incoming root. The flip (`GateFlip`) is a component delta on the publication log. A ready gate
    whose incoming records were written since the last publication waits one commit (structurally changed slots are unclaimed in that
    publication).
-   **Adoption.** The frame start hands the installed publications' flips to the component (`Extensions::ComponentsAdopt`), which calls
    `LodGates::Flip` (incoming shown, outgoing hidden) in the frame whose claims are post-swap; outcomes (`LodGates::DrainOutcomes`)
    retire gates, and the next LOD drain releases the outgoing through the engine's own functions. A reattached held block posts a
    `kept` notice (`ApplyGateKept`). Initial loads outside a load screen are gated like swaps.
-   Lock-free: one atomic per gate, compare-exchange between the swapping thread and `Flip`. Forced releases keep the claims.

## Views

Core's bucketed view machinery and the feature contracts it is taken through are [dclf-layout.md](./dclf-layout.md), "The view
contract" and "The shadow-view contract".

### Shadows

#### The shadow views

Shadow views and lights are DCLF's: nothing reads a cull or the renderer's state at a view's draw, and the engine's values feed only
the parities.
-   **Sun views** (`Features/Shadows/Sun/SunViews`): read at `BSShadowDirectionalLight::UpdateCamera`'s return, each cascade's camera
    and caster volume and the full-frustum camera. DCLF derives view, projection, eye, viewport and each culling process's planes
    (`SunViews::CullPlanes`: the cascade process's `customCullPlanes` when `doCustomCullPlanes`), which feed the shadow epoch,
    `SunAccumulation` and the entry rule.
-   **Local light views** (`Features/Shadows/Local/LightViews`): the same arithmetic per descriptor. A point light's
    `BSParabolicCullingProcess` culls without planes.
-   **Light selection** (`Features/LocalLights/LightSelection`) ports `CalculateActiveShadowCasterLights` (`0x1414cc570`): it walks
    `activeShadowLights` in order and keeps up to 4 lights (the sun counts when it draws), with DCLF's visibility verdict in the
    engine's operation order and DCLF's room test ([PortalViews::WalkLight](#portalviewswalklight)). It assigns slots and mask
    indices from the 64-bit counter at `0x14338c904`.
-   **Focus shadows** (`Features/Shadows/Local/FocusViews`): from the cameras `BSShadowDirectionalLight::sub` has just aimed at the
    focus targets, for whichever light hosts them. The casters are the members whose fade root is the target's
    (`BuildDrawsLatch::focusRoot`). A view is drawn over the whole slice with the sub-rectangle's viewport transform folded into the
    matrices (`FocusToSlice`).
-   **When the list is decided**: inside `CalculateAndDrawShadowCasterLights` (`Features/Shadows/Engine/ShadowCasterPoints.cpp`), at
    three points: `FrameDriver::BeforeShadowCasters` begins the views and decides coverage (`IndirectDraws::DecideShadowCoverage`),
    `ShadowCasterKept` runs at each kept light's slot write (`ShadowViews::AddCaster`), and `ShadowCastersSelected` lists them
    (`ShadowViews::Rebuild`). So withholding tests this frame's coverage.
-   **Targets and slices** are predicted at Rebuild (`ShadowViews::PredictTargets`). `RenderShadowmap` binds the target only for a
    descriptor that clears, so a point light's hemisphere 1 draws into hemisphere 0's slice; focus views go to their own slices.
-   **Capture.** Each view's `FinishAccumulatingPreResolveDepth` (`0x2A`) captures it after the engine's own setup: target and slice,
    viewport (views can be sub-rectangles of a slice), `posAdjust`, PerTechnique and `VS_PerFrame` (`Core/Render/Shadow/ShadowCapture.cpp`).
    The shadow epoch then draws every captured view.
-   **Slots.** Skylighting's map draws through view slot 0 and the frame's shadow views through the slots after it. A slot is a
    sequence buffer, a count buffer and a row of the view blocks (`ShadowResources::viewBlocks`). More slots are more buffers the
    passes declare, so the render thread makes them (`ShadowSlotsRequest`) and builds the graph with them. Either growth is a new
    shadow latch layout. A view is never left native for want of a slot.
-   **Still the engine's**: the scissor rectangle, `UpdateCamera` and its focus counterpart themselves, and a view whose rasterizer
    state no pipeline can express (wireframe, no depth clip).

#### The caster rule

`ShadowCasterReject` / `ShadowReject` (`Core/Handoff/ShadowViews.h`): `None`, `NotLighting`, `DeclZero`, `Faded`, `Refraction`,
`AlphaBlended`, `DecalNoZWrite`, `NoCastShadows`, `VolumetricOnly`, `DecalPointLight`, `Layer` and `Lod`.
-   `DecalNoZWrite` rejects every decal outside the registration's one exception (`kZBufferWrite`, bit 18 and blended). The sun's and
    spot lights' accumulators have `drawDecals` = 0. A paraboloid light keeps it 1, so by the code it would cast a decal without bit
    18, but it never does (every insertion watched): such a decal is `DecalPointLight`, no caster of any mode, and the point lights'
    observer reports one if a scene ever registers it.
-   **The volumetric lighting copy is DCLF's.** `kCastShadows` clear while the shadow global is 2 makes a volumetric-only caster
    (`kObjectVolumetricOnly`), drawn only into the sun's volumetric copy, never a cascade. One input list holds both classes: a copy
    view sets `kCullVolumetricOnly`, every other shadow view `kCullCastersOnly`, and `BuildDrawsCS` drops the other class first. A
    point light registers a volumetric-only caster like any other, so in the paraboloid mode it is an ordinary caster.
-   Properties no shadow view takes let their entry go (`CastsNoShadow`, `SunEntryAllows`, shared by sun and point lights): decals the
    rule rejects, refraction, alpha blending outside the decal exception, a Utility declaration of 0. A fade does not count, because
    it changes with no event; DCLF writes the masks such geometry needs.

#### Shadow draws

-   **One epoch draws every view** ([One epoch for every shadow view](#one-epoch-for-every-shadow-view)), with plain indirect draws
    (`ShadowViewPass`): one `DrawIndexedIndirectCount` per bucket. The buckets are the distinct pipelines a rasterizer state's map row
    names, depth-only class first (`BucketsOfRow`), sized from `ShadowPayload::keySlotDraws` (`SizeShadowBuckets`; sticky, grown to a
    power of two). BuildDrawsCS takes a slot from `ShadowResources::bucketCounts` and writes the sequence there
    (`StoreShadowSequence`). Utility's pulled vertex stage reads the sequence at the call's first sequence plus the instance. Not
    device-generated commands: they cost about 1 ms of the views' 2.25 ms; plain draws with the index pool brought the views to
    1.97 ms against native's 1.98 ms.
-   **Depth-only and discarding classes.** `FragmentDefersDepth` scans the pixel SPIR-V for `OpKill`, `OpTerminateInvocation`,
    `OpDemoteToHelperInvocation`, depth or sample-mask export, image writes and atomics (`kShadowDiscards` against
    `kShadowDepthOnly`). Utility compiles DCLF's depth-only techniques without an alpha test and with an empty pixel `main`. Never mix
    the two classes in one indirect execution set: the driver put the whole set on late Z (0 early-Z samples against the engine's
    51 M). Paraboloid techniques (`RENDER_SHADOWMAP_PB`) discard by design and stay late Z.
-   **The view's rasterizer state.** Each view draws with its own: depth bias, slope, clamp and cull, as the engine binds it. A view's
    state is its rasterizer table entry at cull mode 1, not the renderer's current one (the Utility shader sets the cull mode per pass,
    so the renderer's is whatever drew last); a view with no entry at 1 (the volumetric copy) takes its entry at 0. A two-sided caster
    draws without culling (`kRasterTwoSided`). CS's `ShadowmapCascadeRasterizerFix` swaps per-cascade states in around the cascade
    loop only. States are registered by `DrawPipelines::ShadowRasterStateId` from `EngineRasterStates()`; the id is
    `ShadowPipelineKey::viewState`, a mode's states are sorted lists (`ModeRasterStates`: casters and volumetric-only), and a shadow
    input names a key slot resolved through the view's state row (`BuildDrawsLatch::pipelineMapOffset`). Not a union of every raster
    state's buckets per view: it cost 0.2 ms of GPU a frame to save a 0.35 ms wait every several seconds.
-   **The view's depth range is a value.** The engine alternates a shadow viewport's max depth (1 against 0.99997); the view block
    carries it (`DCLFDepthRange`, PerTechnique c3) and the vertex stage applies `z' = min*w + z*(max-min)`, so the shape does not change
    with it.
-   **Cull volumes.** A sun cascade view culls against the engine's caster volume (the latch holds `customCullPlanes`). Clamped views
    (mode 0xE) cull with no near plane (`kCullNoNearPlane`), and `Utility.hlsl` pancakes such casters to z = 0
    (`RENDER_SHADOWMAP_CLAMPED`).
-   **Arena blocks** sit at fixed offsets from resources (`ShadowArenaBlocksOf`: zero block, SharedData, FeatureData), so the first
    revision's shapes match the first commit's.
-   **The sun's full-frustum processes** (the engine culls the scene once per `fullFrustumCullingProcessArray` entry before the
    cascades, and a cascade walks only entries some process kept) are written once per frame slot into a region of the shadow latch
    block (`ShadowLatchLayout::SunEntryOffset`, 112-byte `SunEntryProcess` records), named by every sun view's latch
    (`BuildDrawsLatch::sunEntryOffset`), any number of them. `BuildDrawsLatch` is 320 bytes, its payload block included.

#### The sun's cascades without DCLF's objects

-   **No sun registration for members** (`Features/Shadows/Sun/SunAccumulation.{h,cpp}`; `CS_DCLF_SUN_SKIP`). It hooks `Accumulate`
    (vtable slot 9) and `FUN_140e28af0`'s two calls of the registration. For a member, the registration runs only its early-outs and
    its mask write (`WriteMaskOnly`), and no pass is built only to be withheld.
-   **The entry exclusion** (`CS_DCLF_SUN_EXCLUDE`, needs the skip). Entries all of whose geometries DCLF draws, or whose geometries
    build no shadow pass anyway, are sun candidates (`Coordinator::UpdateSunCandidates`, `SunEntryAllows`; judged again only for
    entries a change touched). The exclusion is built with the shadow payload (`BuildSunExclusion`; `ExclusionBuild`,
    `Features/Shadows/Local/`; the contribution's types in `SunExclusion.h`, which Core's payload holds opaquely) and linked forward
    to later candidates (`ForwardExclusion`). It is applied as a veto while the full-frustum processes insert entries
    (`BSGeometryListCullingProcess::AppendNonAccum`, `0x140e30520`, vtable slot 0x19), armed for the light's processes only, not by
    compacting `objectArray` after the cull. After the cull, `SunAccumulation::ExcludeEntries` gives a process the veto left empty its
    first entry back, because the cascade cull sets its planes up from it. The engine culls everything on a frame whose exclusion is
    stale.
-   **An entry's sun entry** is its reference root for a static reference and its nearest `BSMultiBoundNode` for terrain; for an actor
    it is the cell's container, which the engine never tests (`Coordinator::ResolveSunEntry`, `Tracked::entryRoot`, once per tracked
    geometry). A faded object counts as an engine caster in `BuildSunExclusion`: an entry stays in the culls while anything under it
    fades, because the exclusion is used a frame after it is built.
-   **The whole skip** (`SunAccumulation::ArmWhole`). While DCLF draws every sun cascade with its casters in the frame's set and the
    frame is not a parity frame, the full-frustum cull runs over empty scene lists (each process keeping one empty node of DCLF's so
    its cascade culls set their planes up), and `Accumulate` keeps its bookkeeping but culls and registers nothing. A caster not yet a
    member casts no sun shadow until it joins; it is never handed to the engine's cull.
-   **The GPU applies the same entry rule** to DCLF's inputs (`kCullSunEntry`, `OutsideSunEntry`): a sun input carries its entry
    sphere (`SetSunEntryRow`), and a sun view drops an input whose sphere lies outside every full-frustum process. Spot lights share
    mode 0xE's inputs, so only the sun's views apply it.
-   **The main pass's sun bits.** The engine's main pass reads the sun's bits in `activeLightMask`; for the engine's own main passes
    DCLF writes them at each geometry's main registration (`ApplySunBits`): the cascades its bound meets, against the planes the
    engine's cascade cull used, once a frame until the geometry's first clearing registration (`0xFFFF`), tracked with a
    `FrameAddressSet`. The main registration reads the mask and then clears it, so every later registration reads 0. Owned members'
    masks stay 0 (`ClearOwnedMask`); a geometry the colour build starts drawing has its mask cleared once (`PrimaryCull::Admit`).
-   **Interiors.** When the full-frustum cull does not run (the sun off indoors, `0x14338c911`),
    `SunAccumulation::EndFullFrustumWindow` clears the cascades, the exclusion and the sun's bits.
-   Measured at Riverwood: `Accumulate` 0.59-0.65 down to 0.10-0.11 ms; the whole skip removed a ~3.4 ms render-thread residue at
    uGrids 21.

#### The colour pass's sun test

ShadowDir and DefShadow (pass descriptor bits 13, 14) are decided on the GPU, so one pipeline serves both cases (pipelines fell from
88-90 to 72-74 at Riverwood). `PrimaryCull::SunShadowStatic` gives the bits an object takes when its bound meets a cascade and
`kObjectSunTest` marks it; the colour dispatch tests the bound against the cascades the latch carries (`InSunCascades`) and on a
miss sets `kObjectSunMiss` in the draw's object word, which `Lighting.hlsl`'s `PixelDescriptor()` reads (`DCLFSunMiss`) to drop the
two bits with the light count. The cascades come from DCLF's own views (`Extensions::SampleSunCascades`), captured on every frame the
sun accumulates and written into a region after the main latch (`MainLatchLayout`, 208-byte `Draws::GpuCascade` records). The
engine has no bound on the cascade count (`iNumSplits`, the shadow map array's slices): `SunAccumulation` keeps them in a vector and
the layout grows (`ReserveMainLatch`). A frame where the sun did not accumulate draws sun-lit passes unshadowed.

#### Point lights' shadow culls without DCLF's entries

`Features/Shadows/Local/LocalLightCull` (AE only):
-   **The exclusion** is `BuildSunExclusion`'s for the paraboloid mode, over the light candidates (`FrameView::GetLightCandidates`),
    built on the shadow worker and published after the shadow epoch that drew the paraboloid views (`ParabolicExclusionSlot`).
    `SelectFrame` chooses it at the scene frame's start; it applies only while the set withholds that mode's casters and the exclusion
    was built for the current candidates.
-   **The skip:** `BSParabolicCullingProcess::Process1` (vtable `0x16`, checked against `0x141519a90`) returns at an excluded entry.
-   **The category filter:** inside a point light's cull, a category node (an exact `NiNode` parent of excluded entries) culls its
    children less those entries, through `NiNode::OnVisible`'s slot. Everything above it stays the engine's (cells, multibound tests,
    portals), so portal-strict lights take it too. It is an immutable snapshot built at the selection, holding its node; a node with
    an attach or detach is walked natively until the next build.
-   **Empty culls:** a light is not culled at all when the exclusion is live, the frame is not a parity frame, the light is within the
    mask's 32, every view of it is covered with its casters in the set, and it is not portal-strict. The bookkeeping is done as
    `BSShadowParabolicLight::Accumulate` does it (the light index `+0x160`, the bit `+0x164`, the count).
-   **Light entries** (`LightEntryOf`, `Tracked::lightRoot`): an actor's geometry's light entry is its actor's root; any other
    geometry's is its sun entry, unless that is a category node, so entries never nest.
-   **The lights' bits** (`LocalLightCull::NoteMainRegistration`): for the engine's main passes under a cut entry, DCLF writes a point
    light's bit at the main registration (`+0x160 == 0xFFFF`, not owned) for each light that reached the entry, when untested or
    when the bound is in the light's sphere (from `FUN_141519d20`'s camera position and radius). DCLF's members take theirs on the
    GPU (`LocalShadowLights`: the object word's bits 26-29). The per-frame light list starts at the selection, not at `Main::Draw`'s
    mask clear (`FUN_1414cb640`), which runs only while the player's third-person 3D is drawn.
-   Measured at Riverwood, `Accumulate` per light: 1.4-1.5 ms with the skip off, 0.45-0.53 ms with the skip, 0.10-0.11 ms with the
    skip and the filter; empty culls removed about 4.8 ms a frame at uGrids 21.

### The occlusion maps, drawn by DCLF

The precipitation accumulator's two views (render mode 0x1C) are both drawn by DCLF's occlusion epoch while Skylighting is loaded
(`Features/OcclusionMaps/OcclusionMaps.h`): Skylighting's sky occlusion map (`CS_DCLF_SKYLIGHT`) and the engine's precipitation
occlusion mask (`CS_DCLF_PRECIPITATION`). Without Skylighting the engine draws both.
-   Each map has its set phase and view bit (`kSetOccluderSky` / `kViewSkyOccluder`, `kSetOccluderPrecipitation` /
    `kViewPrecipOccluder`), its occluder rule (`OccluderRules.cpp`), and its target: Skylighting's texture for the sky map, depth
    target 10 for the mask.
-   **No ownership exception is needed**: only Skylighting's `Prepass` compute reads the sky map, so DCLF draws the whole map, with no
    residue.
-   **One pass rule.** Skylighting's rule is the static `Skylighting::OcclusionTechnique(property, geometry, skylighting)`
    (`src/Features/Skylighting.cpp`); its hook and DCLF's walk call the same function. The precipitation mask takes no skinned object,
    no multi-texture landscape, and none of the sky map's BSX filter. The sky view rejects a bound radius of 32 or less on the GPU
    (`kCullMinRadius`). An occluder whose bound lies wholly below Skylighting's probe grid's bottom less the margin draws nothing (a
    cull plane latched each frame, `OcclusionMapContribution::floorHeight`).
-   **The shell's calls** (`src/Features/DrawcallLimitFix.h`: `OcclusionReady`, `OcclusionNeedsEngine`, `OcclusionView`,
    `DrawOcclusion`): when a map is ready, `SetupMask` is skipped and `RenderMask` only sets the camera and clears; `DrawOcclusion`
    draws every ready map in one epoch after the sky map's `RenderMask`. A map is DCLF's only when the selected revision has a shape
    for it; occlusion layouts are predicted from the maps' last captures (`PredictedOcclusion`).
-   **The view** (`OcclusionView(map)`, once the map's camera is set up): DCLF computes the matrices, eye and viewport, with depth range
    0-1, slice 0, back-face culling at the renderer's fill, no bias and no scissor; a two-sided occluder draws without culling.
    Skylighting's `SetViewFrustum` hook narrows the camera to one quarter a frame, and the captured projection is that quarter's, so
    DCLF culls the same quarter with no planes of its own.
-   **Joining occluders** are left out of the map until they join (`FrameView::SetLacking` counts them), never handed to the engine's
    cull.
-   **The fade rule.** `SetupMask`'s cull runs without `cameraRelatedUpdates`, so `BSFadeNode::OnVisible` recurses into a not-settled
    node only while `currentFade > 0` and `fadeAmount != 0`; DCLF's maps apply the same rule from FadeStateCS's state
    (`kCullFadeOnVisible`, `FadedOutOfOcclusion`). An occluder under a root DCLF does not service is not ready for the occluder phases.
-   **The depth pass's alpha reference.** Under `RENDER_DEPTH` with `ALPHA_TEST`, `Utility.hlsl` also discards below `PerGeometry
    AlphaTestRef.x`: the engine writes `0x3F7EFEFF` while the object blends, else `threshold/255` plus just under one step
    (`BSUtilityShader::SetupGeometry`); DCLF computes it from the object record (`DCLFDepthAlphaTestRef`).
-   Measured at Riverwood: `SetupMask` 0.43 ms and `RenderMask` 0.34 ms down to 0.022 ms, plus a 0.03 ms epoch.

### Water reflections

DCLF draws the water reflection cube map's plain faces as a view of the one scene (Features/Reflection; `CS_DCLF_REFLECTIONS`),
with set phase `kSetReflection` and view bit `kViewReflection`.
-   **Members**: objects with a LOD technique (`LodLightingTechnique`: 9, 13, 15, 18), joined once their pipeline slot's forward
    pipeline is built (`PhaseReady`), and the faces' tree LOD. Only plain face renders (mode 0) are DCLF's
    (`ReflectionFaces::Plain`); modes `0x19` and `0x1B` stay the engine's.
-   **One epoch a frame at the depth commit** (`Segment::Reflection`, `Features/Reflection/ReflectionEpoch.cpp`). `TESWaterReflections::Update`
    renders one face per call, twice a frame, before the depth pass, and nothing between a face render and the water's read of the
    cube touches it, so drawing after the depth commit is still before the water reads it. An epoch's draws must never read two
    publications: the faces must follow the depth commit, which brings the index pool and the scene's buffers to the publication whose
    scene list they draw (against the frame before's pool, a reassigned slot drew old indices against new vertices: a device fault).
    The view work that depends on tree LOD's decision runs at `ViewPoint::FrameStartAfterTreeLod` (`Reflection::Prepare`: the targets,
    the LOD slots' forward pipelines).
-   **The shape always has six faces**, so recordings hold across frames; a face the frame does not render culls and draws nothing.
-   **The LOD add-roots are skipped.** `Update` (thunk at `0x14052289c`) asks once per update which cube-camera roots DCLF draws
    (`ReflectionFaces::SetRootsOwned`: `kLodRoots`, LOD land and objects, while both LOD classes are on and the swaps gated;
    `kTreeRoot`, LOD trees, while the faces draw tree LOD). `AddLodRoot` then skips the add-root (`FUN_1405275e0`) at all three LOD
    call sites (`0x14052080d` land, `0x140520826` objects, `0x14052083f` trees), so the engine neither culls nor registers them; the
    sky's stays (`IndirectDraws::ViewLodRootsOwned`). The engine's face draws are then only the sky (about 21 passes). An
    `OwnRenderers` view's residue under those roots is watched (`PassCapture::WatchViewResidue`).
-   **Withholding**: the cube camera's accumulators (`+0x1A0`, `+0x1A8`) are the view's own renderers (`SetViewRenderers`,
    `SetViewWithholdActive`).
-   **Programs and pipelines**: `ShaderPrograms::FindForward(vertex, pixel)` (a main pipeline's pulled vertex stage and the colour pixel
    stage without `Deferred`); `FindDrawFamilyForward` (DistantTree with `AlphaTest`). Pipelines resolve from a catalog passed
    explicitly (`ForwardPipelineOf(catalog, key, targets)`), so a revision resolves from its publication's catalog. The Z-prepass
    layout, the face's colour (`R16G16B16A16_FLOAT`) and DCLF's own `D24_UNORM_S8_UINT` depth, LESS_EQUAL with writes, the engine's
    winding, no blending.
-   **Culling**: BuildDraws over the scene list with `kViewReflection`, frustum only, one latched dispatch per face; the latch's slot
    map sends each LOD slot to its forward pipeline's bucket. Tree LOD: TreeLodCullCS phase 1 per face (no occlusion) into the face's
    list.
-   **Drawing**: per face, a pass on its slice of the engine's cube target (loaded: the engine's sky is in it, and it writes no depth)
    and DCLF's depth (cleared); bucketed plain indirect draws, then the face's tree LOD.
-   **Constants**: the main material and pipeline rows; the frame push is the view's own frame constants
    (`ViewResources::frameConstants`) with VS/PS b12 the face's block, written by the view's commit (`CommitView`) through Core's one
    frame-record writer (`Core/Render/Resources/FrameRecords.h`, shared with the main and shadow commits), and the view's own copies
    of the engine's frame buffers (`ViewResources::frameBuffers`, `LatchFrameBuffer`). The faces' frame textures
    (`kFaceFrameTextures`) each take a source from this frame: the character light, a registered frame texture from each CS
    integration, null where CS may have bound another texture there. Light Limit Fix's t35-t37 read null on purpose (they are the
    main camera's clusters). The face draws' only constant difference from the main rows is `DirLightDirection` in about the fifth
    digit (the faces render with another moment's sun).
-   **The camera**: `ReflectionFaces::SetFaceOriented` (after `FUN_1414ed500`) gives DCLF the face's camera; `SetAfterFaceDraws` the
    observers' capture.
-   Measured at Riverwood: about 0.06 ms of render thread an epoch and 0.09 ms GPU a frame, where the engine's own face work had been
    about 0.5 ms of render thread and 1.66 ms median including its cull.

#### The state

Every face draw culls front faces (`D3D11_CULL_FRONT` with CCW fronts): a cube face's projection mirrors the image. So DCLF's face
pipelines cull front faces unless two-sided (the view contract's `cullBackFaces` off). The LOD draws test and write depth with the
alpha test on; the sky tests without writing.

### First person as a view bit

The player's first-person skeleton is a category root of the one scene, drawn by two view epochs (Features/FirstPerson; Core's view
engine, `Core/Render/Epochs/ViewEpoch*.cpp`, `Core/Render/Views/ViewSlots.h`, shared with the reflection faces;
`CS_DCLF_FIRST_PERSON`, needs skinned and actors; `CS_DCLF_FIRST_PERSON_DECALS`).
-   **Membership**: the first-person root (`CaptureRoot`), its geometry tracked like an actor's. Set phase `kSetFirstPerson`, view bit
    `kViewFirstPerson`, never `kViewMain`, a caster or an occluder bit. The classification stops at the root before the hidden test
    (its `kHidden` is the view's). A first-person slot takes part for opaque, alpha-tested and forward keys; never for refraction or
    shadow-only objects; decals only through `Coordinator::ViewDecalForward`.
-   **The latch** (`FirstPersonLatch`): `Main::Draw`'s condition recomputed: the root's `kHidden` clear, the main menu closed, and the
    camera not the free camera.
-   **Camera**: the engine's first-person camera as `FUN_1414cb020` sets it up (the world camera's rotation at
    `GetFirstPersonNodePosition`, `firstPersonFOV` plus the animated offset, near `fNear1stPersonDistance`, the world's far), in world
    space. The first-person epochs' `CameraPosAdjust` is the world eye, not the first-person camera's translate: the rows are world
    space (`Features/FirstPerson/FirstPersonEpochs.cpp`, `CaptureCamera`). DCLF never reads the skeleton while the engine has it
    shifted: FrameValues takes the members' rows and palettes at the frame start, and the producer never samples a first-person item.
-   **Depth epoch** (`fp.depth`, at RenderDepth's first-person draw): culls with the first-person camera (frustum only) and draws the
    buckets' Z-prepass depth pipelines into the main depth, after the HZB build (the HZB leaves the arms out) and before
    `kPOST_ZPREPASS_COPY` is taken; it sorts the forward range back to front (`view.forward-sort`).
-   **Colour epoch** (`fp.colour`, `Main::RenderFirstPersonView`'s accumulator render): the same sequences with each bucket's
    first-person forward pipeline into kMAIN and the motion vectors, tested against `kPOST_ZPREPASS_COPY`, then its forward range.
    Both epochs use the viewport depth range [0, `f1stPersonFarDepthRange`]. First-person forward keys carry
    `kForwardKeyFirstPerson`: two attachments, every channel written.
-   **Sun shadows on the arms**: no first-person caster (the casters run with both roots hidden); the colour pass samples the
    screen-space shadow mask made from the main depth, which holds the arms' depth.
-   **First-person decals**: a decal in group 1 or 2 on a first-person entry becomes a forward object of the first-person forward part
    (no Deferred, the blended group's blend mode, depth tested); `forwardDecalGroup` carries the group's bias mode in the key.
-   **Withholding**: the first-person accumulator's renderers and the Z-prepass's are among PassCapture's main renderers
    (`InMainRenderers`); a `kSetFirstPerson` member's pass is withheld while the view is covered.
-   **The frame's culled bits**: `FrameGlobals::cullHidden` holds the first-person subtree's live bits, taken at RenderDepth's
    first-person draw after both first-person culls (`FrameGlobals::NoteFirstPersonCulled`), used by the next frame's capture.
-   First-person refraction (`HeatRefraction`, the Flames hand) stays the engine's: DCLF has no billboard turn in the Utility
    refraction stage and no refraction pass in the first-person colour epoch.

### Portals

The main camera's portals and occluders are DCLF's own (`Features/Portals/PortalViews`).
-   **The walk** ports the engine's (`FUN_1401a43c0` -> Traverse `FUN_1414a41e0`) and the `BSCompoundFrustum` builders. It runs on the
    render thread right after `Main::Update`'s own walk (the feature's `PortalWalk` thunk) and writes nothing to an engine object.
-   **The programs** (`kPortal*` in `Records.h`, `PortalPrograms.hlsli`) go to the GPU in one buffer a frame: view planes, a directory
    and the records. Each fade root's program is its nearest room ancestor's (`PortalViews::ProgramOf`). FadeStateCS tests a root
    through it (`EngineInView` -> `PortalTest`), and BuildDraws' main phase 1 rejects a member whose root's program culls its bound.
-   `PortalViews::VisibleSet` answers `Main::IsRoomVisible` ([Room visibility](#room-visibility-the-tree-list-the-actor-stamp)), and
    `PortalViews::VisibleUnbound()` the scene lists' branch.

#### PortalViews::WalkLight

A shadow light's rooms (`PortalViews::WalkLight`, `CommitLightWalk`, `LightSelection::lightRooms`): a port of `FUN_1414a2530` ->
`FUN_1414a6400`, kept per light, with three cases:
-   **Not walked** (the entry stays as constructed: unbound space, no rooms) when there is no graph or no rooms, the NiLight is hidden
    or its fade is under 0.05, or the light is on an object node (`BSLight +0x130`).
-   **Not portal-strict** (`+0x47`): unbound space plus every room whose bound meets the light's sphere.
-   **Portal-strict**: Traverse from every room holding the light camera's near point, else from unbound space. The light's room
    process (`+0x128`) has its own orthographic camera; the portal test is `BSParabolicCullingProcess::TestBaseVisibility2`, and with
    `+0x30200` set either side of the light's plane.

Occluder states and boxes are the main walk's. A light's edge flags persist only once the light is kept (`CommitLightWalk`). An
unwalked light keeps its entry.

## What the engine still draws

DCLF claims whole objects in the phases it draws; the engine keeps:
-   **By class**: water, sky, grass, precipitation, the particle manager's master particle systems (torches, candles, street mist),
    temporary effects outside DCLF's categories (blood impact particles), and the effect variants not ported (sky objects, wireframe,
    cloud lighting, Lighting without Light Limit Fix, projected UV, unrecognised blend pairs, Falloff without blending, membrane,
    strip and mesh particle systems, skins or nested billboards under billboards).
-   **By state**: an object not yet a member (the engine draws it until the publication that claims it is installed), a pair a
    register is missing for, a texture still importing, an object whose geometry buffer the game can map (`UnstableBuffer`).
-   **By view**: first-person refraction; the reflection's modes `0x19` and `0x1B`; the reflection faces' sky; a shadow view whose
    rasterizer state no pipeline expresses; the occlusion maps without Skylighting.
-   **Mixed entries** keep the engine's culls whole, though DCLF still draws their members.

`CS_DCLF_CENSUS=1` counts every pass a registration hook hands on to a batch renderer, by view and class, and gives each Lighting
geometry that is not a member the coordinator's verdict (asked asynchronously, `PostCensusRequest`, answered by
`Diagnostics/Scene/SceneCensus.cpp`); a member drawn by the engine is a defect. The report and its gates are
[dclf-validation.md](./dclf-validation.md); what is still to be taken over is [dclf-roadmap.md](./dclf-roadmap.md).

## Trusted and parity

DCLF runs in one of two configurations: the normal path (asynchronous, persistent, every check off) and parity (the same path, with
the parity switches observing it). A parity check never changes what is drawn. Observers reach the normal path only through
`Diagnostics::Observe()`, `Diagnostics::Policy::*` (each answers the switch-off value on the normal path) and hook points, registered
before DCLF's threads start, so a hook costs one flag test and no lock.

The invariants each run must keep after its first scene:
1.  **Capability, not occurrence.** The phases DCLF draws are the toggles plus what it has set up; which views the engine asks for in
    a frame never changes the set.
2.  **Every view served.** Every view of a claimed phase is drawn from the persistent payload: the shadow revision's shape covers
    every view slot, a slot without a view doing no work.
3.  **Incremental only.** Every per-frame update is bounded by what changed: the candidate tables keep stable indices and per-entry
    versions, and their consumers follow their change log (`ChangedEntriesByLog`); the set's commit evaluates only the slots an event
    or a readiness source named; the shapes are made again only when what they are made from moves (`RevisionShapesKey`). Each has a
    parity counterpart that evaluates in full.
4.  **No overflow fallbacks.** Event queues and logs grow; there are no caps.
5.  **The normal path trusts its producers.** It does no comparison, re-derivation or rebuild of upstream work and has no fallback
    for a producer's mistake: a mistake shows as a visual error, which parity finds. The only checks it keeps are O(1) memory-safety
    bounds (a CPU write clamped to the region it targets, counted). A GPU hazard on a freed or undersized buffer is impossible by
    construction: versions move only at a revision's selection, and a pooled backing is reused only once the frames that bound it
    have retired on the GPU.

The flags, the switches, the standard validation and the CPU tests (`tests/DrawcallLimitFixCPU`) are
[dclf-validation.md](./dclf-validation.md).
