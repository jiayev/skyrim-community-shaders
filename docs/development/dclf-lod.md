# DCLF: LOD terrain, objects and trees (plan)

LOD is the last class of world geometry the engine still draws itself. This plan covers terrain LOD, object LOD and
tree LOD: what the engine draws, what DCLF needs to take each one over, and the order to do it in. The standing rules
apply:
- membership and state by events, not per-frame scans;
- lock-free;
- contracts fixed in the shared component;
- parity before timing;
- validation under the camera traversal.

## What the engine draws: the census (2026-10-03)

`CS_DCLF_LOD_CENSUS=1` (a temporary probe in `Diagnostics/OpenDefectProbes.cpp`) counts two things:
- every native pass drawn through the batch renderer's two hooked loops (`RenderPassImmediately`), split into the
  depth and colour passes;
- every shadow-mode registration DCLF does not withhold.

Each pass is keyed by shader type, LOD flag, material feature, technique, geometry class and scene root. The run was
a 60-second exterior camera traversal (`junk-lod3`). The LOD rows, per frame:

| Class | Root | Pass | Draws | Triangles | Distinct geometries |
| --- | --- | --- | --- | --- | --- |
| Tree LOD: `BSDistantTreeShader`, `BSMultiStreamInstanceTriShape` | `LODRoot` / `DistantRefLOD` | colour | 150–250 | ~1k (base mesh; the instances not counted) | 430–670 |
| | | depth (Utility) | 3–7 | | 15 |
| Object LOD: Lighting, `kLODObjects`, technique 21, `BSSubIndexTriShape` (`obj-LargeRef`, atlas `Terrain\Tamriel\Objects\Tamriel.Objects.DDS`) | `lodLandRoot` / block `BSMultiBoundNode` | colour | 45–70 | 160k–234k | 150–210 |
| HD object LOD: Lighting, `kHDLODObjects`, technique 23, `BSSubIndexTriShape` (`objHD-LargeRef`) | `lodLandRoot` | colour | 14–19 | 40k–52k | 50 |
| Terrain LOD: `BSTriShape` `Land` | `lodLandRoot` | depth (Utility) | 2–3.5 | 4k–7k | 4–8 |

What the census shows:

- **Terrain LOD's colour draws are not in the hooked loops.** The `Land` shapes appear only as Utility depth draws, so
  their colour pass goes through another path. Finding it is the first open question.
- **No LOD class registers into any shadow mode.** It does not cast into the sun's cascades or the point lights' views.
  So LOD is a main-view problem, plus the other views that draw it: water reflections (`bReflectLODObjects`,
  `bReflectLODTrees`), Skylighting's occlusion view (which has a `DistantTree` technique), and the world and local
  maps.
- **Object LOD is `BSSubIndexTriShape`.** The `LargeRef` names are the large-reference system: segments of a block are
  hidden while their references are loaded as full models. `BSSubIndexTriShape` overrides `OnVisible`, so the
  engine decides which segments draw during its cull.
- **Tree LOD is the largest draw count.** 600+ instanced shapes, about 200 draws a frame. Each shape is one tree type in
  one block, drawn instanced from a CPU-side instance array (`BGSDistantTreeBlock::TreeGroup::instances`: position,
  `rotZ`, scale, alpha, hidden).
- **LOD has almost no depth prepass.** It draws in the colour pass. DCLF's colour pass tests depth EQUAL against its own
  Z-prepass, so every LOD draw DCLF takes over also needs a Z-prepass entry. That is a gain as well: terrain LOD is the
  best distant occluder for the HZB.

Two things the census does not measure yet:
- the engine's CPU cost of LOD (the main camera's cull of `lodLandRoot` and `LODRoot`, segment `OnVisible`, and the
  terrain manager's updates);
- the GPU time of the LOD draws.

## How the engine manages LOD (known, and to find)

Known from CommonLib (`BGSTerrainManager`, `BGSTerrainNode`, `BGSTerrainBlock`/`Chunk`, `BGSDistantTreeBlock`,
`TES`):

- **The quadtree.** `BGSTerrainManager` (one per worldspace) owns a quadtree of `BGSTerrainNode`s. Each node holds
  layers for terrain, objects and trees (and map variants), at LOD levels 4 to 32 in the node state
  (`kLODLevel4`…). Blocks load on background threads, attach to the scene (`attached`, `doneLoading`) and are swapped
  between levels as the camera moves (`updateNodes`, `immediateUpdates`).
- **The roots.** Terrain and object LOD hang under `TES::lodLandRoot`, each block as a `BSMultiBoundNode`. Tree LOD
  hangs under the `LODRoot` / `DistantRefLOD` nodes. LOD water is under `TES::objLODWaterRoot` (water shader, outside
  this plan).
