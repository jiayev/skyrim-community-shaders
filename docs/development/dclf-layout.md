# DCLF code layout

This is the layout Drawcall Limit Fix's code is moving to, the rules it keeps, and the order of the move. The present
architecture is in [dclf-architecture.md](./dclf-architecture.md).

## The target

```
src/Features/DrawcallLimitFix.{h,cpp}   the shell: CS's one integration point (Feature overrides, hook thunks, menu)
src/Features/DrawcallLimitFix/
  Api/            the extension API: the only DCLF headers other CS features include
  Core/           what every feature builds on, with no feature knowledge
    Runtime/        the building blocks (KeptState, SlotTable, EventQueue, Retirement, ...) and the scene executor
    Engine/         engine access, the read window, the releases, the render-thread budget, hook helpers, event channels
    Settings/       the switches and toggles
    Scene/          the coordinator's core: mirror, tracked entries, slots, walk, set and claims, publication, pump
    Handoff/        taking objects from the engine: primary cull, scene lists, pass capture
    Shading/        the material port framework, bindings, constants, lighting descriptors
    Gpu/            frame values, pooled backings, GPU resources and textures, layouts, pipelines
    Render/         the snapshot builder, revisions, payloads, the epoch framework, common passes, the frame driver
  Features/       one folder per engine feature DCLF ports (Main, Forward, FirstPerson, Reflection, OcclusionMaps,
                  Shadows/{Common,Sun,Local}, Portals, GpuOcclusion, LocalLights, Actors, Faces, Particles, Billboards,
                  Decals, Effects, Landscape, Fades, Lod, TreeLod, TreeWind, TextureTransforms, HiddenNodes, SwitchNodes)
  Integrations/   one folder per CS feature DCLF supports, written against Api/ (TruePBR, AdvancedSkin, ...)
  Diagnostics/    the observer infrastructure and the parity checks that span features
```

A feature's folder owns everything about that feature:
- its engine hooks;
- its coordinator state;
- its table columns and records;
- its frame-value pool;
- its GPU stage and HLSL;
- its toggles;
- its parity checks and report lines.

Features register at install, before DCLF's threads start. Their callbacks run per phase or per chunk, never per object.
The bits that shaders share stay compile-time: each feature declares its own, and a core manifest checks that they don't
overlap.

## The extension points

A feature is an `Api::Feature`: a name and an install function. It is listed in `Features/FeatureList.cpp`, the one place
that shows what DCLF does. The shell's `PostPostLoad` calls `Extensions::InstallFeatures` before DCLF's threads start.
Install registers the feature's parts through `Api::InstallContext`. The registry (`Core/Extensions/Registry`) is
read-only after that. A feature may also name early hooks (`Api::Feature::earlyHooks`): engine hooks Core's engine side must
find in place when it installs (a vtable slot the primary cull takes, or the source of an engine event channel Core follows), which
the shell runs after Core's scene events and before DCLF's own engine hooks and observers (`Extensions::InstallEarlyHooks`).

