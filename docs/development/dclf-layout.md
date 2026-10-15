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
                  Shadows/{Sun,Local}, Portals, GpuOcclusion, LocalLights, Actors, Faces, Particles, Billboards,
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
the shell runs after Core's scene events and before DCLF's own engine hooks and observers (`Extensions::InstallEarlyHooks`). And it
may name camera hooks (`Api::Feature::cameraHooks`): the engine hooks that write a shadow source's cameras into Core's boards
(`Core/Render/Shadow/ShadowCameras.h`), which the shell runs where the shadow cameras' hooks install (`Extensions::InstallCameraHooks`,
after the sun's accumulation, before the primary cull). They are called once per source, with the source, in `Api::ShadowSource` order
(at each source the features in the list's order), and Core's light selection installs right after the sun's: so the hooks' install
order, and the install log's, is the sources' (the sun's UpdateCamera, the light selection, the focus cameras), whatever the features'
places in the list.

| Contract | Header | What a feature gets |
| --- | --- | --- |
| Frame-value pool | `Api/FrameValues.h` | rows sampled every frame on DCLF's pool into a pooled backing the frame reads by descriptor (`FrameValues::PoolIndex`); its own frame-start reads (`frameStart`), and the rows once the producer is done (`FrameValues::ProducerDone`) |
| GPU stage | `Api/GpuStages.h` | a compute or copy pass (`GpuStageKind`) at a named slot of an epoch (`DepthSegmentBeforeBuild`, `DepthSegmentNextFrameState`, and around a draw family's depth draws: `DepthSegmentBeforeDepthDraw`, `DepthSegmentPhaseTwoBeforeDraw`, `DepthSegmentAfterPhaseTwo`), on a chosen queue (or none asked: `GpuQueue::None`), its programs set up with the scene's buffers, and optionally the segment's latch word |
| Payload product | `Api/Payload.h` | data built with every main payload on the builder, carried with it, and installed with it on the render thread |
| Scene resources | `Api/SceneResources.h` | state held with the scene's tables: versioned buffers registered with the scene's set, their graph registration, each revision's requirement (with the words it posts with the revision's inputs, `postInputs`, compared for a new revision), the slots they hold of the objects (`SceneFit`), and the buffers the depth commit latches (`latched`: the main latched targets and the depth latch's layout) |
| Commit hook | `Api/Commits.h` | its part of each commit's frame data (depth, colour, shadow), staged into the adopted versions on the render thread, or latched into a main commit's latched copies (`CommitContext::latched`), with owners its execution holds (`bindingOwners`) |
| Frame binding | `Api/FrameBindings.h` | a buffer every draw reads at a declared texture register, given by the frame records (main, shadow, the builds' embedded addresses) and checked by the named-address parity |
| Frame constant row | `Api/Commits.h` | a float4 every draw reads in Core's feature rows of PS b13 (`FrameConstantRow`: c17 up to `Extensions::kFrameConstantRowsEnd`, Core/Extensions/Commits.h), sampled by each main commit and latched in ascending row order (`LatchFrameSlots`, `MainLatchedLayout`); Features/Effects' camera planes, light colour and blood (c17-c19, `EffectFrame.cpp`) |
| Frame texture | `Api/FrameBindings.h` | an engine render target's view every colour draw reads at a declared texture register (`FrameTexture`), sampled once per colour commit before its frame textures are resolved (`ResolveFrameTextures`, `Extensions::FrameTextureIndices`), checked against Core's registers and the frame bindings'; Features/Effects' soft depth, t118 (`EffectFrame.cpp`) |
| Switches and toggles | `Api/Settings.h` | its `CS_DCLF_*` switches, listed where it says beside Core's or another feature's (right after a switch, or right before one: Core's, or a feature's registered before it or after, checked once every feature registered, `CheckSwitchAnchors`), and its live toggles, in the menu and the packed word (a toggle may need Core's or an earlier feature's) |
| Report lines | `Api/Reports.h` | its lines at a named point of the periodic report: logged, handed the report's frame (`PrimaryCull`; `Statistics`, the statistics' last), or appended to the report's text (`AsyncWorkViews`, the views' lines in the draws' async work, after the visibility lines and before the forward epoch's; `AsyncWorkForward`, among the forward epoch's, after its refraction line, on a report that prints them; `AsyncWork`, the draws' async work; `AsyncWorkFamilies`, the draw families' after it; `SceneFrame`, the render thread's scene lines); the LOD gates' releases (`GateReleases`, logged); and the coordinator's on the scene lane (`SceneCoordinatorGates`, logged with the scene frame, or a coordinator component's text logged as one line, called by `Coordinator::ComponentsReportLines`: the LOD gates' line; `SceneCoordinatorSegments`, text, or a coordinator component's with its context: `componentText`, called by `Coordinator::ComponentsReportText`) |
| Stand-in observer | `Api/EngineEvents.h` | the primary cull's stood-in roots as they change, and Main::Update's entry; whether the primary cull is installed |
| Engine channel | `Api/EngineEvents.h` | a queue of its own its engine hooks push keys into (`EngineChannel`, `ChannelKind::Keys`: an `EventQueue` whose capacity and wake are the feature's, drained through `drain` into Core's `KeySink`), which every scene pass drains among Core's queues and applies at its point (`ChannelApply`: `BeforeStructural`, after Core's fade snaps and amounts; `AfterSwitches`, after the switch events), handing the keys to the coordinator components' `events.keys` hook; under a load screen kept as the mirror parity's named keys or dropped (`ChannelLoading`); whether the mirror's parity counts its keys named (`namesKeys`); found by name (`FindEngineChannel`). Core keeps a batch slot per channel (`Extensions::kEngineChannelSlots`) |
| Table family | `Api/TableFamilies.h` | columns of its own in every `SceneTables` (`Extensions::TableFamilies`), read through a typed handle; Core keeps beside them the write stamp (the publication copies a family only when it moved), the objects' references into it (bounded by its scene resources' fit, carried in the record at a declared word) and its rows' tags, and recycles its retired rows |
| Coordinator component | `Api/CoordinatorComponents.h` | state the coordinator keeps for it (`make`), and hook sets, one per concern of the coordinator's, each called on the scene lane per event (a changed slot, an erased entry, a freed range, a phase), never per object per frame: `residency` (a slot resident, left, every residency ended, the residents recounted), `accumulate` (a patch accumulated with its draw chain, the joins done), `walk` (begun, the delta walk's scheduling with its counts, a slot rewritten or its skin partitions refreshed, ended), `dense` (the walk parity's dense walk: save and restore), `retirement` (the slot tables reset, a tracked entry erased, every entry gone, a range it retired come free: `RetireRange`), `entryClass` (the class it follows, `EntryClass`: whether it takes it, `Extensions::EntryClassTaken`, read as data; its writes listed, the record's second vertex stream resolved or waiting, the stream's slot) and `events` (an engine channel's keys, every pass, at the channel's point); with a narrow context: the live tables, the scene mirror and its fade node, the pass serial and stamp, the entry's reference, a tracked entry's slot and write state, its write scheduled, seeds the next frame takes (`FrameView::TakeComponentSeeds`), the entries' drawn ranges the walk reads (`EntryRanges`, `EntryRangesOf`, the mirror's `MirroredRanges`, `SetEntryRanges` with the entry's rewrite); and report lines of its own at the coordinator's text point (`ReportLines::componentText`) |
| View contribution | `Api/Views.h` | an engine view Core's view machinery draws in the engine's place once a feature takes it (`EngineView`, in Core's slot order: the reflection, then the first person; see "The view contract" below), under its toggle: its set phase and view bit (the feature's own bits, checked by the manifest and the install), its membership (`CategoryRoot`: the entries under a root the feature captures, an exclusive phase; `LodTechniques`), its forward key rule and shape (faces, trees, forward part, depth pass, names, initial depth range), its shape's life, render area, where its members' engine registrations are withheld (`InMainRenderers`, with the accumulators the feature names, or `OwnRenderers`, the renderers its engine hooks name to `PassCapture`), its target import, its coverage before the first frame and on a failed graph, whether a decal on its entries is a forward object of its forward part, and its once-a-frame callbacks (`covered` over Core's precomputed `ViewCoverage`, `captureRoot`, `accumulators`); its work at the view's engine points (`RegisterViewWork`, `ViewPoint`: the frame's start before the coverage, after tree LOD, and the depth commit), run once a frame in view order. Core keeps a fixed slot per view (`Core/Render/Views/ViewSlots.h`: its epochs, segments, revision key group and graph extension) and resolves each view's entry once at install (`Extensions::Views`), which the per-view loops and the per-object paths read as data (`Extensions::ViewOn`, the set's phases and view bits, `ViewBitsOf`'s table by phase mask: both families', `Extensions::ResolvedViewBits`, the root views, the set decision's order, the InMainRenderers and OwnRenderers views); a view no feature takes has no entry, and every per-view loop skips it. The feature's work and epochs run on the view engine's calls (`IndirectDraws.h`, "The view engine", `Core/Render/Views/ViewEngine.cpp`: the slot's state, targets, depth range, pipelines, resources and target import, its epochs' inputs and submission; `ViewTreeLod.cpp`: a trees view's tree LOD per face) |
| Shadow-view contribution | `Api/ShadowViews.h` | the view contract's sibling for Core's one shadow engine (see "The shadow-view contract" below): an occlusion map (`OcclusionMap`, in Core's slot order: the sky map, then the precipitation mask) under its toggle and a load-time `available` flag, with its set phase and view bit (the feature's own, `Api/ViewCommon.h`'s `ViewBits`), Core's occluder rule it is classified by, its target (the engine's mask, or a texture it imports: `importTarget`), its labels and its optional reference parity (`referenceDue`); a shadow source (`ShadowSource`: sun, spot, point, focus), whose cameras its `cameraHooks` (called by source) write into Core's boards, and, for the spot and point sources, each light the selection keeps (`lightKept`). Core resolves each entry once at install (`Extensions::ShadowViewsSet`: each mode's phase, the set decision's readiness order, each engine view kind's source, the maps' import order; `Extensions::OcclusionOn`), read as data |
| Draw family | `Api/DrawFamilies.h` | a feature's instanced draws at Core's fixed slot per family (`DrawFamily::TreeInstances`; `Core/Render/DrawFamilies/FamilySlots.h`: its pipelines on Core's lane from the program source it names, published for the passes, its draw signature, a trees view's forward key and cull pass name, its PassCapture root, its part of the shapes key; `TreeInstancesLayout.h`: the GPU layout the shaders mirror), gated by an object class's toggle and set up while its scene resources (its tables) are: its frame callbacks (`serve`, the mirror drain at the frame requests; `decide`, the frame's ownership before the main cull, which Core publishes, `IndirectDraws::DrawFamilyOwned` and `PassCapture::SetFamilyOwned`, with the lane's pipelines asked through `DrawFamilyFrame::Pipelines`; `programSource`), its part of the main pass's executions (`declare`, its arguments and draw row; `revision`; `record`, with the slot's pipelines Core prepares: after the depth draws in both phases, and in the colour pass after the main draws under `colourRange`) and of a trees view's faces (`makeCull`, `declare`, `record`, `row`, and `lists`: the units Core's per-face lists hold with its tables), its cull and readback passes as GPU stages at the depth segment's slots, its tables as scene resources and their uploads as a commit hook. Resolved once (`Extensions::DrawFamilies`); a family no feature takes adds no passes |
| Main range | `Api/MainRanges.h` | an ordered range of the colour segment's second pass at Core's fixed slot for it (`MainRangeSlot`: the fixed slots `Fixed0`-`Fixed2`, whose objects take an ordinal each in the range's order, `SceneTables::rangeOrdinal` and `rangeCount`, written by the range's feature; the appended `Appended0` and `Appended1`, filled by the culling), named for its nvperf range (`cs.dclf.colour.<name>`), placed by its order among every registration's, optionally with a depth prepass (a fixed slot's draws first, depth only, in the decal depth pass before the main colour draws), and the class whose objects fill it. The slots, their count words and the object flag bits that select them are BuildDrawsCS's compile-time layout (`GpuLayouts.h`: `MainRangeCountWord`), which `Features/Manifest.cpp` checks a feature's slots against. Core's own ranges register first (`Extensions::CoreMainRanges`: Terrain Blending's groups, orders 2 and 3); the install refuses a slot or order taken twice and a depth prepass on an appended slot. Resolved once into the draw order (`Extensions::MainRanges`), which the colour passes walk (`MainOpaque.cpp`); a slot nobody registers is never drawn |
| Vertex stream | `Api/VertexStreams.h` | the second vertex stream of its entry class's objects (one: Core's draws have one second-stream slot; `Features/Faces`' face positions), a float4 per vertex in each object's range of a versioned buffer its scene resources hold: Core keeps the rows in the scene's tables (`SceneTables::streamRows`, the objects' `streamRow`, `kRetiredStreamRow`; written by the component that follows the class, copied with each publication and into each payload) and reads them as data - the builds' geometry draw per row after the geometry slots, named as the object's second stream (`StreamGeometry`, `AppendStreamRows`; an object of the class without its row waits), the fit (`SceneFit::streamVertices`, the resources' `fit`), the passes' vertex-buffer binding of the buffer `select` names at setup (`SceneBuffers::vertexStream`) - and embeds the buffer's address while the class is taken (`Extensions::VertexStreamTaken`); the stream's scene resources are set up, registered with a graph and required first (`Extensions::SceneResourcesOrder`; their setup giving no state leaves the scene's tables unmade), and its `upload` runs first in each commit, before the commit hooks, with the payload's rows (its count is the shadow report's "face regions uploaded") |
| Palette rows | `Api/FrameValues.h` | a class of FrameValues' items (`FrameValueItemClass`, Core's to decide: `ParticleSystem`, a particle system without a skin, named by the coordinator's plans, `PlacementPlan::Item::itemClass`) whose palette block it fills every frame in FrameValues' palette part (the skins' part): the block's size by its rule (`BlockRule`: rows a unit, the units clamped and rounded to a power of two; the walk reads it as data, `Extensions::PaletteRowsOf`), the geometry slot its draws read (`geometrySource`, at a record's write), its frame start (`FrameDriver::BeginFrame`) and kick (its frame input, `kPaletteRowsInputBytes` held by the frame's job), and the producer's run: `beginRun` with the run's items of the class, `sampleChunk` once a `CS.DCLF.FrameValues.Items` chunk with the chunk's writes (the block's rows, and the placement row's world and bound), `endRun`. The class's items are due every frame; Core samples each chunk's items in order, defers the class's, and finishes them (compares, marks, owes) where each stood, so the chunk's lists keep the run's order; it keeps the first-person writes' counters the first person's report reads (`PaletteRowsChunk::NoteFirstPerson`, `FrameValues::TakeFirstPersonPaletteStats`). Resolved once (`Extensions::PaletteRows`) |
| Object contributor | `Api/ObjectContributors.h` | three kinds, read as data where Core decides: membership by an engine key (a technique it ports: its eligibility under a toggle and its objects' record flags; a geometry type it ports: a class under a toggle, read by the eligibility, the walk and the captures); membership by a Core class, one enum value per class a feature takes (`EngineParent` and `ParentLeaves`: the leaves under a billboard, a switch node or an actor, read by the classification and the sun and light candidates; `ObjectClass`: a decal or a multi-index layer Core's ports derive but leave native, read by the ports' eligibility; an effect, read by the eligibility, the primary cull's membership passes, the captures and the census, and an opaque effect, read by the Effect port as taken whatever its toggle (`Extensions::ObjectClassTaken`); object and terrain LOD, read by the ports' eligibility and the LOD root's listing; tree LOD, read by Core's tree LOD path; the LOD classes, read by a view's LOD roots; `EntryClass`: a face shape whose records wait on the coordinator component that follows it); and derivations (the shading values it derives for a property flag of its own, and engine globals captured with the frame's: `FrameGlobals::features`). Adding a Core class is the same three steps for every enum: the value in the header, Core's test of it through its `Extensions` query, the feature's registration (the install fails a second claim) |
| Material port | `Api/ObjectContributors.h` (section 4) | an engine shader other than Lighting whose objects Core captures, classifies, joins and draws through the port while its object class is on (`MaterialPortContribution`): its property RTTI and material vtable (resolved once at install), the material bytes and texture fields a snapshot keeps, its constant space a stage (`MaterialStageSpace`: variables, sized and scratch ones, groups, first variables, names) and the rules Core reads as data (`MaterialPortRules`: pass enum base, deferred pixel bit, frame VS variable, technique rows, raw permutation, signature components, emissive always listed, billboard-turned and particle leaves); its callbacks per event, pure: `capture` (the property's record), `derive` (the descriptors or why it stays native: the eligibility, the joins, the primary cull's membership passes), `evaluate` (a material record), `shading` (a shading row), `variants` (the census). Resolved once by kind (`Core/Extensions/MaterialPorts.h`, `Extensions::MaterialPorts`: 0 Core's Lighting, `CoreLightingPort`; then the features' in registration order, the kind `RegisterMaterialPort` returns: Features/Effects' Effect port is 1, `kMaterialKindEffect`, checked at its registration), the kind the records carry (`PropertyRecord::port`, `MaterialSnapshot::port`, `LightingDescriptors::port`, `MaterialRecord::kind`), read as data where Core decides (`PortOfRtti`, `PortOfVtable`, `PortOn`, `PortRules`, `MaterialSpaceOf`) |
| Program family | `Api/DrawFamilies.h` (`ProgramFamilyContribution`) | a material port's keys drawn by the main pipeline set at Core's fixed slot per family (`ProgramFamily::Effect`; `Core/Gpu/Pipelines/ProgramFamilySlots.h`: its raster key bit, `KeyFamily`, three spare key bits, its stage-cache key bit, its program-id bit, its Z key's salt; its counters, `DrawPipelines::Stats::families`): its names (logs, key and recipe names), its HLSL source loaded with Core's (`ShaderPrograms::FindFamily`), its stages and build rule (`ProgramBuild::ForwardOrDecal`), the rules Core reads per object per frame as data (`Extensions::FamilyRules`, `FamilyRulesOfKind`: row blocks, root views forward only, billboard-turned key bits, its record bits) and its program source (render thread, the lane's inputs). Resolved once after the material ports (`Core/Extensions/ProgramFamilies.h`, `Extensions::ProgramFamilies`; Features/Effects' Effect family, `EffectPrograms.cpp`) |

Each contract's thread, timing and lifetime is documented in its header. A feature's install also installs its engine
hooks and subscribes its observers (`Diagnostics::Subscribe`, under Core's `Observe()` flags). Its thunks read the shell's state
through `Core/Engine/ShellState.h`: whether DCLF runs (`ShellState::Running`, the query the shell installs before the early hooks) and
where the engine's passes are (`ShellState::Passes`: in the depth pass, RenderDepth's control-driven argument), which the shell's
Main::RenderDepth thunk writes.

The headers in `Api/` name only standard and ORG types. The contexts their callbacks receive are opaque there. Core
defines them in companions under `Core/Extensions/` (`FrameValuePools.h`, `GpuStages.h`, `PayloadProducts.h`,
`SceneResources.h`, `Commits.h`, `FrameBindings.h`, `Reports.h`, `EngineEvents.h`, `TableFamilies.h`,
`CoordinatorComponents.h`, `ObjectContributors.h`, `ViewCommon.h`, `Views.h`, `ShadowViews.h`, `DrawFamilies.h`, `MainRanges.h`, `VertexStreams.h`, `PaletteRows.h`, `MaterialPorts.h`, `ProgramFamilies.h`), which features inside DCLF include for typed
access to Core's types.

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
re-keyed by the walk's rewrites, ordered when the joins are done, writing the tables' ordinals), its groups' ranges of the colour segment's
second pass on the main range contract (`Api/MainRanges.h`: `Fixed0` "decals-opaque", order 0, with the depth prepass; `Fixed2`
"decals-layers", order 1; `Fixed1` "decals-blended", order 4, after Core's terrain built-ins at 2 and 3) and its parity. What it leaves
Core is the main pass's structure: the ports' decal groups and states (the patch's decal key, `kObjectDecal` and its group bits, the raster
keys), the tables' ordinal columns (`rangeOrdinal`, `rangeCount`), the main build's decal inputs and templates, the sequence buffer's range
slots and count words (`GpuLayouts.h`, `BuildDrawsCS`), and the colour passes that draw the registered ranges in their order (`MainOpaque.cpp`). What SwitchNodes and HiddenNodes leave Core is the visibility every object is judged by: the switch
selections the primary cull follows under every stood-in root whatever the toggle (the index stores' and child edits' events, without
which DCLF does not run; the pending switches, the catch-ups, the cut's selection reads, tree LOD's switch), and the hidden-bit
vocabulary (the channel, the chains and their drain, the frame's capture of the culls' bits, the Hidden verdict, the LOD gates' veto).
`Features/Faces` followed: the face morphing's thunks and the head snapshots (`FaceSnapshots`), and the face shapes as a coordinator component following
their entry class (`EntryClass::FaceShape`, taken once the thunks are in): their heads' lists, the heads' publications taken as each walk
begins and applied as the delta walk schedules, each written shape's snapshot, region of the positions buffer and stream, and the regions
retired as their streams go. Its vertex stream followed, on the vertex stream contract (`Api/VertexStreams.h`): the positions buffer as
scene resources (`face-positions`: set up, registered and required first, its fit), the commits' uploads into it (the stream's `upload`,
with what each positions version holds), and the stream's registration (`EntryClass::FaceShape`, named "face positions" for the
named-address parity). What it leaves Core is what every draw of the stream reads, generic: the stream rows in the scene's tables
(`streamRows`, the objects' `streamRow`, `kRetiredStreamRow`, the released rows its component frees regions by) and the payloads' copies of
them, the builds' second-stream geometry draws (`StreamGeometry`, `AppendStreamRows`, `SetSequenceStream`), the fit's test, and the passes'
bindings (`SceneBuffers::vertexStream`); and a face shape's classification (`Tracked::faceShape`, its head held from the attach's pins, the
per-frame face trait and the walk's face names in the reports), which needs an entry-class contribution to the classification and the
attach. The rows are Core's columns, not a table family's: a family is copied only when its stamp moves and its stamp is in the
publication key, so a head's new snapshot taken in place would publish where it does not now, and the replay line would gain its count.
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
`Features/FirstPerson` followed: its switches and toggle (which needs the skinned and the actors), its report lines at the draws' async work
and the skeleton's parity; then, on the view contract, the whole first-person view: its set phase and view bit (`kSetFirstPerson`,
`kViewFirstPerson`), its contribution (a category root, the first-person root, with its root capture; one face with a forward part and a
depth pass; dropped while off; the main colour viewport; withheld in the main accumulators, with the first-person accumulator; drawn into
kPOST_ZPREPASS_COPY), its coverage callback, its frame-start work (the latch, targets, skeleton walk, pipelines and resources), its two epochs
on the view engine's inputs and submission (the same skip causes and log lines), its state and report counters (`FirstPersonFrame`), and the
two engine hooks that run the epochs (RenderDepth's first-person draw and Main::RenderFirstPersonView's accumulator render, installed from its
install; they read `ShellState`). What it leaves Core is what every bucketed view shares, keyed by its slot or its membership: the epochs'
passes and forward part (`ViewEpoch*.cpp`), their revisions (`ViewKind`, `Epoch::FirstPersonDepth`/`FirstPersonColour`, the shape, key group
and coverage inputs), the slot's state (`ViewEpochState`) and the first-person forward keys' targets (`DrawPipelines`' `kForwardFirstPerson`);
a CategoryRoot view's scene side (the root as a category node, the classification's root stop, `NoteFirstPersonHidden` and the misfiled
observer, the set's decisions and their parity, the decals' forward keys, the placements' per-frame chains, the frame requests' root, the
frame globals' culled bits); FrameValues' root-view rows, palettes and particles; `PassCapture`'s withholding (InMainRenderers) and the
census's view. BuildDrawsCS mirrors `kViewFirstPerson` (the manifest checks it).
`Features/Reflection` followed, the second view on the contract: its switch and toggle (listed where Core's were, after the precipitation
mask's, and registered right after the occlusion maps' toggles, so the toggle's bit in the packed word is where Core's was), its set phase and view bit (`kSetReflection`,
`kViewReflection`; no HLSL mirror), its contribution (the LOD techniques of the main set, a LOD Lighting technique's slots only; six faces
with tree LOD; its last shape kept while off; its imported target's area; withheld in renderers of its own; drawn into the engine's cube
with DCLF's own depth; covered before its first coverage and on a failed graph), its coverage callback, its work after tree LOD's decision
(the faces' targets, the LOD slots' forward pipelines, its resources) and at the depth commit (the frame's faces in one epoch,
`CS.DCLF.ExecuteReflection`, with the same skip causes and log lines), its state and report counters (`ReflectionFrame`, its lines at
`AsyncWorkViews`), the faces' camera parity, and the water reflection's engine hooks (`ReflectionFaces`: the update, its LOD add-roots, the
face render with its orientation and draws; the feature's early hooks, first of them, gated by `ShellState::Running`; the face render names
the cube camera's accumulators as the view's renderers, `PassCapture::SetViewRenderers`, and its plain mode as their withholding,
`SetViewWithholdActive`). What it leaves Core is what every bucketed view shares (the epoch's passes, its revisions, the slot's state, a
LodTechniques view's slot and phase readiness, the colour target's import, `PassCapture`'s OwnRenderers withholding and the census's view)
and the view engine's parts that read tree LOD and the LOD gates (the classes' contributions as data once `Features/Lod` and `Features/TreeLod`
moved; the feature includes neither): the tree LOD drawn per face (its pipeline, ownership and row: `IndirectDraws::ResolveViewTreeLod`,
`DecideViewTreeLod`, `SubmitViewEpoch`'s row), the cube camera's LOD roots its faces draw in the engine's place (`IndirectDraws::ViewLodRootsOwned`,
with `LodGates` and `PassCapture::kLodRoots`/`kTreeRoot`), the residue watch under them (`PassCapture::WatchReflectionResidue`,
`Policy::KeepReflectionRoots`, the residue classes), and the forward targets the faces' pipelines are built for (`DrawPipelines::SetForwardTargets`).
`Features/OcclusionMaps` followed, the first feature on the shadow-view contract: its switches (`CS_DCLF_SKYLIGHT`, `CS_DCLF_PRECIPITATION`,
listed where Core's were: before `CS_DCLF_OPAQUE_EFFECTS`, after the trees' list-all switch, with the reflection's and the first person's
following them there) and its two toggles (which need the shadows; the first feature toggles registered, so their bits in the packed word
are where Core's were, the feature listed first), the maps' set phases and view bits (`kSetOccluderSky`, `kSetOccluderPrecipitation`,
`kViewSkyOccluder`, `kViewPrecipOccluder`, their values unchanged; no HLSL mirror; the manifest checks them and Core's `kCoreSetPhases` and
`kCoreViewBits` no longer hold them), and the two `RegisterOcclusionMap` contributions: the sky map (Skylighting's occluder rule, its target
Skylighting's own occlusion texture, imported by `importTarget`) and the precipitation mask (its rule, the engine's mask), both available
while Skylighting is loaded (`skylighting.loaded`, read as data), with their labels and reference parity (`Policy::RenderOcclusionReference`).
What it leaves Core is the shadow engine's: the occlusion epoch on the shadow commit, the maps' modes, view slots, depth targets and technique
columns, the occluder rules' bodies (`Coordinator::OcclusionTechniqueOf`), the set decisions and the reports, all reading the maps' entries.
The shell keeps its six occlusion methods and the 0x1C capture, which Skylighting calls (`DrawcallLimitFix::OcclusionReady` and its
siblings), and the diagnostics keep `CS_DCLF_SKYLIGHT_PARITY`'s compare (`Diagnostics/Feature/Skylight`: its switches, its `Observation`
flag and its place among the feature observers' installs are Core's).
`Features/Shadows/Sun` and `Features/Shadows/Local` followed, the shadow sources: the sun's (`RegisterShadowSource({Sun})`, and its camera
hook, BSShadowDirectionalLight::UpdateCamera, writing the `Sun` board) and the spot, point and focus sources (the spot and point sources'
`lightKept`, each kept light's views into the `Lights` board, and the focus cameras' hook, BSShadowDirectionalLight::sub, writing the
`Focus` board). They register no switch, toggle or bits (the sources own no phase, view bit or toggle), and their hooks install by source
(`Api::Feature::cameraHooks`), so the install log keeps its order: the sun views, the light selection, the focus views. What they leave
Core is the shadow engine (both epochs, the capability and captures, the views list), the light selection, the boards and the camera
math, the engine's focus targets, the sun's accumulation, the caster phases and modes, and the shadow toggles; the local shadows' port
(`Scene/Ports/LocalShadows`) moves with LocalLights.
`Features/Lod` and `Features/TreeLod` followed. Lod: object and terrain LOD's switches (`CS_DCLF_LOD_OBJECTS`, `CS_DCLF_LOD_TERRAIN`, listed
where Core's were, before `CS_DCLF_LOD_GATES`) and toggles (after the refraction's, behind the particles', billboards' and switch nodes'),
and two object classes (`ObjectClass::LodObjects`, `LodTerrain`), which Core reads as data: the Lighting port's LOD techniques and flags
(`Ineligible::Lod`), the sub-index shapes' eligibility, `TES::lodLandRoot`'s listing as a category node (`FrameRequests`), and the cube
camera's LOD land and objects roots (`ViewLodRootsOwned`). TreeLod: its switches (`CS_DCLF_LOD_TREES` after Lod's, `CS_DCLF_TREE_LOD_AUDIT`
after the texture paths' trace) and toggle (after Lod's), the tree LOD class (`ObjectClass::TreeLod`: the draw family's gate, the revisions' trees
pipeline, a trees view's `ResolveViewTreeLod`), the source of Core's tree LOD channel at its early hooks (the refill, block update, AddGroup
and RemoveGroup hooks, pushing through `Push`, where Core's scene events installed them; between the billboards' early hooks and
the hidden nodes') and the tree LOD audit. Both are listed after SwitchNodes, Lod first, so the toggles' and switches' order is Core's.
What they leave Core: the LOD gates whole (`CS_DCLF_LOD_GATES` and the shell's install, `Engine/Cull/LodGates`, the gate event, the
coordinator's gates and the publication's flips, `FrameView`'s flip at adoption, the hidden stores' veto, their reports and observers),
which need an engine event channel contract for a gated swap (its capture, flip, outcomes and veto). Object LOD's segment events followed
on the engine channel contract (`Api/EngineEvents.h`, `RegisterEngineChannel`): the writers' hooks at Lod's early hooks (between the billboards'
and tree LOD's; `LodSegmentHooks.cpp`), its keyed channel ("object LOD segments": `AfterSwitches`, drained after the switch events as Core's
queue was; `KeepKeys`; `namesKeys`, as the mirror parity counted them), its coordinator component (`events.keys`: each named tracked shape's
ranges taken again from the mirror into Core's kept ranges, `SetEntryRanges`, the shape's rewrite on a change), its report line at
`SceneCoordinatorSegments` (`componentText`) and its parity (`Diagnostics/LodSegmentParity.cpp`). What it leaves Core is what the walk reads:
the kept ranges (`lodRanges`, from the attach's capture, erased with the entry), the segments' layout and capture (`Core/Engine/LodSegments.h`,
`GeometryRecord::segments`) and the range slots (`ResolveLodRangeSlots`).
Tree LOD's renderer followed, on the draw family contract (`Api/DrawFamilies.h`): Features/TreeLod takes the `TreeInstances` family. It keeps
the channel and the render thread's mirror (`TreeLodMirror`, drained by the family's `serve`, running or not), its tables as scene resources
(the mirror's shape and mesh slots posted with the revision's inputs, `postInputs`; the draw row and list header the depth commit latches,
`latched`) and their uploads as a commit hook (the depth commit's, through `CommitContext::latched`, with `TreeLodMissed`), TreeLodCullCS's
two phases and the counts' readback as GPU stages at the depth segment's slots (`cs.dclf.z.tree-lod-cull`, `cs.dclf.tree-lod-cull-phase2`,
`cs.dclf.z.tree-lod-counts`), the family's decision (the mirror's shapes, the atlas, the lane's pipelines), program source and draws, a trees
view's faces (the per-face cull, the faces' declarations, draw and row, the lists' units), its report lines at `AsyncWorkFamilies` and
`SceneFrame`, and the mirror's parity. What it leaves Core is the family's slot: the pipelines on the lane (`DrawPipelines`' tree LOD family,
`ForwardPipelineKey::kForwardTreeLod`) and their publication (`SceneBuffers::familyPipelines`), the ownership published from the decision
(`IndirectDraws::DecideDrawFamilies`, `PassCapture`'s withholding of the family's engine shapes), the shapes key's part, a trees view's lists,
rows and latch (`ViewResources`, `ViewLatchLayout`, `CommitView`), the GPU layout the shaders mirror (`TreeInstancesLayout.h`) and
`TreeLodCullCS.hlsl`'s place.
The particles' palette rows followed, on the palette rows contract (`Api/FrameValues.h`, `PaletteRowsContribution`): Features/Particles
takes `FrameValueItemClass::ParticleSystem`. It keeps the snapshot store (`Features/Particles/ParticleSnapshots`: the writer's thunks, the
followed records, the expansion), the class's rule (five rows a particle of its capacity bucket), the quads' geometry slot (the renderer's
quads, one slot a bucket), the frame's cameras (the main one and the first person's, from its frame start and kick), and the run: the
records followed at its start, each chunk's members expanded into their blocks with their placements, the table published at its end. What
it leaves Core is the class and where it is decided (the plans' `itemClass` for a particle system without a skin, the walk's block and
slot calls for a taken particle system, `ParticleSystemTaken` and the Effect port's particle descriptors), the class's items due every frame,
the deferral in FrameValues' chunks (sampled in order, finished in place), and the first-person writes' counters the first person's report
reads (so Features/FirstPerson includes no Particles header). `Features/Effects` followed: its switches (`CS_DCLF_EFFECTS` after the forward epoch's;
`CS_DCLF_OPAQUE_EFFECTS` after the primary exclusion's, behind tree wind's list-all, with the occlusion maps', the reflection's and the first
person's switches still listed before it and the billboard particles' after it: the switch contract's anchors may name a feature's switch,
registered before or after, `CheckSwitchAnchors`), its toggle (right after the forward epoch's, before the particles' and billboards', which
need it: the feature is listed before them), two object classes (`ObjectClass::Effect`, read where Core read the toggle: the eligibility, the
primary cull's membership passes, the scene capture's and the effect controllers' material captures, the census; `ObjectClass::OpaqueEffect`,
taken unless the switch is 0, read by the Effect port's opaque rule as taken, `Extensions::ObjectClassTaken`, the rule's toggle being the
decals'), its report lines among the forward epoch's (`ReportPoint::AsyncWorkForward`, inside `ForwardReport`, so they print when the forward
epoch's do) and the Lighting variant's parity (`<- EFFECT LIGHT`, its own observer of the withheld blended passes). The Effect port followed it
there, on the material port, program family, frame constant row and frame texture contracts (Core classifies every effect-shader item through it
before a feature sees it: the particle systems and the billboards' effect shapes too): its constants (`EffectConstants.h`: the variable space,
`EffectFlag`, `kMaterialKindEffect`, the frame rows, `kSoftDepthRegister`, the record bits), its descriptors and variants (`EffectDescriptors`),
its material records (`EffectMaterial.cpp`, kind 1: `RegisterMaterialPort` returns it and the feature checks it), its capture and shading
(`EffectProperty.cpp`), its frame rows and soft depth (`EffectFrame.cpp`) and its program family (`EffectPrograms.cpp`). What it leaves Core is
what Core decides by: the verdicts (`Ineligible::EffectOpaque`/`EffectVariant`), the Effect family's slot (`kRasterEffect`, its spare bits, its
programs' and pipelines' counters), the builds and passes, Effect.hlsl (package/Shaders), and the effect controllers' material-write hooks
(`Engine/Capture/MaterialWrites.cpp`, waiting on a material-write channel). The billboards' per-object path stays Core's for now: the billboard placement
row FrameValues samples per moving object (`Engine/Capture/Billboards`, Effect.hlsl's layout; a per-row function, not a class's block). The
extension points still to come, as the next features need them:
- frame-value contributors that write into a built-in part per chunk (the billboard placement row, Advanced Skin's wetness in the shading
  rows);
- per-object record columns (the record's TreeParams and WindTimers block, `treeAnim`, is still Core's: the Lighting port's
  layout of `DCLFObjectRecord`);
- more engine event channels: the scene-stack kind, root marks, entry vetoes and the commit's settle (the LOD gates); the stand-in change
  still names the BSTreeNodes among its roots;
- material contributors;
- the shadow views' sibling of the view contract (`Api/ShadowViews.h`): the occlusion maps and the shadow views on the outer layer the
  bucketed views share (view points, the running query, the phase and view-bit manifest, coverage callback, withholding, reports);
- the shared-bit manifest (object and record flags);
- entry-class contributions to the walk's classification and the attach (a face shape's head pin, its per-frame trait).

## The view contract

The engine views Core's bucketed view machinery draws (`ViewTraits`, `ViewFrame`, `ViewResources`, `ViewLatchLayout`, `PlanViewBuckets`,
`MakeViewShape`, `CommitView`, `ViewExtension`) are taken by features through one contract: `Api/Views.h` (`RegisterView`,
`RegisterViewWork`), with its Core companion `Core/Extensions/Views.h`. The reflection's faces and the first person are on it. The
occlusion maps and the shadow views are another family (`ShadowFrame`), on the sibling contract below; what both families share is
`Api/ViewCommon.h` (`ViewPoint`, a feature view's `ViewBits`) and its companion `Core/Extensions/ViewCommon.h`. The portals, local
lights, GPU occlusion and forward epochs are not views.

- **Core's fixed slot per view.** `Api::EngineView` is Core's slot order (`Reflection`, `FirstPerson`): the epochs' indices, the latches,
  the revision key groups, the coverage and the setup run in it. `Core/Render/Views/ViewSlots.h`'s `kViewSlots[v]` reserves the view's
  epochs (`Epoch::*`, whose `kEpochTraits` order is load-bearing), its `RenderGraphRuntime::Segment`s, its key group (the shapes-made
  report's names, "reflection" and "first person") and its graph extension's id. A new view is the enum value, a CS segment, its slot
  row, then the feature's registration.
- **The feature supplies data and per-frame callbacks.** `ViewContribution`: the view, its toggle, its set phase (one bit, the feature's)
  and its shape's view bit (one bit, the feature's: both checked against Core's and each other's by the install and by
  `Features/Manifest.cpp`), its membership (`CategoryRoot`: the entries under the root `captureRoot` names, an exclusive phase whose main
  mask is the view bit alone; `LodTechniques`: a main-eligible slot whose pipeline is a LOD Lighting technique, beside the main phase),
  its key rule (`lodTechniquesOnly`, `forwardRasterToForwardPart`, `keyFlags`), its shape (`faces` up to `kMaxViewFaces`, `trees`,
  `forwardPart`, `depthPass`, `viewBits`, names and latch labels, the initial `depthRange`), its life (`KeepLast`, `DropWhenOff`: dropped
  from the revision while off), its area (`Target`, `MainColourViewport`), its withholding (`InMainRenderers` with the `accumulators`
  callback; `OwnRenderers`, whose renderers its engine hooks name), its target import (`ColourWithOwnDepth`, `DepthTarget`), its coverage
  before the first decision and on a failed graph, its decals as forward objects, and its `covered` callback (required; render thread,
  once a frame at `DecideCoverage`, the views in slot order, over Core's precomputed, side-effect-free `ViewCoverage`). `ViewWork` runs
  the feature's work at a `ViewPoint` (`FrameStartBeforeCoverage`, `FrameStartAfterTreeLod`, `DepthCommitted`), once a frame on the render
  thread, the views in slot order and a view's work in registration order, whatever the toggle (the work reads it).
- **Resolved once, read as data.** `Extensions::ResolveViews` (the end of `InstallFeatures`) makes each view's `ViewEntry` (none for a view
  no feature takes: every per-view loop skips it) and derives what the per-object paths read: the set's phases, `rootPhases` and
  `lodPhases`, the view bits, the root views, the set decision's order (the CategoryRoot views first),
  and the InMainRenderers and OwnRenderers views. Classification, the set decisions (`SetParticipation`, `DecideSlot`, the slot
  readiness by membership: `IndirectDraws::ViewSlotReady`, a LodTechniques view's phase readiness: `PhaseReady`), the lacking phases
  (`lodPhases`: FrameView's third count), the builds' masks, the frame requests' roots, the revisions (traits, forward keys, key groups,
  `DropWhenOff`) and `PassCapture` read the entries; no per-object loop calls a feature. Then `Extensions::ResolveViewBits` checks
  both families' phases and view bits (one bit each, none Core's nor another view's) and makes `ViewBitsOf`'s table by phase mask and
  the lacking counts' phases (`Extensions::ResolvedViewBits`: the occlusion maps', then `lodPhases`).
- **The view engine.** A feature's work and epochs reproduce Core's own over the view's slot (`IndirectDraws.h`, "The view engine"):
  `ViewState`, `MainTargetFormats`, `SetViewTargets` (published for the scene lane), `PostViewInputs`, `SetViewDepthRange`,
  `ExchangeViewForwardCompatible`, `ResolveViewPipelines` (wanted slots or every slot; with the programs and waits counted on request),
  `DropViewPipelines`, `SetUpView`, `ImportViewTarget` (by its target import), `TakeViewInputs` (`DepthThisFrame`,
  `DepthThisFrameColourBefore`: the frame before's colour frame record, the target imported after; `ColourThisFrame`), `SubmitViewEpoch`
  (the revision's recording and shape, the payloads held, the faces committed: `Submitted`, `NoRevision`, `NoOwners`, `Failed`, which the
  feature maps onto its own skip causes and log lines), `ViewCovered`, `ViewDrawable`. A trees view's tree LOD per face is the view engine's
  (`ResolveViewTreeLod`, `DecideViewTreeLod`, the row `SubmitViewEpoch` commits), drawn by the `TreeInstances` draw family's faces while
  the family is on (Features/TreeLod's, under `ObjectClass::TreeLod`), as is the cube camera's LOD roots' ownership (`ViewLodRootsOwned`: the tree
  root by the faces' tree LOD, the LOD land and objects roots by the LOD classes, Features/Lod's, and Core's LOD gates); a view feature
  reaches them through these calls, never a LOD feature's headers.
- **Withholding.** `PassCapture` keeps each view's renderers and coverage (before the first decision, its `coveredInitially`). An
  InMainRenderers view's member is tested inside `WithholdMain`, before the main phase's; an OwnRenderers view's after it
  (`WithholdOwn`): its member's pass into the renderers its hooks named (`SetViewRenderers`) while its render withholds
  (`SetViewWithholdActive`) and its epochs draw, or tree LOD's while its tree LOD per face is DCLF's (`SetViewTreeLodOwned`).
  `SetViewRendering` marks its render for the observers.
- **Hooks and shell state.** A view feature's engine hooks are its own (from its install, or its early hooks when Core's engine side and
  observers must find them in place); they read `Core/Engine/ShellState.h` (`Running`, `Passes`). The shell's `Main_RenderDepth`
  world-drawn point runs `FrameDriver::DepthCommitted` (the depth commit, then the `DepthCommitted` work).
- **Reports.** A view's lines go at `ReportPoint::AsyncWorkViews` (after the visibility lines, before the forward epoch's) or
  `AsyncWork` (after it).

## The shadow-view contract

Every caster view (sun, spot, point, focus) is drawn by one shadow engine: one execution (`ExecuteShadowFrame`), one `ShadowPayload`,
one set of resources and index pool, one latch block and one key group (`kKeyShadow`); the occlusion epoch (`ExecuteOcclusion`) draws
from the same commit. Casters are keyed by the engine's render mode, not by source, so the sources own no phase or view bit. The
features take the parts that are theirs through `Api/ShadowViews.h` (`RegisterOcclusionMap`, `RegisterShadowSource`), with its Core
companion `Core/Extensions/ShadowViews.h`; the engine itself, its capability and captures, both epochs, the light selection, the camera
boards and their math, the sun's accumulation, the caster phases and modes, and the shadow toggles stay Core's.

- **The occlusion maps.** `OcclusionMapContribution`: the map (Core's slot order, `kOcclusionSky`, `kOcclusionPrecipitation`: its mode,
  view slot, depth target and technique column), its toggle and `available` flag, its `ViewBits` (the feature's own), the occluder rule
  Core's classification applies per object (`OccluderRule`, read as data), its target (`EngineMask`, or `ImportedTexture` with
  `importTarget`, called once as the capability sets the maps up, in Core's import order: the precipitation mask, then the sky map), its
  labels (the view-mask parity's list, the report lines', the imported target's) and its optional `referenceDue` (the engine's
  reference frame for the map's parity, asked by the shell's `OcclusionReferenceFrame`). `Extensions::OcclusionOn` is whether DCLF
  draws a map (taken, available, its toggle on); a map's phase and view bit reach the set decisions, the classification, the builds'
  masks, both epochs and the reports through its entry.
- **The shadow sources.** `ShadowSourceContribution`: the source (`Sun`, `Spot`, `Point`, `Focus`), and for a spot or point source
  `lightKept`, called at the end of the light selection for each kept light of its kind, in the selection's order. Its cameras are
  the feature's to compute, from its `Api::Feature::cameraHooks` (called with each source in `ShadowSource` order, Core's light
  selection installed right after the sun's, so the install order is the sources' whatever the list's), into Core's boards
  (`Core/Render/Shadow/ShadowCameras.h`: `Sun`, `Lights`, `Focus`, render thread, read in place by the capture, the inputs' preparation, the sun's accumulation after its
  `bitsReady`, the local shadows and the actors' rate). The view list covers a view only while its kind's source is taken
  (`ShadowViewSet::sourceOf`), and the capture reads its camera from that source's board.
- **Resolved once.** `Extensions::ResolveShadowViews` (in `InstallFeatures`, after the views) makes each entry and derives each mode's
  phase (`phaseOfMode`), the set decision's readiness order (the casters', then the maps'), each engine view kind's source and the
  import order. A second registration of a source fails the install.
- **Where it stands.** The occlusion maps are `Features/OcclusionMaps`' (their switches, toggles, bits, Skylighting's flag and texture
  and their reference parity). The sun source is `Features/Shadows/Sun`'s (its UpdateCamera hook, `SunViews`, writing the `Sun` board),
  and the spot, point and focus sources are `Features/Shadows/Local`'s (the kept lights' views, `LightViews`, as their `lightKept`,
  writing the `Lights` board; the focus cameras' hook, `FocusViews`, writing the `Focus` board). Core keeps the light selection, which
  hands the local sources their lights, the engine's focus targets (`ShadowCameras::EngineFocusTarget`, which the focus parity reads
  too), and everything above.

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