- **Tree instances.** `BGSDistantTreeBlock` keeps every instance in `TreeGroup::instances` and an `instanceMap` by form
  ID. A tree's LOD instance is hidden (`hidden`, or `alpha` to 0) while its full model is loaded, and
  `allVisible`/`shaderPropertyUpToDate` track the group's state.
- **The tree shader.** `DistantTree.hlsl` (CS's): the vertex shader takes the instance data in `TEXCOORD4`–`7`
  (position and scale; cos and sin of the Z rotation), a block world matrix (`PerGeometry`), and one diffuse texture.
  It is alpha-tested.

Still to find (reverse engineering, before the step that needs each one):

1. **The terrain LOD colour path.** Which call site draws `Land` in colour, and with what technique (LODLandNoise, or
   plain Lighting under TruePBR or the LOD generator's settings)?
2. **Block attach and detach.** On which thread do blocks attach to `lodLandRoot` and `LODRoot`, through which calls?
   Are DCLF's existing node child-edit hooks enough to see them, given that they fire for category nodes?
3. **Level switching.** How a node swaps level: app-cull or hidden bits on block nodes (DCLF's hidden events would
   catch those), or attach and detach. Is there a fade or morph between levels, or a hard swap?
4. **Segments.** What `BSSubIndexTriShape::OnVisible` decides:
   - per-segment bound culling (per view);
   - persistent per-segment hiding for loaded large references (the write that hides a segment, and its thread);
   - how the pass draws the visible segments (one draw per run of segments, or an index list rebuilt per frame).
5. **Tree instance hiding.** Found (2026-10-04), below, with how the records reach the GPU ("Tree LOD: what the engine
   does").
6. **The handover to full models.** At what point the engine switches between LOD and the full model:
   - objects: the large reference's cell load against the segment hide;
   - trees: the full tree's fade-in against the instance hide;
   - terrain: the loaded grid against the LOD quads under it (`kInActiveGrid`), and the loaded land's
     `MTLandLODBlend`.

   DCLF must hand over on the same frame as the engine, or there are gaps or double draws.
7. **Which views draw LOD.** Reflections, Skylighting, the maps. The main camera's own cull of the LOD roots (what to
   cut later).

### Tree instance hiding (2026-10-04)

`BGSDistantTreeBlock::UpdateBlockVisibility` (AE `0x140503FF0`, ID 31660; Engine Fixes replaces it with
`bTreeLodReferenceCaching`, to the same rule) runs over every instance of a block:

- **The reference.** `InstanceData::hidden` is a byte on AE: bit 0 hidden, bit 1 the form ID is exact, bit 2 searched.
  Without bit 1 the engine scans the loaded files (`i << 24 | id & 0xFFFFFF`) for the first reference whose base has tree
  LOD, then stores the full ID and sets bits 1 and 2. Engine Fixes keeps its own cache (misses included) and writes the
  byte as a bool, which drops bits 1 and 2.
- **The rule.** With the reference's 3D loaded, not app-culled (`+0xF4` bit 0), and its cell attached (state 7): under
  `bEnableStippleFade` the instance's alpha is `1 - currentFade` of the 3D's fade node (`+0x130`), hidden at 0; without
  stipple fade, hidden at once. A disabled or deleted reference (`0x820`) is hidden. A change clears the group's
  `shaderPropertyUpToDate` (+0x24); a hidden instance clears the block's `allVisible`.

So the LOD instance crossfades with its full tree, by the fade the engine's main cull writes on the tree's node.
`CS_DCLF_TREE_LOD_AUDIT` counts instances shown over a loaded, visible full tree.

**The stand-in left tree LOD over its trees** (fixed: drawcall-limit-fix.md, "The fade write-back"). A tree root DCLF stands in for (`kFadeRootStoodIn`) is not culled by
the engine, so its node keeps the fade the engine last wrote (often 0 from the load), and its LOD instance stays at alpha
1 while DCLF draws the faded-in tree. On the teleport route's last location (cell (1, -13), above the Guardian Stones),
about 200 instances stayed shown over loaded trees to the end of the run with the stand-in on, with or without Engine
Fixes' cache. With the stand-in off (`CS_DCLF_PRIMARY_EXCLUDE=0`) they faded out within about 15 s as the camera turned
to them, to 3. It is the stale node fade that the Skylighting reference also reads ("The fade roots",
drawcall-limit-fix.md).

## Tree LOD: what the engine does (2026-10-04)

All addresses are AE 1.6.1170.

- **The shapes.** A block's attach (`FUN_140503630`) makes one `BSMultiStreamInstanceTriShape` per `TreeGroup`, cloned from
  its tree type's base shape (the type table at `0x14314d170`, 0x28 bytes an entry: width `+4`, height `+8`, base shape
  `+0x20`). Each shape gets a new `BSDistantTreeShaderProperty` (two-sided) and its translation from the block's terrain
  node, and is attached under the LOD trees root (`*0x14315b880`, `AttachChild`).
- **The records.** The refill (`FUN_140504a40(block, group)`) packs at most 75 of the group's instances (`0x4B`) into 32-byte
  records of 16 halfs:
  - position (`x`, `y`, `z` copied, block-relative);
  - scale, or 0 when hidden (the hidden byte's bit 0, ignored while the block's `allVisible` is set);
  - `cos` and `sin` of `rotZ`;
  - alpha (copied);
  - 1.0;
  - eight zeros.

  It gives them to the shape's `AddGroup` (vfunc `0x1E0`), which makes the group's vertex buffer and its AABB (padded by the
  type's larger extent times the largest scale), then sets the group's `shaderPropertyUpToDate`. The refill is called from
  the attach and from the block's update (`FUN_140503f70`), which refills each group whose `shaderPropertyUpToDate` is
  clear, after removing its old group (`RemoveGroup`, vfunc `0x1E8`). The detach (`FUN_1405033d0`) removes the group, then
  detaches and releases the shape. These run on the terrain manager's threads, not the render thread.
- **The draw.** `OnVisible` (`0x140e1e480`) tests each group's AABB against the view (with the far plane moved to the shape's
  `renderDistance` when it has one) and draws the shape if any group passed. `GetRenderPasses` gives the colour pass technique
  `0x5c00002e` and render modes `0xC`-`0xF` (the depth prepass among them) `0x5c00002f`, both with accumulation hint 7: in
  the main view that is geometry group 1, which the deferred pass draws with depth mode 4 (`kTestEqual`) against the depth
  prepass. The depth technique is the one that reads the instance alpha: `DistantTree.hlsl`'s `RENDER_DEPTH` discards by a
  4x4 stipple of it. So the crossfade with a full tree shows in colour only through the prepass's depth.
- **Constants.** `SetupTechnique` (`0x1414eca50`) writes the fog (VS `PerTechnique`) and the sun's colour, the ambient and
  the sun's direction. `SetupGeometry` (`0x1414ecef0`) writes `WorldViewProj`, `World` and `PreviousWorld`, the last from
  the current world transform and the previous `posAdjust`, as for the other LOD techniques.
- **allVisible without a refill.** The node update (`FUN_140510730`) sets a block's `allVisible` without clearing its
  groups' `shaderPropertyUpToDate`, so no refill follows: the engine keeps drawing the records it packed before, with
  the instances that were hidden still at scale 0, until something else changes the group. On the teleport route this was
  10-52 shapes after each load. DCLF draws by the rule instead (an accepted difference: dclf-open-defects.md, "Tree LOD
  instances the engine leaves hidden after `allVisible`").

## Tree LOD: the mirror (L4a, 2026-10-04)

`TreeLod.h` / `TreeLod.cpp`. The mirror is not under a toggle: it changes nothing the engine does.

- **Events.** The refill's two calls are patched (`0x1405037f6`, `0x140503fbf`); each names the block and group it packs,
  per thread. The `AddGroup` and `RemoveGroup` slots of `BSMultiStreamInstanceTriShape` are patched too:
  - an `AddGroup` made inside a refill pushes the records it was given, the exact bytes of the engine's vertex buffer;
  - a `RemoveGroup` on a shape with a `BSDistantTreeShaderProperty` pushes a removal.

  The block update's one call (`0x1405108ff`, in the node update) is patched too: after it, every group of the block is
  packed again by the rule with the block's current `allVisible`, on that thread, and pushed as an intended event. The drain
  applies one only where it changes the records, which is where the engine kept hidden records (above).

  The events go through a lock-free `EventQueue`, from the terrain manager's threads. `ProcessEvents` drains them in push
  order into a map of shape to records.
- **Parity** (`CS_DCLF_PERSISTENT_PARITY`, every 60 frames). The root's tree LOD shapes are compared with the mirror:
  - a shape it lacks (missing), or one it holds that the root does not (stale);
  - each group's records against the group's instances packed again by the refill's rule, with the engine's own half
    conversions and `cosf`/`sinf`. A group whose refill is due (`shaderPropertyUpToDate` clear) is skipped. A shape that
    differs is confirmed at the next drain, unless an event for it arrived (refilled after the drain).
- **Results** (the teleport route, `landjump.sh`, three runs). Up to 597 shapes and 10,139 instances; up to 2,297 events in a
  10-second report while loading, all from other threads, and none once settled. Every report: 0 missing, 0 stale, 0 differ.
  Before the block update's events, the only differences were `allVisible` without a refill (10-52 shapes in the reports
  after each load); with them, the rule overrode 8-13 groups after the loads and parity stayed at 0.

## Tree LOD: the engine's draw state (2026-10-04)

Measured at `BSDistantTreeShader::SetupGeometry` (a temporary probe), over the teleport route:
- **The main view.** Its depth prepass draws tree LOD with the depth technique (`0x5c00002f`), depth test and write, culling
  off. Its colour pass draws it with `0x5c00002e`, depth EQUAL, write mode 1, culling off. Every tree LOD shape takes both.
- **Other views.** Most of the calls are 256x256 views with no colour target (the cube map renders). They stay the engine's.
- **State.** The texture is always the worldspace's atlas (`Textures\Terrain\Tamriel\Trees\TamrielTreeLOD.DDS`), which
  `SetupTechnique` binds from the shader's static at `0x1433dcd18` (an `NiSourceTexture`; the property's own `+0x90` is null).
  Slot 0's address mode is 0. Its filter mode is whatever the last draw left: 2 in most colour draws, 3 in some. The alpha
  test is on, reference 128/255.
