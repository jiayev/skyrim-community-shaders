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
                  Shadows/{Sun,Local}, Portals, GpuOcclusion, LocalLights, Skins, Actors, Faces, Particles, Billboards,
                  Decals, Effects, Landscape, Fades, Lod, TreeLod, TreeWind, TextureTransforms, HiddenNodes, SwitchNodes)
  Integrations/   one folder per CS feature DCLF supports, written against Api/ (AdvancedSkin, TerrainBlending, LightLimitFix; TruePBR, ... to come)
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

Features register at install, before DCLF's threads start. Their callbacks run per phase or per chunk, never per object (the exceptions
run per event or per walked entry, where Core already classifies: a material port's callbacks, an occlusion map's occluder rule).
The bits that shaders share stay compile-time: each feature declares its own, and a core manifest checks that they don't
overlap.

## The extension points

A feature is an `Api::Feature`: a name and an install function. It is listed in `Features/FeatureList.cpp`, the one place
that shows what DCLF does. The shell's `PostPostLoad` calls `Extensions::InstallFeatures` before DCLF's threads start.
Install registers the feature's parts through `Api::InstallContext`. The registry (`Core/Extensions/Registry`) is
read-only after that. A feature may also name early hooks (`Api::Feature::earlyHooks`): engine hooks Core's engine side must
find in place when it installs (a vtable slot the primary cull takes, a call site the primary cull requires: Features/Portals' walk,
noted with `Extensions::NoteMainWalkInstalled`; the source of an engine event channel Core follows, or a writer completing Core's movers
on a move-writer slot it reserves first), which
the shell runs after Core's scene events and before DCLF's own engine hooks and observers (`Extensions::InstallEarlyHooks`). And it
may name source hooks (`Api::Feature::sourceHooks(Api::ShadowSourceHooks&)`): a shadow source's engine hooks, its culls' (which call
`ShadowSourceHooks::CullsInstalled` once they are in, read as `Extensions::SourceCullsInstalled(source)`) and those that write its cameras
into Core's boards (`Core/Render/Shadow/ShadowCameras.h`), which the shell runs where the shadow sources' hooks install
(`Extensions::InstallSourceHooks`, before the primary cull). They are called once per source, with the source, in `Api::ShadowSource`
order (at each source the features in the list's order): Features/LocalLights' light selection installs at the Spot source, Features/Shadows/Local's
point culls at the Point source, so the hooks' install order, and the install log's, is the sources' (the sun's accumulation and
UpdateCamera, the light selection, the point lights' culls, the focus cameras), whatever the features' places in the list.

An integration (a CS feature DCLF supports) is an `Api::Feature` too, listed in `Integrations/IntegrationList.cpp` (`Integrations::List`,
ordered as `Features/FeatureList.cpp` is) and installed by `Extensions::InstallFeatures(features, integrations)` after every feature and
before the resolutions, so what it registers may name what a feature registered (Integrations/AdvancedSkin's wetness: Features/Actors'
published actor-owned class). It has no early or source hooks (the install fails otherwise). Its parities are in
`Integrations/<X>/Diagnostics/`, called from the Diagnostics observers that report them (CaptureParity's skin line, SceneFrameLines'
shading line), their text unchanged.

| Contract | Header | What a feature gets |
| --- | --- | --- |
| Frame-value pool | `Api/FrameValues.h` | rows sampled every frame on DCLF's pool into a pooled backing the frame reads by descriptor (`Extensions::FrameValuePoolIndex`, `Core/Extensions/FrameValuePools.cpp`); its own frame-start reads (`frameStart`), the producer's view of the frame (`FrameValuePoolProducer`: the globals, the kick's input, the texture-transform buffer the frame's shaders read, `TextureTransformBuffer`), and the rows once the producer is done (`FrameValues::ProducerDone`) |
| GPU stage | `Api/GpuStages.h` | a compute or copy pass (`GpuStageKind`) at a named slot of an epoch (`DepthSegmentBeforeBuild`, `DepthSegmentNextFrameState`, and around a draw family's depth draws: `DepthSegmentBeforeDepthDraw`, `DepthSegmentPhaseTwoBeforeDraw`, `DepthSegmentAfterPhaseTwo`), on a chosen queue (or none asked: `GpuQueue::None`), its programs set up with the scene's buffers, and optionally the segment's latch word |
| Payload product | `Api/Payload.h` | data built with every main payload on the builder, carried with it, and installed with it on the render thread |
| Scene resources | `Api/SceneResources.h` | state held with the scene's tables: versioned buffers registered with the scene's set, their graph registration, each revision's requirement (with the words it posts with the revision's inputs, `postInputs`, compared for a new revision), the slots they hold of the objects (`SceneFit`), and the buffers the depth commit latches (`latched`: the main latched targets and the depth latch's layout) |
| Commit hook | `Api/Commits.h` | its part of each commit's frame data (depth, colour, shadow), staged into the adopted versions on the render thread, or latched into a main commit's latched copies (`CommitContext::latched`), with owners its execution holds (`bindingOwners`) |
| Frame binding | `Api/FrameBindings.h` | a buffer every draw reads at a declared texture register, given by the frame records (main, shadow, the builds' embedded addresses) and checked by the named-address parity |
| Frame constant row | `Api/Commits.h` | a float4 every draw reads in Core's feature rows of PS b13 (`FrameConstantRow`: c17 up to `Extensions::kFrameConstantRowsEnd`, Core/Extensions/Commits.h), sampled by each main commit and latched in ascending row order (`LatchFrameSlots`, `MainLatchedLayout`); Features/Effects' camera planes, light colour and blood (c17-c19, `EffectFrame.cpp`) |
| Frame texture | `Api/FrameBindings.h` | an engine render target's view every colour draw reads at a declared texture register (`FrameTexture`), sampled once per colour commit before its frame textures are resolved (`ResolveFrameTextures`, `Extensions::FrameTextureIndices`), checked against Core's registers and the frame bindings'; a null view reads the null texture, or with `capturedWhenNull` the engine view the colour commit captured at the register (`psViews[t]`); Features/Effects' soft depth, t118 (`EffectFrame.cpp`); Integrations/TerrainBlending's mask, t55 (its terrain depth while it holds the terrain, else the captured view) |
| Frame graph buffers | `Api/FrameBindings.h` (`FrameGraphBuffers`) | another CS feature's graph buffers every colour draw reads at consecutive texture registers (`InstallContext::RegisterFrameGraphBuffers`: `name`, `firstRegister`, `resources`, the ORG ids the colour passes declare, as many as are non-empty and without a gap; `indices`, render thread, at the resources' check and once per colour commit: each resource's bindless index, false leaving the registers to the frame buffers the commit captured there). Checked against Core's registers, the frame bindings', the frame textures' and each other's (the install refuses an overlap, a register outside t16-t127, or past `Extensions::kFrameGraphBufferSlots`, 2, of up to 4 registers each); Core keeps the graph's declaration (`Passes/Common.cpp`), the frame slots' skip (`Extensions::FrameGraphBufferRegister`) and the commit's indices (`Extensions::FrameGraphBufferIndices`, `Core/Extensions/FrameBindings.h`). Integrations/LightLimitFix's lights, light index list and light grid (t35-t37, `cs.llf.*`) |
| Switches and toggles | `Api/Settings.h` | its `CS_DCLF_*` switches, listed where it says beside Core's or another feature's (right after a switch, or right before one: Core's, or a feature's registered before it or after, checked once every feature registered, `CheckSwitchAnchors`), and its live toggles, in the menu and the packed word, placed right after (`after`) or right before (`before`) a toggle, Core's or any feature's (placed by `Toggles::SeedRegistered` once every feature registered); a toggle may need Core's or one listed before it. A Core flag may need a feature's toggle by name (`ToggleInfo::needNames[2]`, resolved by `SeedRegistered` into its needs at the same index: one listed after it fails the install, a missing one leaves the flag never in effect; Core's "exclude primary entries" needs Features/Shadows/Sun's "exclude sun entries") |
| Report lines | `Api/Reports.h` | its lines at a named point of the periodic report, logged (`ReportLines::lines`, handed the report's frame) or composed into the report's text (`text`), as the point's kind says (`Api::ReportPointComposesText`); the coordinator's points take a coordinator component's lines too, with its context (`componentText`, `Api::ReportPointTakesComponents`). On the render thread: `RenderThread` (logged, first, at Present once a frame number, the lines testing their own report frames: Features/Shadows/Sun's accumulation lines, before the primary cull's); `PrimaryCull` (logged); `PrimaryCullSources` (logged, handed the frame, right after the primary cull's, before the stand-in walk parity, on the same report frames: Features/Shadows/Local's point culls); `AsyncWorkViews` (text: the views' lines in the draws' async work, after the visibility lines, before the forward epoch's); `AccumulatedWrites` (text: inside the accumulated writes' lines, after the every-listed-root lines, before the accumulated parity's; Features/Actors' actor rate); `AsyncWorkForward` (text: among the forward epoch's, after its refraction line, on a report that prints them); `AsyncWork` and `AsyncWorkFamilies` (text: the draws' async work, then the draw families' after it); `SceneFrame` (text: the render thread's scene lines); `GateReleases` (logged: the LOD gates' releases); `ShadowViews` (logged: the shadow views' lines in the statistics, before the shadow counters' reset; Features/OcclusionMaps' map lines); `Statistics` (logged: the statistics' last). On the scene lane, in the coordinator's report pass (`Coordinator::ComponentsReportLines`, `ComponentsReportText`): `SceneCoordinatorGates` (logged with the scene frame: the LOD gates' line); `SceneCoordinatorSet` (logged, right after the set's line, before its leaves by cause: Features/FirstPerson's filing, held-out and takes-no-part lines); `SceneCoordinatorSegments` (text: the object LOD segments' line); `SceneCoordinatorMaterials` (text, after the material records' line: Features/Decals' order parity); `SceneCoordinatorDelta` (text fragments, each "; ...", at the end of the scene delta line: Features/Faces' publications); `SceneCoordinatorLastPass` (logged, the last pass's statistics after the fading line: Features/Skins' skinned line, then Features/Decals' candidates) |
| Stand-in observer | `Api/EngineEvents.h` | the primary cull's stood-in roots as they change (`changed`: the roots that left and joined, nothing else; a feature picks its own out of them, as Features/TreeWind's tree list takes the BSTreeNodes), the stand-in dropped (`cleared`), Main::Update's entry (`updateEntry`, after the stood-in roots' kAccumulated is written) and the end of the writes there (`afterWrites`: after the writer tables' publication, the fade stores and the UpdateEntry diagnostics event, `Extensions::StandInAfterWrites`, `PrimaryCull::AtUpdateEntry`'s last call) and each attach or detach the primary cull's structure events follow (`structure`, any thread, with the parent, first in `PrimaryCull::NoteListStructure`, before the main cull input's, `Extensions::StandInStructure`: Features/Shadows/Local's point culls' category filter); whether the primary cull is installed. A per-root loop may read the verdicts directly (`Extensions::StandInVerdict`, `Extensions::ListedVerdictOf`, `Core/Extensions/EngineEvents.h`) |
| Main cull input | `Api/CullInputs.h` | the main camera's cull input besides the frustum, from a walk the feature makes after Main::Update's own (one: a second fails the install; `Features/Portals`' portal/occluder walk, its thunk at its early hooks, which call `Extensions::NoteMainWalkInstalled`: the primary cull's install requires it, and stays off with its warning without it). Core reads it through `Extensions::MainCullInput()` (`Core/Extensions/CullInputs.h`; null without one), resolved once with its names owned and the encoding of no programs (`headerWords` zeros): `unboundVisible`, the scene lists' keep check (once a frame); `structure`, per attach/detach the primary cull's structure events follow; `programs`, the words the revision's inputs size the programs buffer by (`programWords`) and the depth commit sends once a frame (the zero header when they outgrow the named version); `programsVersion` and `judge`, each fade root's program, asked in one batch (`Api::RootProgramsJudge`: a root without a node takes `noProgram` unasked) per depth commit when the roots or the version moved, the incremental mirror Core's (`UploadFadeRootPrograms`). The programs buffer (`programsName`) is made, registered and bound in Core's fade block (`SceneBuffers::cullPrograms`; none without an input: FadeStateCS and the builds take the frustum alone) |
| Engine channel | `Api/EngineEvents.h` | a queue of its own its engine hooks push keys into (`EngineChannel`, `ChannelKind::Keys`: an `EventQueue` whose capacity and wake are the feature's, drained through `drain` into Core's `KeySink`), which every scene pass drains among Core's queues and applies at its point (`ChannelApply`: `BeforeStructural`, after Core's fade snaps and amounts; `AfterSwitches`, after the switch events), handing the keys to the coordinator components' `events.keys` hook; under a load screen kept as the mirror parity's named keys or dropped (`ChannelLoading`); whether the mirror's parity counts its keys named (`namesKeys`); found by name (`FindEngineChannel`). Core keeps a batch slot per channel (`Extensions::kEngineChannelSlots`) |
| Engine pushes | `Core/Extensions/EngineEvents.h` | what a feature's own engine hooks hand Core's event sources. Reserved move writers: `InstallContext::ReserveMoveWriter(name)` at install, or `Extensions::ReserveMoveWriter` from early hooks, before the writer's hooks (`Api::MoveWriterId`; Core's writers keep slots 0, 1, 5 and 6, the reserved ones take the free slots in reservation order, so the report's "move writers' calls" order is unchanged; `kMoveWriterSlots` is 8, and the install fails past it); `MoveWriterInstalled(id)` once its hooks are in (`Scene::MoveEventsLive`: Core's writers in and no reserved writer pending); `CoreMoveWritersLive()`, which a writer completing Core's movers waits on (all or none); `PushReservedMove(id, key, Api::MoveReach)` (`ScenePass`: MoveEvents only; `WithPlacements`: FrameValueMoves too; a null key is counted, not pushed). `PushFadeAmount(root)`: Core's fadeAmount event and, for a fade node, the mirror's kFadeAmount (Features/Actors' SetAlpha). `PushRangesUpdate(geometry)`: a feature's own store changed what its ranged geometry type captures, as the mirror's `kRanges` update (Features/Lod's segment writers). `HiddenEventsLive()`: the hidden-node source is installed (the LOD gates engage) |
| Node capture | `Api/EngineEvents.h` | an engine node class (its exact RTTI) whose OnVisible visits an ordered list of its own in its children's place (`InstallContext::RegisterNodeCapture`, `NodeCaptureContribution`: a name, `rtti` called once at registration, `capture` on the capturing thread inside the engine-read window into the record's list), recorded with the node (`SceneCapture::kKindCaptured`, `NodeRecord::captured`, field `kCaptured` = `Api::kNodeFieldCaptured`) and again on each update the feature's own hooks push (`Extensions::PushNodeUpdate(node, Api::kNodeField*)`, `Core/Extensions/NodeCaptures.h`); resolved by RTTI (`Extensions::NodeCaptureOf`), one contribution a class; read back through `CoordinatorComponentContext::MirroredNodeOf` (`Api::MirroredNode::captured`). Features/Decals' decal nodes |
| Table family | `Api/TableFamilies.h` | columns of its own in every `SceneTables` (`Extensions::TableFamilies`), read through a typed handle; Core keeps beside them the write stamp (the publication copies a family only when it moved), the objects' references into it (bounded by its scene resources' fit, carried in the record at a declared word) and its rows' tags, and recycles its retired rows |
| Coordinator component | `Api/CoordinatorComponents.h` | state the coordinator keeps for it (`make`), and hook sets, one per concern of the coordinator's, each called on the scene lane per event (a changed slot, an erased entry, a freed range, a phase), never per object per frame: `residency` (a slot resident, left, every residency ended, the residents recounted), `accumulate` (a patch accumulated with its draw chain, the joins done), `walk` (begun, the delta walk's scheduling with its counts, the component's own for its report lines, a slot rewritten or its skin partitions refreshed, ended), `dense` (the walk parity's dense walk: save and restore), `retirement` (the slot tables reset, a tracked entry erased, every entry gone, a range it retired come free: `RetireRange`), `entryClass` (the class it follows, `EntryClass`: whether it takes it, `Extensions::EntryClassTaken`, read as data; its writes listed with the entry's held source node, `EntryWrite::source`, the record's second vertex stream resolved or waiting, the stream's slot) and `events` (an engine channel's keys, every pass, at the channel's point); with a narrow context: the live tables, the scene mirror and its fade node, the pass serial and stamp, the entry's reference, a tracked entry's slot and write state, its write scheduled, seeds the next frame takes (`FrameView::TakeComponentSeeds`), the entries' drawn ranges the walk reads (`EntryRanges`, `EntryRangesOf`, the mirror's `MirroredRanges`, `SetEntryRanges` with the entry's rewrite), the mirror's nodes (`MirroredNodeOf`: the parent chain walk, kind, RTTI, children and captured list; the kind bits `Api::kMirroredKindFaceGen`, `kMirroredKindTree`, `kMirroredKindCaptured`, checked against the capture's), the object slots' columns (`ObjectSlots`, `ObjectGeometry`, `RangeSpan`, `RangeSpanOf`) and the fixed main ranges' writes (`SizedRangeOrdinals`, `RangeCounts`, `NoteRangeOrdinalChanged`); and report lines of its own at the coordinator's points (`ReportLines::componentText`) |
| View contribution | `Api/Views.h` | an engine view Core's view machinery draws in the engine's place once a feature takes it (`EngineView`, in Core's slot order: the reflection, then the first person; see "The view contract" below), under its toggle: its set phase and view bit (the feature's own bits, checked by the manifest and the install), its membership (`CategoryRoot`: the entries under a root the feature captures, an exclusive phase; `LodTechniques`), its forward key rule (`keyFlags`: the view's own forward key bits, by which the pipeline lane finds a key's view, `Extensions::ForwardKeyView`; `cullBackFaces`) and shape (faces, trees, forward part, depth pass, names, initial depth range), its shape's life, render area, where its members' engine registrations are withheld (`InMainRenderers`, with the accumulators the feature names, or `OwnRenderers`, the renderers its engine hooks name to `PassCapture`), its target import, its coverage before the first frame and on a failed graph, whether a decal on its entries is a forward object of its forward part, its once-a-frame callbacks (`covered` over Core's precomputed `ViewCoverage`, `captureRoot`, `accumulators`) and an OwnRenderers view's residue question (`residueDue`), with its forward targets per view (`DrawPipelines::SetViewForwardTargets`); its work at the view's engine points (`RegisterViewWork`, `ViewPoint`: the frame's start before the coverage, after tree LOD, and the depth commit), run once a frame in view order. Core keeps a fixed slot per view (`Core/Render/Views/ViewSlots.h`: its epochs, segments, revision key group and graph extension) and resolves each view's entry once at install (`Extensions::Views`), which the per-view loops and the per-object paths read as data (`Extensions::ViewOn`, the set's phases and view bits, `ViewBitsOf`'s table by phase mask: both families', `Extensions::ResolvedViewBits`, the root views, the set decision's order, the InMainRenderers and OwnRenderers views); a view no feature takes has no entry, and every per-view loop skips it. The feature's work and epochs run on the view engine's calls (`IndirectDraws.h`, "The view engine", `Core/Render/Views/ViewEngine.cpp`: the slot's state, targets, depth range, pipelines, resources and target import, its epochs' inputs and submission; `ViewFaces.cpp`: a trees view's draw families per face, `ResolveViewFaces`, `DecideViewFaces`, `ViewFaceRows`; `ViewRoots.cpp`: a covered view's owned roots, `ViewLodRootsOwned`) |
| View root | `Api/Views.h` (`ViewRootContribution`) | an engine root whose members a covered view's faces draw in the engine's place while the feature says so (`ViewRoot`: `LodLandAndObjects`, `LodTrees`; `ViewRootBit`; `InstallContext::RegisterViewRoot`; `owned(const ViewRootFrame&)`: render thread, once per plain render of a covered, drawable view, handed the view and the active toggles, no engine read). One owner per root, a view root contribution or a draw family's faces (`faces.root`), refused twice by the install and listed by `Features/Manifest.cpp`. Features/Lod registers `LodLandAndObjects` (`LodRootsOwned`: the LOD gates' show veto registered and both LOD classes on); TreeInstances' faces own `LodTrees`. Core decides a view's owned roots once a render (`ViewLodRootsOwned`, `Core/Render/Views/ViewRoots.cpp`: see the view contract) |
| Shadow-view contribution | `Api/ShadowViews.h` | the view contract's sibling for Core's one shadow engine (see "The shadow-view contract" below): an occlusion map (`OcclusionMap`, in Core's slot order: the sky map, then the precipitation mask) under its toggle and a load-time `available` flag, with its set phase and view bit (the feature's own, `Api/ViewCommon.h`'s `ViewBits`), its occluder rule (`technique`, called by the classification per walked entry with an `OccluderLeaf`: the object's property flags, alpha test, whether it has a reference, and the nearest fade node's BSX flags walked on demand), its cull's size test (`minRadius`), its target (the engine's mask, or a texture it imports: `importTarget`), its labels, its optional reference parity (`referenceDue`), and its camera and accumulator (`camera`, `ownsAccumulator`: what the shell's occlusion view and 0x1C capture ask); a shadow source (`ShadowSource`: sun, spot, point, focus), whose culls and cameras its `sourceHooks` (called by source) install, the cameras written into Core's boards; for the spot and point sources each light the selection keeps (`lightKept`), and for the sun the end of the full-frustum cull's window (`fullFrustumWindowEnd`: required for the Sun, refused on the others; render thread, once a frame at `PrimaryCull::AfterFullFrustum`; whether the frame's full-frustum cull applied the exclusion), and for any source its frame's start and end (`frameStart`, render thread, in the frame driver after the frame requests are served, before the main renderers' refresh, `Extensions::ShadowSourcesFrameStart`; `frameEnd`, at Present, first in `PrimaryCull::EndFrame`, `Extensions::ShadowSourcesFrameEnd`; both in source order: the Point source's culls, which take the paraboloid exclusion at their frameStart, `ParabolicExclusionSlot::Pending`). Core resolves each entry once at install (`Extensions::ShadowViewsSet`: each mode's phase, the set decision's readiness order, each engine view kind's source, the maps' import order; `Extensions::OcclusionOn`), read as data |
| Draw family | `Api/DrawFamilies.h` | a feature's instanced draws at Core's fixed slot per family (`DrawFamily::TreeInstances`; `Core/Render/DrawFamilies/FamilySlots.h`, `FamilySlot`: the family and a trees view's forward key flags, its pipelines published for the passes, its part of the shapes key; `Core/Gpu/Pipelines/DrawFamilyLaneSlots.h`, the lane's part below the render layer: its stages' stage-cache key bit (36), pulled, its forward program's id (~0 - family), the forward key flags; the GPU layout is its feature's, `Features/TreeLod/TreeInstancesLayout.h`, sized for Core by its faces' data), gated by an object class's toggle and set up while its scene resources (its tables) are: its frame callbacks (`serve`, the mirror drain at the frame requests; `decide`, the frame's ownership before the main cull, which Core publishes, `IndirectDraws::DrawFamilyOwned` and `PassCapture::SetFamilyOwned`, with the lane's pipelines asked through `DrawFamilyFrame::Pipelines`; `programSource`), its part of the main pass's executions (`declare`, its arguments and draw row; `revision`; `record`, with the slot's pipelines Core prepares: after the depth draws in both phases, and in the colour pass after the main draws under `colourRange`) and of a trees view's faces (`makeCull`, `declare`, `record`, `row`, writing a byte span of `rowBytes`, `lists`: the units Core's per-face lists hold with its tables, and `face`: per face of each trees view's epoch commit, the face's row and list header, `DrawFamilyFaceWrite`; with their data, all or none with the six callbacks: `cullName`, `rowName`, `listName`, `rowBytes`, `headerBytes`, `unitWords`, `cullGroup`, and `root`, the `Api::ViewRoot` its faces draw in the engine's place), its cull and readback passes as GPU stages at the depth segment's slots, its tables as scene resources and their uploads as a commit hook. Its `program` (`DrawFamilyProgram`: `shaderType`, `name`, `label`, `keyName`, `sourcePath`, `nativeNote`, `descriptors` {colour, depth, forward}, `build` = `DrawFamilyBuild::DepthThenEqualColour`, `signature` = `DrawFamilySignature::DrawInstanced`) is what the lane builds the slot's pipelines of (`ShaderPrograms::FindDrawFamily`, `FindDrawFamilyForward`; `DrawPipelines::RequestFamily`, `SetFamilyInputs`, `NoteFamilyWait`, `NoteForwardWait`; `PipelineCatalog::families`, `Stats::drawFamilies`, `FamilyPipelines`, `CatalogFamilyPipelines`; recipes `dclf.{keyName}`, `dclf.forward.{keyName}`), and its `engineShapes` (`DrawFamilyEngineShapes::propertyVtable`, called once at install) the engine shapes whose passes `PassCapture` withholds while the family is DCLF's (`DrawFamilyEntry::engineShapeVtable`; `PassCapture::Stats::familyWithheld[f]`, `viewFamilyWithheld[v][f]`; a view's `familyOwned` bitmask, `SetViewFamilyOwned`). The install refuses an incomplete contribution, a shader type taken and a root taken. Resolved once (`Extensions::DrawFamilies`); a family no feature takes adds no passes |
| Main range | `Api/MainRanges.h` | an ordered range of the colour segment's second pass at Core's fixed slot for it (`MainRangeSlot`: the fixed slots `Fixed0`-`Fixed2`, whose objects take an ordinal each in the range's order, `SceneTables::rangeOrdinal` and `rangeCount`, written by the range's feature; the appended `Appended0` and `Appended1`, filled by the culling), named for its nvperf range (`cs.dclf.colour.<name>`), placed by its order among every registration's, optionally with a depth prepass (a fixed slot's draws first, depth only, in the decal depth pass before the main colour draws), and the class whose objects fill it. The slots, their count words and the object flag bits that select them are BuildDrawsCS's compile-time layout (`GpuLayouts.h`: `MainRangeCountWord`), which `Features/Manifest.cpp` checks a feature's slots against. a held group registers its range (below: Integrations/TerrainBlending's, `Appended0` "terrain" and `Appended1` "terrain-no-blend", orders 2 and 3); the install refuses a slot or order taken twice and a depth prepass on an appended slot. Resolved once into the draw order (`Extensions::MainRanges`), which the colour passes walk (`MainOpaque.cpp`); a slot nobody registers is never drawn |
| Held group | `Api/MainRanges.h` (`HeldGroupContribution`) | objects a CS feature takes out of the opaque pass and replays after it, which DCLF draws in an appended slot (`InstallContext::RegisterHeldGroup`: `heldWhile`, the classify input that holds them; `slot`, `Appended0` or `Appended1`, registered as a main range with its `name` and `order`, `objectClass` Count; `propertyFlags`, the Lighting property flags whose objects it takes, the first group in registration order whose flags match taking the object; `toggle`, none for always, else `Ineligible::Held` while off; a decal or layer is always `Ineligible::Held`, text "terrain-no-blend"). Core keeps each slot's state and depth test, as the decal groups'. Read as data by the Lighting descriptors' derive per event (`Extensions::HeldGroups()`). Integrations/TerrainBlending's terrain blocks (`Appended0`, `kMultiTextureLandscape`, always) and no-blend meshes (`Appended1`, `kNoTransparencyMultiSample`, under "terrain under TB") |
| Near twin | `Api/MainRanges.h` (`NearTwinContribution`) | `Appended0`'s near objects drawn twice in the depth segment, into a target of the feature's and, offset, into the main depth (`InstallContext::RegisterNearTwin`, one; `Extensions::NearTwin()`): `name` (the NotReady 12 texts, built once: "<name> cannot be described", "<name> could not be imported"), `debugName` (the import's), `resource` (the ORG id the main extension registers it under), `target` (the scene resources' setup: the engine depth texture to import, null for no twin target) and `nearTest` (render thread, per depth commit: the eye and distance into the latch, false for w 0, no test). Core keeps the twin buckets, the terrain depth pass and the latch's layout. Integrations/TerrainBlending's terrain depth ("DCLF terrain blending depth", `cs.dclf.terrain-blending-depth`, its depth-pass eye and `kTerrainDepthDistance`) |
| Classify input | `Api/ClassifyInputs.h` | a world fact outside the toggles that classification reads (`ClassifyInputContribution{name, sample}`, `InstallContext::RegisterClassifyInput` returning a `ClassifyInputId`; `kClassifyInputSlots` 8; a name another input has fails the install): `sample`, any thread, cheap, the live state. A feature that reads an input another registers (installed after it, or never) asks for it by name (`InstallContext::NeedClassifyInput`, `Extensions::kClassifyInputNeedSlots` 4: an id with `kClassifyInputNeedBit`, bound by name once every feature and integration installed, `BindClassifyInputNeeds`; unbound, it reads false; not for a technique gate or a held group): Features/Effects' "light limit fix", per derive. Core samples every input once a frame where the frame driver begins its toggles (a change drops the verdicts, `RequestVerdictsInvalidated`) and once a scene pass at its start (`PassSnapshot::inputs`, installed per thread by `ClassifyInputsScope`, so the pool's evaluations see the pass's answer); `Extensions::ClassifyInputOn(id)` reads the thread's snapshot, else a live sample (`Core/Extensions/ClassifyInputs.{h,cpp}`). Read by the technique gates, the held groups and the ancestor value. Integrations/TerrainBlending's "terrain blending defers" (loaded and enabled); Integrations/LightLimitFix's "light limit fix" (loaded) |
| Light volumes | `Api/LightVolumes.h` | the cull volumes of the frame's local shadow lights, which each colour commit's BuildDraws tests every object against for its main-pass shadow mask (`InstallContext::RegisterLightVolumes`, one: a second fails the install; `LightVolumesContribution{name, landscapeFlags, sample}`): `sample`, render thread, once per colour commit (`CommitShadowInputs`), each `LightVolume` (mask bit, `affectsLand`, sphere, 12 planes, the two plane masks) put into Core's list (`LightVolumeSink`, `Extensions::SampleLightVolumes`, `Core/Extensions/LightVolumes.h`); `landscapeFlags`, the property flags whose objects take only the `affectsLand` volumes, read per join as data (`Extensions::LightVolumes()`). Core keeps the test HLSL mirrors (`GpuShadowVolume`, `ShadowVolumeMaskOf`, `GpuLayouts.h`), the latch region, its growth and its clamp (the sampled list stays unclamped, BuildParity's reference), and `kObjectLocalShadow*`/`kObjectLandscapeLights`. Without one, no volume and no landscape rule. Features/LocalLights' "local shadow lights" (`(1<<14)|(1<<46)`) |
| Depth pyramid | `Api/DepthPyramid.h` | the hierarchical depth Core's cull tests against (the HZB: BuildDrawsCS's HZB branch, `HzbTest.hlsli`, tree LOD's cull and phase 2's retest), made and filled by the feature (`InstallContext::RegisterDepthPyramid`, one: a second fails the install; `DepthPyramidContribution{resource, scratchResource, passName, setup, makePass}`): `setup`, render thread, where Core makes the main view's resources (`Impl::Setup`: the first colour frame, a target change; after the refraction normals' import, before the frame buffers), handed the device and the main view's extent (`DepthPyramidSetup`), fills a `DepthPyramid` (`texture`, mip 0 half the depth's next power of two a side; `width`, `height`, `mips`; an optional `scratch` Core zeroes at the first depth commit after it) and returns the producer's state, held with those resources (`Resources::hzb`, `hzbWidth`, `hzbHeight`, `hzbMips`, `hzbScratch`, `hzbState`; `Extensions::SetUpDepthPyramid`, `Core/Extensions/DepthPyramid.h`); `makePass`, at the graph's build, handed the main resources and that state (`DepthPyramidContext`), its pass added under `passName` in the depth epoch after `cs.dclf.z.depth`, before `cs.dclf.build-draws-phase2`; the main extension registers the texture and scratch under `resource` and `scratchResource`. Core keeps the test: `CS_DCLF_CULL`'s mode, the latch's `hzbUvScalePacked`, the bindings, the phase 2 block and its stages, the stats. Without one, or without its texture, no pyramid: mode 2 culls by the frustum alone, with no phase 2 passes (Core logs nothing; the producer warns when its program fails). Resolved once (`Extensions::DepthPyramidOf()`, null: none). Features/GpuOcclusion's HZB (`cs.dclf.hzb`, `cs.dclf.hzb-counter`, pass `cs.dclf.hzb`) |
| Ancestor value | `Api/AncestorValues.h` | a per-object value another CS feature keys by an engine node above the object, which every draw reads from the object's record (`InstallContext::RegisterAncestorValue`, one; `AncestorValueContribution`): `recordWord` (`DCLFObjectRecord`'s word, checked by `Features/Manifest.cpp`), `none`, `ancestorRtti` (called once at install: the nearest instance of either class above the object is its key), `whileOn` (a registered classify input: off, no map is posted and no lookup made), `generation` (render thread, once a frame at `PostAccumulateInputs`) and `fill` (when the generation moved: the map, `AncestorMapSink::Reserve`, `Put`). Core keeps the key walk (the mirror's parents and RTTI, at the prep and the attachment), the map's post and take (`AncestorMapInput`), the lookup at a join while the entry's generation is behind, and the column (`SceneTables::ancestorValue`, `ObjectAncestorValue`, `kChangeAncestorValue`, `BindlessObject::ancestorValue`); nothing per frame per object (`Extensions::AncestorValue()`, null without one; `AncestorValueOn()`; `Core/Extensions/AncestorValues.h`). Integrations/LightLimitFix's room index (BSMultiBoundRoom, BSPortalSharedNode; word 0, -1) |
| Vertex stream | `Api/VertexStreams.h` | the second vertex stream of its entry class's objects (one: Core's draws have one second-stream slot; `Features/Faces`' face positions), a float4 per vertex in each object's range of a versioned buffer its scene resources hold: Core keeps the rows in the scene's tables (`SceneTables::streamRows`, the objects' `streamRow`, `kRetiredStreamRow`; written by the component that follows the class, copied with each publication and into each payload) and reads them as data - the builds' geometry draw per row after the geometry slots, named as the object's second stream (`StreamGeometry`, `AppendStreamRows`; an object of the class without its row waits), the fit (`SceneFit::streamVertices`, the resources' `fit`), the passes' vertex-buffer binding of the buffer `select` names at setup (`SceneBuffers::vertexStream`) - and embeds the buffer's address while the class is taken (`Extensions::VertexStreamTaken`); the stream's scene resources are set up, registered with a graph and required first (`Extensions::SceneResourcesOrder`; their setup giving no state leaves the scene's tables unmade), and its `upload` runs first in each commit, before the commit hooks, with the payload's rows (its count is the shadow report's "face regions uploaded") |
| Palette rows | `Api/FrameValues.h` | a class of FrameValues' items (`FrameValueItemClass`, Core's to decide: `ParticleSystem`, a particle system without a skin, named by the coordinator's plans, `PlacementPlan::Item::itemClass`) whose palette block it fills every frame in FrameValues' palette part (the skins' part): the block's size by its rule (`BlockRule`: rows a unit, the units clamped and rounded to a power of two; the walk reads it as data, `Extensions::PaletteRowsOf`), the geometry slot its draws read (`geometrySource`, at a record's write), its frame start (`FrameDriver::BeginFrame`) and kick (its frame input, `kPaletteRowsInputBytes` held by the frame's job), and the producer's run: `beginRun` with the run's items of the class, `sampleChunk` once a `CS.DCLF.FrameValues.Items` chunk with the chunk's writes (the block's rows, and the placement row's world and bound), `endRun`. The class's items are due every frame; Core samples each chunk's items in order, defers the class's, and finishes them (compares, marks, owes) where each stood, so the chunk's lists keep the run's order; it keeps the first-person writes' counters the first person's report reads (`PaletteRowsChunk::NoteFirstPerson`, `FrameValues::TakeFirstPersonPaletteStats`). Resolved once (`Extensions::PaletteRows`) |
| Object contributor | `Api/ObjectContributors.h` | three kinds, read as data where Core decides: membership by an engine key (a technique it ports: its eligibility under a toggle and its objects' record flags, and optionally a property flag rule, `propertyFlags` and `geometryType`: an object whose Lighting property has any of the flags is DCLF's only while the toggle is on and its geometry is that type, else `Ineligible::Lod`; resolved once, `Extensions::PropertyFlagRules`, read by the Lighting port's eligibility; a geometry type it ports: a class under a toggle, read by the eligibility, the walk and the captures, which may carry `propertyFlags` (only objects whose Lighting property has the flags are the class's, drawn as a tri-shape: object LOD's sub-index shapes), `itemClass` (the FrameValues item class of its skinless objects with a palette block: the walk's block, the geometry slot, the plans and the frame values' parity, `Extensions::PlannedItemClass`), `moves` (movers in the walk's per-frame traits) and an `entry` rule (the `EntryClass` its objects under a parent of a mirrored kind are, `kMirroredKindFaceGen`; `perFrame`: classified every frame, the per-frame entry-class trait; `holdsParent`: the parent held as the entry's source node), and `ranges` (`Api/EntryRanges.h`: the index ranges an entry of the type draws, captured on the capturing thread in the read window per record capture and per `kRanges` push, never per frame, whatever the toggle; Core asks `Extensions::GeometryRangesOf(type)`, `GeometryTypeRanged(type)`; object LOD's sub-index shapes, Features/Lod's `CaptureDrawnRanges`), and may name an entry class alone, without a toggle); membership by a Core class, one enum value per class a feature takes (`EngineParent` and `ParentLeaves`: the leaves under a billboard, a switch node or an actor, read by the classification and the sun and light candidates; `ObjectClass`: a decal or a multi-index layer Core's ports derive but leave native, read by the ports' eligibility; the skinned shapes and the skins of several partitions (`Skinned`, `SkinPartitions`: Features/Skins'), read by the eligibility, the Lighting descriptors, the walk and the Effect descriptors; an effect, read by the eligibility, the primary cull's membership passes, the captures and the census, and an opaque effect, read by the Effect port as taken whatever its toggle (`Extensions::ObjectClassTaken`); the forward epoch's blended objects (`Forward`, which also gates the epoch itself: its coverage, recording and execution) and refraction objects (`Refraction`, needing Forward's toggle), read through `Extensions::ObjectClassOn` by the epoch, the eligibility, the classification, the Lighting descriptors and the primary cull (Features/Forward's); object and terrain LOD, read by the ports' eligibility and their category root (`TES::lodLandRoot`); tree LOD, read by Core's tree LOD path; the LOD classes, read by a view's LOD roots; an object class may carry `categoryRoot`, an engine root listed as a category node while the class is on, called per frame by the category capture, two classes may list one root; `EntryClass`: a face shape whose records wait on the coordinator component that follows it, matched by a geometry type's entry rule, or an owner class, by its leaf's reference's form type (`EntryClassContribution`, `InstallContext::RegisterEntryClass`: `entryClass`, `name`, `ownerFormType`, `everyFrame`, `ownMotion` (needs `everyFrame`), `published`; read as `Extensions::OwnerClassOf`, a table by form type built at install: `Tracked::owner`, the walk's owner class trait `kTraitOwnerClass`, `Coordinator::FrameVerdictKept`, a published class's members in `SceneTables::classMembers`; Features/Actors registers {`ActorOwned`, "actor", ActorCharacter, every frame, own motion, published} whatever its toggle)); and derivations (the shading values it derives for a property flag of its own; the feature block a shading contribution may claim, one slot, a second claimant failing the install: `objectFlags`, `blockWords` and `vsVariables` of `ShadingVariable{variable, firstWord, floats, compared}`, in `Api::kShadingBlockWords` = 8 words, `BindlessObject::featureBlock` words 4-11 and `BindlessShading::featureBlock` words 16-23, resolved once as `Extensions::ShadingBlockRule()`, which the Lighting port's object constants, geometry patches and shading sample and the capture parity read as data: Features/TreeWind's `kObjectTreeAnim`, six words, TreeParams and WindTimers; and engine globals captured with the frame's: `FrameGlobals::features`). Adding a Core class is the same three steps for every enum: the value in the header, Core's test of it through its `Extensions` query, the feature's registration (the install fails a second claim) |
| Technique gate and extras | `Api/ObjectContributors.h` | a technique contribution's per-object VS extras (`TechniqueContribution::extras`, `ObjectExtras`: `None`, `LandBlend`, Core's land-blend rows; read per join, `Extensions::TechniqueExtras(t)`: Features/Landscape's MTLand and MTLandLODBlend); and a gate on another's technique (`TechniqueGate{technique, whileOn, needs}`, `InstallContext::RegisterTechniqueGate`): while the classify input `whileOn` holds, the technique needs the toggle `needs` too, resolved per technique and read in `ContributedTechniqueOn` as data (Integrations/TerrainBlending's on techniques 8 and 19, needing "terrain under TB") |
| Material port | `Api/ObjectContributors.h` (section 4) | an engine shader other than Lighting whose objects Core captures, classifies, joins and draws through the port while its object class is on (`MaterialPortContribution`): its name (the census's class prefix; required), its property RTTI and material vtable (resolved once at install), the material bytes and texture fields a snapshot keeps, its constant space a stage (`MaterialStageSpace`: variables, sized and scratch ones, groups, first variables, names) and the rules Core reads as data (`MaterialPortRules`: pass enum base, deferred pixel bit, frame VS variable, technique rows, raw permutation, signature components, emissive always listed, billboard-turned and particle leaves); its callbacks per event, pure: `capture` (the property's record), `derive` (the descriptors or why it stays native: the eligibility, the joins, the primary cull's membership passes), `evaluate` (a material record, read through `MaterialPortEvaluation`'s accessors, so the port includes no `Scene/Ports` header), `shading` (a shading row), `variants` (the census), `emissiveOf` (optional: per external-emittance re-point, `SetExternalEmittance`'s hook, the colour the property's emissive points at now). The billboards' turned-shape rule is shared by the classification and the census (`Extensions::BillboardTurnedLeaf`). Resolved once by kind (`Core/Extensions/MaterialPorts.h`, `Extensions::MaterialPorts`: 0 Core's Lighting, `CoreLightingPort`; then the features' in registration order, the kind `RegisterMaterialPort` returns: Features/Effects' Effect port is 1, `kMaterialKindEffect`, checked at its registration), the kind the records carry (`PropertyRecord::port`, `MaterialSnapshot::port`, `LightingDescriptors::port`, `MaterialRecord::kind`), read as data where Core decides (`PortOfRtti`, `PortOfVtable`, `PortOn`, `PortRules`, `MaterialSpaceOf`) |
| Program family | `Api/DrawFamilies.h` (`ProgramFamilyContribution`) | a material port's keys drawn by the main pipeline set at Core's fixed slot per family (`ProgramFamily::Effect`; `Core/Gpu/Pipelines/ProgramFamilySlots.h`: its raster key bit, `KeyFamily`, three spare key bits, its stage-cache key bit, its program-id bit, its Z key's salt; its counters, `DrawPipelines::Stats::families`): its names (logs, key and recipe names), its HLSL source loaded with Core's (`ShaderPrograms::FindFamily`), its stages and build rule (`ProgramBuild::ForwardOrDecal`), the rules Core reads per object per frame as data (`Extensions::FamilyRules`, `FamilyRulesOfKind`: row blocks, root views forward only, billboard-turned key bits, its record bits) and its program source (render thread, the lane's inputs). Resolved once after the material ports (`Core/Extensions/ProgramFamilies.h`, `Extensions::ProgramFamilies`; Features/Effects' Effect family, `EffectPrograms.cpp`) |
| Material-write sink | `Api/MaterialWrites.h` | a feature's engine hooks name an in-place write to Core, any thread, lock-free: `NoteMaterialWritten(const RE::BSShaderMaterial*)` (the material captured for the coordinator's records, and queued for the capture parity) and `NotePropertyShading(const RE::BSShaderProperty*)` (the next frame's shading sample resamples its dependents); and, at install, `InstallContext::RegisterFrameVSVariablePatch()`: the feature writes every material row's frame VS variable (the ports' `MaterialPortRules::frameVSVariable`, TexcoordOffset) every frame, so a capture or controller write that moved only the texture transforms rewrites no record (read as `Extensions::FrameVSVariablePatched`; unregistered, those transforms are the records' and such a capture is rewritten). Implemented by `Engine/Capture/MaterialWrites.cpp` (the sink, beside Core's own Lighting write hooks and their queues) and `Core/Extensions/MaterialWrites.{h,cpp}` (the patch flag). The texture-transform moves also go to Core's `TransformMoves` queue, whatever patches the frame VS variable, which the kept shadow build drains (`Extensions::DrainTransformMoves`, `Core/Extensions/MaterialWrites.h`; ShadowBuild's `WatchTransformMoves`: its rows read the transform themselves). Features/Effects' write hooks and Features/TextureTransforms' patch |
| Shading field | `Api/ShadingFields.h` | a field of the shading rows whose value is an owner's (`ShadingRowField::SkinPerGeometry`), carried by a published entry class's members (`ShadingFieldContribution{name, field, members, membership, capture}`, `InstallContext::RegisterShadingField` returning a `ShadingFieldId`): `membership`, render thread at the frame's start, per publication, with the class's membership writes (`ClassMemberChange`); `capture`, at the frame's start before `FrameValues::Kick`, the members' values (`ShadingFieldCapture::Put`), which FrameValues applies after sampling the rest of the row (every other slot's field zero). Core side `Core/Extensions/ShadingFields.{h,cpp}` (`ResolveShadingFields`, `ShadingFields()`, `PublishedEntryClasses()`, `ForwardClassMembers`, `CaptureShadingFields(FrameView&)`) and the generic tables: `SceneTables::classMembers[kPublishedEntryClasses]` (`ClassMembers`: the sorted objects, the owners by slot, the changes; `SetClassMembers`, `ClearClassMembers`, `ClassMemberChangesPending`, `ShadingFieldsOwned`), `PublicationDeltas::classMembers[c]`, `ShadingItem::fieldsOwned`, `ShadingFieldValue` (= `Api::ShadingFieldValue`). Integrations/AdvancedSkin's wetness |
| Entry ranges | `Api/EntryRanges.h` | the drawn index ranges a ranged geometry type's `ranges` capture writes (`EntryRange{firstIndex, indexCount, key}`, equal by its triangles alone; `EntryRangesWhole`): the mirror's record (`GeometryRecord::ranges`, `kRanges`, whose printed field name stays "segments"), Core's kept ranges by tracked entry (`entryRanges`, from the attach's capture, erased with the entry; `CoordinatorComponentContext::EntryRanges`) and the range slots the walk chains (`ResolveEntryRangeSlots`) |

Each contract's thread, timing and lifetime is documented in its header. A feature's install also installs its engine
hooks and subscribes its observers (`Diagnostics::Subscribe`, under Core's `Observe()` flags). Its thunks read the shell's state
through `Core/Engine/ShellState.h`: whether DCLF runs (`ShellState::Running`, the query the shell installs before the early hooks) and
where the engine's passes are (`ShellState::Passes`: in the depth pass, RenderDepth's control-driven argument), which the shell's
Main::RenderDepth thunk writes. The render thread's frame number is `Core/Engine/FrameClock.h`'s (`FrameClock::Frame`, any thread;
set at the frame's start through `SceneCapture::SetFrame`): Features/Lod's gates stamp their frames with it.

The headers in `Api/` name only standard and ORG types. The contexts their callbacks receive are opaque there. Core
defines them in companions under `Core/Extensions/` (`FrameValuePools.h`, `GpuStages.h`, `PayloadProducts.h`,
`SceneResources.h`, `Commits.h`, `FrameBindings.h`, `Reports.h`, `EngineEvents.h`, `TableFamilies.h`,
`CoordinatorComponents.h`, `CullInputs.h`, `ObjectContributors.h`, `ViewCommon.h`, `Views.h`, `ShadowViews.h`, `DrawFamilies.h`, `MainRanges.h`,
`VertexStreams.h`, `PaletteRows.h`, `MaterialPorts.h`, `ProgramFamilies.h`, `MaterialWrites.h`, `NodeCaptures.h`, `ShadingFields.h`, `ClassifyInputs.h`, `LightVolumes.h`, `AncestorValues.h`, `DepthPyramid.h`), which features
inside DCLF include for typed access to Core's types.

Residency changes are events, so a coordinator component's hooks are function pointers called per changed slot. The loops
that run every frame over every object (the walk's evaluate, the builds' packing, the commit) never call a feature: they read
what it registered as data (a technique's flags and toggle, a geometry type's or a parent's toggle, a family's references and record
word). The switches and toggles register with `Core/Settings`, which owns them.

The shaders' shared constants stay compile-time: a feature declares its own (a frame binding's register beside its HLSL
declaration, its object flag bits within `kFeatureObjectFlags`, its family's record word), and `Features/Manifest.cpp`
checks them against Core's and each other's.

`Features/TextureTransforms/` was the first feature built on them: a pool, a stage and a product. It also registers the frame VS
variable's patch (`InstallContext::RegisterFrameVSVariablePatch`), the one condition of Core's transform-only skips (the coordinator's
capture drain, the Lighting float controller's types above 0x13, the effect controllers' UV scroll; the coordinator's transform watch is
gone, its "watched transforms" counter, `MaterialRecordStats::transformsWatched`, counted where the watch was called), and keeps its pool's
parity and stats line (`Diagnostics/TextureTransformsParity.cpp`); it reads the frame through the frame-value pool contract
(`FrameValuePoolProducer::TextureTransformBuffer`, `Extensions::FrameValuePoolIndex`), including neither FrameGlobals.h nor
`Core/Gpu/FrameValues.h`. Its shadow UV scroll stays Core's ("Not generalised", below). `Features/TreeWind/` was
the second: a pool with frame-start reads, scene resources with their commit hook and frame binding, a next-frame stage on
the compute queue, its switches and toggle, a report line, a stand-in observer with its engine hooks, its parity, and its
table family (the tree slots), coordinator component (listing them as members join and leave) and object contributions
(technique 12's eligibility and record flag, its shading derivation, its frame globals). Its tree list takes the BSTreeNodes from the
stand-in's roots that left and joined, and the mirror's tree kind reaches it as a node kind bit (`Api::kMirroredKindTree`, which
`MirroredFadeNode` reports); its per-object data is the feature block (below: the record's and the shading row's
`featureBlock`, `objectFlags = kObjectTreeAnim`, six words; the dead `treeAnim` column is gone). `Features/Particles/` and
`Features/Billboards/` followed: their switches and toggles, report lines at the draws' async work and parity, a geometry type
contribution (the particle types) and parent contributions (the leaves under an NiBillboardNode), the particle snapshots' engine
hooks, and the billboard parity's early hooks. `Features/SwitchNodes` (its switch and toggle, and the switch parent's contribution) and
`Features/HiddenNodes` (the hidden events' source, the engine's kHidden stores patched at its early hooks, and its watch: a switch, an
observer and report lines at the statistics) followed. `Features/Decals` followed: its switches and toggles (the layers' needing the decals'),
the decal and layer object classes' contributions, the member decals' order as a coordinator component (joined by its patches' chains,
re-keyed by the walk's rewrites, ordered when the joins are done, writing the tables' ordinals), its groups' ranges of the colour segment's
second pass on the main range contract (`Api/MainRanges.h`: `Fixed0` "decals-opaque", order 0, with the depth prepass; `Fixed2`
"decals-layers", order 1; `Fixed1` "decals-blended", order 4, after Integrations/TerrainBlending's held groups at 2 and 3) and its parity. Its report lines
followed, with the order parity's state in its component (`SceneCoordinatorMaterials`: the order parity; `SceneCoordinatorLastPass`: the
candidates, which it counts), then the decal array edits' hooks (`DecalHooks.cpp`, its early hooks, after the hidden nodes': no shared
site) and the decal node's capture, its node capture contribution; `DecalOrder` reads the mirror and the tables through the context only.
What it leaves Core is the main pass's structure: the ports' decal groups and states (the patch's decal key, `kObjectDecal` and its group
bits, the raster
keys), the tables' ordinal columns (`rangeOrdinal`, `rangeCount`), the main build's decal inputs and templates, the sequence buffer's range
slots and count words (`GpuLayouts.h`, `BuildDrawsCS`), the colour passes that draw the registered ranges in their order (`MainOpaque.cpp`),
and the generic node capture field and kind (`kKindCaptured`, `NodeRecord::captured`), whose parity name stays "decals"
(`NodeRecord::kFieldNames`), so the mirror parity's text is unchanged. What SwitchNodes and HiddenNodes leave Core is the visibility every
object is judged by: the switch
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
bindings (`SceneBuffers::vertexStream`). The face shapes' classification is contribution data: a geometry type contribution for
`kDynamicTriShape` whose entry rule is `FaceShape` under `kMirroredKindFaceGen` (`perFrame`, `holdsParent`); Core keeps the entry's class
(`Tracked::entryClass`, resolved once), its held source node (`Tracked::sourceRef`, handed as `EntryWrite::source`), the per-frame
entry-class trait (`kTraitEntryClass`) and the reports' "face" names. Its publications' counts and their fragment of the scene delta line
are the component's (`SceneCoordinatorDelta`). The rows are Core's columns, not a table family's: a family is copied only when its stamp
moves and its stamp is in the
publication key, so a head's new snapshot taken in place would publish where it does not now, and the replay line would gain its count.
`Features/Actors` followed: its switch and toggle (which the first person's needs), the
actor parent's contribution (`EngineParent::Actor`, `ParentLeaves::AllLeaves`: the classification's chain test) and the actor techniques'
(actor skin, faces, hair, eyes). Its engine events followed: Actor::SetAlpha's hook and the three actor move writers
(ActorUpdate3DPosition, SkyCellSkin, RagdollJob) at its early hooks (`ActorEvents.cpp`, right after the reflection's), pushing into Core's
fadeAmount channel (`Extensions::PushFadeAmount`; its conversion waits on Fades) and reserved move-writer slots 2-4; and the high actors'
animation stamp from DCLF's shadow views with its "actor rate" line (`ActorRate.cpp`: the stand-in observer's `afterWrites`,
`ReportPoint::AccumulatedWrites`). What it leaves Core is the path an actor's objects take whatever the toggle: the skinned geometry, which is
any NiSkinInstance's and not the actors' alone (the bone palettes in FrameValues' palette part, the skin partitions in the tables and the walk),
whose switches, toggles and classes are `Features/Skins`' (below); an owner class's entry's classification (`Tracked::owner`, the per-frame
owner class trait `kTraitOwnerClass`, `Coordinator::FrameVerdictKept`, `SceneTables::classMembers`), whose class is the feature's
registration (`EntryClass::ActorOwned`, below); and Core's own movers (FUN_14066afa0, Update3DPosition, the graph animation and update-pass
writers) with the fadeAmount channel. Advanced Skin's wetness is `Integrations/AdvancedSkin`'s (below); its material textures (t71/t74,
`MaterialPortFeature`) and their parity still wait for a material contributor.
`Features/FirstPerson` followed: its switches and toggle (which needs the skinned and the actors), its report lines at the draws' async work
and the skeleton's parity; then, on the view contract, the whole first-person view: its set phase and view bit (`kSetFirstPerson`,
`kViewFirstPerson`), its contribution (a category root, the first-person root, with its root capture; one face with a forward part and a
depth pass; dropped while off; the main colour viewport; withheld in the main accumulators, with the first-person accumulator; drawn into
kPOST_ZPREPASS_COPY), its coverage callback, its frame-start work (the latch, targets, skeleton walk, pipelines and resources), its two epochs
on the view engine's inputs and submission (the same skip causes and log lines), its state and report counters (`FirstPersonFrame`), and the
two engine hooks that run the epochs (RenderDepth's first-person draw and Main::RenderFirstPersonView's accumulator render, installed from its
install, their addresses `kDepthFirstPersonDraw` and `kFirstPersonViewDraw` in `FirstPersonHooks.cpp`; they read `ShellState`). Its
diagnostics followed (`Diagnostics/RootViewChecks.cpp`): the subtree parity, the subtree chain and hidden-geometry logs, the hidden verdict
log, the misfiled filing and the left-out slots (the commit's `SetSlotDecided`, the commit parity's `SetSlotRedecided`), with their three
report lines at `SceneCoordinatorSet`. What it leaves Core is what every bucketed view shares, keyed by its slot or its membership: the epochs'
passes and forward part (`ViewEpoch*.cpp`), their revisions (`ViewKind`, `Epoch::FirstPersonDepth`/`FirstPersonColour`, the shape, key group
and coverage inputs), the slot's state (`ViewEpochState`) and the per-view forward targets (`DrawPipelines::SetViewForwardTargets`, keyed
by the feature's `kForwardKeyFirstPerson`); a CategoryRoot view's scene side (the root as a category node, the classification's root stop,
the root view hook points it announces on instead of logging, `Diagnostics/Scene/RootViewEvents.h`: `RootSubtreeSynced`, `RootViewHidden`,
`SetSlotRedecided`; FrameGlobals' capture of the view's subtree through `captureRoot`, `FrameGlobals.h`'s `RootSubtreeNode`; the set's
decisions and their parity, the decals' forward keys, the placements' per-frame chains, the frame requests' root, the
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
moved; the feature includes neither): the draw families drawn per face (their pipelines, ownership and rows: `IndirectDraws::ResolveViewFaces`,
`DecideViewFaces`, `SubmitViewEpoch`'s rows), the cube camera's roots its faces draw in the engine's place (`IndirectDraws::ViewLodRootsOwned`,
from the draw families' faces' roots and the view root contributions: Features/Lod's reads the LOD gates' show veto,
`Extensions::ShowVetoRegistered`), an OwnRenderers view's residue watch
(`PassCapture::WatchViewResidue`, `TakeViewResidue`, `TakeViewResidueGeometries`, per view) on the renders its `residueDue` keeps the
roots (the feature's `ResidueDue`), and the per-view forward targets the faces' pipelines are built for (`DrawPipelines::SetViewForwardTargets`).
The residue classification and its classes are the feature's (`Diagnostics/Residue.cpp`: drained at CaptureDrained, classified at
JoinsBegin, `ResidueKinds` for the third line).
`Features/OcclusionMaps` followed, the first feature on the shadow-view contract: its switches (`CS_DCLF_SKYLIGHT`, `CS_DCLF_PRECIPITATION`,
listed where Core's were: before `CS_DCLF_OPAQUE_EFFECTS`, after the trees' list-all switch, with the reflection's and the first person's
following them there) and its two toggles (which need the shadows; the first feature toggles registered, so their bits in the packed word
are where Core's were, the feature listed first), the maps' set phases and view bits (`kSetOccluderSky`, `kSetOccluderPrecipitation`,
`kViewSkyOccluder`, `kViewPrecipOccluder`, their values unchanged; no HLSL mirror; the manifest checks them and Core's `kCoreSetPhases` and
`kCoreViewBits` no longer hold them), and the two `RegisterOcclusionMap` contributions: the sky map (Skylighting's occluder rule, its target
Skylighting's own occlusion texture, imported by `importTarget`) and the precipitation mask (its rule, the engine's mask), both available
while Skylighting is loaded (`skylighting.loaded`, read as data), both with the size test (`minRadius`: the mask's cull has always taken
Skylighting's) and the precipitation accumulator's camera and accumulator (the sky map's while Skylighting draws it, `inOcclusion`), with
their labels and reference parity (`ReferenceDue`, CS_DCLF_SKYLIGHT_PARITY's). The rules themselves are the feature's (`OccluderRules.cpp`:
`SkylightingTechnique`, `PrecipitationTechnique`). What it leaves Core is the shadow engine's: the occlusion epoch on the shadow commit, the
maps' modes, view slots, depth targets and technique columns, the walk that hands each object to the rules (`Api::OccluderLeaf`,
`Coordinator::NearestFadeNodeBsx`), the set decisions and the reports but the maps' two lines, all reading the maps' entries. The shell keeps
its six occlusion methods and the 0x1C capture's thunk, which ask the maps' `camera` and `ownsAccumulator` (in slot order)
(`DrawcallLimitFix::OcclusionReady` and its siblings). Its diagnostics followed: `CS_DCLF_SKYLIGHT_PARITY` and `CS_DCLF_SKYLIGHT_DUMP_DIR`
(registered by the feature, listed where Core's were: after `CS_DCLF_CAPTURE_POINT_PARITY` and `CS_DCLF_TABLE_START`), the compare and its
observers (`Diagnostics/SkylightParity.cpp`; no `Observation` flag, no Policy question), the maps' report lines
(`Diagnostics/OcclusionReport.cpp`, at `ReportPoint::ShadowViews`, reading the driver's `FrameDriver::TakeOcclusionFrames`) and the maps'
view check against the engine's (`Diagnostics/OcclusionViewCheck.cpp`, under either parity switch).
`Features/Shadows/Sun` and `Features/Shadows/Local` followed, the shadow sources: the sun's (`RegisterShadowSource({Sun})`, and its camera
hook, BSShadowDirectionalLight::UpdateCamera, writing the `Sun` board) and the spot, point and focus sources (the spot and point sources'
`lightKept`, each kept light's views into the `Lights` board, and the focus cameras' hook, BSShadowDirectionalLight::sub, writing the
`Focus` board). The sources own no phase or view bit (the sun's switches and toggles came with its accumulation, below), and their hooks
install by source (`Api::Feature::sourceHooks`), so the install log keeps its order: the sun's accumulation and views, the light
selection, the point lights' culls, the focus views. What they leave Core is the shadow engine (both epochs, the capability and captures,
the views list), the boards and the camera math, the views' tallies the shadow epochs' checks fill (`Diagnostics::ShadowViewTally`), the
caster phases and modes, and the shadows' toggles. The engine's focus targets and each source's parity followed into its feature ("The
shadow-view contract", "Where it stands").
The point lights' culls followed into `Features/Shadows/Local` (`LocalLightCull.{h,cpp}`, namespace `DCLF::Features::Shadows::Local`: the
Point source's culls without DCLF's entries, their category filter, their bits for cut entries and the portal-strict lights' reach,
`kReachPortal`), on the source's `frameStart` (`SelectFrame`, with the paraboloid exclusion the shadow epoch published,
`ParabolicExclusionSlot::Pending`) and `frameEnd` (`EndFrame`), a stand-in observer's `structure` (`NoteStructure`: the category filter's
attaches and detaches), installed by its source hooks at the Point source, before the primary cull reads NiNode's OnVisible slot (then
`CullsInstalled`, read as `Extensions::SourceCullsInstalled(Point)`); its lines at `ReportPoint::PrimaryCullSources`
(`Diagnostics/PointCullLines.cpp`, formerly `LocalLightLines`) and its parity (`Diagnostics/LightCulls`, `LocalLightCullView.h`, under
`Observe().scene`). What it leaves Core is the paraboloid exclusion's build and slot (`Core/Render/Shadow/SunExclusion.h`:
`ParabolicExclusionSlot`, published by ShadowEpochs only while the Point source's culls are installed; formerly `LocalLightCull::Publish`)
and the diagnostics events the parity takes. The Sun's registration thunk calls its `NoteMainRegistration` directly (Shadows/Sun DEPENDS
{Shadows/Local}, listed before it).
`Features/LocalLights` followed, the selection and receiver side of the local shadow lights: the light selection (`LightSelection.{h,cpp}`,
the CalculateActiveShadowCasterLights port, formerly `Engine/Views/`; installed by its source hooks at the Spot source; handing the Spot and
Point sources their kept lights, `lightKept`, and publishing the frame's slots on Core's `LightSlots` board), its room test through Portals
(`PortalViews::WalkLight`, `SharesRoom`: static calls, LocalLights DEPENDS {Portals}), each object's main-pass shadow mask
(`LocalShadowLights.{h,cpp}`, formerly `Scene/Ports/LocalShadows`: the kept lights' volumes, sampled per colour commit on the light volumes
contract, "local shadow lights", with its landscape rule as data, `kLandscapeFlags`), and its parity and lines
(`Diagnostics/LightSelectionParity`, `LightSelectionView.h`, under `Observe().claimsOrScene`; `Diagnostics/LightSelectionLines.cpp`,
self-scheduled every 300 selections). It has no switch, toggle, pool, stage or product. What it leaves Core is the light candidates
(`LightEntryOf`, with the sun's), the boards (`ShadowCameras::LightSlots`: render thread, invalidated at the selection's start, published at
its end, `Current(frame)` for the shadow views' rebuild, with the focus hosts), the light volumes' test, latch and clamp, and BuildDraws'
receiver test. The character light is the Lighting port's, not a local light.
`Features/Lod` and `Features/TreeLod` followed. Lod: object and terrain LOD's switches (`CS_DCLF_LOD_OBJECTS`, `CS_DCLF_LOD_TERRAIN`, listed
where Core's were, before `CS_DCLF_LOD_GATES`) and toggles (after the refraction's, behind the particles', billboards' and switch nodes'),
two object classes (`ObjectClass::LodObjects`, `LodTerrain`, both with `categoryRoot = TES::lodLandRoot`), its four techniques with
their property flag rules (LODObjects and LODObjectHD with the LOD object flags on `kSubIndexTriShape`; LODLand and LODLandNoise with the
LOD landscape flag on `kTriShape`) and the `kSubIndexTriShape` geometry type with the LOD object flags, which Core reads as data
(`Ineligible::Lod`, `Extensions::PropertyFlagRules`, the category capture's roots, the cube camera's LOD land and objects roots,
`ViewLodRootsOwned`); Core no longer names the LOD techniques, flags or root (`LightingDescriptors.cpp`, `Eligibility.cpp`,
`FrameRequests.cpp`). TreeLod: its switches (`CS_DCLF_LOD_TREES` after Lod's, `CS_DCLF_TREE_LOD_AUDIT`
after the texture paths' trace) and toggle (after Lod's), the tree LOD class (`ObjectClass::TreeLod`: the draw family's gate, the revisions' trees
pipeline, a trees view's `ResolveViewFaces`), the source of Core's tree LOD channel at its early hooks (the refill, block update, AddGroup
and RemoveGroup hooks, pushing through `Push`, where Core's scene events installed them; between the billboards' early hooks and
the hidden nodes') and the tree LOD audit. Both are listed after SwitchNodes, Lod first, so the toggles' and switches' order is Core's.
What they left Core then: the LOD gates whole, which needed an engine event channel contract for a gated swap (its capture, flip, outcomes
and veto); they followed (below). Object LOD's segment events followed
on the engine channel contract (`Api/EngineEvents.h`, `RegisterEngineChannel`): the writers' hooks at Lod's early hooks (between the billboards'
and tree LOD's; `LodSegmentHooks.cpp`), its keyed channel ("object LOD segments": `AfterSwitches`, drained after the switch events as Core's
queue was; `KeepKeys`; `namesKeys`, as the mirror parity counted them), its coordinator component (`events.keys`: each named tracked shape's
ranges taken again from the mirror into Core's kept ranges, `SetEntryRanges`, the shape's rewrite on a change), its report line at
`SceneCoordinatorSegments` (`componentText`) and its parity (`Diagnostics/LodSegmentParity.cpp`). What it leaves Core is what the walk reads, generic since:
the kept ranges (`entryRanges`, from the attach's capture, erased with the entry), `GeometryRecord::ranges` (`kRanges`; the printed field
name is still "segments") and the range slots (`ResolveEntryRangeSlots`). The segments' layout and capture are the feature's
(`Features/Lod/LodSegments.h`), registered as the `kSubIndexTriShape` contribution's `ranges` (`CaptureDrawnRanges`; `Api/EntryRanges.h`);
Core no longer tests `kSubIndexTriShape` in the capture, the walk or the attach's prepare (`Extensions::GeometryTypeRanged`).
The LOD gates followed into Features/Lod, once the generic structures they use existed (`Api/EngineEvents.h`: the `SceneStack` channel kind,
the show veto, adoption guards, reserved hidden sites; `Api/CoordinatorComponents.h`: root marks, entry vetoes and observed slots, the
commit's settle, take and publication hooks, the adoption hook). The feature now registers their switch (`CS_DCLF_LOD_GATES`, listed where
Core's was: before `CS_DCLF_SHADOWS`, after Lod's two and TreeLod's, which name it), installs their engine hooks at its install
(`LodGates.cpp`: the swaps' thunks, the LOD drain's and retirement's detours, the forced paths; `ShellState::Running` for the running
query; no other hook shares their sites, so the move from the shell, right before the features, changed no patch), and registers, while
they are installed and first of its registrations (where Core registered them, before every feature), their SceneStack channel, show
veto, reserved hidden site, adoption guard, coordinator component (`LodGateComponent.cpp`, `LodGateAdoption.cpp`; after tree wind's,
which shares no hook with it, and before the segments') and report lines (`LodGateReports.cpp`: `SceneCoordinatorGates`, `GateReleases`,
alone at both), all in `LodGateChannel.cpp`; their observers and parity (`Diagnostics/LodGateObservers.cpp`, `LodGateParity.cpp`) are
subscribed under the switches Core's observer install used. What they leave Core is the generic structures, the show veto's cells the
hidden-node source's stubs load, the view engine's read of the gates through the show veto (`ViewLodRootsOwned`), and the Diagnostics
event their observers take (`SetCommitted::gatesLive`). The gates' hook points are the feature's (`Diagnostics/LodGateEvents.h`:
`GateFlipsPublished`, and `GateReleasing` with its claims read opaquely through `Diagnostics::SceneView::ClaimedPhases`), the gates engage
once the hidden-node source is installed (`Extensions::HiddenEventsLive`), and they stamp their frames with Core's frame clock.
Tree LOD's renderer followed, on the draw family contract (`Api/DrawFamilies.h`): Features/TreeLod takes the `TreeInstances` family. It keeps
the channel and the render thread's mirror (`TreeLodMirror`, drained by the family's `serve`, running or not), its tables as scene resources
(the mirror's shape and mesh slots posted with the revision's inputs, `postInputs`; the draw row and list header the depth commit latches,
`latched`) and their uploads as a commit hook (the depth commit's, through `CommitContext::latched`, with `TreeLodMissed`: its event, log
and count, `Diagnostics/TreeLodEvents.h`, `TreeLodReport.cpp`), TreeLodCullCS's
two phases and the counts' readback as GPU stages at the depth segment's slots (`cs.dclf.z.tree-lod-cull`, `cs.dclf.tree-lod-cull-phase2`,
`cs.dclf.z.tree-lod-counts`), the family's decision (the mirror's shapes, the atlas, the lane's pipelines), program source and draws, a trees
view's faces (the per-face cull, the faces' declarations, draw and row, the lists' units), its report lines at `AsyncWorkFamilies` and
`SceneFrame`, the mirror's parity, the atlas's address (`TreeLod::Address`), and the cull shader (`features/Drawcall Limit
Fix/Shaders/DrawcallLimitFix/Features/TreeLod/TreeLodCullCS.hlsl`) with its constants and path (`TreeLodTables.h`: `kTreeLodCullShader`,
`TreeLodCullConstants`, `kTreeLodCullConstantWords`). What it leaves Core is the family's slot, keyed by family: the pipelines on the lane, built from the family's program
block (`FamilyPipelines`, `ForwardPipelineKey::kForwardTreeLod`), and their publication (`SceneBuffers::familyPipelines`), the ownership
published from the decision (`IndirectDraws::DecideDrawFamilies`, `PassCapture`'s withholding of the family's engine shapes by their
property's vtable), the shapes key's part, and a trees view's per-family state (`ViewFrame::faces[f]`: pipeline, signature, groups;
`ViewResources::faceRows[f][face]`, `faceLists[f][face]`, named by the faces' `rowName`/`listName` and the face's index; `ViewLatchLayout`'s
`faceBytes`, `Extensions::DrawFamilyFaceBytes()`, each family's row and header at its `DrawFamilyFaceLayout`; `CommitView` calling
`faces.face` per face). The GPU layout the shaders mirror is the feature's (`Features/TreeLod/TreeInstancesLayout.h`), and so is the cull
group (`Features::TreeLod::kTreeLodCullGroup`, `TreeLodTables.h`), reported to Core as `faces.cullGroup`.
The particles' palette rows followed, on the palette rows contract (`Api/FrameValues.h`, `PaletteRowsContribution`): Features/Particles
takes `FrameValueItemClass::ParticleSystem`. It keeps the snapshot store (`Features/Particles/ParticleSnapshots`: the writer's thunks, the
followed records, the expansion), the class's rule (five rows a particle of its capacity bucket), the quads' geometry slot (the renderer's
quads, one slot a bucket), the frame's cameras (the main one and the first person's, from its frame start and kick), and the run: the
records followed at its start, each chunk's members expanded into their blocks with their placements, the table published at its end; and
its move writer, a reserved slot ("particle update", reserved through `InstallContext::ReserveMoveWriter` in `ParticleSnapshots::Install`
before its thunks). The class is the kParticles contribution's data (`itemClass`: the plans' class, the walk's block and the geometry slot;
`moves`: the mover trait). What it leaves Core is where the class is decided (`Extensions::PlannedItemClass`; `PaletteItemClassOf` in
`LightingDescriptors`, Core's capture-side acceptance: a complete particle record; the Effect port's particle descriptors), the class's
items due every frame,
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
its material records (`EffectMaterial.cpp`, kind 1: `RegisterMaterialPort` returns it and the feature checks it; the port's `name`, "effect",
names the census's classes, and its `emissiveOf` follows SetExternalEmittance), its capture and shading (`EffectProperty.cpp`), its frame rows
and soft depth (`EffectFrame.cpp`), its program family (`EffectPrograms.cpp`) and its engine addresses (`Effects::Address`,
`EffectConstants.h`: the effect shader, camera and blood). The effect controllers' and the effect material's write hooks followed, on the
material-write sink (`EffectWrites.cpp`: BSEffectShaderPropertyFloat/ColorController::Update, slot 0x27, and
BSEffectShaderMaterial::CopyMembers, slot 2; installed at the feature's install, whatever the toggle, logged as "effect write hooks
installed"). What it leaves Core is what Core decides by: the verdicts (`Ineligible::EffectOpaque`/`EffectVariant`), the Effect family's slot
(`kRasterEffect`, its spare bits, its programs' and pipelines' counters), the builds and passes, Effect.hlsl (package/Shaders), and the
Lighting controllers' and the 14 Lighting material vtables' write hooks (`Engine/Capture/MaterialWrites`). `Features/TreeLod`'s draws followed onto contracts keyed by draw family: the family's program block and engine shapes (the lane's
programs, pipelines and recipes, and PassCapture's withholding, by family), the faces' data and per-face write (the per-family view state
above), and the view roots (`Api::ViewRoot`: the faces' `root`, `LodTrees`; Features/Lod's `LodLandAndObjects` contribution).
`Features/Shadows/Sun` took the sun's engine side: the accumulation (`Features/Shadows/Sun/SunAccumulation.{h,cpp}`, namespace
`DCLF::Features::Shadows::Sun`: Accumulate, its cascade culls and registrations, the full-frustum cull and the entry exclusion, with the
feature's own `ExcludeSun`), installed by its source hooks before `SunViews` (then `ShadowSourceHooks::CullsInstalled`, which
Features/Shadows/Local's `LocalLightCull::Install` reads as `Extensions::SourceCullsInstalled(Sun)`); the source's `fullFrustumWindowEnd`; its switches and toggles
(`CS_DCLF_SUN_SKIP`, `CS_DCLF_SUN_EXCLUDE`, after `CS_DCLF_SHADOWS`; "skip sun accumulation", "exclude sun entries", after "shadows"), which
Core's "exclude primary entries" needs by name; its report lines at `ReportPoint::RenderThread` (`Diagnostics/SunAccumulationReport.cpp`)
and its diagnostics (`Diagnostics/SunCasters`, `SunAccumulationView.h`). Its registration thunk also does the main
registration's other work (PrimaryCull's owned masks, Shadows/Local's `LocalLightCull::NoteMainRegistration`, a static call: Shadows/Sun
DEPENDS {Shadows/Local}), so without the feature the point lights' culls stay off (LocalLightCull's install warning), as when the call
sites fail their check: no fallback. What it leaves Core is the exclusion's data:
`Core/Render/Shadow/SunExclusion.{h,cpp}` (`SunExclusion`, `TranslateExclusion`, `ForwardExclusion`, and `SunExclusionSlot`: render thread,
`Publish`, `Pending`, `Take` at the full-frustum cull, `BeginFrame` at the frame's start, `TakeForwardCounts` for the feature's stats), the
cascades' GPU form (`Draws::GpuCascade`, `GpuLayouts.h`; Uploads.cpp's `GpuCascades`), the entry index space the exclusion is over
(`Core/Scene/EntrySpace.{h,cpp}`: `EntrySpace`, `EntrySpaceChanges`, the ChangedEntries readers; the table, `EntrySpaceTable`, in
`Scene/Coordinator`; formerly SunCandidates and CandidateTable), and the sun's cascade bits as a Diagnostics event (`Diagnostics::SunBitsSet`).
`Features/Skins` followed, the policy of which skinned shapes are DCLF's: its switches (`CS_DCLF_SKINNED`, `CS_DCLF_SKIN_PARTITIONS`, after
`CS_DCLF_CULL`), its toggles ("skinned", `.before = "projected"`, opening "Object classes"; "skin partitions", `.after = "tree LOD"`, needing
"skinned"), two object classes (`ObjectClass::Skinned`, `SkinPartitions`, read through `Extensions::ObjectClassOn` by the eligibility, the
Lighting descriptors, the walk and the Effect descriptors) and its report line at `SceneCoordinatorLastPass` (`Diagnostics/SkinsReport.cpp`).
Its toggles needed the toggle contract's `before`, and `after`/`before` naming any feature's toggle. The features that read skins declare it
(DEPENDS: FirstPerson = {Actors, Skins}, Effects = {Skins}), and the layout check keeps a dependency listed before its dependent (rule 4:
Skins right after Reflection, before Actors). What it leaves Core is the path a skinned shape takes once its class is on, shared with the
classes that read skins (Actors, the LOD chains, FirstPerson, Effects, the blood decals): the skin engine facts (`Core/Engine/SkinPartitions.h`:
the LOD table, `Mask` from the live skin or its record, `Of` the record's skin partitions, `LodOf` its LOD partitions;
`Core/Engine/SkinPalettes.h`: the palette rows, `kRowsPerBone` 3, `kMaxRows` 240, `RowsOf(record)`, `Fits`, the engine's palette update
`Update`, `Of` the palettes, `Sample` the update then the read, under the caller's lease), the palette part FrameValues shares with the
particles (`kObjectPaletteBlock`, the C++ name of HLSL's `kObjectSkinned`), the drawn-range column the LOD chains and the decals share, the
vertex stages' bones, `kTraitSkin` and the Ineligible reasons. The palette sample stays a per-item engine read; Skins as a palette-rows class
is optional, and only with a timing A/B.
`Features/Actors`' owner class followed: `Api::EntryClass::ActorOwned` on the entry class contract (`EntryClassContribution`,
`RegisterEntryClass`; `Extensions::OwnerClassOf(const NodeRecord&)`, `OwnerClassOfForm`, `RegisteredOwnerClasses`, `OwnedEntriesLabel`), read
as data where Core tested for an actor (`Tracked::owner`, `ownerReference`, `kTraitOwnerClass`, `FrameVerdictKept`,
`EvaluateKind::Owned`/`OwnedNoRecord`, the reports' "actor" from the registered name).
`Integrations/AdvancedSkin` followed, the first integration: Advanced Skin's wetness as a shading field (`SkinPerGeometry`) of the published
actor-owned class, its owners' values (`OwnerValues.h`, formerly `Scene/Model/ActorValueIndex.h`) kept from the membership writes and
captured at the frame's start, and its parities (`Diagnostics/`: the draw wetness in CaptureParity's skin line, the row wetness in
SceneFrameLines' shading line). Its material textures (t71/t74) and their parity wait for the material contributor.
`Features/Landscape` and `Integrations/TerrainBlending` followed. Landscape: its switch (`CS_DCLF_MTLAND`, `.before = "CS_DCLF_FORWARD"`),
its toggle ("terrain", `.after = "projected"`) and the MTLand and MTLandLODBlend techniques (8, 19) under it, taking the land-blend extras
(`ObjectExtras::LandBlend`). Terrain Blending: its classify input ("terrain blending defers": loaded and enabled, which replaced
`TerrainBlendingDefersTerrain` and its snapshot scope), its switch (`CS_DCLF_TB_TERRAIN`, after the MTLand's) and toggle ("terrain under TB",
after the terrain's and needing it), the gates on techniques 8 and 19 (needing its toggle while it holds the terrain: `MtLandEnabled`'s
rule), its two held groups (`Appended0` "terrain", order 2, `kMultiTextureLandscape`, always; `Appended1` "terrain-no-blend", order 3,
`kNoTransparencyMultiSample`, under its toggle; formerly Core's built-in ranges, `CoreMainRanges`; `Ineligible::TerrainNoBlend` became
`Ineligible::Held`, its census text kept), its mask (t55, a frame texture with `capturedWhenNull`) and its near twin (its terrain depth's
import and the depth pass's near test). Core no longer includes `Features/TerrainBlending.h`; the integration does. What they leave Core:
the MTLand material handling (the Lighting port's SetupMaterial replica, the Landscape and LODLandscape vtables, the source signature's
landscape bit, the PS names: one evaluator over a shared class table and constant space, until the material contributors), the land quads'
`LandSetupProperties`, the landscape lights (`kObjectLandscapeLights`), the land-blend extras (`kObjectLandBlend`, its rows shared with the
projected UV's mode word, the LandBlendParams hook, the capture and its parity) and the main pass's structure (the appended slots, the twin
buckets, the terrain depth pass, BuildDrawsCS's layout). The near-test parity and the install's t55 line stay in Diagnostics, reading the
feature live. LOD terrain was already `Features/Lod`'s.
`Integrations/LightLimitFix` followed: its classify input ("light limit fix": loaded), which Features/Effects' Lighting variant reads by
name (`NeedClassifyInput`; `kEffectVariantLightingNoLlf` stays the Effects'), its room index as the ancestor value (the nearest
BSMultiBoundRoom or BSPortalSharedNode, LLF's GetParentRoomNode, looked up in its room map by `GetRoomMapGeneration`; record word 0, -1
outside rooms), its graph buffers (t35-t37, `cs.llf.lights`, `cs.llf.light-index-list`, `cs.llf.light-grid`, from its ORG culling's
`GetShaderResourceIndices`) and its parity (`Diagnostics/LightDataParity.cpp`: CaptureParity's StrictLightData check, called from it as
AdvancedSkin's is; the dead shadow mask printed as 0). `Features/Manifest.cpp` checks t35-t37 and word 0. What it leaves Core is the key
walk, the map's post and the join's lookup, the column (formerly `ObjectLights`, its `shadowBitMask` dead; now `ObjectAncestorValue`), and
the zeroed StrictLightData block every draw reads (PS b3, frame-slot layout). Without the integration DCLF draws as with LLF not loaded:
-1, t35-t37 left to the frame buffers, "lighting-without-llf".
`Features/GpuOcclusion` followed, on the depth pyramid contract (`Api/DepthPyramid.h`): the HZB's producer, its texture (`cs.dclf.hzb`),
its downsample's program (`HzbCS.hlsl`, FidelityFX SPD, in the feature's shader folder; `FidelityFX/` stays where it is), its finished-group
counter (the pyramid's scratch, `cs.dclf.hzb-counter`) and its pass (`cs.dclf.hzb`), with `HzbLayout.h`. It registers no switch, toggle,
report line or parity. What it leaves Core is the test, shared by everything the cull reads: `CS_DCLF_CULL` (mode 2 tests the pyramid when
there is one), BuildDrawsCS's HZB branch and `HzbTest.hlsli`, the two-phase visibility words, the phase 2 block and its stages, the latch's
mip-0 convention, the bindings of BuildDraws and the tree LOD cull, and the HZB and two-phase lines and SET_PARITY's reasons in Diagnostics.
The visibility events (`Core/Render/Passes/VisibilityEvents.cpp`, formerly `Occlusion.cpp`), their readback and the kAccumulated write-back
are not occlusion: they read the fade roots' InView and phase 1's frustum stamps, and are the stand-in's write-back, Handoff's (with Fades).
Without the feature, culling is frustum-only, as when the program fails.
`Features/Forward` followed: the forward epoch's switches (`CS_DCLF_FORWARD`, `CS_DCLF_REFRACTION`, before `CS_DCLF_PROJECTED_UV`) and toggles
("forward", before "shadows"; "refraction", after "effects", needing "forward"), two object classes (`ObjectClass::Forward`, `Refraction`,
read where Core read the toggles) and the engine's two forward points (the main pass's blended-draw thunks, which run
`IndirectDraws::ExecuteForward`, and Main::Draw's refraction check, with the refraction normals' engine marks written once
`ExecuteForward` says it drew them), installed at its install. It is listed right before Effects, whose toggle needs "forward". What it
leaves Core is the forward epoch's machinery, shared by Lighting's blended objects, Effects (with Particles and Billboards) and FirstPerson:
the forward and refraction ranges, their keys and bits (`kRasterForward`, `kObjectForward`, `kViewForward`), the forward sort, the epoch's
recording, coverage and stats, refraction's pipeline and normals import, `DeriveForward`; and the views' forward parts, which are the view
engine's (`TryRequestForward`, `ForwardTargets`). The forward and refraction lines stay in Diagnostics.

Not generalised (each a deliberate Core part, not a contribution):
- **Billboard placement row** (`Core/Gpu/PlacementRows/BillboardRow`: `PlacementRows::SampleBillboardRow`, `BillboardFaceMode`). An item has
  one class (a particle under an NiBillboardNode gets both treatments; a class would change its rows and make them due every frame); three
  parities need the row per slot (`FrameValues::SampleSlot`); FrameValues samples it for every moving object whatever the toggle; and its
  layout is the frozen decode of Effect.hlsl's `DCLFBillboardWorld`. A static call. Features/Billboards keeps the switches, toggle, parent
  contributions, the CPU turn and its parity.
- **Shadow texture-transform scroll** (`Extensions::DrainTransformMoves`, `Core/Extensions/MaterialWrites.h`; ShadowBuild's `ShadowTexcoord`
  and `WatchTransformMoves`), and not Features/TextureTransforms'. The shadow material rows read the transform on the CPU in every
  configuration; TextureTransforms has no switch, and depending on it would need a banned fallback; a GPU patch would add a shadow pass and
  a material map; a ShadowViews row contribution would be a per-object-per-frame callback; and `ShadowMaterialRow` is Utility.hlsl's frozen
  layout. The hooks push every move to the `TransformMoves` queue (the Lighting float controller's, in `Engine/Capture/MaterialWrites.cpp`)
  whatever patches the frame VS variable; the kept shadow build drains it (the plain build rereads every row each build).

The extension points still to come, as the next features need them:
- material contributors (Advanced Skin's t71/t74, TruePBR);
- the shared-bit manifest (object and record flags).

## The view contract

The engine views Core's bucketed view machinery draws (`ViewTraits`, `ViewFrame`, `ViewResources`, `ViewLatchLayout`, `PlanViewBuckets`,
`MakeViewShape`, `CommitView`, `ViewExtension`) are taken by features through one contract: `Api/Views.h` (`RegisterView`,
`RegisterViewWork`), with its Core companion `Core/Extensions/Views.h`. The reflection's faces and the first person are on it. The
occlusion maps and the shadow views are another family (`ShadowFrame`), on the sibling contract below; what both families share is
`Api/ViewCommon.h` (`ViewPoint`, a feature view's `ViewBits`) and its companion `Core/Extensions/ViewCommon.h`. The portals (the main
cull input, `Api/CullInputs.h`), local lights, GPU occlusion (the depth pyramid, `Api/DepthPyramid.h`) and the forward epoch (two object
classes and the engine's forward points) are not views.

- **Core's fixed slot per view.** `Api::EngineView` is Core's slot order (`Reflection`, `FirstPerson`): the epochs' indices, the latches,
  the revision key groups, the coverage and the setup run in it. `Core/Render/Views/ViewSlots.h`'s `kViewSlots[v]` reserves the view's
  epochs (`Epoch::*`, whose `kEpochTraits` order is load-bearing), its `RenderGraphRuntime::Segment`s, its key group (the shapes-made
  report's names, "reflection" and "first person") and its graph extension's id. A new view is the enum value, a CS segment, its slot
  row, then the feature's registration.
- **The feature supplies data and per-frame callbacks.** `ViewContribution`: the view, its toggle, its set phase (one bit, the feature's)
  and its shape's view bit (one bit, the feature's: both checked against Core's and each other's by the install and by
  `Features/Manifest.cpp`), its membership (`CategoryRoot`: the entries under the root `captureRoot` names, an exclusive phase whose main
  mask is the view bit alone; `LodTechniques`: a main-eligible slot whose pipeline is a LOD Lighting technique, beside the main phase),
  its key rule (`lodTechniquesOnly`, `forwardRasterToForwardPart`; `keyFlags`, the view's own forward key bits, above Core's two-sided and
  tree LOD bits, at most one view without any, by which the pipeline lane finds a key's view, `Extensions::ForwardKeyView`,
  `ViewSet::plainKeyView`, `ResolveViews` failing overlapping bits or a second view without any; `cullBackFaces`: back faces unless
  two-sided, else front faces, a cube face's mirrored projection), its shape (`faces` up to `kMaxViewFaces`, `trees`,
  `forwardPart`, `depthPass`, `viewBits`, names and latch labels, the initial `depthRange`), its life (`KeepLast`, `DropWhenOff`: dropped
  from the revision while off), its area (`Target`, `MainColourViewport`), its withholding (`InMainRenderers` with the `accumulators`
  callback; `OwnRenderers`, whose renderers its engine hooks name), its target import (`ColourWithOwnDepth`, `DepthTarget`), its coverage
  before the first decision and on a failed graph, its decals as forward objects, and its `covered` callback (required; render thread,
  once a frame at `DecideCoverage`, the views in slot order, over Core's precomputed, side-effect-free `ViewCoverage`), and an OwnRenderers
  view's optional `residueDue` (render thread, once per render that would leave the view's LOD roots to DCLF,
  `IndirectDraws::ViewLodRootsOwned`: whether that render keeps them, the view's renderers' unwithheld passes under them watched,
  `PassCapture::WatchViewResidue`, `TakeViewResidue`, `TakeViewResidueGeometries`; the install refuses it on another view). `ViewWork` runs
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
  `ExchangeViewForwardCompatible`, `DrawPipelines::SetViewForwardTargets` (the view's forward targets, `PipelineCatalog::viewTargets`),
  `ResolveViewPipelines` (wanted slots or every slot; with the programs and waits counted on request),
  `DropViewPipelines`, `SetUpView`, `ImportViewTarget` (by its target import), `TakeViewInputs` (`DepthThisFrame`,
  `DepthThisFrameColourBefore`: the frame before's colour frame record, the target imported after; `ColourThisFrame`), `SubmitViewEpoch`
  (the revision's recording and shape, the payloads held, the faces committed: `Submitted`, `NoRevision`, `NoOwners`, `Failed`, which the
  feature maps onto its own skip causes and log lines), `ViewCovered`, `ViewDrawable`. A trees view's draw families per face are the view engine's
  (`ResolveViewFaces`, `DecideViewFaces`, per family, a no-op unless the view has trees; the rows `SubmitViewEpoch` commits), drawn by each
  family's faces while the family is on (`TreeInstances`: Features/TreeLod's, under `ObjectClass::TreeLod`); a view feature reaches them
  through these calls, never a LOD feature's headers.
- **View roots.** A covered view's owned roots (`ViewLodRootsOwned`, `Core/Render/Views/ViewRoots.cpp`, once per plain render) are the OR of
  `Api::ViewRootBit(faces.root)` over the draw families whose faces the view draws (`ViewEpochState::facesOwned[f]`) and the roots of the
  view root contributions whose `owned()` is true (`Extensions::Views().rootOwned`; Features/Lod's `LodRootsOwned` decides the LOD land and
  objects roots from the LOD gates' show veto and the LOD classes). The residue watch is `Views().rootBits` (the contributions' roots) or'd
  with the owned roots; PassCapture's residue test is generic: a draw family's engine shape is watched under its faces' root, anything else
  under `Views().rootBits`.
- **Withholding.** `PassCapture` keeps each view's renderers and coverage (before the first decision, its `coveredInitially`). An
  InMainRenderers view's member is tested inside `WithholdMain`, before the main phase's; an OwnRenderers view's after it
  (`WithholdOwn`): its member's pass into the renderers its hooks named (`SetViewRenderers`) while its render withholds
  (`SetViewWithholdActive`) and its epochs draw, or a draw family's engine shapes while its faces are DCLF's (`SetViewFamilyOwned(view, family, owned)`; the slot's `facesOwned[f]`).
  `SetViewRendering` marks its render for the observers.
- **Hooks and shell state.** A view feature's engine hooks are its own (from its install, or its early hooks when Core's engine side and
  observers must find them in place); they read `Core/Engine/ShellState.h` (`Running`, `Passes`). The shell's `Main_RenderDepth`
  world-drawn point runs `FrameDriver::DepthCommitted` (the depth commit, then the `DepthCommitted` work).
- **Reports.** A view's lines go at `ReportPoint::AsyncWorkViews` (after the visibility lines, before the forward epoch's) or
  `AsyncWork` (after it); a CategoryRoot view's scene-side lines at `SceneCoordinatorSet` (the coordinator's set lines).

## The shadow-view contract

Every caster view (sun, spot, point, focus) is drawn by one shadow engine: one execution (`ExecuteShadowFrame`), one `ShadowPayload`,
one set of resources and index pool, one latch block and one key group (`kKeyShadow`); the occlusion epoch (`ExecuteOcclusion`) draws
from the same commit. Casters are keyed by the engine's render mode, not by source, so the sources own no phase or view bit. The
features take the parts that are theirs through `Api/ShadowViews.h` (`RegisterOcclusionMap`, `RegisterShadowSource`), with its Core
companion `Core/Extensions/ShadowViews.h`; the engine itself, its capability and captures, both epochs, the light candidates, the camera
boards and their math (with the selection's `LightSlots`), the exclusion slots (`SunExclusionSlot`, `ParabolicExclusionSlot`), the caster
phases and modes, and the shadows' toggles stay Core's. The light selection is Features/LocalLights' (below), and its volumes reach Core on
the light volumes contract (`Api/LightVolumes.h`).

- **The occlusion maps.** `OcclusionMapContribution`: the map (Core's slot order, `kOcclusionSky`, `kOcclusionPrecipitation`: its mode,
  view slot, depth target and technique column), its toggle and `available` flag, its `ViewBits` (the feature's own), its occluder rule
  (`technique`: the scene work, per walked entry while the map is on, for a non-LOD object with a Lighting property, pure; resolved once
  at install, called at the walk, the classification's one call per entry per map), its size test (`minRadius`, `kCullMinRadius` in the
  map's latch), its target (`EngineMask`, or `ImportedTexture` with
  `importTarget`, called once as the capability sets the maps up, in Core's import order: the precipitation mask, then the sky map), its
  labels (the view-mask parity's list, the report lines', the imported target's) and its optional `referenceDue` (the engine's
  reference frame for the map's parity, asked by the shell's `OcclusionReferenceFrame`), its `camera` (the shell's `OcclusionView`) and
  `ownsAccumulator` (the shell's 0x1C capture, the maps asked in slot order until one owns the accumulator). `Extensions::OcclusionOn` is whether DCLF
  draws a map (taken, available, its toggle on); a map's phase and view bit reach the set decisions, the classification, the builds'
  masks, both epochs and the reports through its entry.
- **The shadow sources.** `ShadowSourceContribution`: the source (`Sun`, `Spot`, `Point`, `Focus`), for a spot or point source
  `lightKept`, called at the end of the light selection for each kept light of its kind, in the selection's order, and for the sun
  `fullFrustumWindowEnd` (required, refused on the others: render thread, once a frame at `PrimaryCull::AfterFullFrustum`, whether the
  frame's full-frustum cull applied the exclusion; the primary cull's window is live only while the Sun is registered and it says so),
  and for any source the optional `frameStart` (render thread, in the frame driver after the frame requests are served, before the main
  renderers' refresh) and `frameEnd` (at Present, first in `PrimaryCull::EndFrame`), each in source order.
  Its culls and cameras are the feature's, from its `Api::Feature::sourceHooks` (called with each source in `ShadowSource` order:
  Features/LocalLights' light selection at the Spot source, Features/Shadows/Local's point culls at the Point source, so the install order
  is the sources' whatever the list's; a source's cull hooks report themselves with `ShadowSourceHooks::CullsInstalled`, read as
  `Extensions::SourceCullsInstalled(source)`), into Core's boards (`Core/Render/Shadow/ShadowCameras.h`: `Sun`, `LightSlots`, `Lights`,
  `Focus`, render thread, read in place by the capture, the inputs' preparation, the shadow views' rebuild, the sun's accumulation after its
  `bitsReady`, the local shadow lights and the actors' rate). The paraboloid mode's exclusion goes from the shadow epoch to the next frame's
  point culls through Core's `ParabolicExclusionSlot` (`SunExclusion.h`: published only while the Point source's culls are installed, taken
  by its `frameStart`). The view list covers a view only while its kind's source is taken
  (`ShadowViewSet::sourceOf`), and the capture reads its camera from that source's board.
- **Resolved once.** `Extensions::ResolveShadowViews` (in `InstallFeatures`, after the views) makes each entry and derives each mode's
  phase (`phaseOfMode`), the set decision's readiness order (the casters', then the maps'), each engine view kind's source and the
  import order. A second registration of a source fails the install.
- **Where it stands.** The occlusion maps are `Features/OcclusionMaps`' (their switches, toggles, bits, Skylighting's flag and texture,
  occluder rules, camera and accumulator, their reference parity, view check and report lines). The sun source is `Features/Shadows/Sun`'s
  (the accumulation, `SunAccumulation`, with its switches, toggles, report lines at `ReportPoint::RenderThread` and `fullFrustumWindowEnd`;
  its UpdateCamera hook, `SunViews`, writing the `Sun` board; Core keeps the exclusion's slot over the entry index space, `EntrySpace`),
  and the spot, point and focus sources are `Features/Shadows/Local`'s (the kept lights' views, `LightViews`, as their `lightKept`,
  writing the `Lights` board; the point lights' culls, `LocalLightCull`, on the Point source's `frameStart` and `frameEnd` and a stand-in
  observer's `structure`, their lines at `ReportPoint::PrimaryCullSources`; the focus cameras' hook, `FocusViews`, writing the `Focus`
  board). The light selection, which hands the local sources their lights and publishes the `LightSlots` board, and the local shadow
  lights' volumes are `Features/LocalLights`' (DEPENDS {Portals}, its room test). Core keeps the views' tallies the shadow epochs' checks
  fill (`Diagnostics::ShadowViewTally`), and everything above. The engine's focus targets are `Features/Shadows/Local`'s
  (`EngineFocusTarget`, in FocusViews). Each source's parity is its feature's (`Diagnostics/`, under either parity switch): the sun views'
  line, the accumulation's cascades against the cascade culls' and the entry rule's planes (`Shadows/Sun`); the light and focus views'
  lines, the focus membership and the local shadow volumes, and the point lights' culls under `CS_DCLF_PERSISTENT_PARITY` (`LightCulls`,
  `Shadows/Local`); the light selection and its rooms (`LightSelectionParity`, `LocalLights`). The occlusion maps' view check is `Features/OcclusionMaps`' (`Diagnostics/OcclusionViewCheck.cpp`).

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
   - The dependencies are the check's `DEPENDS` (Shadows/Sun: Shadows/Local; LocalLights: Portals; FirstPerson: Actors, Skins; Effects:
     Skins).
   - `Diagnostics/` may be included from anywhere. Production code reaches it only through its observer contract (`Hooks.h`,
     `Observation`, `Policy`), which the parity rework will redefine.
4. **A dependency installs first.** Every feature `DEPENDS` names is listed in `Features/FeatureList.cpp` before the feature that
   depends on it, so its registrations (toggles, classes, report lines) run first.
5. **Old includes may only shrink.** Includes into the folders that haven't moved yet (`Engine/`, `Scene/`) are
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