| Contract | Header | What a feature gets |
| --- | --- | --- |
| Frame-value pool | `Api/FrameValues.h` | rows sampled every frame on DCLF's pool into a pooled backing the frame reads by descriptor (`FrameValues::PoolIndex`); its own frame-start reads (`frameStart`), and the rows once the producer is done (`FrameValues::ProducerDone`) |
| GPU stage | `Api/GpuStages.h` | a compute pass at a named slot of an epoch (`DepthSegmentBeforeBuild`, `DepthSegmentNextFrameState`), on a chosen queue, its programs set up with the scene's buffers, and optionally the segment's latch word |
| Payload product | `Api/Payload.h` | data built with every main payload on the builder, carried with it, and installed with it on the render thread |
| Scene resources | `Api/SceneResources.h` | state held with the scene's tables: versioned buffers registered with the scene's set, their graph registration, each revision's requirement, and the slots they hold of the objects (`SceneFit`) |
| Commit hook | `Api/Commits.h` | its part of each commit's frame data (depth, colour, shadow), staged into the adopted versions on the render thread |
| Frame binding | `Api/FrameBindings.h` | a buffer every draw reads at a declared texture register, given by the frame records (main, shadow, the builds' embedded addresses) and checked by the named-address parity |
| Switches and toggles | `Api/Settings.h` | its `CS_DCLF_*` switches, listed where it says beside Core's, and its live toggles, in the menu and the packed word (a toggle may need Core's or an earlier feature's) |
| Report lines | `Api/Reports.h` | its lines at a named point of the periodic report: logged, handed the report's frame (`PrimaryCull`; `Statistics`, the statistics' last), or appended to the report's text (`AsyncWorkViews`, the views' lines in the draws' async work, after the visibility lines and before the forward epoch's; `AsyncWork`, the draws' async work) |
| Stand-in observer | `Api/EngineEvents.h` | the primary cull's stood-in roots as they change, and Main::Update's entry; whether the primary cull is installed |
| Table family | `Api/TableFamilies.h` | columns of its own in every `SceneTables` (`Extensions::TableFamilies`), read through a typed handle; Core keeps beside them the write stamp (the publication copies a family only when it moved), the objects' references into it (bounded by its scene resources' fit, carried in the record at a declared word) and its rows' tags, and recycles its retired rows |
| Coordinator component | `Api/CoordinatorComponents.h` | state the coordinator keeps for it (`make`), and hook sets, one per concern of the coordinator's, each called on the scene lane per event (a changed slot, an erased entry, a freed range, a phase), never per object per frame: `residency` (a slot resident, left, every residency ended, the residents recounted), `accumulate` (a patch accumulated with its draw chain, the joins done), `walk` (begun, the delta walk's scheduling with its counts, a slot rewritten or its skin partitions refreshed, ended), `dense` (the walk parity's dense walk: save and restore), `retirement` (the slot tables reset, a tracked entry erased, every entry gone, a range it retired come free: `RetireRange`) and `entryClass` (the class it follows, `EntryClass`: whether it takes it, `Extensions::EntryClassTaken`, read as data; its writes listed, the record's second vertex stream resolved or waiting, the stream's slot); with a narrow context: the live tables, the scene mirror and its fade node, the pass serial, the entry's reference, a tracked entry's slot and write state, its write scheduled, seeds the next frame takes (`FrameView::TakeComponentSeeds`) |
| View contribution | `Api/Views.h` | an engine view Core's view machinery draws in the engine's place once a feature takes it (`EngineView`, in Core's slot order: the reflection, still Core's, then the first person), under its toggle, and whether a decal on the view's entries is a forward object of its forward part; its work at the view's engine points (`RegisterViewWork`, `ViewPoint`: the frame's start before the coverage, after tree LOD, and the depth commit), run once a frame in view order after Core's own. Core keeps a fixed slot per view (`Core/Render/Views/ViewSlots.h`: its epochs, segments, revision key group and graph extension) and resolves each view's entry once at install (`Extensions::Views`: its toggle, set phase, view bit, membership, shape life, coverage on a failed graph, work), which the per-view loops and the per-object paths read as data (`Extensions::ViewOn`, the set's phases, `ViewBitsOf`'s table by phase mask) |
| Object contributor | `Api/ObjectContributors.h` | three kinds, read as data where Core decides: membership by an engine key (a technique it ports: its eligibility under a toggle and its objects' record flags; a geometry type it ports: a class under a toggle, read by the eligibility, the walk and the captures); membership by a Core class, one enum value per class a feature takes (`EngineParent` and `ParentLeaves`: the leaves under a billboard, a switch node or an actor, read by the classification and the sun and light candidates; `ObjectClass`: a decal or a multi-index layer Core's ports derive but leave native, read by the ports' eligibility; `EntryClass`: a face shape whose records wait on the coordinator component that follows it); and derivations (the shading values it derives for a property flag of its own, and engine globals captured with the frame's: `FrameGlobals::features`). Adding a Core class is the same three steps for every enum: the value in the header, Core's test of it through its `Extensions` query, the feature's registration (the install fails a second claim) |

Each contract's thread, timing and lifetime is documented in its header. A feature's install also installs its engine
hooks and subscribes its observers (`Diagnostics::Subscribe`, under Core's `Observe()` flags). Its thunks read the shell's state
through `Core/Engine/ShellState.h`: whether DCLF runs (`ShellState::Running`, the query the shell installs before the early hooks) and
where the engine's passes are (`ShellState::Passes`: in the depth pass, RenderDepth's control-driven argument), which the shell's
Main::RenderDepth thunk writes.

The headers in `Api/` name only standard and ORG types. The contexts their callbacks receive are opaque there. Core
defines them in companions under `Core/Extensions/` (`FrameValuePools.h`, `GpuStages.h`, `PayloadProducts.h`,
`SceneResources.h`, `Commits.h`, `FrameBindings.h`, `Reports.h`, `EngineEvents.h`, `TableFamilies.h`,
`CoordinatorComponents.h`, `ObjectContributors.h`, `Views.h`), which features inside DCLF include for typed access to Core's types.

Residency changes are events, so a coordinator component's hooks are function pointers called per changed slot. The loops
that run every frame over every object (the walk's evaluate, the builds' packing, the commit) never call a feature: they read
what it registered as data (a technique's flags and toggle, a geometry type's or a parent's toggle, a family's references and record
word). The switches and toggles register with `Core/Settings`, which owns them.

The shaders' shared constants stay compile-time: a feature declares its own (a frame binding's register beside its HLSL
declaration, its object flag bits within `kFeatureObjectFlags`, its family's record word), and `Features/Manifest.cpp`
checks them against Core's and each other's.

`Features/TextureTransforms/` was the first feature built on them: a pool, a stage and a product. `Features/TreeWind/` was
the second: a pool with frame-start reads, scene resources with their commit hook and frame binding, a next-frame stage on
the compute queue, its switches and toggle, a report line, a stand-in observer with its engine hooks, its parity, and its
table family (the tree slots), coordinator component (listing them as members join and leave) and object contributions
(technique 12's eligibility and record flag, its shading derivation, its frame globals). `Features/Particles/` and
`Features/Billboards/` followed: their switches and toggles, report lines at the draws' async work and parity, a geometry type
contribution (the particle types) and parent contributions (the leaves under an NiBillboardNode), the particle snapshots' engine
hooks, and the billboard parity's early hooks. `Features/SwitchNodes` (its switch and toggle, and the switch parent's contribution) and
`Features/HiddenNodes` (the hidden events' source, the engine's kHidden stores patched at its early hooks, and its watch: a switch, an
observer and report lines at the statistics) followed. `Features/Decals` followed: its switches and toggles (the layers' needing the decals'),
the decal and layer object classes' contributions, the member decals' order as a coordinator component (joined by its patches' chains,
re-keyed by the walk's rewrites, ordered when the joins are done, writing the tables' ordinals) and its parity. What it leaves Core is the
main pass's structure: the ports' decal groups and states (the patch's decal key, `kObjectDecal` and its group bits, the raster keys), the
tables' ordinal columns, the main build's decal inputs and templates, the sequence buffer's decal ranges and count words (`GpuLayouts.h`,
`BuildDrawsCS`), and the colour segment's decal depth pass and second pass, interleaved with the terrain groups (`MainOpaque.cpp`); moving
them needs an ordered fixed-slot range contract for the main pass. What SwitchNodes and HiddenNodes leave Core is the visibility every object is judged by: the switch
selections the primary cull follows under every stood-in root whatever the toggle (the index stores' and child edits' events, without
which DCLF does not run; the pending switches, the catch-ups, the cut's selection reads, tree LOD's switch), and the hidden-bit
vocabulary (the channel, the chains and their drain, the frame's capture of the culls' bits, the Hidden verdict, the LOD gates' veto).
`Features/Faces` followed: the face morphing's thunks and the head snapshots (`FaceSnapshots`), and the face shapes as a coordinator component following
their entry class (`EntryClass::FaceShape`, taken once the thunks are in): their heads' lists, the heads' publications taken as each walk
begins and applied as the delta walk schedules, each written shape's snapshot, region of the positions buffer and stream, and the regions
retired as their streams go. What it leaves Core is what every draw reads and the walk's classification: the face streams in the scene's
tables (`faceStreams`, the objects' `faceStream`, `kRetiredFaceStream`), the positions buffer, the commits' uploads into it, the builds'
second-stream geometry draws and the passes' bindings (`SceneBuffers::facePositions`, `UploadFaceStreams`, `FaceStreamGeometry`), and a face
shape's classification (`Tracked::faceShape`, its head held from the attach's pins, the per-frame face trait and the walk's face names in
the reports). Moving the first needs a per-object vertex stream contract (a feature's rows in a buffer of its scene resources that the
builds, the commits and the passes read as data, and the second stream's slot in the draws); the second, an entry-class contribution
to the classification and the attach.
`Features/Actors` followed: its switch and toggle (which the first person's needs), the
actor parent's contribution (`EngineParent::Actor`, `ParentLeaves::AllLeaves`: the classification's chain test) and the actor techniques'
(actor skin, faces, hair, eyes). What it leaves Core is the path an actor's objects take whatever the toggle: the skinned geometry, which is
any NiSkinInstance's and not the actors' alone (the bone palettes in FrameValues' palette part, the skin partitions in the tables and the walk,
the skinned and skin partitions toggles, which the first person and the effects read too); an actor-owned entry's classification
(`Tracked::actorOwned`, the per-frame actor trait, `Coordinator::ActorRecordKept`, `SceneTables::actorObjects`), which needs the entry-class
contribution below; the move and fade-amount events of the actors' updates, which every mover follows; and the actors' animation stamp from
DCLF's shadow views (`Engine/Cull/SideEffects`, its report line inside the accumulated writes' text), which belongs with the shadow views.
Advanced Skin's coupling stays too, for `Integrations/AdvancedSkin` once its contracts exist: the actors' wetness (membership in the tables,
published to the render thread's index, captured at the frame's start and fanned out by FrameValues into the shading rows' SkinPerGeometry)
needs a frame-value contributor into a built-in part and a published membership; its material textures (t71/t74, `MaterialPortFeature`) need a
material contributor; its capture-parity checks follow them.
`Features/FirstPerson` followed: its switches and toggle (which needs the skinned and the actors), the first-person view's contribution
(`EngineView::FirstPerson`: its toggle, and the first-person decals as forward objects, read once), its report lines at the draws' async work
and the skeleton's parity. What it leaves Core is the view machinery the reflection, the occlusion maps, the portals, the local lights and
the shadows share: the epochs (`Epochs/FirstPerson.cpp`: the latch, targets, pipelines and the two epochs, whole-epoch segments of
`RenderGraphRuntime` rather than GPU stages; `ViewEpoch*.cpp`: the view's passes and forward part), their revisions (`ViewKind`, `Epoch::
FirstPersonDepth`/`FirstPersonColour`, the shape, key group and coverage), the view's state and the counters its report takes
(`ImplViews::FirstPersonState`, `IndirectDraws::FirstPersonReport`); the scene's side (the first-person root as a category node, the
classification's category stop and `NoteFirstPersonHidden` and its misfiled observer, `kSetFirstPerson` and the set's first-person decisions
and their parity, `kViewFirstPerson`, the placements' per-frame chains, the decals' forward keys, the frame requests' root, the frame globals'
culled bits); FrameValues' first-person rows, palettes and particles; `PassCapture`'s withholding of the members' registrations and the
census's view; and the shell's two engine hooks that run the epochs (they read the shell's `Running()` and the depth pass's control-driven
range). Moving them needs a view-kind contract: a view's scene set and view bit, its category root, its epochs at engine points with their
segments, its revision shape and coverage, its state and report counters, its frame-value rows and the engine registrations it withholds.
The particles' and billboards' per-object paths stay Core's for now: the particle snapshot store and the
expansion FrameValues' producer runs per particle member into the palette part it shares with the skins (`Engine/Capture/
ParticleSnapshots`), and the billboard placement row it samples per moving object (`Engine/Capture/Billboards`, Effect.hlsl's
layout). The extension points still to come, as the next features need them:
- frame-value contributors that write into a built-in part per chunk (the particles' palette rows, the billboard placement row, Advanced
  Skin's wetness in the shading rows);
- per-object record columns (the record's TreeParams and WindTimers block, `treeAnim`, is still Core's: the Lighting port's
  layout of `DCLFObjectRecord`);
- more engine event channels (the stand-in change still names the BSTreeNodes among its roots);
- material contributors;
- view kinds (the first person's and the reflection's view machinery, see above): Core's side is in place (the slots, the resolved
  entries, the view work, `ShellState`); the features still to supply their shape, key rule, coverage, root capture and accumulators;
- the shared-bit manifest (object and record flags);
- ordered fixed-slot ranges of the main pass: a feature's ranges in the sequence buffer, their count words and draws in the colour
  segment's second pass (the decal groups, interleaved there with Terrain Blending's groups).
- a per-object vertex stream: a feature's rows in a versioned buffer of its scene resources, which the builds' second-stream geometry
  draws, the commits' uploads and the passes' bindings read as data (Features/Faces' face streams and positions);
- entry-class contributions to the walk's classification and the attach (a face shape's head pin, its per-frame trait).

## The rules

`tools/dclf-layout-check.py` checks these.

1. **Only the shell tests `CS_HAS_DCLF`.**
   - CMake defines it when the render graph and ORGModuleServices are present, and otherwise leaves the DCLF tree out of
     the build.
   - No file in the tree tests a dependency macro.
2. **The rest of CS reaches DCLF only through the shell.** Later it will also go through `Api/`.
3. **Includes point one way.**
   - `Api/` includes `Api/`.
   - `Core/` includes `Api/` and `Core/`.
   - A feature includes `Api/`, `Core/`, its own folder, and the features it declares as dependencies.
   - An integration includes `Api/` and `Core/`.
   - `Diagnostics/` may be included from anywhere. Production code reaches it only through its observer contract (`Hooks.h`,
     `Observation`, `Policy`), which the parity rework will redefine.
4. **Old includes may only shrink.** Includes into the folders that haven't moved yet (`Engine/`, `Scene/`) are
   counted against a baseline total, which each step lowers. The same goes for the functions over the complexity thresholds
   (120 lines or cyclomatic complexity 40).

## The order

Each step is a move-only commit, then a commit that extracts the code. It is verified with:
- the build, both with and without the render graph;
- the CPU tests;
- a normal run, plus `PERSISTENT_PARITY` and `SET_PARITY` runs;
- the motion timing run, for steps that change code.

The steps:
1. Put the dependency guard in one place, and add the layout check. *Done.*
2. `Core/Runtime`, `Core/Engine`, `Core/Settings`, `Core/Scene/SceneWake`. *Done.*
3. Split `IndirectDraws` into `Core/Render`. *Done.*
4. Split `Coordinator.h` into the coordinator's core plus one component per concern. *Done (Scene/Coordinator/State/).*
5. Two pilot features, `TextureTransforms` then `TreeWind`. Together they define the first extension points. *Done.*
6. The other features, least entangled first and `Fades` last. Then the integrations, then `Main` with what remains of Core.