- **The meshes.** Every type's base mesh is two crossed quads: 8 vertices, 4 triangles, vertex layout `0x0000300000000405`
  (a float4 position and a half2 texture coordinate at byte 16, 20 bytes a vertex).

## Tree LOD: the draws (L4b, 2026-10-04)

Under the `lodTrees` toggle (`CS_DCLF_LOD_TREES`, "tree LOD"), on when unset.

- **The tables** (`SceneBuffers`, `Scene/TreeLod.h`). The mirror gives each shape a slot and each mesh (a type's base
  shape's renderer data) a slot of its own:
  - a shape row per slot: translation, mesh slot, record count, its type's extent;
  - 75 instance records per slot, the engine's 32-byte records;
  - a mesh row per mesh slot: its vertex and index buffers' device addresses (leased through `GpuResources`, the buffers held
    from the hook until the render thread leases them), stride, index count and texture coordinate offset;
  - a draw row: the tables' addresses, the atlas's and sampler's descriptors, the alpha reference, the slot count;
  - the visible list: a non-indexed `DrawInstanced`'s arguments, then the culled records' indices.

  The depth commit uploads only the slots and meshes the drain changed (`UploadTreeLod`). A slot freed by a detach is
  rewritten with a count of 0. The tables grow with the mirror (`ReserveSceneTables`), and a new backing gets every slot
  again. A mesh no shape uses any more hands its leases to the next execution.
- **The cull** (`TreeLodCullCS.hlsl`, the depth epoch, before the Z-prepass draw). One thread per record: a record past its
  slot's count, or hidden (scale 0), is skipped. The rest are tested against the depth segment's frustum (a sphere of the
  type's extent times the scale, about the instance's position) and appended to the visible list.
- **The draws.** One instanced draw in each main pass, of the largest mesh's index count per instance. Both pipelines are
  pulled, under the Z-prepass's layout: the draw's push data holds the draw row's address, and the vertex stage fetches its
  record, its shape's row, its mesh's index and vertex through it (`DistantTree.hlsl`, `DCLF_PULLED`). The engine's
  instance math is kept as is, `precise`, with `World` as the shape's translation, `PreviousWorld` from the previous
  `posAdjust`. The Z-prepass draws the depth technique (the 4x4 stipple of the instance's alpha, then the alpha test), depth
  LESS with writes. The colour pass draws the deferred technique, depth EQUAL, the engine's write mode 1. Both draw
  two-sided. The pixel stages take the atlas, the sampler (address mode 0, filter 2) and 128/255 from the draw row.
- **Ownership.** At the frame's first DCLF point, after the scene events and before the main camera's cull,
  `IndirectDraws::DecideTreeLod` decides whether DCLF draws tree LOD this frame: the toggle, the programs, the pipelines and
  the tables built, and the atlas loaded. On it, the registration withholds every tree LOD pass into the main camera's
  views (`PassCapture::WithholdMain`: hint 7 into group 1, from both the colour pass's registration and the Z-prepass's), and
  the depth commit names the slots in the draw row. Otherwise the draw row names none, the cull appends nothing, and both
  passes draw no instances. So the passes, which are prepared ahead of the commits, never disagree within a frame.
- **Programs and pipelines.** `ShaderPrograms::FindTreeLod` compiles `DistantTree.hlsl` with `DCLF_PULLED` for descriptors
  `0x10100` (deferred) and `0x10001` (depth). A pulled DistantTree build takes `Permutation.hlsli`'s values as zero statics
  (`DCLF_PULLED_ROWS`): DCLF draws no reflection. `DrawPipelines::FindTreeLod` builds the two pipelines through the pipeline
  service.
- **A defect fixed on the way.** `DrawPipelines::Update` took a reference to a finished build's artifact, then reset the
  future. When `PipelineService::PublishReady` had already dropped its own reference, that destroyed the artifact under the
  reference (tree LOD's build failed with an empty error). It is copied now, at all three sites.

### Results

- **The teleport route** (`landjump.sh`, `CS_DCLF_PERSISTENT_PARITY`, 60 s): DCLF drew tree LOD in every report, up to 414
  engine passes withheld a frame and no frame withheld but undrawn. Mirror parity was OK in all 21 reports, with no main
  camera leak and no crash. The depth commits sent 0 shape slots once settled, and 550-1,100 per report after a teleport.
- **The vista** (Lake Ilinalta, tree LOD on against off): no difference visible.

### Open

- **No occlusion culling.** The cull tests the frustum only. The HZB test (phase 1 against the last frame's, as BuildDraws
  does) is next.
- **Other views.** The cube map renders, the water reflections and Skylighting's occlusion map keep the engine's tree LOD.
- **Capture parity.** The constants are not compared with the engine's draws: the filter mode (stale engine state) and the
  alpha reference are as measured.

## Design

### Membership: LOD roots as tracked categories

- `UpdateCategoryNodes` adds `TES::lodLandRoot` and the tree LOD root. Their block nodes become category nodes, and
  what attaches under them is tracked by the same structural events as the loaded cells.
- A LOD geometry gets an object record like any other, with an LOD kind (terrain, object, tree group). It is
  classified by the same `ClassifyStatic`; `Ineligible::Lod` goes away kind by kind as each one lands.
- Block visibility (level swaps) comes from the hidden events if the engine uses app-cull or hidden bits, or from the
  structural events if it attaches and detaches. That is answered in question 3.

### Object and terrain LOD: the Lighting path, with segments

- Both draw through DCLF's existing Lighting pipelines, with CS's `Lighting.hlsl` permutations: `LODOBJECTS`,
  `LODOBJECTSHD`, `LODLANDNOISE` / `LODLANDSCAPE`. They need these techniques' constants:
  - the per-object rows, derived and gated by capture parity, as was done for trees, ProjectedUV and MTLand;
  - anything CS features add to them (TruePBR's LOD land, Terrain Blending's `LOD_LAND_BLEND`).
- **Segments.** A `BSSubIndexTriShape` becomes one record with a segment table (index range and bound). Its draws
  expand like a skin's partitions (`PartitionsOf`): one draw per visible segment range, with the segment's bound
  tested on the GPU (frustum and HZB). Hidden segments come from the engine's segment hide, as events (question 4).
  The per-segment visibility mask lives in a persistent GPU buffer, written only when it changes.
- Both go into the Z-prepass and the HZB. Terrain LOD is mostly an occluder of other LOD and of distant objects.

### Tree LOD: a new program family, one cull pass and very few draws

- **The program.** A DCLF `DistantTree` program: CS's `DistantTree.hlsl`, compiled through `ORGModuleServices` like
  the other pulled programs. It has a pulled vertex stage that reads the instance from a structured buffer, not from
  vertex streams, plus its depth variant for the Z-prepass.
- **The instances.** A persistent GPU instance table: one row per tree instance (position, rotation, scale, alpha,
  hidden, tree group). It is written when a block attaches or detaches, and when an instance is hidden or shown
  (question 5), never per frame.
- **Culling and drawing.** A compute pass culls instances (frustum, HZB, hidden, alpha) and compacts the survivors into
  one indirect instanced draw per texture, or a single draw when the texture is bindless per instance. Today's ~200
  draws become a handful.
- **Reuse.** `GrassOptimizations` already captures `BSMultiStreamInstanceTriShape` instance streams (`GrassBucketStore`).
  Its capture code and what it learned about the stream layout are reused where they fit.

### Taking LOD away from the engine

Two steps, as for the other classes:
1. **Withhold.** Withhold the engine's LOD passes from the views DCLF draws (the claims, `PassCapture`), with parity:
   every withheld pass has a DCLF draw.
2. **Cut the walk.** Take the LOD roots out of the main camera's cull, which saves the walk and the segment
   `OnVisible`.

Views DCLF does not draw keep the engine's LOD, or get a DCLF native variant (as for Skylighting's occlusion view):
- the water reflections;
- the maps;
- Skylighting.

### Parity

- **Membership parity.** On parity frames:
  - every engine LOD registration in a view DCLF draws must match a DCLF record that is visible that frame, with the
    same segment set and the same visible tree instances;
  - every visible DCLF LOD record must have an engine registration.

  This runs from the first step, before anything draws, the way scene membership was validated.
- **Capture parity.** The LOD techniques' constants against the native draws' (`CS_DCLF_CAPTURE_POINT_PARITY`).
- **Pixel parity.** The target probe at LOD handover distances, and traversal screenshots against DCLF off.

## Object LOD: built (L0–L2, 2026-10-03)

### What the engine does (L0)

skyrim-engine-notes.md has the details: "Object LOD: segments and where LOD is drawn". In short:
- **Segments.** Each object LOD shape (`BSSubIndexTriShape`) has one segment per cell of its terrain node. While the node
  is in the active or large-reference grid, the terrain manager hides the segments of the attached cells and shows the
  rest. Three writers do it, all called directly at nine call sites: show, hide, and show all.
- **The draw.** It rebuilds the runs of consecutive enabled segments when dirty, and issues one `DrawIndexed` per run.
- **The main view.** It draws object LOD in its depth prepass and its deferred pass, with write mode 1 whether
  alpha-tested or not.
- **Other views.** The census's large LOD counts were mostly the water reflection's view, which renders before the
  world.

### What DCLF does (L1–L2)

**Toggle.** `CS_DCLF_LOD_OBJECTS` / the `lodObjects` toggle ("object LOD"), on when unset like the other classes.

**Membership.** `TES::lodLandRoot` is a category node while the toggle is on (`RefreshCategoryNodes`, and the root in
`CategorySignature`). Everything under it is tracked through the same attach and detach events as a cell's content.
Terrain LOD under it stays `Lod`.

**Segments by events (`LodSegments.h`).**
- **Hooks.** The nine call sites of the three writers are patched (`InstallLodSegmentHooks`). Each makes the write, then
  pushes the shape onto `lodSegmentEvents`, a lock-free queue, from whichever thread runs the terrain manager's update.
- **The mirror.** `ProcessEvents` drains the queue and re-reads each tracked shape's drawn ranges (`DrawnRanges`):
  - the runs when clean;
  - the enabled segments when dirty, which covers the same triangles the rebuild will merge;
  - the whole shape when non-segmented.

  A change re-queues the shape for evaluation in the same frame's walk.
- **Parity.** Under `CS_DCLF_PERSISTENT_PARITY`, every tracked shape's ranges are compared with its live state on parity
  frames: "object LOD segments … <- OK".

**Draws.**
- A shape drawn whole uses its TriShape's geometry slot. A partly hidden shape gets one geometry slot per visible range,
  each keyed by its first segment's record and carrying its own `firstIndex` (`GeometrySource::firstIndex`). The slots
  are linked by `nextPartition`.
- The object's partitions word is a chain (`kPartitionChain` | count), which BuildDrawsCS, `ForEachDrawnGeometry`, the
  stale-slot sweep and capture parity walk link by link, with no cap.
- A shape with no visible range is a member that draws nothing (`kNoPartitions`).

**Shading.**
- The LODObjects and LODObjectHD techniques are supported under the toggle, with CS's `LODOBJECTS` / `LODOBJECTSHD`
  permutations.
- Their pipelines take write mode 1, alpha-tested or not, as the engine's draws do. Write mode 10 had written the
  shape's alpha into the G-buffer's alpha channels: the albedo target's alpha was 0.70 against the native 0.01.
- Object LOD never casts: `ShadowReject::Lod`, and `CastsNoShadow`.

**The engine's draws.** In the main view they are skipped by `SkipNativePass`, in the depth prepass and the deferred
pass: with object LOD on, the census finds none left there. The water reflection's LOD stays the engine's.

### Results

- **Traversal with every parity check** (`junk-lodFinal`, 60 s): 731 OK, 1 ENGINE FADE (the engine's race); segment
  parity 1,095 shapes compared, 0 differ; no holes.
- **CPU.** The tables' CPU rose by about 0.1 ms a frame for about 230 more objects.
- **Capture parity**, from a high vantage (no-clip, `player.setpos z 25000`): LODObjects 0 of 5,935 checked draws
  mismatched, LODObjectHD 0 of 3,708.
- **Screenshots** of the same vantage, object LOD on against off:
  - a mean difference of 0.5/255;
  - 0.05% of pixels over 16, all on silhouettes, with the shading identical;
  - two runs with object LOD off differ by at most 6.

  The edge differences are of the kind DCLF's other classes already show against DCLF off (0.45% of pixels over 16 at
  the same vantage, most of it the tree clusters), so they are DCLF's general draw path, not object LOD's.

### Open

- **The water reflection view** draws about 74 object LOD, 50 terrain LOD and the tree LOD of its own each frame, all the
  engine's. It is the largest LOD consumer, and L5's first candidate: a DCLF native variant of the reflection view.
- **The engine still culls and registers object LOD** in the main view; DCLF only skips the draws (L5).
- **Newly attached shapes.** A few draws a frame during the traversal come from shapes with no record yet, or not yet
  bound. The engine draws them, so there is no hole.
- **Capture parity** compares at most 8 draws of one pass (a diagnostic's array, not a draw limit).
- **`CS_DCLF_LOD_CENSUS`** (`OpenDefectProbes.cpp`) stays as the LOD steps' measuring tool: per draw class, pass, write
  mode and frame phase.

## Terrain LOD: built (L3, 2026-10-03)

### What the engine does

skyrim-engine-notes.md has the details: "Terrain LOD: registration, constants and placement". In short:
- **The shapes.** Each block is a `chunk` node with one `Land` `BSTriShape` under `TES::lodLandRoot`, technique
  LODLandNoise (18).
- **The draws.** Its main pass has accumulation hint 6, which the main registration puts straight into geometry group 0
  (`FUN_1414f5090`, not `RegisterPass`). The deferred pass draws group 0 last, through the group draw at `0x1414f1acd`,
  in write mode 1. That is the "other call site" the census could not see. Its depth prepass draw is an ordinary Utility
  pass.
- **The technique.** It writes `HighDetailRange` (the loaded grid's centre less the camera, and its half extents
  less 15), which the vertex shader uses to lower the LOD land under the loaded cells. It samples the diffuse and normal
  maps bilinear.
- **`PreviousWorld`.** For every LOD technique (9, 13, 15, 18) `SetupGeometry` writes it from the current world
  transform with the previous camera, never from `previousWorld`. A placed chunk keeps the origin there.

### What DCLF does

- **Toggle.** `CS_DCLF_LOD_TERRAIN` / the `lodTerrain` toggle ("terrain LOD"), on when unset. `lodLandRoot` is a category
  node while either LOD toggle is on; a class whose toggle is off classifies as `Lod`.
- **Classification and shading.** `IsLodLand` (a `BSTriShape` with `kLODLandscape`) lets the land through
  `DeriveLightingDescriptors`, and LODLand and LODLandNoise are supported techniques under the toggle. The pipelines
  take write mode 1, as for object LOD. Terrain LOD never casts (`ShadowReject::Lod`).
- **Technique constants** (`EvaluateTechnique`, once a frame): bilinear filtering on slots 0 and 1, and `HighDetailRange`:
  the loaded grid's centre, absolute (the vertex shader takes the draw's eye off it). It moves vertices, so the Z-prepass
  and the colour pass must draw a frame with the same value: it is taken once, before the Z-prepass build is kicked
  (`RefreshLodTechniqueRanges`, `HoldLodHighDetailRange`), and the colour pass's evaluation serves the held value. The grid
  can move between the two in one frame (a load finishing after a teleport): with each pass reading the globals, about
  600,000 pixels of terrain LOD failed the colour pass's EQUAL test for one frame.
- **`PreviousWorld`** (`DrawnPreviousWorld`, the shared contract): the record's previous transform is the current one for
  LOD, as the engine draws it. The walk's settling check uses the same rule, so a placed chunk no longer re-evaluates
  every frame.
- **The engine's draws.** Terrain LOD is a member of the DCLF set like any object (drawcall-limit-fix.md, "The DCLF set"): its
  passes into the main camera's views are withheld at registration, and never reach the batch renderers. The group draw at
  `0x1414f1acd` (`BSBatchRenderer_RenderPassImmediately<3>`) only counts a native draw of a member (LEAK). The water
  reflection's stay the engine's.
- **No occlusion.** LOD is no occluder of the occlusion maps: `Precipitation::SetupMask` culls the scene lists, which do not
  hold the LOD root, so the engine never draws it there.
- **Textures.** BGRX diffuse maps were rejected by the texture import (no format), so no terrain LOD pair could draw.
  BasicRHI now has `B8G8R8X8_Typeless`/`UNorm`/`UNorm_sRGB`: DXGI's own format on D3D12, and on Vulkan the B8G8R8A8 format
  with alpha swizzled to one, in both SRV paths. Any other object with a BGRX texture can now be drawn by DCLF as well.

### Results

- **Capture parity** from the high vantage (`tourcapy`, `player.setpos z 25000`): LODLandNoise 0 mismatched of about
  9,000–11,000 checked draws in every report but the first. The first report (at load) has 2 in `HighDetailRange`.
  Before the fixes, every draw mismatched (the filter mode), then about 1.5% (`PreviousWorld`). Object LOD stays at 0.
- **The census.** In the deferred pass, no engine terrain LOD draws are left (about 17–31 a frame before), and none in
  the depth prepass.
- **Screenshots**, terrain LOD on against off: a mean difference of 0.5/255, 0.12% of pixels over 16, max 71. The
  differences are where small object LOD (snow, rock) meets the terrain at the same depth. The engine draws terrain LOD
  (group 0) after object LOD (group 1), so the terrain wins those ties; DCLF orders its colour draws by pipeline.
- **Traversal with every parity check** (`junk-landFinal`, 60 s): 786 OK, 13 ENGINE FADE, 1 CLAIM HOLES, 1 STAND-IN WALK.
  The same traversal with terrain LOD off (`junk-landCtl`) gives 787 OK, 13 ENGINE FADE, 1 CLAIM HOLES, and also MISSED,
  CASCADE CULL and 4 RESIDENT PARITY (a fade LOD row on two ordinary objects). None names a LOD shape: they are the
  route's, not terrain LOD's.

- **The holes at transitions** (triangles dropped as LOD chunks met the loaded terrain): a new chunk's Z-prepass draw wrote
  depth while its colour pair was not ready (its textures still importing), and the native skip, which read the colour pass's
  draws of the frame before, let the engine draw its colour against DCLF's depth. Fixed by the DCLF set: an object is a main
  member only once both passes can draw it, and then the engine's passes are withheld. The teleport route
  (`landjump.sh`, `CS_DCLF_FOLIAGE_PARITY=land`) gives 0 unshaded terrain LOD pixels in every report.
- **One-frame holes 1-4 s after a teleport** (up to 2.1 million pixels), on 7 of 9 runs of the teleport route. The two
  segments selected the same chunks, it was not the async builds, and the hole's owner slot was always taken by a new
  object two frames later: a chunk the engine was swapping out.

  DCLF draws the game's vertex and index buffers by their device addresses, which DXVK does not see. A geometry slot's
  import leases were dropped the moment the walk cleared the slot. The engine had already released the chunk, so DXVK
  freed the buffer and gave its memory to the next buffer the game created (a chunk streamed in), while a frame already
  submitted still had its colour pass to run. That pass drew other vertices than its Z-prepass had, and failed EQUAL.

  A cleared slot's leases (and a stale slot's old ones) now go to the next execution committed
  (`SceneStore::TakeRetiredImports`), which holds them until the GPU retires it. The route then gave 0 unshaded pixels
  in all 63 reports of three runs. Shadow views draw the same slots and get the same protection.

### Open

- **Equal-depth ties with object LOD** (above). Matching them needs the engine's group order between LOD classes in
  DCLF's colour pass.
- **`HighDetailRange` at load**: two draws in the first report differ from the engine's: DCLF draws a frame with the range
  taken before the Z-prepass, the engine with its own at its draw.
- **The water reflection view** still draws its own terrain LOD (about 60 draws a frame at the vista): L5.
- **`CS_DCLF_LOD_CENSUS`** is a temporary probe.

## Steps

| Step | What | Gate |
| --- | --- | --- |
| L0 | Measure and reverse-engineer: the terrain LOD colour path, LOD's CPU and GPU cost, questions 2–7 | answers written into skyrim-engine-notes.md |
| L1 | Membership and visibility mirror, no drawing: LOD roots tracked, blocks, levels, segments and tree instances by events | membership parity clean under the traversal |
| L2 | Object LOD and HD object LOD drawn by DCLF (Lighting, segments, Z-prepass), native passes withheld | capture parity, membership parity, traversal screenshots |
| L3 | Terrain LOD drawn by DCLF | as L2 |
| L4 | Tree LOD: the DistantTree program, instance table, cull pass, few draws | instance parity, capture parity, screenshots |
| L4a | The mirror of the groups' records, by events (done) | mirror parity clean under the teleport route |
| L4b | DCLF's tree LOD tables, cull and draws, the engine's passes withheld (done) | no holes, parity, the vista |
| L5 | The engine's LOD culls cut in the main view; other views decided (native variants, or the engine's) | the main-camera cull's time against the L0 baseline |

### Order

Object LOD first. It uses the Lighting path DCLF already has, and builds most of the shared machinery:
- the LOD roots as categories;
- block levels;
- segments;
- the handover to full models.

Terrain LOD sits under the same root, with one more technique. Tree LOD is the largest draw count (about 200 a frame
against about 74 for objects). But it needs a new program family and an instance pipeline, and it rests on L1's
membership work anyway. Doing trees right after L1 would bring the biggest draw reduction earliest, at the cost of
building the new program before the shared LOD machinery has been proven on the simpler class.

## Risks

- **The handover to full models.** It is where the visible artifacts will come from (gaps, double draws, popping),
  because three engine state machines meet there: the large reference's load, the tree's fade, and the loaded grid.
  L1's parity has to cover it before anything draws.
- **Load-order variance.** LOD content varies with the LOD generator (xLODGen, DynDOLOD):
  - 3D tree LOD is object LOD with a tree atlas, not `BSDistantTreeShader`;
  - grass LOD is alpha-tested object LOD;
  - terrain LOD may or may not use the noise technique.

  Classification must key on the shader, technique and flags, as now, not on names.
- **Feature interactions.** CS features that change LOD shading: TruePBR (LOD land), Terrain Blending (`LOD_LAND_BLEND`
  on loaded land, already native), Skylighting (tree LOD in its occlusion), Unified Water (LOD water, out of scope).
  Each one's constants and textures must reach DCLF's draws, under capture parity.
