# Drawcall Limit Fix

> **The historical record.** This document is the log of how DCLF was built, phase by phase, with the measurements
> behind each decision. Parts of it describe code, switches and paths that have since been removed. The current design
> is [dclf-architecture.md](./dclf-architecture.md); what DCLF covers is [dclf-status.md](./dclf-status.md).

Drawcall Limit Fix (DCLF) replaces the game's per-object opaque draw loop with GPU-driven indirect draws
that the render graph executes on DXVK's Vulkan device (see [OpenRenderGraph on DXVK](./render-graph.md)).
It is built in phases:

1.  **Scene capture** (done): CPU tables describing every object DCLF will draw.
2.  **Indirect pipeline** (current): Vulkan device-generated commands, shaders recompiled for the render graph,
    tables uploaded to the GPU.
3.  **Integration and parity**: the native loop skips what DCLF draws; the images must match.
4.  **GPU culling**: two-pass object occlusion culling at the Z-prepass.
5.  **Cutting back the native loop** and its CPU occlusion culling.

What the game binary does, as far as DCLF depends on it, is recorded in
[Skyrim engine notes](./skyrim-engine-notes.md).

## Phase 1: scene capture

Source: `src/Features/DrawcallLimitFix.{h,cpp}` and `src/Features/DrawcallLimitFix/`.

### Which objects

Static rigid geometry:
-   a `BSTriShape` (not dynamic, multi-index or LOD) with no skin;
-   a `BSLightingShaderProperty`;
-   a technique DCLF supports: None, Envmap, Glowmap or Parallax, including TruePBR materials;
-   no alpha blending, no decal flags, no LOD flags, no projected UV and no refraction;
-   not under a `BSOrderedNode`; under an `NiSwitchNode` only while the switch selects it, and part of an actor only with `CS_DCLF_ACTORS` (see "Trees and actors").

Per frame it must also:
-   be visible, meaning not app-culled or hidden up to its tracked root;
-   be fully faded in.

Objects whose LOD fade inputs are undefined in the engine (see `LightingDescriptors.cpp`) stay native. So do
the few objects whose drawn technique cannot be known when the tables are built (`alpha-test-state`, see
the engine notes' batch renderer open question). Anything else stays on the native loop, which draws it as
before.

The reasons are counted per frame (`CS_DCLF_STATS=1`, or the feature's settings page).

### Tracking

`SceneTracker` detours the `NiNode` child functions (`AttachChild`, `DetachChild*`, `SetAt*`). From
whatever thread attaches or detaches, it pushes events onto a lock-free stack. Detach events carry the
geometry leaves collected on that thread while the subtree is intact.

`SceneStore` belongs to the render thread. Every Present (`Feature::Reset`, so also in menus) it:

1.  Refreshes the set of tracked roots. New roots are scanned and removed roots dropped. The roots are:
    -   the Static, Dynamic and MultiBound category nodes of each attached cell (the interior cell, or
        the loaded grid);
    -   the `BSMultiBoundNode` children of `TES::objRoot`, where the engine moves references into
        multibounds (exteriors) and rooms (interiors);
    -   the interior portal graph's rooms and shared node.
2.  Applies the queued events in order.
3.  Re-checks a slice of tracked geometry against its root, as a safety net for missed detaches.

### Tables

At the start of the main pass (`Feature::Prepass`, when `Deferred::StartDeferred` runs), `BuildFrame`
first walks the main-camera accumulator's batches. That yields, for every geometry, the pass the main pass
will draw this frame and the technique it is registered under. It then rebuilds the tables from the
tracked geometry the accumulator holds; the rest of the tracked set was culled by the engine and cannot be
drawn this frame. Phase 5 takes objects out of the accumulator, so it will need the whole set again.

| Table | Content |
| --- | --- |
| `objects` (`ObjectRecord`, 128 bytes, GPU layout) | world and previous world 3×4, world bounding sphere, geometry / material / pipeline indices, flags (alpha test, two-sided, alpha threshold, external emittance suppression) |
| `geometries` (`GeometryRecord`) | the game's vertex and index buffer, vertex description and stride, index count |
| `pipelines` (`PipelineKey`) | final vertex and pixel shader descriptors (as `State::ModifyShaderLookup` produces them for the deferred pass), two-sided, and the raw pass descriptor |
| `geometryConstants` (per pipeline) | the per-frame `PerGeometry` constants for that pass descriptor |
| `techniqueConstants` (per pipeline) | the `PerTechnique` constants (fog, colour output clamp, `VPOSOffset`), sampler filter modes, and the shadow mask binding |
| `permutations` (per pipeline) | Community Shaders' permutation buffer (b4) for the draw |
| `materials` (`MaterialRecord`) | the `PerMaterial` constants, textures, address and filter modes for each (material, pass descriptor) |
| `shading` (`ObjectShading`, 32 bytes) | the per-object `PerGeometry` pixel constants (LOD fades, alpha, emissive colour, SSR specular) |
| `lights` (`ObjectLights`) | Light Limit Fix's per-object `StrictLightData` (b3): room index and shadow mask channels |
| `draws` (`DrawSequence`, 48 bytes) | one indirect draw per object in the device-generated-commands token order, addresses filled in Phase 2 |

The pass descriptor is the accumulated technique: it is what `SetupTechnique` receives. Deriving it from
the property flags is not reliable while the native loop runs, for three reasons:
-   Some bits come from per-frame engine state: the light and shadow assignment, and DoAlphaTest, which
    depends on an early-Z global.
-   `GetRenderPasses` rebuilds a pass only when the light state changes, so the specular and envmap fade
    decisions a pass carries can be older than the current fade metric.
-   The fade values the draw reads are the property fields that same call stored.

`LightingDescriptors.cpp` still derives the descriptor from the flags, following `GetRenderPasses` and
`SetupTechnique` with the LOD fades and TruePBR's changes, because Phase 5 will need it. `CS_DCLF_STATS`
reports how often the derivation disagrees with the drawn technique outside the per-frame bits: in the
coverage runs, at most one object per frame, a specular fade crossing.

Material and geometry constants are not ported. `ConstantEvaluator` runs the engine's own
`BSLightingShader::SetupMaterial` and `SetupGeometry` outside the render loop, against stand-in shader
objects (engine notes: "Evaluating the Setup functions outside the render loop"). The results therefore
include every Community Shaders hook on those functions.
-   **Evaluation frequency:** materials once per (material, pass descriptor) per frame, geometry
    constants once per pipeline per frame.
-   **Per-object values:** World, PreviousWorld, the LOD fades, alpha, emissive colour and SSR specular,
    stored in `objects` and `shading`.
-   **The one port:** `SetupTechnique` binds real shaders, so it is ported (`EvaluateTechnique`) from the
    decompile.

Constant components the engine does not write keep a sentinel (`kUnwrittenBits`). Phase 2 uploads zero for
them. The engine leaves them undefined too; they belong to variables the permutation does not read.

What Community Shaders binds on top of the engine was checked draw by draw (see below):
-   **Per object:** the permutation buffer (b4), Light Limit Fix's `StrictLightData` (b3), and the
    alpha-test reference (b11).
-   **Per pass:** everything else Community Shaders binds for these permutations (b5, b6, b12, and every
    feature texture outside t0–t15). DCLF can bind it once per pass.
-   **Never read by these permutations:** the bone palettes (b9, b10) and Skin's buffers and textures
    (b7, t71, t74, t75).

### Checking it: capture parity

`CS_DCLF_CAPTURE_PARITY=1` compares every native main-pass lighting draw of an object in the tables with
the tables, and logs every 300 frames:

-   `location`: the player's cell.
-   `capture parity OK|MISMATCH` covers (bit for bit, except `EmitColor`, compared within 0.1% because
    time-of-day emittance moves between the table build and the draw):
    -   that the drawn pass is the one the accumulator walk found;
    -   the full pass descriptor, and the final shader descriptors;
    -   the world transform;
    -   `PerMaterial` constants, textures and address modes;
    -   `PerGeometry` constants (with the per-object values applied);
    -   `PerTechnique` constants (re-read at the draw because of the late `VPOSOffset` write), filter
        modes and the shadow mask binding;
    -   tracking misses: eligible geometry under a tracked root that is not tracked.

    Native constants are captured from the Map/Unmap detours, and only components the engine writes are
    compared.
-   `draw parity`: the `DrawIndexed` arguments and the bound index and vertex buffers.
-   `light data parity`: Light Limit Fix's `StrictLightData` (no strict lights in the main pass, room
    index, shadow mask channels).
-   `permutation parity`: the permutation buffer (b4), limited to the extra bits the Lighting shader reads.
    `IsSun` and `GrassSphereNormal` stay set from other shaders' draws.
-   `feature binding parity`: within a frame, every constant buffer b3+ and shader resource outside t0–t15
    must stay the same, and must not be rewritten, across the eligible draws. The exceptions are the
    per-draw buffers listed above.
-   `render state … ->`: the cull, depth, stencil, blend and alpha-test state the native draw used, per
    property-derived key. A key marked `VARIES` means the state is not a function of the key.
-   Diagnostics:
    -   statically eligible geometry drawn outside the tracked roots, with the parent chains of a few;
    -   variables the native shaders have that DCLF leaves unwritten.

Results with `CS_DCLF_TEST_COMMANDS=600:coc WhiterunDragonsreach;1800:coc BleakFallsBarrow01;3000:coc Riverwood`:

| Location | Tracked geometry | Objects in the tables per frame | Checked draws per frame | Table CPU per frame | Mismatches |
| --- | --- | --- | --- | --- | --- |
| Whiterun (exterior) | 9,256 | 915 | about 910 | 1.05 ms | 0 |
| Dragonsreach (interior, rooms and portals) | 2,690 | 921 | about 920 | 0.69 ms | 0 |
| Bleak Falls Temple (interior) | 6,442 | about 300 | about 300 | 0.30 ms | 0 |
| Riverwood (exterior, after the cell transition) | 10,078 | 200 to 1,200 (camera moving) | 540 to 1,200 | 0.63 to 1.23 ms | 0 |

Every other check (draw, light data, permutation, feature bindings) is OK in all four.

Community Shaders fixes that came out of it:
-   TruePBR wrote partly initialized constant arrays (`TruePBR.cpp`, now value-initialized).
-   Light Limit Fix's room index and shadow mask derivation moved into `LightLimitFix::GetRoomIndex` /
    `GetShadowBitMask`, shared with DCLF.

## Phase 2: indirect pipeline (done)

DCLF's objects are drawn every frame by the render graph with one indirect command stream per pass, into
off-screen copies of the main pass's targets; the frame itself is unchanged. `CS_DCLF_DEBUG_VIEW=1` copies
those targets over the native ones before the deferred composite, so the frame shows only what DCLF drew.

### Game resources

-   **Buffers and images.** The DXVK fork exports `dxvkGetInteropResourceInfo` (ordinal 135). Given a D3D11
    buffer, texture or shader resource view, it returns the Vulkan handle, offset, size and device address
    (images: create info, view, swizzle and the layout DXVK keeps them in) and marks the resource stable,
    the same lock DXVK's NVX interop uses. Buffers the game can map are rejected. `GpuResources` resolves
    each geometry's vertex and index buffers once and holds a reference while they are cached (Whiterun:
    806 buffers, 0 rejected).
-   **Textures** (`GpuTextures`). DXVK keeps every image it hands out in `VK_IMAGE_LAYOUT_GENERAL` and uses
    it between the graph's commands, which is D3D12's simultaneous-access contract. Each shader resource
    view is imported into BasicRHI without ownership, with simultaneous access, and gets a view in ORG's
    shader-visible heap (a slot from ORG's descriptor service, retired against ORG's fences on eviction).
    A null view (the engine binds none) reads zero through a null descriptor, as D3D11's null SRV does.
    An import is not made on the render thread (since 2026-09-29). Marking the image stable is a synchronous
    chunk on DXVK's CS thread, and the view creation blocks in the driver, for milliseconds each while the game
    streams textures (up to 20 ms inside the Z-prepass inputs). `GpuTextures::RequestBinding` queues an unknown
    view for a DCLF import thread and returns it pending. The material stays unresolved, and its draws are
    deferred, which leaves them the engine's (a deferred draw is never marked drawn, so nothing is claimed).
    A later refresh takes the settled import. The same applies to projected textures, the shadow mask and the
    shadow casters' diffuse. Only the frame's textures (t16 and up) still import inline
    (`ResolveBinding`): their draws cannot be deferred, and they are a few stable render targets. A settled
    import nobody asks for again within 30 s is released.
-   **Samplers** are copies of the renderer's own D3D11 sampler states, the table the engine selects from by
    the shadow state's address and filter modes (engine notes: samplers).
-   **Per-frame constant buffers and dynamic structured buffers** are never locked: `ConstantMirror` keeps
    a CPU copy of each one DCLF watches, fed by the immediate context's `Map`/`Unmap` and
    `UpdateSubresource`, and re-reads mapped buffers from their last mapping (the engine writes some
    constants after `Unmap`). CPU-written structured buffers the pass reads (t98) are copied into graph
    buffers each epoch. Light Limit Fix's lights, light list and light grid (t35-t37) are its graph buffers.

### Pipelines and bindings

Every draw pushes one address, of its binding record (`DrawBindings`, 800 bytes: vertex and pixel constant
buffer addresses for b0-b13, resource heap indices for t0-t127, sampler heap indices for s0-s15). The
pipeline layout maps the shaders' registers onto that record with `VK_EXT_descriptor_heap`'s indirect
mappings; vertices come through the `VertexBuffer` argument. Lighting.hlsl needs no binding changes.

-   **SPIR-V.** `ShaderPrograms` requests each pipeline key's Lighting VS and PS through
    ORGModuleServices, with the defines of the D3D11 build (`ShaderCache::GetCompileDefines`), HLSL 2018,
    and register shifts per class (`ShaderPrograms.h`). DXC scopes cbuffers declared inside namespaces
    differently from FXC; those members are declared through `Common/NamespacedCBuffer.hlsli` (FXC output
    byte-identical). CS's initial compilation workers also request ORG Lighting stages (including the
    depth-only PS) and Utility stages, including native disk-cache hits. Requests made before ORG setup
    are retained until `DrawcallLimitFix::SetupResources`; the same bounded CS pool processes them and
    the initial compilation gate includes that pool's outstanding work. Stage futures are shared by
    descriptor, stage and source family, so pipeline pairs sharing a VS do not repeatedly scan shader
    dependencies. Runtime requests remain a fallback for unseen stages; no gameplay work is frame-deferred.
    `CS_DCLF_PRECOMPILE=0` disables only this startup path for profiling/control runs. A cold ORG cache
    can significantly lengthen initial compilation, especially with shader debug information enabled.
-   **Pipelines** (`DrawPipelines`) are built asynchronously through ORGModuleServices' `PipelineService`,
    two per key: the main pass's (color, depth test EQUAL, no depth writes) and DCLF's own Z-prepass (depth
    only, LESS, writes). Vertex input is the engine's input layout for (VS input mask & geometry
    `VertexDesc`), ported from its builder (`VertexInput.cpp`); the pipeline key includes the geometry's
    vertex layout. A pipeline whose shaders read a register the layout cannot map, or a vertex input the
    geometry lacks, fails and its objects stay native. Both variants of a key get the same index in their
    indirect pipeline sets.
-   **Constants** are packed with the native shaders' constant tables (`LightingConstants`, shared with the
    capture parity check): PerTechnique per pipeline, PerMaterial per (material, pipeline), PerGeometry per
    object, Light Limit Fix's StrictLightData per (room, shadow mask), the permutation per (pipeline, object
    flags), the alpha-test reference per threshold, Linear Lighting's `LLPerGeometry` (PS b8, per object;
    Phase 1 missed that it is per draw) per multiplier, and every other bound buffer from its mirror.

### An epoch

Where the main pass's opaque batches start, DCLF records what the pass binds; just before the deferred
composite (`Deferred::EndDeferred`) it runs the graph's `MainOpaque` segment. Its callback assembles and
uploads the constants, records, draw inputs and geometry table (about 2.5 ms of render-thread CPU and
1.6 MB for 915 objects in Whiterun). Then:

1.  `BuildDrawsCS` (compute) writes one `DrawSequence` per draw from the inputs and appends it with an
    atomic count, the shape Phase 4's culling will extend.
2.  The main-opaque pass executes the sequences twice with the GPU count: the depth variant into DCLF's own
    D24S8 depth, then the color variant (EQUAL) into eight graph-owned targets with the main pass's formats.
    The render area, viewport and depth range are the main pass's (the engine draws with depth range
    [0, 0.999998]; BasicRHI's `PassBeginInfo` gained the range).

The atomic append leaves the sequences in whatever order BuildDraws' threads finish, so the indirect draw switched
pipeline on nearly every sequence. `CS_DCLF_SORT_DRAWS` (on by default) groups them by pipeline, as a counting sort:

1.  BuildDraws (phase 1 and the colour segment; phase 2's few rescues stay unsorted in their own range) writes each
    sequence to its append slot of `cs.dclf.sort-staging`, counts it into its pipeline's word of `cs.dclf.sort-counts`,
    and keeps the atomic's return as its rank among that pipeline's sequences (`cs.dclf.sort-ranks`).
2.  `PrefixSum` (`src/RenderGraph/PrefixSum.cpp`, `package/Shaders/RenderGraph/PrefixSumCS.hlsl`) turns the 4096
    counts into each pipeline's first slot. It is BasicRenderer's two-level block scan (`materialPrefixSum.hlsl`) as a
    reusable pass, and it clears the counts as it reads them, so after the first commit's zero upload nothing clears
    them again.
3.  `SortSequencesCS` writes sequence i to its pipeline's first slot plus its rank. Every slot of `[0, count)` is
    written once, so the draw's count and range are unchanged. Order within a pipeline is still the append order.

Build parity compares the sequences as sets, so it holds with the sort on.

The capture (`DrawcallLimitFix::BeforeOpaquePass`) is taken after `StartDeferred`, with the engine's pending state
applied by `SetDirtyStates` as the first draw would apply it (that is when the engine binds the G-buffer and clears
it). It used to be taken at the first lighting draw the engine made, which a frame whose every lighting draw in view
is DCLF's does not have: in an interior under `DCLF_BINDLESS`, facing away from anything left native, the colour
epoch was dropped (292 of 300 frames) and the objects it withheld went undrawn, leaving only the depth under fog.
The end of the opaque pass is no substitute: the sky's clouds have rebound Cloud Shadows' t26 by then.
`CS_DCLF_CAPTURE_POINT_PARITY=1` compares the capture with what is bound at the first lighting draw the engine
makes: identical at Riverwood and Whiterun, and in the interior once PS b7 (Advanced Skin's `SkinPerGeometry`, bound
per draw) was classed as per draw.

The graph holds every feature's passes and each epoch executes all of them; passes record only in their own
segment (`RenderGraphRuntime::Segment`: LightCulling, MainOpaque, DebugView) and include it in their
invocation revision. An empty pass is effectively free.

### Gate

-   `CS_DCLF_BUILD_PARITY=1`: BuildDraws' output (read back through D3D11 wrappers of the graph buffers)
    equals the CPU templates. OK on every check in Whiterun (915 draws), Dragonsreach (921) and Bleak Falls
    Barrow (about 300).
-   The debug view shows the drawn objects textured, lit and depth-tested; nothing is skipped.
-   No validation errors besides the upstream `vkQueueSubmit2-semaphore-03868`.
-   One ExecuteIndirect per pass (two passes: depth, color) by construction; not yet confirmed in a capture.

### Fixed in shared components on the way

-   **ORG** `PersistentGraphHost` never released upload pages or retired descriptors: every page leaked
    (about 9 GB after a minute of DCLF uploads). It now keeps a per-slot frame timeline, waits for a slot's
    previous frame before reusing it, and calls the upload and descriptor deferred releases.
-   **ORG** persistent execution ignored `commonLayoutOnly` on external textures and transitioned DXVK's
    depth buffer out of GENERAL; `CompileResourceShape` now carries it and compiled states keep layout
    Common. `ExternalTextureResource` also reports the view flags its description implies (persistent graphs
    rejected its depth-stencil views).
-   **BasicRHI**: per-stage descriptor-heap mappings; depth-stencil and packed 10/11-bit formats; images
    imported with simultaneous access (views, attachments and transfers in GENERAL); SRV component mappings
    on Vulkan; null SRVs on both backends (Vulkan: `robustness2` `nullDescriptor`, which the DXVK fork now
    reports); the viewport depth range in `PassBeginInfo`.

### The hybrid path (Phase 3, in progress)

`CS_DCLF_HYBRID=1` makes DCLF draw into the main pass's own targets and depth, and the native loop leave
those objects to it (`DrawcallLimitFix::SkipNativePass`, on the `RenderPassImmediately` call sites Light
Limit Fix hooks, inside the main camera's depth and opaque ranges only). The decision is one frame old,
because the epoch runs after the native passes.

Depth is written in its own segment (`RenderGraphRuntime::Segment::ZPrepass`), at the **end of the native
depth pass**: everything the rest of the frame derives from the depth buffer - the native draws' own EQUAL
test, the sky, Terrain Blending's blended depth and every effect that reads it - has to see DCLF's objects.
The colour pass then runs before the composite and tests EQUAL against it, as the native main pass does.

The two epochs assemble separately, from their own captures, because the depth pass has not bound the main
pass's pixel-stage state yet. They draw the same tables with the same camera, so their depths agree. Only
the colour epoch records what it drew (`drawnFrame`), so the native loop can never skip an object that the
Z-prepass drew but the colour pass then left out - that would leave a hole that writes depth and shows the
background.

Whiterun exterior: 942 objects drawn, 1884 native passes skipped, 2.7 ms of render-thread CPU.

#### The depth-only pipeline variant

Running the Z-prepass inside the native depth pass is only possible because its pixel stage reads nothing
that is not bound yet. Both pipeline variants used to share the full Lighting pixel shader, which reads the
main pass's per-frame constants and textures. The depth variant is now built from its own SPIR-V of
`Lighting.hlsl`, compiled with `DCLF_DEPTH_ONLY`, which returns immediately after the alpha test:

-   alpha testing is a compile-time permutation (`DO_ALPHA_TEST`), so the cut is exact - the alpha-tested
    permutations keep their discard, the rest compile down to nothing;
-   everything past it is dead code, so the compiler drops the per-frame pixel bindings with it;
-   `RegisterUsage` is now kept per variant, so a draw only has to supply what its own variant declares.

The assembly for that epoch therefore skips the pixel-stage per-frame constants, the frame textures at t16
and up, and the structured-buffer copies - all of which come from the main pass's capture - and keeps the
material textures, samplers and vertex-stage constants, which come from the tables.

#### The ghost background, and what depth the rest of the frame reads

Writing the depth at the first draw of the main pass fixed the native draws, the sky and the composite, but
the frame still showed terrain and LOD washed over DCLF's objects. The cause is that the two depth
resources the rest of the frame actually reads are built at the **end of the native depth pass**, before
DCLF has drawn anything:

-   the engine's prepass depth copy (`kPOST_ZPREPASS_COPY`), and
-   with Terrain Blending on, its blended depth - which it also points `kMAIN` and `kPOST_ZPREPASS_COPY`'s
    depth SRVs at for the whole rest of the frame (`TerrainBlending::Hooks::Main_RenderDepth`), so every
    effect reached through `Util::GetCurrentSceneDepthSRV` reads it.

Both therefore held a scene without DCLF's objects, and each depth-reading effect painted the background
over them.

`DrawcallLimitFix::RefreshDepthConsumers`, called straight after the Z-prepass epoch, rebuilds both from
the depth buffer as it now stands: it re-runs `TerrainBlending::BlendPrepassDepths` and re-copies `kMAIN`
into the prepass copy, saving and restoring the render targets around it because the main pass is mid-draw.
That clears the wash completely in Whiterun exterior.

With the Z-prepass now running inside the native depth pass, almost none of that rebuilding is needed:
Terrain Blending builds its blended depth further up the same call site, after DCLF's thunk returns, so it
already sees DCLF's objects. DCLF installs its `Main_RenderDepth` hook before Terrain Blending does, which
is what makes its thunk the inner one of the chain and puts the prepass ahead of the blend.
`RefreshDepthConsumers` is now only the engine's prepass depth copy, and only when Terrain Blending is off
to redirect it - that copy is taken inside the depth pass, before the prepass runs.

#### The Z-prepass must draw exactly what the colour pass draws

Splitting the depth and the colour into two epochs introduced a trap that took a long time to find,
because every symptom pointed somewhere else: DCLF's objects rendered flat and grey, as though unlit.

The cause is an invariant, not a bug in any one pass. The Z-prepass was assembled without the main pass's
pixel-stage bindings, so it could draw objects the colour epoch would later drop for a missing constant.
Such an object ends up with depth in the buffer and nothing to shade it:

-   DCLF's own colour pass skipped it, so it writes no G-buffer;
-   the native loop would have drawn it - it is not in `drawnFrame`, so it is not skipped - but the native
    main pass tests depth EQUAL, and the depth it now finds is the one DCLF computed rather than the one
    the native prepass wrote, so every one of its fragments is rejected too.

Two changes keep the two sets equal:

-   The Z-prepass only draws objects the native loop is actually leaving to DCLF, which is the set the
    colour epoch drew in the frame before - the same rule `SkipNativePass` uses. Anything else keeps its
    native depth, and the native draw that owns it still works.
-   The three per-frame pixel buffers the depth-only shader reads before its alpha test - b5 SharedData,
    b6 FeatureData and b12 the game's PerFrame - are supplied from Community Shaders and from the game's
    own cache rather than from the main pass's bindings or the constant mirror, so the prepass can satisfy
    them during `RenderDepth`. With those in place both epochs assemble the same objects: Whiterun
    exterior and Dragonsreach both report **915 and 921 drawn with nothing skipped**.

Both epochs also rasterise with the main pass's viewport depth range. The game's depth pass and its main
pass do not use the same one - `[0, 0.999968]` against `[0, 0.999998]` - and the range scales the value
written to the buffer, so the same vertex would land about 500 D24 units apart in the two passes.

**How it was found.** By readback, after a long detour of guessing. The instrument that worked copies one
texel of every target, and of the depth, into a buffer from *inside* the epoch, as graph passes ordered
against the draws, and reads that buffer back a few frames later (`CS_DCLF_GBUFFER_PROBE=<x>x<y>`). An
equivalent D3D11 readback issued around the epoch is **not** ordered against ORG's submissions and
silently reports that nothing ever changes; it produced several confident and wrong conclusions before a
control - sampling a pixel only DCLF draws, and seeing no change - exposed it.

### Open
-   The sampler table is read at its AE 1.6.1170 address; other runtimes need its Address Library ID.
-   Material textures are imported per shader resource view; views of one image share nothing yet.
-   Records and constants are rebuilt and uploaded every frame; Phase 4 moves to persistent tables with
    dirty-range uploads.

### Why the indirect draws did not match the native ones

Phase 2 left an open question: testing depth EQUAL against the native Z-prepass rejected nearly every
fragment, and the error grew as objects came nearer. The answer turned out to be neither the arithmetic of
the vertex shaders nor the camera: **the pipelines drew the wrong side of every triangle.**

BasicRHI renders with a y-flipped viewport (it negates the viewport height, as DXVK does for D3D11), which
mirrors the winding of a triangle in framebuffer space. Its `RasterState::frontCCW` was passed straight
through to `VK_FRONT_FACE_*`, so leaving it at its default made the clockwise faces front-facing in flipped
space - the opposite of what the engine's meshes, wound for D3D's "clockwise is front", require. Every
pipeline therefore culled the faces it should have drawn and drew the ones it should have culled.

That single mistake produced all of the symptoms:

-   A flat surface (a floor tile, a wall panel) has no second side, so it disappeared entirely.
-   A closed object still rendered, from its far side, so it looked plausible but sat one object-thickness
    too deep. That is what made the depth comparison look like a systematic scale: along a floor, DCLF's
    view depth was a constant 1.0141 times the native one.
-   Depth EQUAL against the native prepass therefore failed almost everywhere, which is why Phase 2 needed
    a Z-prepass of its own.

The gap was closed in BasicRHI rather than worked around here. `RasterState::frontCCW` is now defined in
clip space - false, the default, is D3D's "clockwise is front"; true is glTF's - and the Vulkan backend
inverts it when it fills `VkPipelineRasterizationStateCreateInfo`, so one pipeline description means the
same thing on both backends and no caller branches on the API. DCLF therefore leaves `frontCCW` at its
default, which is already the convention the engine's meshes are wound for.

**How it was measured.** A throwaway depth probe allocated DCLF's depth as a readable texture and compared
it with the native depth on the CPU, with `CS_DCLF_ONLY_ELIGIBLE=1` restricting the native frame to the
objects DCLF also draws, so the two held the same scene and could be compared pixel for pixel. In
Dragonsreach:

| | before | after |
| --- | --- | --- |
| pixels DCLF covers | 2 508 133 | 3 667 129 |
| pixels only the native frame covers | 1 156 228 | 0 |
| pixels equal to the bit | 517 (0.02%) | 1 284 595 (35%) |
| difference, 5th to 95th percentile | +34 to +194 470 D24 units | -3 to +3 D24 units |
| view depth ratio | 1.0073 to 1.0202 | 1.00000 |

The remaining few D24 units are the FXC/DXC difference described below, which is two orders of magnitude
smaller than the bug it was hiding behind.

### The residual: FXC against DXC

The native shaders are compiled by FXC and reach the GPU through DXVK's DXBC path, which marks positions
invariant and keeps `precise` multiply-adds fused; DCLF's are compiled by DXC, which emits NoContraction.
Lighting.hlsl's vertex shader composes `mul(ViewProj, world4x4)` per vertex, whose intermediate terms carry
the object's world position, so the orders differ in the last bits and the perspective divide amplifies the
difference as objects come nearer. It is worth a few D24 units, and no correctness gate should depend on
the two compilers agreeing exactly: DCLF owns the depth of the objects it draws, and the native loop skips
them.

### A gap this left in the checks

Every parity check of Phases 1 and 2 passed while the frame was visibly wrong, because each of them
compares what DCLF *would* submit with what the engine submits: the tables, the constants (276 300 draws,
every block compared), the draw arguments and bound buffers, and the generated sequences. None of them
looked at what the indirect draws actually produced. The depth comparison above is that missing check, and
it is the one that found this. It was a throwaway instrument and has been removed again; Phase 3's image
parity (`CS_DCLF_PARITY`) is its durable form, and `CS_DCLF_ONLY_ELIGIBLE` is what gives it a native frame
holding the same objects.

## Phase 4: what the culling is given to cull

The tables used to be built from Skyrim's main-camera accumulator, which is the engine's output *after* it
has run its own frustum, occlusion-plane and room/portal culling. That made GPU culling impossible to
evaluate: in Whiterun the accumulator held 915 objects out of 9256 tracked, and frustum culling rejected
0 of them. A rejection count of zero reads like a clean pass, but it only meant the engine had already
removed everything the test could have caught.

`SceneStore::BuildFrame` now classifies the whole tracked set, and the engine's decision is kept per object
as `kObjectNativeVisible` rather than thrown away. Two things follow.

**The culling has real work.** The same Whiterun frame offers 3057 candidates, of which frustum culling
rejects 932.

**The engine is the reference oracle.** Because every candidate carries what the engine decided,
BuildDrawsCS cross-tabulates the two for free:

| | GPU keeps | GPU rejects |
| --- | --- | --- |
| **engine kept** | drawn | **false negative — a defect** |
| **engine culled** | rescued (expected: the GPU culling is the more conservative of the two until the HZB lands) | agreement |

The false-negative count is the gate. It is reported every 300 frames whether or not `CS_DCLF_STATS` is on,
because an object the engine kept and the culling here dropped is missing from the frame.

### The gate: which candidates may be drawn

`CS_DCLF_CULL_INPUT` decides whether a candidate the engine culled may actually be drawn. It defaults to
`native`, which draws only what the engine kept, so the frame is exactly what it was before while the
counters still measure the whole set. Whiterun, `native`, with frustum culling on:

    culling: 915 draws written, 932 of 3057 tested were rejected (30.5%);
    against the engine: 1210 it culled were gated out, 1210 it culled were kept, 0 it kept were rejected

915 + 932 + 1210 = 3057, and no false negatives. The frame is unchanged and there are no validation errors.

Two hazards came with widening the input, both worth remembering:

-   **Anything that marks an object "drawn" must be gated by what will actually be drawn.** The hybrid skip
    and the Z-prepass both key off `drawnFrame`. Marking a candidate the gate drops would make the native
    loop skip a pass DCLF never drew, and — because the Z-prepass draws exactly what the colour epoch drew
    last frame — would write depth for an object nothing then shades. That is the grey-object failure of
    Phase 3 all over again. The gate is a per-object flag test and so is predictable on the CPU; frustum
    rejection is not, which is precisely what the false-negative counter is for.
-   **Widen the readback with the counters.** The draw-count buffer grew to eight words but its D3D11 view
    was still four, so the two new counters read past its end and came back zero — indistinguishable from a
    clean result. The first run duly reported zero false negatives and zero rescued, and only the arithmetic
    (rescued had to equal the 1210 gated out) gave it away.

### `CS_DCLF_CULL_INPUT=tracked` is not yet correct

Handing the decision to the GPU culling alone works mechanically: 2125 objects draw, nothing is skipped for
missing resources, no buffer overruns, no validation errors. The shading is wrong, in a way that saturates
the frame red — so this mode is a Phase 4/5 target, not a usable setting.

Two known reasons, both structural rather than incidental:

-   Objects outside the accumulator never went through the engine's per-frame light and shadow assignment,
    so their pass descriptors come from the property derivation alone, with its guesses for
    `kRuntimePassBits`, and their `shadowBitMask` is zero. Making that derivation authoritative is Phase 5's
    stated prerequisite.
-   The colour pass tests depth `EQUAL`, and the Z-prepass draws only what the colour epoch drew last frame.
    An object the engine culled has depth from neither pass, so DCLF does not own its depth in the way the
    main pass requires.

The CPU cost of the wider input is also real and unaddressed: building bindings for 3057 candidates instead
of 915 takes 8.4 ms per frame against 3.2 ms. The culling has to move ahead of the per-object binding build
before this set size is affordable.

### The HZB and occlusion culling

`CS_DCLF_CULL=occlusion` adds a hierarchical-depth test after the frustum test. The pyramid is built by
`HzbCS.hlsl` in a pass at the end of the ZPrepass segment, which is the end of the native depth pass, so it
describes the depth the frame actually has: the native occluders the engine drew, plus DCLF's own depth
draws.

Decisions worth keeping:

-   **It stores raw NDC depth, not linear view depth.** Skyrim's projection is monotonic in distance and
    the test only ever compares two depths for order, so linearising would add a step that can be got wrong
    without making any comparison more correct.
-   **Every level is the maximum of the level below**, so a texel holds the farthest surface under it, and
    a footprint that is partly empty sky keeps a large value and nothing under it is culled. Texels outside
    the rendered area read the far plane for the same reason.
-   **Mip 0 is half the next power of two** of the depth, so the chain is a clean sequence of halvings and a
    level can be chosen from a screen extent by `log2` alone.
-   **The whole chain is one pass** with a full memory barrier between dispatches. Levels of one texture are
    not separate resources to the graph, so a pass per level would declare the same resource as its own
    input and output.
-   **A margin on the comparison.** The object's nearest corner is in raw clip space while the HZB holds
    what was stored, which the viewport depth range scaled — and the native depth pass and DCLF do not use
    the same range (`[0, 0.999968]` against `[0, 0.999998]`). The gap is small but systematically in the
    direction that culls, and it bites hardest on flat objects lying against the surface behind them.

Dragonsreach, with the whole tracked set as input:

    culling: 825 draws written, 865 of 1962 tested were rejected (44.1%: 181 outside the frustum,
    684 occluded, 96 of those the engine had kept); against the engine: 0 it kept the frustum test rejected

One validation VUID (the upstream semaphore one), and the frame is pixel-identical to a frustum-only
control run of the same scene.

#### Objects behind the camera were never frustum-culled

`Culled` projected the box's corners and kept the object as soon as one corner had `w <= 0`, "crossing the near plane".
An object wholly behind the eye has all eight there, so the main camera, whose culling has no world-space planes
(`cullPlaneMask` 0), kept everything behind it, and `ScreenExtent` could not give the HZB a footprint for it either.
The plane tests are on the homogeneous coordinates (`-w <= x <= w`, `-w <= y <= w`, `0 <= z <= w`), each linear in the
point, so they hold behind the eye as in front and need no early exit: an object behind the camera is outside the near
plane.

Riverwood, per frame (full featureset, frame push on):

| | frustum rejects | engine culled, DCLF kept | draws written | colour / Z-prepass draws |
| --- | --- | --- | --- | --- |
| before | 772 | 1509 | about 2960 | 3174 / 2960 |
| after | 3342 | 108 | 1524 | 1752 / 1546 |

With residency off, every object carries the engine's own verdict: the engine culled and DCLF kept went from 2789 to
219, with 0 frustum rejections of anything the engine kept. The GPU time hardly moved (colour 2.11 to 2.07 ms, the same
pixel shader warps): the draws behind the camera never rasterized. The report's "false negatives" count residents,
whose `kObjectNativeVisible` is not an engine verdict: every resident behind the camera is one.

The census that found it (`CS_DCLF_TEST_CULL_CLASSES=1`, since removed) classified, every 300 frames, each in-frustum
object the engine culled and the GPU kept, by the engine's reason (app culling, the fade root, its fade distance) and by
what the HZB saw (near-plane crossing, a footprint of far plane only, depth nearer than the object), from bits 28-30 of
the frustum words the visibility feedback reads back.

#### The engine is not an oracle for occlusion

The cross-tabulation against the engine is a real instrument for the **frustum** test, where the engine is
exact and the two must agree — it holds at zero disagreement in both Whiterun and Dragonsreach. It is
**not** one for occlusion, and reading it that way cost a detour here.

The first occlusion runs reported dozens of "false negatives": objects the engine kept that the HZB
rejected. Recording one rejection in full rather than guessing at the count settled it in a single run:

    HZB rejection sample: farthest 0.997283 against nearest 0.998389, uv (0.4537 0.1599)-(0.4573 0.1624),
    mip 3, engine kept it

Those depths are w ≈ 5434 and w ≈ 9066, so the object was some 3600 units behind its occluder. It was
genuinely hidden. The engine's occlusion is planes, boxes and room/portal visibility, all of which keep
plenty of geometry that is in fact invisible — which is the whole reason for testing against a depth
pyramid. An object the engine kept and the HZB rejected is the expected win, not a defect. The counters now
keep the two apart, and whether such a rejection was correct is a question about visibility that only a
depth test can answer, not one the engine's opinion can settle.

#### Getting there: two wrong HZBs first

Both failures looked like plausible culling results and neither was:

-   **The depth was imported twice**, once for the draws to attach and once for the build to sample. The
    graph then saw two resources it believed unrelated, ordered nothing between them and inserted no
    barrier. One import carrying both a depth-stencil and a shader-resource view fixes it.
-   **The UVs were never scaled for the power-of-two padding.** The HZB covers a larger, power-of-two area
    than the rendered image, so a texture coordinate in the image is not one in the HZB. Without the scale
    the culling sampled mostly padding, which is all far plane, and the HZB looked uniformly empty however
    correctly it had been built.

What found both was a counter, not a screenshot: the number of sampled footprints whose farthest depth came
back `~0` or `~1`. 918 of 927 all-far says "this pyramid is empty" in one run, and no amount of staring at
a frame says that. The same counters are still there, and so is the one recorded rejection.

#### Still open in Phase 4

-   **The false-negative detector** (`CS_DCLF_CULL_VALIDATE`) is not built. It is the real gate: draw every
    culled object against the final depth with `LESS_EQUAL`, no writes and early-fragment tests, and flag
    any object that would have produced a fragment. Until it exists, "no objects were wrongly culled" rests
    on frame comparisons, which is weaker than the plan asks for.
-   **The two-phase replay** is not built. Today occlusion culling decides once, in the colour epoch,
    against the HZB built earlier in the same frame; there is no phase-1 list tested against the previous
    frame's HZB and no second depth draw to rescue its rejects. The consequence is that an object that
    becomes visible is drawn a frame late rather than rescued within the frame.
-   **Rigorous image parity** needs a scene that is not clipped. The Dragonsreach comparison is
    pixel-identical by eye, but the frame after a `coc` teleport is so overexposed that per-pixel deltas
    there measure the clipping, not the geometry.

## Phase 4.5 Step 0 and Step 1: the derivation, and what the stand-in leaks

### Step 0: the derivation counter now says what it measures

The counter that reports the property-only derivation against the accumulated pass descriptor used to be
masked with `& ~kRuntimePassBits`, so `0 differ` covered the 25 property bits and silently excluded the
four runtime groups (shadow light count 6-8, ShadowDir 13, DefShadow 14, DoAlphaTest 20) - which are
exactly the bits `GetRenderPasses` produces, and exactly the ones a Phase 5 that stops calling it would
have to derive. It also counted every object in the tables, although an object outside the accumulator has
no pass to compare against: `descriptors.pass` *is* the derivation for those, so they compared the
derivation with itself and could never differ.

Both are fixed. The report is now two halves plus a per-bit breakdown, over the comparable objects only.
Whiterun exterior, 3057 tracked:

```
derivation (last frame): 915 objects compared, 0 would stay native;
  property bits: 0 differ (00000000);
  runtime bits: 602 differ (00006000);
  per bit (* = runtime): bit 13*=602, bit 14*=602
```

The result is better than expected, and it changes what Step 6 costs:

-   The property half is exact: **0 of 915** differ.
-   The shadow light count (bits 6-8) and DoAlphaTest (bit 20) are **already derived correctly** - they
    never appear in the breakdown.
-   The whole gap is ShadowDir and DefShadow, always together, on about two thirds of the objects.

So skipping `GetRenderPasses` is one binary decision away, not four. Withholding registration (Step 5)
remains the right first move regardless, because it does not depend on deriving anything.

### Step 1: the artifact was a shared per-pipeline table, not a leak

**A reproduction that is not subtle.** The artifact was first seen as slightly blown-out additive lighting
in a night exterior, which is a poor instrument: the difference is small, and the time of day and weather
drift between runs. `coc WhiterunBanneredMare` at hour 22 is the opposite - a lit interior where the
defect covers the entire frame in white and red and cannot be mistaken for anything else. Every result
below is from that scene against a `CS_DCLF=0` control taken the same way.

**Two things that made every earlier comparison worthless.** `CS_DCLF=0` was read by nothing: it appeared
in the startup summary line and in no other code, so the feature installed unconditionally and every run
previously labelled a "`CS_DCLF=0` baseline" had DCLF fully enabled. And `CS_DCLF_TEST_COMMANDS` was
driven off `SceneStore`'s frame number inside `Prepass`, which does not run when the feature is off, so a
control never executed the commands and never reached the same cell at the same hour. Both are fixed: the
switch is a real gate, and the command driver has its own counter driven from `Reset`, ahead of the
install check, skipping loading-screen frames so the two runs stay in step.

**The cause.** Several tables are per-*pipeline*, not per-object, and the object that first creates a
pipeline fixes their contents for every object that shares it. The important one is
`tables.geometryConstants`, which comes from `FindLightingPass(property)` of that first object and carries
**that object's scene light list**, from which the engine reads the sun and the per-frame lighting.

While the tables held only the accumulator's output this was harmless: every candidate was visible, in the
lighting situation being drawn. Widening them to the whole tracked set made it a defect. A candidate the
engine had culled - in another room, or unlit - would often create the pipeline and hand its lights to the
visible objects drawn on it. In an interior that is a whole room lit by the wrong light list.

The fix is ordering, in `SceneStore::BuildFrame`: objects the engine kept are classified first, the rest
afterwards. An object the engine kept is by definition in the lighting situation being drawn, so it is the
correct template, and objects that cannot be drawn should never displace it. It costs one extra pass over
a hash map.

**How it was found, and two wrong turns worth recording.**

`CS_DCLF_EVAL` (new) suppresses parts of the stand-in evaluation. The first table it produced looked
decisive and was not:

| Configuration | Result |
| --- | --- |
| `CS_DCLF_TABLES=accumulated` | clean |
| `CS_DCLF_TABLES=tracked` | whole frame blown out |
| `… CS_DCLF_EVAL=off` | clean |
| `… CS_DCLF_EVAL=geometry` (only `SetupGeometry`) | clean |
| `… CS_DCLF_EVAL=material` (only `SetupMaterial`) | blown out |

This was read as "the per-material stand-in leaks state into the engine", on the further argument that the
corruption covered actors, which are skinned and never in DCLF's coverage. **Both halves were wrong.**
Suppressing the material evaluation makes `EvaluateMaterial` fail, which makes its objects ineligible, so
DCLF draws almost nothing - the "clean" rows were clean because nothing was drawn, not because nothing
leaked. And the actors were not corrupted; they were being washed out by the over-bright surfaces around
them.

The control that settled it was one that had never been run: **wide tables and the full evaluation, with
`CS_DCLF_HYBRID` off** so DCLF still builds and evaluates everything but its output never reaches the
frame. That frame is clean, which rules out leaked engine state entirely and puts the fault in DCLF's own
draws. The draw counts said the rest: `tracked` and `accumulated` draw the same ~300 objects in that
scene, so the difference had to be in what the shared tables contained, not in what was drawn.

The lesson is that a switch which suppresses a computation also suppresses everything downstream of it,
so "suppress it and see" only localises a fault when the downstream effects are held constant. The
`HYBRID`-off control does hold them constant, and should have come first.

### The rest of Step 1: evaluations that were pure waste

A candidate the engine culled cannot be drawn while the draws are gated on the engine's own visibility
(`CS_DCLF_CULL_INPUT=native`). It is still kept in the tables so the GPU culling has it as a candidate and
can be measured, but everything downstream of being drawn was being computed for it anyway - the
material's stand-in evaluation and the pipeline's per-frame constants, which are the two most expensive
things in `BuildFrame`.

Skipping those for undrawable candidates, in Whiterun exterior at night:

| | Before | After |
| --- | --- | --- |
| Material evaluations per frame | 636 | **132** (1783 skipped) |
| Material evaluation cost | 0.39 ms | **0.237 ms** |

The parallel per-object arrays are all still appended for a skipped candidate, with its material and
pipeline indices left at zero - nothing reads them, because `kObjectNativeVisible` is unset and
`BuildDrawsCS` rejects it first. That is what the earlier attempt at this got wrong: skipping one of the
arrays shifts every later object's index.

**The cross-frame material cache is not worth building, and the measurement says so.**
`CS_DCLF_MATERIAL_CACHE=probe` keeps the previous frame's record per (material, pass descriptor) and
compares it with a fresh evaluation. It reports **131 unchanged against 1 changed** per frame, so a cache
would hit about 99% - the worry that `SetupMaterial` reads enough per-frame engine state to make caching
unsound turns out to be wrong in practice.

But it would now save only the remaining 0.237 ms, against classification at 2.450 ms and the indirect
epoch at roughly 3 ms twice over. It also cannot be made exactly safe: the one record that does change
each frame is evidence that something outside the material's own fields feeds the result, so any cache
needs either a validity token that provably covers it or bounded-staleness re-evaluation, and neither is
worth it at this size. The probe switch stays so the decision can be revisited if the balance changes.

### `CS_DCLF_EVAL=audit`, and making `RunStandIn` transparent

Before the control above, the leak hypothesis was tested properly rather than by eye.
`CS_DCLF_EVAL=audit` snapshots the pipeline state around every stand-in call - VS and PS constant buffers,
PS shader resources 0-127, VS shader resources, PS samplers, and the bytes of the `BSLightingShader`
object - and reports every slot that differs after the restore, once per distinct finding.

The first version audited only the first 8 of roughly 640 material evaluations a frame and reported
nothing, which says nothing at all when the leaking call could be any of the other 630. Audited across
every call, it still reported nothing on the material path: the stand-in genuinely leaks no pipeline
state, which is what finally broke the wrong hypothesis.

It did find one real leak, on the geometry path: a stand-in `SetupGeometry` left a constant buffer bound
at PS slot 7 that was not bound before, because a feature hook binds its own per-geometry buffer there and
`RunStandIn` only restored the one slot matching the level being evaluated. That assumption is now gone -
all 14 constant buffer slots are saved and restored on both stages, which removes the class of problem
rather than the instance. The audit now reports zero leaks over a full run.

Light Limit Fix's `BSLightingShader_SetupGeometry_After` was a second real defect found on the way. It
publishes `strictLightDataCB` conditionally, keyed on four cache variables recording what the buffer
already holds, and binds b3 through a once-per-frame latch. A stand-in call updated all of it, so the next
real draw whose lights matched the cache skipped its own upload and shaded with the stand-in's lights.
`ConstantEvaluator::Evaluating()` - which already existed, documented as "hooks on the shader functions
must ignore it", and was consulted by nothing - now guards it.

Neither of these was the artifact. Both are fixed, and `RunStandIn` is transparent by the audit's measure.

**Result.** `CS_DCLF_TABLES=tracked` now matches a `CS_DCLF=0` control in the Bannered Mare at hour 22
with DCLF drawing 953 of 1102 objects, and the audit reports no leaks. `accumulated` is no longer needed
as a fallback.

### Two crashes and a false trail

`SceneStore::ProcessEvents` walked the scene graph every Present, including while a load screen was up,
and a load rebuilds `TES::objRoot` and the cell 3D under it. That crashed twice in the same subsystem: in
`RefreshCategoryNodes`' `objRoot` walk on a child that read back as `0x0001000000020001`, and the day
before in `AddSubtree`. The walks are now skipped while a load screen is up, the queued events are drained
but discarded, and the first frame afterwards rebuilds the tracked set from scratch. `BuildFrame` also
returns empty tables during a load: the tracked entries hold `NiPointer`s to the objects but not to their
renderer data, so classifying them across a load reads freed `BSGraphics::TriShape` data.

A `VK_ERROR_DEVICE_LOST` then appeared on `coc` teleports and was chased through five builds on the
assumption that it was one of these changes. It was not: the tree that failed four runs in a row passed
six times afterwards while byte-identical, so the bisect proved nothing and the fault was a persistent GPU
context left by the earlier crash. Worth remembering before bisecting an intermittent fault again - the
control has to be re-run at the end, not only at the start.

## Step 2: the per-draw epoch cost

The indirect epoch assembles one `DrawBindings` record per draw and runs twice a frame, for the depth
pass and the colour pass. The plan assumed the 128-iteration texture loop was the expensive part. It was
not, and the first thing built here was the instrument that says so: `CS_DCLF_STATS` now breaks the epoch
down by part. Bannered Mare at hour 22, 959 draws:

| Part | Before | After | |
| --- | --- | --- | --- |
| Textures and samplers | 0.286 ms | 0.258 ms | resolved per (material, pipeline) |
| **Constant groups** | **1.613 ms** | **0.538 ms** | packed per pipeline, patched per object |
| Binding record | 0.089 ms | 0.073 ms | |
| Rest (uploads, the epoch itself) | 0.710 ms | 0.773 ms | untouched |
| **Total per epoch** | **2.74 ms** | **1.62 ms** | 2.85 → 1.91 µs per draw |

### Textures and samplers, resolved once per (material, pipeline)

Neither loop depends on anything per-object: they read the material's textures, the pipeline's technique
(for the shadow mask) and its register usage, plus the frame's shared textures. Running them per draw
meant 128 + 16 iterations and a descriptor heap lookup each for every draw, when about 150 distinct
(material, pipeline) pairs stand behind ~950 of them. They are now resolved on first use and copied
afterwards, exactly as the constant blocks already were.

This is the CPU half of the plan's step 2a. The other half - having the shader read
`ResourceDescriptorHeap[…]` so the indices need not travel in the record at all - is not done, and on this
measurement is worth less than it looked: the loops were 10% of the epoch, not the bulk of it.

### The PerGeometry group: pack per pipeline, patch per object

This was the real cost. `PackConstantGroup` walks every variable in the layout and every component of
each, checking a written-sentinel and copying four bytes at a time; it ran twice per object, over a group
that is identical for every object on a pipeline apart from **five variables** - the two world matrices
and three shading values (`MaterialData`, `EmitColor`, and the w of `SSRParams`).

The group is now packed once per pipeline into a template, and each object copies the template and
rewrites only those five, at byte offsets resolved once per pipeline (`GeometryPatchOffsetsOf` /
`PatchObjectGeometry` in `LightingConstants.cpp`). An unwritten component still packs as zero, which is
what the full pack's initial `memset` produced for it.

It also removes the `GeometryConstants` struct copy per object - two `ConstantBlock`s, about 2 KB - which
`ObjectGeometryConstants` made only to patch four fields in it. That function stays, because the capture
parity check uses it as the reference implementation.

**Validated by capture parity, not by eye.** With `CS_DCLF_HYBRID` off so the native loop still draws
everything and the check has something to compare against: **125,100 draws checked, 0 mismatched (0
material, 0 per-geometry, 0 technique)**. That is a much stronger statement than a screenshot, and it is
the right gate for a change that only ever alters constant *values*.

### A crash this introduced, and the shape of it

Skipping the material and pipeline entries for undrawable candidates left their `materialIndex` and
`pipelineIndex` meaningless, and the first version left them at zero. The first frame after a teleport has
tracked geometry but nothing accumulated yet, so *every* candidate is undrawable, the pipeline table is
empty, and `pipelineBlocks[0]` is out of bounds - an access violation on `coc`. Index zero is not a safe
placeholder when the table can be empty. There is now an explicit `kObjectNoBindings` flag, checked before
anything indexes the tables with those fields, and such objects are reported under their own skip reason
(`candidate-only`) rather than being conflated with pipelines that are not ready yet.

### What is left of step 2

The record is still 800 bytes and still uploaded per draw; `rest` (uploads and the epoch) is now the
largest single part at about 0.8 ms. Cutting it means the bindless work proper - the shader reading its
textures and per-object constants out of GPU-resident tables indexed by a single object index, so
`DrawBindings` and the `IndirectAddress`/`IndirectIndex` ranges disappear. That is a pipeline layout and
`Lighting.hlsl` change (a `DCLF_BINDLESS` permutation beside `DCLF_DEPTH_ONLY`), and it is not started.

## Step 3: both epochs now share a table generation

The Z-prepass epoch runs at the end of `Main_RenderDepth` and `BuildFrame` ran at `Prepass`, from
`StartDeferred`, which is after it. So the depth segment read the **previous** frame's tables while the
colour segment read the current ones, and the per-object visibility verdicts the depth segment writes
could not be applied by index in the colour segment at all - object index *i* meant a different object in
each. That is what blocked two-pass culling.

### The measurement that was wrong, and the one that settled it

An earlier probe counted the accumulated passes at each candidate hook point and found 0 before the depth
pass against 604 at `Prepass`, and this was written up as "`BuildFrame` cannot run any earlier". **That was
wrong**, and the Ghidra decompilation is what exposed it:

-   `Main::Draw` (`0x1406444b0`) queues `DrawWorld_BuildSceneLists` (`0x14064bc20`) on the
    `gJobList_SceneListAccumCulling` job list and **`JobList__Finish`es it before**
    `NiCamera::CalculateAndDrawShadowCasterLights`, i.e. before the shadow maps and long before the depth
    pass.
-   `BSShaderAccumulator::FinishAccumulating` (`0x1414b2240`) is `FinishAccumulatingPreResolveDepth` then
    `FinishAccumulatingPostResolveDepth`; for the main render mode the first of those (`FUN_1414b2d90`)
    **draws** the accumulated passes, over ranges of technique ids, reading the early-Z global at
    `0x14328cc79`. It does not assemble anything.

So the accumulator is complete early, and `globals::game::currentAccumulator` is a *currently rendering*
pointer that is simply not set yet. Re-probing against the accumulator latched from a frame where the
global was set:

| Hook point | via latched accumulator | via `currentAccumulator` |
| --- | --- | --- |
| `EarlyPrepass` (before the depth pass) | **612** | 0 |
| end of `Main_RenderDepth` | **612** | 0 |
| `Prepass` | 612 | 612 |

The passes were there the whole time.

### What was done

`BuildFrame` and the pipeline/program build move to `EarlyPrepass`, which runs from
`Main_RenderShadowMaps`. `CollectAccumulatedPasses` reads `SceneStore::latchedAccumulator`, refreshed by
`LatchAccumulator()` from `Prepass` where `currentAccumulator` is valid; a change of accumulator is logged
once. The first frame has no latch and builds empty tables, which is harmless.

`DrewLastFrame`'s one-frame tolerance absorbs the shift in when the frame counter advances, so the hybrid
skip needed no change.

### Two things that had to move back, and what caught them

Capture parity caught both; neither was visible in a screenshot.

1.  **`EyePosition` (VS PerGeometry variable 2): 134,700 mismatches of 157,800.** Most of what
    `SetupGeometry` writes is per frame, and `EyePosition` is relative to `posAdjust`. At `EarlyPrepass`
    the renderer's shadow state still belongs to the shadow-map camera just drawn, so the evaluation
    produced that camera's eye (113.07) where the main pass writes 0.
2.  **`EmitColor` (PS PerGeometry variable 8): 6-21 mismatches.** Candle and chandelier emissives flicker,
    and sampling `emissiveMult` at `EarlyPrepass` put it far enough from the draw that the parity check's
    0.1% relative tolerance on `EmitColor` no longer covered the difference (0.49074 against 0.49183, and
    1.2350 against 1.2546).

`SceneStore::RefreshFrameConstants()`, called from `Prepass`, re-evaluates the per-pipeline per-frame
constants and resamples the per-object shading against the main camera's state. To do that it needs the
property whose lighting pass supplied each pipeline's constants, so `tables.geometryTemplate` is kept
parallel to `tables.pipelines`. The object, geometry and material tables are camera- and
time-independent and stay in `BuildFrame`.

The Z-prepass epoch runs between the two and so uses the previous frame's values for those constants.
That is harmless: vertex position comes from `World`, which is patched per object at epoch time with that
epoch's own eye, and never from these.

**Gate:** capture parity **OK, 254,698 draws checked, 0 mismatched**; `CS_DCLF_BUILD_PARITY` OK on 959
sequences; the two epochs now report consecutive table generations (colour 5764, then the next frame's
z-prepass 5765) where they used to report the same one; no crashes, no device loss, 0 VUIDs.

## The object index reaches the shaders

Groundwork for the bindless step, landed and verified on its own.

`DrawSequence` now carries the object's index in `SceneStore::Tables::objects` as a third root constant
word, beside the two that already hold the `DrawBindings` address. The record grows from 64 to 68 bytes;
the push constant range and the `Constant` indirect argument both go from 2 words to 3, and because the
command signature's arguments are positional the vertex buffer, index buffer and draw arguments shift with
it.

Nothing reads it yet. It is the piece the shader needs in order to fetch per-object data from a
GPU-resident table instead of receiving it through a per-draw constant buffer, and it is what makes the
rest of the bindless work possible:

-   Of the PerGeometry group, only **five variables are per-object** - `World`, `PreviousWorld`,
    `MaterialData`, `EmitColor` and the w of `SSRParams` (this is the same finding the Step 2 template
    rests on). Everything else is per-pipeline.
-   Once the shader reads those five from a structured buffer indexed by this word, the PerGeometry
    cbuffer becomes *per-pipeline*, so the per-object arena allocations and packing disappear entirely.
-   With no per-object constant addresses left in it, `DrawBindings` becomes identical for every draw
    sharing a (material, pipeline) pair, so the records deduplicate and the per-draw upload collapses.

That last point is the one that matters for culling: the reason the whole tracked set cannot be handed to
the GPU today is that the CPU builds a binding record per candidate. When records are per-(material,
pipeline), feeding every candidate costs nothing per candidate, and the culling genuinely happens before
any per-draw work exists.

**Gate:** `CS_DCLF_BUILD_PARITY` reports 337 sequences matching the CPU templates byte for byte with the
new 68-byte layout, and the frame is unchanged. Only 21 sites in `Lighting.hlsl` reference those five
variables, so the shader change is small.

## The DCLF set (2026-10-03)

This section supersedes how ownership worked in the sections that follow it chronologically: "Static ownership", the
claims, the native skip and the drawn marks are gone.

**The rule.** An object in the DCLF set is drawn by DCLF in every phase it is a member of, and the engine never draws it in
a view of that phase. Before this, who drew an object was decided by about eight gates in four or five stores, each per
pass: the main claims, the colour build's drawn marks (read back as `DrewLastFrame`), `SkipNativePass`, PrimaryCull's
admission, three shadow claim sets, and the occlusion maps' whole-map handoff. They lagged each other by a frame and
disagreed, and every disagreement was a hole or a double draw. The terrain LOD holes were one: the Z-prepass wrote depth
for a new chunk whose colour pair was not ready, the native skip let the engine draw its colour against DCLF's depth, and
triangles dropped (dclf-lod.md, "Terrain LOD").

**The phases** (`Scene/SceneSet.h`): main (the Z-prepass and the colour pass), casters in the plain and clamped shadow views
(the sun's cascades, spot lights), casters in the paraboloid views (point lights), and each occlusion map. An object is a
member of a phase for a frame when:
- it is eligible (its verdict is None);
- it takes part in the phase: for main, it is bound (its record has its pipeline and material slots); for the shadow and
  occlusion phases, its record casts or occludes;
- it is ready for the phase: for main, its pipeline is compiled, its material's textures are imported, its pipeline's
  shadow mask and the shared lookups are resolved, its geometry is resolved, and a decal has its slot; for a shadow or
  occlusion phase, its Utility pipeline exists under every rasterizer state of its class in each of the phase's modes the
  last epoch drew, and an alpha-tested technique's diffuse is imported (`IndirectDraws::PhaseReady`).

Each phase is whole: its views withhold exactly what its draws draw. A base and its multi-index layer are main members
together. A phase is DCLF's (`SetSnapshot::drawn`) for the main camera always; for a shadow mode once its last epoch drew
its views; for an occlusion map once its last `ExecuteOcclusion` drew it. A kind of view seen for the first time is the
engine's for that frame, so its casters are never withheld from views DCLF has no pipelines for.

**Decided once a frame** (`SceneStore::CommitSet`), at the scene phase, after the walk and before the main camera's cull.
Readiness is taken by events, never by a scan: the tables' change log (every write of a record, its residency or its
bindings, whoever made it), the lookups' version when it moves (the slots waiting for readiness are evaluated again), the
shadow modes' serial (a new mode or rasterizer state), the bind queue (a member whose binding is taken again this frame
leaves before the accumulate phase rebinds it), and the membership witness (a change rebinds every resident). Readiness
held is never lost: a material or a shadow mask whose view is re-imported keeps drawing with the old one until the new one
arrives (`RefreshMaterialLookups`).

**Applied a frame later** (2026-10-04, dclf-async-publication.md, "Phase 3"). `CommitSet` decides the next frame's set into
`setPhasesNext` and the snapshot. `ApplySet`, at the next `BeginSceneFrame` before the frame's events, makes it the frame's:
- it writes the changed slots' `Tables::setPhases`, `kObjectMember` and lacking counts;
- it publishes the snapshot to `PassCapture`.

The claims must be in place at `Main::Draw`, before the frame's scene work can have run once that work is on a worker, and
the frame draws exactly the set its claims are. So an object joins or leaves the set one frame after the walk that decided
it. A slot freed or given to another geometry between the commit and `ApplySet` takes no phase. A slot freed after
`ApplySet` loses its phases with its record (`Tables::ResetObject`), so the shadow builds, which read `setPhases`, never draw
a freed slot.

**One GPU-resident source of truth.** The set is `Tables::setPhases` (per slot), with the main phase also in the record's
`kObjectMember`, which the GPU reads. Every build selects by it: a main build's input is drawable iff the object is a main
member and its pair resolved; a shadow or occlusion mode's inputs are the members of its phase. A member a build cannot
draw is a defect, counted and reported (set parity, "members a shadow build could not draw").

**The engine's copy.** The commit publishes an immutable snapshot (geometry -> phases, and the phases DCLF draws) to
`PassCapture::PublishSet`, lock-free. The registration hooks withhold a member's passes from the views of its phases:
- the main camera's: `RegisterPass`, the main modes' direct group insertions in `FUN_1414b2330` (hints 1-7, 9, 11, 13-18,
  and the layer's hint 12) and, for the Z-prepass's accumulator (render mode 0xC, which registers through the shadow
  modes' `FUN_1414b2a60`), its group insertions (hints 3, 7, 8, 11). A LOD cross-fade's copy of the old level (hint 10)
  is the engine's; a fade DCLF does not model (blended, or a decal's) is left to the engine and its member leaves the set
  at the next commit until the fade ends.
- the shadow views': the same hooks, by the view's render mode's phase.
- the occlusion maps': `FUN_1414b2c20`'s one insertion (group 14). The engine still culls and registers a map's scene when
  some occluder of it is not a member (`SetLacking`); when every one is, `SetupMask` is skipped as before.

The stand-in's admission (PrimaryCull), the leaf exclusion, the list filter, the sun's cascade skip and the point lights'
exclusion are optimisations keyed by the same snapshot. Correctness does not depend on them.

**Checks.** `[DCLF] DCLF set:` (members, waiting and why, joins and leaves, defects); `main camera since the last report`
(a native draw of a main member: `<- LEAK`, must be 0); `CS_DCLF_SET_PARITY` (the GPU's visibility words against the set:
in the set and drawn by nobody, depth without colour, drawn outside the set, a record disagreeing with the set);
`CS_DCLF_PARITY_BOTH=1` withholds nothing, so the engine draws everything while DCLF draws its set too (capture parity).

**Measured** (the teleport route, `landjump.sh`, every 300 frames): LEAK 0; set parity OK; terrain LOD coverage 0
unshaded pixels; cascade culling parity OK; shadow views all drawn.

**A view's rasterizer state and its sibling.** At first, a shadow view whose rasterizer state was new in a frame left that
frame's members unready for it (`<- SET SHADOW`, one frame: 8,085 members at startup, 1,957 at the first point light). The
set withholds a phase's casters before the frame's views are seen. The new states were each a known state's sibling under
the other cull mode. The renderer's cull mode when a view is captured is its last pass's (the Utility shader sets it per
pass: none for a two-sided property, back otherwise). So a mode's views draw with both, and the first view of a state now
registers its sibling as well (`PendingView::cullStates`), so both are ready together. The route then had 0 members a
shadow build could not draw in every report.

**Capture parity and the bits the draw decides.** Under `CS_DCLF_PARITY_BOTH=1` most non-LOD draws mismatched in the pass
descriptor's sun bits, Specular, and the Envmap technique, and trees in TreeParams. None of these are the pipeline's:
- BuildDraws drops a sun-test record's ShadowDir, DefShadow and light count on a cascade miss.
- The draw fades Specular and Envmap by the LOD fades, where `GetRenderPasses` drops them.
- TreeWindCS writes a resident tree's wind on the GPU.

Capture parity now leaves those bits to their own checks: `PerDrawBits` covers the sun test's counters and the accumulate
phase's LOD fade check, and `GpuTreeWind` defers to the tree wind parity. A native pass that lacks those bits was built with
another technique (no shadow mask, the faded features dropped), so for such a draw only the descriptors are compared. It
is counted as "whose pass differs only in the bits the draw decides", and the same object is compared whole on the frames
its native pass has the bits.

The same fix applies elsewhere:
- **The permutation buffer**, which also leaves out IsBeastRace except on the face techniques. Subsurface Scattering writes
  it only at a face's SetupGeometry, and Lighting.hlsl reads it only under `SKIN`.
- **StrictLightData's ShadowBitMask**, which a DCLF draw takes from its object word (BuildDrawsCS's local shadow lights,
  checked by draw parity).

**Result** (`tourcapy`, 15 reports, 255k-405k draws each): 0 mismatched draws in every technique; light data, bone
palettes, draw and skin parity OK. The permutation residue was 0-1,449 draws a report, and both causes were real
defects:
- **IsBeastRace** was never set on DCLF's face and body draws (the Khajiit player's), so Subsurface Scattering's mask
  treated a beast race's skin as not one. It is per actor, so it is now an object flag (`kObjectBeastRace`, set by the
  accumulate phase with Subsurface Scattering's rule) carried in the object record's flag word (`recordFlags`, which
  replaces the unused ShadowBitMask). Lighting.hlsl reads it there under `DCLF_BINDLESS`.
- **AdditiveLighting** was missing on additively blended decals (`EdgeBlood01`). A blended decal's key carries the
  blend mode, so the pipeline's permutation now takes the bit from the alpha property, by CS's own rule.

With both fixed, capture parity is OK in every report (11 of 11), with every sub-check OK.

## Static ownership: capture at registration, then withhold

DCLF was a guest in the engine's per-frame loop. It now owns a set of objects outright: their passes are
built, lit and shadowed exactly as before, and then kept out of the main camera's batch renderer so the
native loop has nothing to draw.

### Capture belongs at `BSBatchRenderer::RegisterPass`, not at `GetRenderPasses`

The plan called for hooking `BSLightingShaderProperty::GetRenderPasses` (vfunc 0x2A). That cannot work:
the two fields the tables depend on do not exist when it returns. `AccumulatedPass::technique` is the
batch group's key, which the *caller* computes and hands to registration - it differs from the pass's own
`passEnum` because DoAlphaTest is added there - and `subPass` is chosen inside registration.

`BSBatchRenderer::RegisterPass(BSRenderPass*, std::uint32_t techniqueID)` (vfunc **0x02**, CommonLib
declares it) is the right place. It receives the technique as an argument, and it is also where
withholding belongs: not calling the original is exactly "the batch renderer never receives this pass".

`subPass` is derived rather than observed. The engine's classifier (`FUN_1414f4790`, called from
`RegisterPass` at `0x1414f2a20`) reads **only** the shader property's flags and the geometry's alpha
property: flag bit 54 alone puts a pass in list 4, otherwise the list is bit 36 as value 2, plus whether
`NiAlphaProperty::alphaFlags` bit 9 (alpha testing) is set. Nothing per-frame enters it, which is what
lets DCLF keep the field once passes stop reaching a batch renderer at all.

**Gate:** 634 registrations captured, 634 compared against the accumulator walk, **0 missing, 0 extra, 0
technique differs, 0 subPass differs**. The derivation matches the engine on every pass. The tables are
now built from the capture (`CS_DCLF_PASS_SOURCE=accumulator` restores the walk), and capture parity
stayed OK over 157,800 draws through the switch-over.

Incidentally, registration turns out to be **single-threaded** in practice, despite running under a job
list and despite the engine's classifier taking a mutex. The capture buffer is lock-free anyway, and
reports the thread count so a change would be noticed.

### Withholding

`CS_DCLF_OWNERSHIP=static`. The hook consults an immutable claim set, published whole once per frame and
read from whatever thread registers; a claim is a standing statement that DCLF owns an object, not the
per-frame decision the old `drawnFrame` skip was.

The claim is **what the colour epoch actually drew**, not what DCLF would like to draw. Withholding means
the native loop will not draw it either, so claiming something DCLF then fails to draw - a pipeline still
compiling, a texture not resolved - leaves a hole. Having drawn it once is the evidence that it can be
drawn again. Claims are dropped across a load screen, because they name geometry from the cell being torn
down.

Only the main camera's batch renderers are affected; the shadow cameras keep their passes. The native
**depth** pass is also untouched, because it registers through a different virtual
(`BSLightingShaderProperty::GetRenderDepthPass`, `0x1414aff30`) into a different renderer - so DCLF's
existing hybrid depth skip still does that half.

### The hole detector, and what it caught

Per frame on the CPU, with no readback: an object that was **withheld and not drawn** is a hole.

Being claimed is not sufficient on its own, and the first version got this wrong - it counted every
claimed object that went undrawn, which over-reported badly at cell transitions (6, then 14 "holes"). An
object the engine culled this frame is never registered, so it is never withheld either, and nobody was
going to draw it. The test for "the engine would have drawn it" is that its pass was captured this frame.

**Gate:** Bannered Mare, then `coc` to Whiterun, then to Dragonsreach - 921 passes withheld in steady
state, **0 claimed but not drawn** at every report including across all three transitions; no crashes, no
device loss, 0 VUIDs; the native opaque pass now has nothing left to skip (`0 in the opaque pass`).

### What this unblocks

The Phase 4 defect was that the native loop had already skipped an object by the time the depth segment
ran, so anything phase 1 rejected got no native draw, no DCLF depth and therefore no DCLF colour. With
ownership there is no skip decision to get wrong: the engine never draws a claimed object, so DCLF
culling one is simply correct. The Z-prepass no longer gates on `DrewLastFrame` under static ownership.

## Two-pass culling, switched back on

`CS_DCLF_CULL` had been unset since Phase 4, because a culling verdict could not mean anything while the
native loop decided ownership a frame late. With the table generations aligned (Step 3) and the engine no
longer drawing what DCLF owns (static ownership), it runs.

### Results

Dragonsreach, `CS_DCLF_OWNERSHIP=static`, `CS_DCLF_CULL=occlusion`:

- 1949 candidates tested, 835 rejected (42.8%): 159 outside the frustum, **676 occluded**.
- **158 of the occluded were objects the engine had kept** - the win the HZB exists for, since the
  engine's occlusion is planes, boxes and portals.
- Phase 2 brought back **75** objects the stale HZB had rejected and drew depth for 23 of them, so the
  two-phase rescue is doing its job.
- HZB: 1795 footprints sampled, 0 all-near, 264 all-far.
- **0 claimed but not drawn**, 0 VUIDs, no crashes.

Whiterun exterior, occlusion against a frustum-only control at the same place and hour:

| | Draws written | Rejected |
| --- | --- | --- |
| Frustum only | 337 | 162 frustum |
| Occlusion | **232** | 162 frustum + 250 occluded (210 the engine had kept) |

A 31% cut in DCLF's draws, and the two frames are equivalent - nothing is missing from the occluded one.

### Why claiming on "drawn last frame" does not fight the culling

There was a real worry here: claims are published from `drawnFrame`, so if culling an object stopped it
being drawn, the claim would lapse, the pass would stop being withheld, and the engine would simply draw
it again - correct output, no saving.

It does not happen, and the measurement says so rather than the argument. `claim churn: +0 -0` in steady
state with occlusion on. The reason is that `drawnFrame` records what the **CPU submitted**, not what the
GPU rasterised: the epoch loop marks every object it builds a record for, and culling happens afterwards
in `BuildDrawsCS`. So an occluded object stays owned and simply is not rasterised, which is what should
happen. The churn counters stay in the stats as a guard, since the property is not obvious from the code.

The same distinction is what keeps the hole detector meaningful: it reports objects DCLF failed to submit,
not objects it deliberately culled.

## Step C: the per-object constants leave the constant buffer

The five PerGeometry variables that differ between the objects of one pipeline - `World`,
`PreviousWorld`, `MaterialData`, `EmitColor` and the w of `SSRParams` - now come from a GPU-resident
table indexed by the object index the draw already carried, instead of from a constant buffer packed per
draw. Built behind `CS_DCLF_BINDLESS=1`, default off.

### How the shader reaches its own index

The draw's push data was already three words: the binding record's address, which the pipeline layout
consumes to resolve the draw's buffers and descriptors, and the object's table index, which nothing read.
It turns out no new binding kind is needed to read it.

BasicRHI's descriptor-heap path gives every push constant range a `VkDescriptorSetAndBindingMappingEXT`
of its own, sourced from `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT` at the range's `set` and `binding`
(`rhi_vulkan.cpp`, the loop over `layout.pushConstantRanges`). The `set`/`binding` fields are documented
in `rhi.h` as "ignored on Vulkan", which is true of the classic path and not of this one. So a shader
that declares an ordinary `cbuffer` at the range's register reads push data directly:

```hlsl
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFRecordAddress : packoffset(c0.x);  // consumed by the layout, not read here
	uint DCLFObjectIndex : packoffset(c0.z);
};
```

The object table itself rides the existing `DrawBindings::textures` mechanism at t127, so it needs no new
binding kind either. The one layout change is a vertex-stage range for that single register - the pixel
stage already had the whole t range, and the vertex half needs exactly one SRV. `ResourceDescriptorHeap[]`
was not needed after all, so the spike the plan reserved for it is not owed.

### The record

```hlsl
struct DCLFObjectRecord
{
	float4 World[3];          // row_major float3x4, already relative to the eye
	float4 PreviousWorld[3];
	float4 MaterialData;
	float4 EmitColor;         // emissive in xyz, the per-object w of SSRParams in w
};
```

128 bytes, and its second half is `ObjectShading` unchanged, which is what lets it be copied straight
through. The five names are reintroduced as `static` globals initialised from the table, so the 21 sites
that read them are untouched; `SSRParams` stays in the constant buffer, because only its w is per-object
and it is read at exactly one site.

`BuildObjectRecord` fills a record from the same inputs `PatchObjectGeometry` writes into a packed group,
through the same unwritten-component rule, and the table is filled for **every** candidate rather than
only the drawn ones - a candidate skipped by the epoch still reaches the culling, and an index that
addressed nothing would be worse than one that addresses a record no draw reads.

### The gate is a CPU comparison, not a screenshot

`CS_DCLF_BINDLESS_PARITY=1` compares each record against the constant group the non-bindless path packs
for the same object, component by component and bit for bit - a tolerance here would only hide a layout
mistake. It needs no readback and does not need both forms in one run, which matters because the
permutations are compiled against the switch and the two cannot coexist in one process.

**It held:** 0 of ~500 million components differ, across the Bannered Mare at hour 22 and the Whiterun
exterior. The frame is correct, static ownership reports 0 claimed-but-undrawn, and the culling still
reports 0 false negatives.

The second half of that gate is structural rather than counted: under `DCLF_BINDLESS` the vertex stage
stops declaring the PerGeometry buffer at all - SPIR-V binding 2 is simply absent from the module - so a
frame with objects in the right places is proof that the transforms came from the table.

### Then the buffer becomes per-pipeline

With nothing per-object left in the group, the objects of a pipeline share one pair of arena blocks
written once, and the per-object allocate/copy/patch disappears. Bannered Mare, per epoch:

| Part | Per-object buffer | Per-pipeline buffer |
| --- | --- | --- |
| Textures and samplers | 0.82 µs/draw | 0.25 µs/draw |
| **Constant groups** | **0.82 µs/draw** | **0.33 µs/draw** |
| Binding record | 0.09 µs/draw | 0.08 µs/draw |
| **Total per draw** | **1.96 µs** | **1.47 µs** |

Upload fell from 1.8 MB to 1.1 MB an epoch, and that is *after* adding the object table, which costs 128
bytes per candidate and lands in `rest` at no measurable change (0.76 ms against 0.77 ms).

What remains of `constant groups` is the material, light, permutation and alpha blocks, which were
already deduplicated and are not what this step was aimed at.

A measurement trap worth recording: with `CS_DCLF_BINDLESS_PARITY=1` the epoch still reported 0.85 ms of
constant groups, because the check repacks the group per object inside the timed region. The switch is a
gate, not a thing to measure through.

### What is left: deduplicating the binding record

The plan's third item - `DrawBindings` becoming identical for every draw sharing a (material, pipeline)
pair, so the records deduplicate and the per-draw upload collapses - is **not** reached yet, and the
reason is specific. Two entries of the record still vary per object:

-   the light block, keyed on (room index, shadow bit mask);
-   the alpha threshold block.

Both are constant buffer addresses, so the record differs whenever those differ. Moving them into the
object table is the remaining work, and it is a larger change than this one because the room index and
shadow mask are read by Light Limit Fix's own `StrictLightData`, not by `Lighting.hlsl` alone.

## Step D: the binding record deduplicates

The 800-byte `DrawBindings` record was assembled and uploaded once per draw. It is now one per
(material, pipeline) pair: **1616 candidates in Dragonsreach build from 105 records**, and the epoch's
upload falls from 0.9 MB to 0.5 MB. Behind `CS_DCLF_BINDLESS_DRAW=1`, which implies `CS_DCLF_BINDLESS`.

### Four blocks varied per object, not the two previously recorded

| Register | Keyed on | Where it is read |
| --- | --- | --- |
| PS b3 | `(roomIndex, shadowBitMask)` | `LightLimitFix::IsLightIgnored`, one call site in `Lighting.hlsl` |
| PS b11 | alpha threshold | one site |
| PS b8 | emissive multiplier | `Color::EmitColor`, one site |
| VS/PS b4 | `(pipeline, extra)` | - |

Three of those turned out cheaper than they looked:

-   **The permutation block's per-object variation is dead.** The only per-object bit DCLF puts in it is
    `SuppressExternalEmittance`, and that bit is read at exactly one place in the whole shader tree,
    `Effect.hlsl`'s `GetLightingColor` - never by `Lighting.hlsl` or anything it includes. So the block
    keys on the pipeline alone with no shader change at all. The non-bindless build keeps the bit, so
    capture parity goes on comparing the real thing against the engine.
-   **The 1216-byte light block was 16 useful bytes.** DCLF writes `{0, roomIndex, shadowBitMask, 0}` and
    nothing else, so `NumStrictLights` is 0 and the `StrictLights[15]` tail is never read. With the two
    live values moved, b3 is one zeroed block for the whole epoch.
-   **The `NSCB_ALIAS` shim made the b3 move nearly free.** `RoomIndex` and `ShadowBitMask` already
    resolve through `static const` aliases under DXC, so two alias lines become statics reading the object
    record. The cbuffer keeps its declaration and its layout, and Effect, Particle, Water and RunGrass -
    which include the same header and never define the macro - are untouched.

The record grew from 128 to 144 bytes to carry the four values. They went in a row of their own rather
than the spare `ObjectShading::materialData[3]`: that struct is shared with `ObjectGeometryConstants` and
`PatchObjectGeometry`, so anything parked there would leak into the non-bindless build's `MaterialData.w`
and into the parity comparison of that group.

`Lighting.hlsl`'s `DCLF_BINDLESS` block moved into `Common/DCLFObjects.hlsli`, because `Color.hlsli`
consumes the emissive multiplier and is included before the point where the block used to sit.

### Three defects this uncovered, all pre-existing

The capacity check `if (records.size() >= kMaxDraws)` was doing four jobs, and two of the others were
already wrong:

-   **`inputs` could overrun.** The depth epoch's cull-only push happened *before* that check, so the
    input count was bounded by the tracked-set size rather than by the buffer. Past 16384 tracked objects
    the upload would write beyond it and BuildDraws would dispatch over inputs never uploaded.
-   **`visibility` was indexed out of bounds**, by table index into a buffer sized for draws.
-   An object past the record table's capacity still got a draw, whose object index read zeros - a world
    matrix of zeros collapses it to a point at the eye with no other sign.

Both buffers are now sized by the table, and the caps are explicit and separate: object index against the
table, inputs against their own buffer, sequences against the draw cap, records against their own with a
`record-capacity` skip reason of their own.

Two more would have made the validation lie rather than fail:

-   **`CS_DCLF_BUILD_PARITY` sorted both sides by `bindingsAddress`.** Once records are shared that is not
    a total order, and equal-key runs land in arbitrary relative order on the two sides. It now sorts by
    object index, which is unique per sequence. While fixing it, a second limitation showed up: the check
    compared position by position, so with culling on it reported every sequence past the first culled
    object as differing. What the GPU writes is a *subset* of the CPU's templates, so it is now a
    subsequence check - and works for the first time on the configuration DCLF actually ships.
-   **The upload guard covered three buffers with one condition.** A depth epoch where every candidate is
    cull-only has no records and plenty of inputs, and BuildDraws would then run over a stale buffer.

### A parity check that was itself wrong

`emissiveMult` moved from a per-draw scene-graph dereference in the epoch onto the tables, beside the
shading that `MakeShading` already reads it for. The natural check was the record against a live read of
the property, and it fired: about 100 components in half a billion, always on flickering emissives.

**The check was what was wrong.** The multiplier is animated, and the shader divides it back out of the
emissive colour before re-applying it, so the record's multiplier has to be the *same sample* that
produced that object's `EmitColor` - which is why `MakeShading` now hands both back together. A live read
at epoch time is a strictly later sample, and matching it would have broken the cancellation rather than
proved anything. What is checked instead is the plumbing: `emissiveMult` is a new parallel array, and a
gap in one of those shifts every later object's index. Whether it is sampled at the right point in the
frame is already covered, and covered better, by capture parity's `EmitColor` comparison against the
engine's own draw.

### Gates

-   **`CS_DCLF_DEDUP_PARITY=1`**, the check that matters: rebuild the record per draw and compare it byte
    for byte against the one its pair holds. **3,981,022 rebuilt records, 0 differ.** This is the only
    check that would catch a fifth per-object dependency nobody had noticed.
-   `CS_DCLF_BINDLESS_PARITY` extended to the four new values: clean.
-   `CS_DCLF_BUILD_PARITY` OK with culling on, 825 of 921 sequences matching with 96 rejected.
-   0 claimed-but-undrawn, 0 VUIDs, correct frames in the Bannered Mare, Whiterun exterior and
    Dragonsreach - which between them exercise all three moved registers: alpha-tested banners, emissive
    fires, and portal-strict interior lighting.
-   Offline, before any run: `DCLF_BINDLESS_DRAW` removes bindings 8 and 11 from the pixel module and
    keeps 3, which is reflection-level proof the registers really left the shader.

### What it cost and what it bought

Bannered Mare and Dragonsreach, per epoch:

| | Before Step C | After Step C | After Step D |
| --- | --- | --- | --- |
| Per draw | 1.96 µs | 1.47 µs | **0.74 µs** |
| Records per epoch | one per draw | one per draw | one per (material, pipeline) |
| Upload | 1.8 MB | 1.1 MB | **0.5 MB** |

Distinct pairs measured 85 to 201 across three areas, so the record buffer is sized at 2048 entries
(1.6 MB) rather than 16384 (13.1 MB) when deduplication is on, and stays at the draw cap when it is not.

**The structural result matters more than the microseconds.** The reason the whole tracked set could not
be handed to the GPU was that the CPU built a binding record per candidate. It no longer does, so feeding
every candidate to the culling now costs a draw input and a sequence - and nothing else.

## Per-object work, Stage 0: the measurement was lying

`BuildFrame`'s cost was reported in four parts, one of which was called "classification" and read 1.272
ms a frame in Dragonsreach. It was the largest single DCLF cost and the obvious next target.

It was not one thing. `PartTimer` is called at the *top* of each iteration, so that bucket accrued
everything since the previous object: both `FindAccumulatedPass` lookups, `ClassifyStatic`,
`ClassifyFrame`, the derivation counters, two `GpuResources::Resolve` calls, three dedup probes, two
transforms, `ExternalEmittance::ShouldSuppress`, `GetRoomIndex`, `MakeShading`, the `objectIndex` insert
and the `DrawSequence` build. Nine things under one label.

The parts are now ten, each measuring one thing, behind `CS_DCLF_PROFILE=1` (default off, so the loop
makes no clock calls in a normal run). Whiterun exterior, 9022 tracked / 2885 eligible:

| Part | ms | | Part | ms |
| --- | --- | --- | --- | --- |
| **record** | **1.444** | | resolve | 0.199 |
| classify-static | 0.907 | | diagnostics | 0.118 |
| classify-frame | 0.532 | | dedup | 0.051 |
| pass-lookup | 0.395 | | pipeline-eval | 0.045 |
| material-eval | 0.208 | | walk | 0.129 |

**The largest part is not classification.** It is `record` - the object record assembly: transforms,
bounds, flags, emittance, room index, shading and the draw. Classification is second, and its two halves
together (1.439 ms) only just match it.

The two scale differently, which is what matters for coverage:

-   **classification scales with the tracked set** (9022 objects, ~160 ns each). It is paid for every
    object whether or not it is ever drawn - including the 6097 the exterior rejects every frame.
-   **`record` scales with the eligible set** (2885 objects). It is the cost of actually drawing.

So classification is the part that grows as coverage grows, and `record` is the part that grows as
coverage *succeeds*. Both need work; the priority between them was not what it looked like.

One caveat the numbers carry: timing a loop from inside it perturbs it. Profiled, the exterior reads
4.035 ms; unprofiled it is 2.855 ms. The instrument costs 41%, so the parts are upper bounds and only
their ranking should be trusted.

## Stage 1: dead and duplicate work

No caching yet - only removing work that was being done twice or for nobody.

-   **One pass lookup per object, not two.** The ordering pass called `FindAccumulatedPass` to split
    engine-kept objects from culled ones, then the loop called it again for the same object. The pointer
    is now carried in the ordering entry; nothing mutates the map while the loop runs.
-   **One pass map per frame, not three.** `CollectAccumulatedPasses` walked the batch renderers to fill
    `accumulatedPasses`, `CompareCapturedPasses` built a second map of the same size to compare against
    it, and then the first was discarded and refilled from the second. The capture now fills the table
    directly; the walk and the comparison run only under the new `CS_DCLF_PASS_PARITY=1`, or as the
    first-frame fallback. The batch-renderer set the capture hook needs is refreshed separately, since
    that part *is* needed every frame.
-   **`RefreshCategoryNodes` no longer runs every Present.** It rebuilt and diffed the whole category set
    on every frame, with a full linear scan over the tracked map whenever any node had gone, to detect
    something that changes only when a cell attaches or detaches. It now runs when a cheap signature
    changes, when a detach was drained this frame, or on a 30-frame backstop. The signature deliberately
    folds in everything the refresh reads to *find* nodes - the interior cell, the grid cells, each
    cell's loaded data, its cell3D and its child count - because missing a change means geometry attached
    under a new node is never tracked at all.
-   **Frame-globals hoisted out of the loop.** `Util::IsInterior()` was being evaluated per object inside
    `ExternalEmittance::ShouldSuppress`; that function gained an overload taking the answer, so the
    interior test happens once a frame. Same for the two feature-loaded flags.
-   **The derivation counters are computed only when they are reported.** The whole block, including a
    popcount loop, existed to feed one log line that only `CS_DCLF_STATS` prints, and ran per object per
    frame regardless.
-   **Two vectors stopped being reallocated.** The ordering vectors were locals, so a 9000-object
    exterior allocated and freed two ~140 KB buffers every frame; and the engine-kept count was taken in a
    second pass over the finished table rather than as the objects were built.

### One item was dropped, and the reason is the interesting part

The plan called for removing the duplicated `MakeShading` in `BuildFrame`, on the grounds that
`RefreshFrameConstants` overwrites it at Prepass. It does - but **the Z-prepass epoch runs between
them**, and under `DCLF_BINDLESS` it reads `MaterialData` out of the per-object record, which comes from
exactly that write. `Lighting.hlsl` uses `MaterialData.z` for the alpha test at lines 2833 and 2840,
*before* the `DCLF_DEPTH_ONLY` return at 2857. The same argument applies to the duplicated
`EvaluateGeometry`: `RefreshFrameConstants` deliberately keeps `BuildFrame`'s value for a pipeline whose
template has no lighting pass.

Neither is dead. They are the depth epoch's copy, and the plan's "carefully" was warranted.

### Result

| | Before | After |
| --- | --- | --- |
| Whiterun exterior | 2.855 ms | **2.479 ms** |
| Dragonsreach | 1.676 ms | **1.531 ms** |

**Gates:** `CS_DCLF_PASS_PARITY` reports 1127 compared, 0 missing, 0 extra, 0 technique differs, 0
subPass differs (with ownership off - with static ownership the walk sees only what is not withheld, so
921 show as "extra", which is the whole point of capturing at registration instead). Capture parity 0
mismatched over 276,300 draws. Object, geometry, pipeline and material counts identical across three
cells and two transitions; 0 claimed-but-undrawn; 0 VUIDs.

## Stage 2: why the depth pass is not owned, measured

Owning the depth pass would delete `SkipNativePass` and the per-draw `drawnFrame` map. The plan assumed
it was the same mechanism as the opaque pass with a different virtual. It is not, and a probe on
`RegisterPass` says so directly (`CS_DCLF_REGISTER_PROBE=1`, Dragonsreach, per frame):

| Shader type | Registrations | Into a main-camera batch renderer |
| --- | --- | --- |
| Lighting (6) | 1125 | **1125** |
| Utility (8) | 2396 | **3** |

`BSLightingShaderProperty::GetRenderDepthPass` builds its pass with **`BSUtilityShader`**, not the
lighting shader, and those passes go into batch renderers that are not the main camera's. Withholding
filters on `mainBatchRenderers`, so it never sees them.

Owning them means telling the main camera's depth renderer apart from the shadow cameras', which is most
of that 2396 - and withholding a shadow camera's pass removes the object from every shadow it casts.
Against that, the measured prize is ~921 hash lookups a frame, on the order of 0.05 ms. **Deferred, with
the evidence recorded rather than the assumption.** The probe stays behind its switch.

## Stage 3: the classification cache is correct, and buys almost nothing

The plan called this "the main event": in the Whiterun exterior 6097 of 9022 tracked objects run a full
classification every frame to produce "no", and are discarded. Caching that verdict should have removed
most of `classify-static`.

It did not.

| | ms |
| --- | --- |
| `classify-static`, no cache | 0.952 |
| `classify-static`, 5959 objects served from the cache | 0.847 |
| Whole table build, exterior | 2.479 -> 2.460 |

About 17 ns saved per cached object, against the ~160 ns the part costs per tracked object. The reason is
that the work being cached was cheaper than it looked: `netimmerse_cast` is a walk up a chain of RTTI
*pointers*, not a string comparison, and most negatives never get that far - `NotTriShape` and `Skinned`
are decided in the first three lines. What remains in `classify-static` is the witness read itself, which
has to happen for every tracked object either way.

### The part that matters more than the code

The negative cache helps objects that are **ineligible**. As coverage grows to new object classes,
objects move from ineligible to eligible - so this cache helps *less* after coverage grows, not more.
That is backwards from the goal it was written for.

What grows with coverage is the other two:

-   **`record`** (1.218 ms, the largest part) - the object record assembly, paid per *eligible* object.
-   the **positive** half of the derivation, which cannot be cached as it stands because its result comes
    from the engine's per-frame render pass.

So the work that makes per-frame cost fall as coverage rises is the record assembly and the positive
path, not the negative verdict. That is a finding about the plan, not about the code, and it is the
reason to record it here.

### What was kept, and why it is narrow

The cache is on `Tracked`, validated by four pointer witnesses (renderer data, shader property, material,
and a one-byte fade state) compared in full every frame. Only four verdicts are cached:
`NotTriShape`, `Skinned`, `NoRendererData` and `NotLightingShader` - the ones that follow from the
geometry's own shape, which is exactly what the pointer witnesses cover.

Everything decided later is deliberately excluded, because its inputs change behind an unchanged pointer:
`AlphaBlend` reads `materialAlpha`, which is animated; and the reasons `DeriveLightingDescriptors`
produces read property *flags*, which Community Shaders' own features set in place. Caching those would
have traded a real correctness surface for the cheapest third of the population. The first version did
cache them, and the restriction is the more defensible code even though it halves an already small win.

**Gate:** `CS_DCLF_CLASSIFY_CACHE=probe` serves the cached verdict *and* recomputes it, comparing the two.
571 recomputed and compared per frame, **0 differ**, across the Bannered Mare, the Whiterun exterior and
Dragonsreach with two transitions; table counts identical; 0 claimed-but-undrawn; 0 VUIDs.

### The fade metric, reduced to one byte

`FadeStateOf` is the reusable part of this stage. The LOD metric feeds the derivation in exactly three
ways - reject on `!isfinite`, clear `kSpecular` past its fade end, clear `kEnvMap` past its - so two bits
and an invalid marker capture the whole camera dependence of the classification. An object with none of
the fade-sensitive flags returns 0 without touching the fade node at all. The fade *floats* the metric
also produces are overwritten from the property by `RefreshFrameConstants` before any draw reads them, so
they are not part of the witness.

## Switches

The switches are defined, each with its kind and a one-line description, in the registry in
`src/Features/DrawcallLimitFix/Common/Switches.cpp`. [dclf-architecture.md](./dclf-architecture.md) ("Switches",
"Parity gates") says how they are read and which ones validate a change. Switches this document names that are not in
the registry were removed with the paths they chose between: DCLF is AE only and hybrid only, and a default-on feature
has no startup fallback.

Every feature is on by default, and test runs leave it that way: the startup log says `[DCLF] featureset: full`, or
names the switches that reduce it. The ones most runs use:

| Switch | Effect |
| --- | --- |
| `CS_DCLF=0` | DCLF does not install. The menu's toggle, by contrast, applies live. |
| `CS_DCLF_PARITY_BOTH=1` | Nothing is withheld from the engine, which draws everything while DCLF draws its set too: the capture parity configuration (it double-draws). |
| `CS_DCLF_CULL=off\|frustum` | Less GPU culling than the default (frustum and HZB occlusion). |
| `CS_DCLF_STATS=1` | The periodic report, with the GPU time of each render-graph segment and pass. |
| `CS_DCLF_*_PARITY` | The parity checks ([dclf-architecture.md](./dclf-architecture.md), "Parity gates"). |
| `CS_DCLF_TEST_MOVE`, `CS_DCLF_TEST_TURN`, `CS_DCLF_TEST_TOGGLE`, `CS_DCLF_TEST_COMMANDS` | The unattended test harness: a flight, turns, the live toggle, console commands at given frames. |

### Capture tools and GPU timing

- Every render-graph pass is a `VK_EXT_debug_utils` region named after the pass (`cs.dclf.main-opaque`,
  `cs.dclf.build-draws`, ...), opened before the pass's entry barriers, so a wait on the previous pass
  shows up inside the pass that needed it. Each epoch's submission is also a queue label named after its
  segment (`CS DCLF: main opaque`, ...).
- DCLF's draws are device-generated commands (`vkCmdExecuteGeneratedCommandsEXT` over an indirect
  execution set), not `vkCmdDrawIndexedIndirect`. Nsight does not time the generated draws as a draw
  of their own, and bills their GPU time to the barrier recorded just before them. The pass timestamps
  are the reference instead. Measured on the auto-loaded save: main opaque 3.08 ms in the draw pass
  against 0.006 ms in BuildDraws' dispatches; Z-prepass 0.49 ms of draws and 0.10 ms of HZB; shadow
  views 1.29 ms of draws. The `vkCmdDrawIndexedIndirect` calls outside every DCLF region are DXVK's translation of
  Grass Optimizations' D3D11 `DrawIndexedInstancedIndirect` calls.
- The profiling window lists every render-graph pass that did work, from the same timestamps, under its
  feature: `DrawcallLimitFix::<segment> / <pass>`, and `LightLimitFix::RenderGraphCull / <pass>`. No D3D11
  timer brackets an epoch: it would straddle the epoch's submission and count the queue's idle time too
  (see "GPU timing in the profiling window" in [render-graph.md](render-graph.md)).
- `CS_GPU_EVENT_TIMERS=<frames>` times every perf event on the GPU with D3D11 queries and logs, per interval, the
  events' mean inclusive and self times and the GPU frame time. It works with DCLF off too: the oldest frame's read may flush the context,
  since without DCLF's interop nothing else did and every frame was dropped unread.
- **Time only with the game in the foreground.** Community Shaders skips its upscaler while another process has
  the foreground (`Upscaling::IsWindowUnusable`), so an unattended run renders without DLSS and is 1.5-2 ms faster
  at 4K. A run's log has the `[Streamline] DLSS evaluate` line only if it upscaled; compare only such runs. With both
  in the foreground at Riverwood (2560x1440 internal, 4K output): GPU frame 12.2 ms with DCLF, 10.5 ms without.
  By segment, switching one back to native at a time: DCLF's sun shadows +0.5 ms, Skylighting +0.1 ms, the main
  view +1.1 ms (the colour epoch and what follows it +0.5 ms by `CS_GPU_IDLE_TRACE`'s submissions, the Z-prepass and
  the extra epoch boundaries the rest). Both configurations also idle about 1.8 ms a frame, before the last
  submission (upscaling), waiting for the render thread.
- **nvperf's range times favour DCLF.** The range profiler isolates each range, so the native pass's 47 small
  `RenderBatches` ranges each pay a drain that DCLF's few large ranges do not: native's ranges sum to about 0.5 ms more
  than its D3D11 timestamps. Compare whole frames (GPU event timers, the idle trace) before per-range numbers.
- **Background capture skews every frame-time measurement.** On this machine Discord's Clips (`discord_clips.exe`,
  NVENC at about 8%) captures the game at 30 fps: every ~33 ms, whatever the frame rate, one frame's GPU time is
  1-1.5 ms (10-15%) longer, spread over every pass in proportion - with DCLF on or off. Seen as a lag-3 (or lag-4,
  at higher frame rates) autocorrelation of the GPU frame time, confirmed with PresentMon's GPU busy times and
  `nvidia-smi pmon`'s encoder column. Turn it off before timing.
- RenderDoc (1.46) does not support `VK_EXT_descriptor_heap`. With RenderDoc capture on, the render graph
  cannot start and DCLF is forced off for the session; the log and the feature's menu page say why.

## Known upstream issues

Found while building DCLF. None is caused by DCLF; each is recorded here until it is fixed or ruled out.

| Where | Issue | Status |
| --- | --- | --- |
| DXVK fork (`extern/dxvk`) | Intermittent validation error `VUID-vkQueueSubmit2-semaphore-03868`: a binary semaphore in a submit's signal list is still signaled. 0 to 5 per run, in the swapchain/present path. It started in this branch's runs; DCLF issues no Vulkan calls. The newest `extern/dxvk` commits include a presenter revert, the suspected cause. | Open, not investigated |
| Community Shaders, Subsurface Scattering | `ExtraShaderDescriptors::IsBeastRace` is set or cleared only in the face/skin `SetupGeometry` path (`SubsurfaceScattering::BSLightingShader_SetupSkin`), but `Lighting.hlsl` writes `!IsBeastRace` into the G-buffer mask (`psout.Masks.y`) for every object. After a beast-race face draw, the static objects drawn next inherit the bit. DCLF uses the intended value (0), so once DCLF draws them, its output can differ from native in that mask channel. | Open; not seen in the coverage runs (no beast-race faces in view) |
| Community Shaders, permutation buffer | `IsSun` and `GrassSphereNormal` stay set in `ExtraShaderDescriptor` from the last sky/grass draw. The Lighting shader does not read them, so this is harmless today, but any shader that starts reading them inherits stale values. | Harmless for now |
| Community Shaders, TruePBR | Constant arrays passed to the shader were only partly initialized (e.g. `ParallaxOccData` zw). | Fixed (`TruePBR.cpp`, value-initialized) |
| Community Shaders, Effects11 | Logs `[E] Required effect file not found: enbseries\enbeffect.fx` on every start without ENB files. | Unrelated noise |
| Skyrim, `BSLightingShader::SetupTechnique` | Writes `VPOSOffset` after unmapping the PerTechnique buffer. It works on DXVK, where dynamic buffers stay mapped. On a native D3D11 driver the write is undefined. | Engine behaviour; DCLF reproduces the value |
| Skyrim, fog constants | `FogFarColor.w` is copied from uninitialized stack memory. | Engine behaviour; the shader does not read it |
| CommonLibSSE-NG | `BSBatchRenderer::renderPass` is declared as `BSTArray<PassGroup*>` but holds `PassGroup` structs inline. | DCLF works around it; not fixed in the library |
| Tooling | The CS deploy step (`*_plugin.stamp`) sometimes fails on its first run and succeeds on the next. Once (`dclf16`) the game recompiled all 3,473 shaders despite logging "Using disk cache"; the cause was not found. | Open, cosmetic |

## Open questions and assumptions (Phase 1)

-   **DoAlphaTest gained after the tables are built.** Resolved: the bit is not a property of the object
    (engine notes, batch renderer), and DCLF now draws every pass in an alpha-test list with it. See
    "Holes on alpha-tested objects" below.
-   **Parentless interior geometry.** About 30 draws per frame in Dragonsreach and Bleak Falls Barrow have
    no parent node; probably the portal graph's `alwaysRenderChildren` or its other object lists. Not
    tracked, so these stay native.
-   **Emissive drift.** `EmitColor` differs from the drawn value by up to about 1e-4 relative on emissive
    windows: the time-of-day emittance moves between the table build and the draw. Where it is updated
    was not traced. Parity compares it within 0.1%.
-   **Shadow mask size.** `VPOSOffset` uses the shadow mask render target's size. The engine uses globals
    at AE `0x14328be9c`. The two were equal at 1920×1080; not checked with a different mask size
    (`iShadowMaskQuarter`) or dynamic resolution.
-   **Early-Z global.** Whether DoAlphaTest is set for alpha-tested geometry depends on which offscreen
    renders ran before (engine notes). Phase 5 must decide how to derive it.
-   **Stand-in side effects.** Evaluation calls `SetupMaterial`/`SetupGeometry` through the vtable, so every
    Community Shaders hook runs. The engine shadow state and CS's permutation data are restored.
    Light Limit Fix's hook uploads its `StrictLightData` for the template pass. It records what it
    uploaded, so its upload cache stays consistent, and the next native draw overwrites it. A hook added
    later that keeps other global state would need the same review.
-   **Template pass for geometry constants.** Per-pipeline `PerGeometry` constants are evaluated with a
    copy of any object's lighting pass (for the sun light). This is valid because everything per object
    is replaced afterwards (engine notes, `SetupGeometry`: what is per object); parity confirms it for the
    main pass only.
-   **Actors.** Resolved: geometry under an actor is eligible with `CS_DCLF_ACTORS` (default on), and the gates hold (see "Trees and actors").
-   **Unwritten constants.** Components the engine never writes are uploaded as zero in Phase 2. The
    engine leaves them undefined, and the permutations do not read them.
-   **SE 1.5.97 and VR.** Only AE 1.6.1170 was tested. DCLF uses two raw offsets, into
    `BSBatchRenderer`'s `renderPassMap` (`+0x2C`, `+0x48`); their SE layout is assumed identical. VR is
    disabled.

## Still open in Phase 1

-   Parentless geometry in interiors (about 30 draws per frame in Dragonsreach and Bleak Falls Barrow)
    stays native; probably the portal graph's always-render lists.
-   The alpha-test-state objects (engine notes, batch renderer open question) stay native.
-   **Cost:** see the results table; tracking adds about 0.05 ms. In Whiterun, 0.54 ms of the 1.05 ms is
    classification and record building (about 1,450 accumulated tracked geometries) and 0.38 ms is the
    stand-in evaluation of 295 materials. Caching materials across frames is the obvious next saving; it
    needs a reference on the material so that a reused address cannot hit a stale entry.
-   Phase 5 cuts the native loop, so the accumulator walk will no longer supply the per-frame technique
    bits. They then have to be derived: the light and shadow assignment (`FUN_1414fcf80`) and the early-Z
    global.

## Stage 4: the per-object loop stops repeating itself

Stage 0 named `record` the largest part of `BuildFrame`. Splitting it showed the label was wrong in the
same way Stage 0's had been. `timer.Add(BuildPart::Dedup)` fired only inside the miss branches of the
three dedup maps, so on the hit path — nearly every object — three hash probes fell through to the next
iteration and were billed to `record`. Whiterun exterior, 3057 objects:

| part | ms |
| --- | --- |
| `record` (the real assembly) | 0.615 |
| `dedup-hit` (the three probes) | 0.396 |
| `loop-tail` (per *tracked* object) | 0.336 |

Half of what `record` reported was not record assembly. The named work was nearly free in that cell
anyway: `GetRoomIndex` early-outs on `roomNodes.empty()` outdoors and `ShouldSuppress` short-circuits on
`!interior`.

**Container churn.** The three dedup maps were locals — three hash maps allocated and freed every frame
to hold the same contents, the waste Stage 1 removed from the ordering vectors and left here. They are
members now, and `objectIndex`, `shading`, `emissiveMult` and `lights` are reserved like the other
per-object containers. The two ordering passes became one: `order` and `culled` were built separately
and then concatenated, up to a ~6000-entry memcpy a frame, purely to guarantee that an engine-kept
object reached a pipeline first.

**The pipeline template is elected, not ordered.** That guarantee is now stated as a rule about the
objects: if a later native-visible object finds a pipeline whose lighting template came from a culled
one, it takes the template over. This survives a table that outlives the frame, which ordering cannot.
The first gate statistic counted *pipelines with a culled template* and read 12 of 31 under
`CS_DCLF_CULL_INPUT=tracked` — a false alarm, because a pipeline drawn only by culled candidates has no
native-visible object to elect and harms nothing. The invariant that matters is the implication, checked
over the finished tables rather than asserted from the election: **if a native-visible object draws on
pipeline p, then `geometryTemplate[p]` came from a native-visible object.** Measured with the election
doing real work — 10 takeovers a frame — it holds at 0, and Dragonsreach lighting is unchanged.

### The material cache, and why the measurement lied twice

The plan called for promoting `materialProbe`, "a 99% hit rate that only needs promoting". It was not.

The probe compared records with `memcmp` over a struct whose members total 2340 bytes while the struct
is 16-aligned, so it was comparing **12 bytes of uninitialised trailing padding**; every comparison came
out unequal. With member-wise equality it still read 104 changed, 0 unchanged — but only the PS block
moved, at three adjacent floats holding the **same value for every material in the frame** (min == max
across all 104) and drifting with the time of day.

`BSLightingShader::SetupMaterial` explains it. In Ghidra it is `BSLightingShader::Func4` at `1414dc310`
— vtable slot 4, named by slot rather than by name. Its tail reads:

```c
if (*(char *)(this + 0xf0) == '\0') { lo = *(this + 0xe0); hi = *(this + 0xe8); }
else                                { lo = *(this + 0xd0); hi = *(this + 0xd8); }
offset = *(byte *)(currentPS + 0x5d);   /* PS constant table[29] */
```

PS PerMaterial **variable 29 is IBLParams** (`ShaderCache.h:50`), and SetupMaterial does not read it
from the material at all: it comes from fields of the BSLightingShader object, selected by a day/night
flag at `this+0xf0`. It is shader-level frame state that happens to live in the per-material group.

> **Superseded** by "Material records from their sources": the frame-global positions are now a fixed
> list from the decompiled SetupMaterial, not learned, and the validator only reports.

So the cache serves the record and patches those positions from a live evaluation. **The positions are
cumulative for the session**, which a failure taught: learning them each frame as "floats that differ
from the cached copy" works only while the value drifts continuously. `set gamehour to 22` steps it once
and then freezes it — the next frame saw no difference, learned an empty set, patched nothing, and
served every material its pre-step value. Capture parity failed on **220,500 of 276,300 draws**, with
IBLParams.y reading 0.757 against a native 0.106.

The values are sampled again at Prepass (`RefreshMaterialPatch`), for the same reason and in the same
place the animated per-object shading is resampled: sampled only at EarlyPrepass they sit a fraction of
a frame behind what the native draws read, visible as IBLParams differing in the sixth decimal across a
fast lighting transition.

The result is that the cache is parity-*better* than evaluating every material, because it fixes a
pre-existing mismatch the uncached path has. Mismatched draws per report interval, same route:

| interval | cache off | cache on |
| --- | --- | --- |
| cell change (`coc`) | 253239 | 20572 |
| `set gamehour` step | 248670 | **0** |
| steady state | 0 | **0** |

104 evaluations a frame became **9** — one to learn the patch, eight rolling validations — with 103
served. `material-eval` went 0.443 → 0.106 ms in the exterior and 0.045 in Dragonsreach. It defaults on.

**Two instrument defects, both of the same kind.** The validator's cursor reset every frame, so with a
stable iteration order it re-checked the same eight materials for ever and reported "0 stale" while
capture parity was failing on 80% of draws. And `probe` forced the evaluate path, so it measured a
configuration with no cache in it. Both now follow Stage 3's shape: serve the cached value, recompute
it, compare, and use the served one — a probe that does not exercise the thing it is probing proves
nothing.

### Buffers resolve once per TriShape

Both `GpuResources::Resolve` results were only ever read inside the `newGeometry` branch; for an object
whose geometry was already in the table they were computed and discarded. With ~3.8 objects per TriShape
in the exterior that is most of the calls, and in steady state a Resolve is a hash probe
(`DescribeResource` runs only on first insert).

The call was **moved**, not cached, and the distinction is the safety argument: GpuResources holds a
reference on each buffer so its address cannot be reused, and it drops that reference when an entry goes
`kEvictFrames` without a Resolve. Resolving per TriShape still touches every entry the tables depend on
every frame. Skipping the call outright would have let the eviction sweep drop the reference underneath
live draw arguments, which is the device-loss hazard `BuildFrame` already documents.

`resolve` went 0.326 → 0.176 ms in the exterior, 0.110 → 0.072 in Dragonsreach.

### The property derivation is dead work for an accumulated object

For an object the accumulator holds, `DeriveLightingDescriptors`' derivation block has exactly one
surviving effect: `a_out.derivedPass`. The pass descriptor comes from `a_accumulated->technique`; the
flag edits the block makes to `f` are never read afterwards; the two LOD fades it computes are
overwritten from the property two lines later; and its return value is not consulted to reject the
object. Only the derivation diagnostic reads `derivedPass`.

So for those objects the fade metric, `SelectLightingTechnique`, ten flag tests and a virtual
`GetFeature()` call were being run and thrown away. They are skipped now unless the diagnostic asks for
them, which also moved that diagnostic from `CS_DCLF_STATS` to its own `CS_DCLF_DERIVE_PROBE`: gating it
on the stats switch meant every reporting run measured a configuration nobody ships. The report line is
gated too, because printed unconditionally it read "0 objects compared, 0 differ", which scans as a
passing check rather than one that never ran.

The saving is smaller than it looks — `classify-static` 0.363 → 0.338 ms in Dragonsreach, and nothing
measurable in the Whiterun exterior. The reason is worth recording: in the exterior most candidates are
*not* accumulated (3057 objects, ~915 engine-kept), and for those the derivation **is** the pass
descriptor, so it has to run. The work removed scales with what the engine kept, not with what DCLF
tracks.

What remains in `classify-static` is not computation. A descriptor memo keyed on the pass descriptor was
tried in an earlier stage and measured neutral, and `ModifyShaderLookup` is pure bit manipulation on that
descriptor. The remaining ~300 ns per call is dominated by touching the geometry, property, material,
alpha property and fade node — five separate allocations, each a likely cache miss. Removing it means
not touching them at all, which is a *positive* verdict cache, not a cheaper derivation.

### The RTTI cast is resolved once per property, and that is where classify-static stops

`netimmerse_cast` is cheap in instructions — it walks a chain of RTTI *pointers*, not strings — but every
step is a dependent load into a different allocation. At ~3000 classifications a frame it was the one
part of `ClassifyStatic` that was neither computation a memo could remove nor memory `BuildFrame`
re-reads later anyway. Whether a property is a `BSLightingShaderProperty` follows from its type, so the
property pointer is a complete witness, and `Tracked` now remembers both the pointer and the result
(null included, which is what makes a negative trustworthy).

**RTTI casts walked per frame: 0.** `classify-static` 1.011 → 0.945 ms in the exterior, 0.363 → 0.312 in
Dragonsreach, with the classification probe still at 0 differ and capture parity clean.

That is ~11%, and it is the end of the road for this part. With the casts at zero and the derivation
skipped for accumulated objects, `classify-static` is still 0.945 ms over ~3060 calls — about 310 ns
each — and what remains is memory: the geometry runtime data, the property, the material, the alpha
property and the fade node, five allocations touched per object. A *positive* verdict cache cannot
recover much of it, because its own witnesses must read the geometry, the property and the fade node,
and `BuildFrame` re-reads the alpha property and the material downstream for the two-sided flag, the
alpha test and the material key. What is left is the pointer chasing the data layout implies, not work
that can be cached away.


### Result

**Dragonsreach 1.531 → ~1.31 ms** a frame, unprofiled, over ten steady-state intervals. Gates: capture
parity 0 mismatched over 276,300 draws an interval, classification cache 0 differ, pipeline templates 0
visible objects on a culled template, material cache 0 stale, 0 claimed-but-undrawn, and no VUIDs beyond
the upstream semaphore one.

**Still open.** Caching the *table slots* on `Tracked` — the remaining `dedup-hit`, 0.373 ms — needs the
tables to persist first, because a slot is only worth caching if it is stable. The plan had that
dependency the other way round.

## Stage 5, attribution: the epoch, and a switch that was off

Splitting `BuildFrame` twice had shown the same failure both times — a bucket named after one thing that
mostly contained another. The epoch's four parts had never been split at all, and "rest" was a
subtraction. Splitting it found two things, the second of which matters more than any optimisation in
this document.

### The parts are per record, not per draw

`mark(0..2)` all sit inside `if (!dedup || recordIndex == kNoRecord || dedupParity)`. Since Step D
deduplicated the binding record to one per (material, pipeline) pair, that block runs ~105 times an
epoch rather than ~1500. So "textures/samplers", "constant groups" and "binding record" were per
*record*, while the per-*draw* work — the loop prologue with its `resolvedBindings` probe, and the tail
that builds the sequence and the draw input — was invisible inside "rest", together with the uploads and
the graph execution.

### Step D's deduplication was never switched on

The split's first measurement read `1538 candidates built from 1538 binding records`. `dedup` is
`BindlessDraws()`, which requires `CS_DCLF_BINDLESS_DRAW`, and that defaults off — so **every
measurement taken since Step D has had its deduplication disabled**, and "constant groups 0.715 ms" was
~465 ns of constant packing per draw rather than per pair.

With the switch on, in Dragonsreach, 1521 candidates collapse to **105 binding records**:

| part | dedup off | dedup on |
| --- | --- | --- |
| textures/samplers | 0.131 | 0.047 |
| constant groups | 0.715 | 0.162 |
| record push | 0.160 | 0.060 |
| per-draw tail | 0.117 | 0.102 |
| per-draw prologue | 0.188 | 0.118 |
| epoch prologue | — | 0.052 |
| graph execute | — | ~0.61 |
| **epoch total** | **~1.98** | **~1.15** |

That is ~1.6 ms a frame across the two epochs, from a feature already written, tested and gated. Its
gates hold: record dedup parity 4.8M rebuilt records matching their pair byte for byte, bindless record
parity 204M components matching the constant groups, no mismatches, no VUIDs beyond the upstream
semaphore one, and Dragonsreach renders identically.

It is **not** defaulted on here, because the standing debt lists a live test matrix — kill cams, the map
menu, first person, water reflections, Dynamic Cubemaps, save and load — that has to be worked through
before any switch flips, and `CS_DCLF_BINDLESS_DRAW` changes the shader build as well as the CPU path.

### What is left is not per-object

Fully split, with dedup on:

| part | ms | scales with |
| --- | --- | --- |
| **graph execute** | **~0.61** | **nothing — fixed per epoch** |
| constant groups | 0.162 | records |
| per-draw prologue | 0.118 | candidates |
| per-draw tail | 0.102 | candidates |
| record push | 0.060 | records |
| textures/samplers | 0.047 | records |
| epoch prologue | 0.052 | nothing |
| uploads | 0.000 | deferred to the graph's upload pass |

Half the epoch is the graph execution and the `PassFrame` build — fixed cost, independent of object
count, and paid twice a frame. The genuinely per-candidate part is the prologue plus the tail: about
**145 ns per candidate per epoch**, not the ~1.2 us a division of the old total implied.

## Stage 5: the transform witness cannot pay for itself

The plan's Stage 5 rested on one idea: most of a Skyrim cell never moves, so a 48-byte transform compare
should skip the record write and the upload for most objects. Stable object slots — a free list, one
array indexed by slot, per-slot frame stamps, every table consumer changed — existed mainly to make that
possible.

Three measurements say it does not work, and none of them needed the slots built.

**The transform stores are 13 ns an object.** `CS_DCLF_TRANSFORM_PROBE=skip` removes both
`StoreTransform` calls outright — it renders wrong, and exists only to bound the cost. In Dragonsreach,
1962 objects: `record` 0.413 → 0.387 ms. That 0.026 ms is the *ceiling* on anything a transform cache
could save. A witness would read two 52-byte `NiTransform`s and copy 48 bytes to avoid 18 multiplies, so
it adds more memory traffic than the arithmetic it removes. It would be a net loss.

**The record build is 26 ns an object.** `BuildObjectRecord` runs for every object before the epoch loop,
and that whole prologue measures 0.052 ms for 1962 objects.

**The upload is already free on the CPU.** `BUFFER_UPLOAD` reaches `UploadService::UploadData`, which
records the pointer and size; the copy happens in the graph's upload pass. The measured "uploads" part is
0.000 ms. The copy is real, but it lands inside `graph execute`, and turning the bindless object table
on and off — 282 KB an epoch — moves `graph execute` by less than run-to-run noise.

So the three things Stage 5 was going to avoid cost 13 ns, 26 ns and approximately nothing. **Stable
object slots are not worth building for this.** They may still be worth building for a different reason —
they would remove the `objectIndex` insert, which `dedup-hit` measures at ~119 ns a draw — but that is a
different argument from the one in the plan, and it should be made on its own evidence.

## The live test matrix for CS_DCLF_BINDLESS_DRAW

Driven through `CS_DCLF_TEST_COMMANDS`, with record-dedup and BuildDraws parity on throughout.

| case | how | result |
| --- | --- | --- |
| interiors with rooms and portals | `coc WhiterunDragonsreach`, `coc WhiterunBanneredMare` | clean |
| exteriors | Whiterun, `coc Riverwood` | clean |
| cell transitions | six, across both runs | clean |
| worldspace change (fast-travel equivalent) | `coc Riverwood`, `coc Whiterun` | clean |
| night lighting / emissives | `set gamehour to 22` | clean |
| dungeon, alpha-tested foliage, god rays | `coc BleakFallsBarrow01` | clean |
| save | `save dclfm2` | clean |
| **load** | `load dclfm2` | clean — the path that frees renderer data |

Across both runs: **0 mismatches**, 21/21 BuildDraws parity OK, 20/20 record dedup parity OK, 0 rejected
buffers, and no VUIDs beyond the upstream semaphore one. Images correct in Dragonsreach, Bleak Falls
Barrow and Riverwood.

**Not covered, because they need input the console cannot drive:** kill cams, the map menu, first
person, Screenshot and Remote Control. Dynamic Cubemaps is not installed in this profile, so it was not
exercised either. The switch is therefore left **off by default** — the automatable part of the matrix is
clean, but the matrix is not complete, and that last step is a human one.

## CS_DCLF_BINDLESS_DRAW defaults on

Validated over the live test matrix and enabled by default, together with `CS_DCLF_BINDLESS` that it
depends on. Both are disabled with `=0`, and `=0` is the control every pre-Step-D measurement in this
document was taken with.

With no switches set, Dragonsreach now builds **85 binding records** from ~1500 candidates and the epoch
costs ~1.02 ms against ~1.98 before. Gates on the default path: 11/11 BuildDraws parity, 11/11 record
dedup parity, 0 mismatches, no VUIDs beyond the upstream semaphore one. The map menu and the
first-person camera were checked by hand; the console-driven part of the matrix (interiors with rooms
and portals, exteriors, six cell transitions, a worldspace change, night lighting, a dungeon with
alpha-tested foliage, save and load) was clean.

## Bringing in a new object class: trees

### Which class, decided by histogram

`Ineligible::Technique` counted 847-1337 objects in the Whiterun exterior with no indication of what was
in it. Recording the rejecting technique gives:

| cell | rejected |
| --- | --- |
| Whiterun exterior | **treeAnim(12) = 1237**, MTLandLODBlend(19) = 96, MTLand(8) = 4 |
| Dragonsreach | refraction(63) = 5 |

One technique is 92% of the class. That is the one to bring in.

**The first version of this histogram read `none(0)=1337` and meant nothing.** `ClassifyStatic` copies
its descriptors to the caller only when the verdict is `None`, so on a rejection the caller kept a
default-initialised `rejectedTechnique` — and 0 is `none`, a *supported* technique. An empty field that
reads as a plausible value is the same failure as the material probe comparing padding and the validator
re-checking a fixed sample. The default is now 62, which cannot be mistaken for a real answer.

### What trees need, measured rather than guessed

`CS_DCLF_TREES=1` adds technique 12 to the supported set. Coverage in the Whiterun exterior goes from
~2900 to **3863 objects**, and `treeAnim` leaves the rejection histogram.

Capture parity then fails, identically with bindless on and off, so it is the trees and not the newly
defaulted switch. The sample list could not say why — it is capped, and the standing `PS PerMaterial 29`
difference filled it, so 83,396 per-geometry mismatches produced not one sample. Counting mismatches
**per variable** instead names them at once:

| variable | mismatches |
| --- | --- |
| **VS PerGeometry 5 — WindTimers** | 83,390 |
| **VS PerGeometry 4 — TreeParams** | 47,851 |
| PS PerMaterial 29 — IBLParams (standing) | 57,300 |

Declaration order in `Lighting.hlsl`'s VS PerGeometry block is World, PreviousWorld, EyePosition,
LandBlendParams, **TreeParams**, **WindTimers**, so 4 and 5 are exactly the two constants tree animation
displaces its vertices with.

This is the defect the switch was put there to catch, and it is the same shape as the culled lighting
template: **DCLF's PerGeometry constants are per PIPELINE**, taken from one template object, while
TreeParams is a property of the individual tree and WindTimers advances during the frame. One template
tree's wind is handed to every other tree sharing its pipeline.

### What finishing it requires

TreeParams and WindTimers have to become per-object values carried in the object record, exactly as Step
C did for World, PreviousWorld and MaterialData: add them to `BindlessObject`, read them per object in
`BuildFrame`, and take them from the record in the shader under `DCLF_BINDLESS_DRAW` (and through
`PatchObjectGeometry` otherwise). Where the engine computes TreeParams for a given tree still has to be
established — `BSLightingShader::SetupGeometry` is the place to read it from, the same way IBLParams was
traced to the shader object. `CS_DCLF_TREES` stays off until that lands.

### Where the engine gets TreeParams and WindTimers

`BSLightingShader::SetupGeometry` is vtable slot 6 — `BSLightingShader::Func6` at `1414dd040`. Its
`case 0xc` is technique 12, TreeAnim. The VS constant table base is `0x50` (the PS one is `0x40`, which
`SetupMaterial`'s `0x5d` for variable 29 confirmed), so `0x54` is variable 4, TreeParams, and `0x55` is
variable 5, WindTimers.

The data comes from a **`BSTreeNode`**, reached from the shader property:

```
property->fadeNode                     // BSShaderProperty + 0x60
    ->vfunc[0x1f8 / 8 = 63]()          // downcast; null for anything that is not a tree
```

`BSTreeNode` is a real engine class — it registers by that name alongside `BSFadeNode` in the class
table at `FUN_14147e2c0`. Its wind state:

| offset | meaning |
| --- | --- |
| `+0x158` | squared distance (the code takes its square root with the `0x5f3759df` fast inverse square root and one Newton step) |
| `+0x15c` | maximum amplitude |
| `+0x160` | leaf frequency |
| `+0x164` | wind timer |
| `+0x168` | previous wind timer |

and the derivation, with `node` null giving `distance = 0` and `maxAmplitude = 1`:

```
TreeParams.x = 0
TreeParams.y = *(0x142033060 + 0x304)                       // global wind magnitude
TreeParams.z = clamp((1 - (sqrt(node[0x158]) - A) / (B - A)) * maxAmplitude, 0, maxAmplitude)
TreeParams.w = node ? node[0x160] : 1
WindTimers.x = node ? node[0x164] * K : 0
WindTimers.y = node ? node[0x168] * K : 0
```

`A` = `DAT_142033100` and `B` = `DAT_142033104` — a distance fade, written by one function and read by
`BSUtilityShader::SetupGeometry` as well. `K` = `DAT_141ad28bc`, a timer scale.

**SetupGeometry mutates the node.** Its last act in this case is

```c
*(undefined4 *)(lVar22 + 0x168) = *(undefined4 *)(lVar22 + 0x164);   // previousWindTimer = windTimer
```

so the engine advances each tree's animation state as a side effect of setting up its draw. Two
consequences. A per-object *stand-in* evaluation — calling the engine's SetupGeometry once per tree the
way `EvaluateMaterial` does for materials — would advance every tree twice a frame and is therefore not
an option here. And whenever DCLF does own the tree pass outright, it has to perform that write itself,
exactly once, or tree motion stops.

The remaining work is bounded: carry TreeParams and WindTimers per object (a parallel array plus two
more `GeometryPatchOffsets` entries is enough for the non-bindless path, and needs no shader change),
derive them as above, and let capture parity's new per-variable histogram confirm it — a wrong constant
shows up immediately as `VS PerGeometry 4` or `5`, which is what makes replicating engine arithmetic
safe to attempt at all. Only then do the two values need adding to `BindlessObject` and the shader.

### Tree wind on the GPU (2026-09-30)

DCLF's members take their tree wind from the GPU (`TreeWindCS.hlsl`). Nothing reads the tree nodes per frame; the
accumulate phase's per-frame `DeriveTreeAnim` over every resident tree (`KeepResidentsAlive`) is gone.

**The engine's update, reverse-engineered** (headless Ghidra; `FUN_140437d40` from `Main::Update`):
-   **Callers and inputs.** The manager is `BSTreeManager` (pointer at `0x1420F6A18`). It runs from `Main::Update` with
    the frame's seconds (`0x1431CC288`, the global `GetSecondsPassed` reads). The update is time-budgeted (it
    breaks when a QPC budget runs out, checked every 4 trees) and runs under the manager's mutexes.
-   **Loop 1** (`FUN_1404381e0`, list `+0x50`) covers each node with `kAccumulated` and a model (`+0xF8`, its
    `+0x40`):
    -   the timer advances: `+0x164 += dt`;
    -   the squared camera distance: `+0x158 = |world translate (+0xA0) - camera|²`;
    -   the gust, when `+0x158 <` the manager's `+0x80`:
        `+0x15C = FUN_140438950(manager+0x78 * +0x164, 3) * model+0xB0`.
-   **Loop 2** (list `+0x38`) covers the nearest trees (`FUN_140437e50`: within `+0x80`, capped and sorted), for
    those whose model has bones (`+0xB8`). They get the same timer and gust, plus the CPU bone animation
    (`FUN_140438ed0`) and a world update.
-   **The gust:** `FUN_140438950(x, 3)` = 0.25 × the sum of sin(kπx) over k = 1, 3, 5, 7. It is SIMD: the argument
    is reduced to [-π, π] with the 2^23 rounding trick, the sine is an odd polynomial (constants at `0x141846E64`),
    and the lanes are summed as (l3 + l2) + (l1 + l0).
-   **The draw:** `SetupGeometry`'s case 0xc then reads the node as before (TreeParams, WindTimers) and keeps the
    timer as the previous one (`+0x168`).

**On the GPU:**
-   **Rows.** `Tables::trees` holds one `TreeStatic` row per tree node a member draws under (`ListTree` when the
    member joins, `UnlistTree` when it leaves; refcounted by members). The row carries the node's position, leaf
    frequency, model scale and its clock at listing. `Tables::treeObjects` lists the members and their tree slots.
    The commits upload both when their versions move (`UploadTrees`).
-   **Clocks.** `TreeWindCS` mode 0 keeps one `TreeClock` per slot, the GPU's alone. It starts from the static row
    when the generation is new, then advances once a frame, whichever epoch runs first.
-   **Records.** Mode 1 writes each member's TreeParams and WindTimers into its object record (the objects buffer
    takes unordered access), as SetupGeometry would. That includes the engine's fast square root, bit for bit.
-   **Order.** Both modes run first in the shadow, Z-prepass and colour epochs, after the uploads, which carry a
    record as it joined.

**Deliberate difference: the clock always advances.** The engine advances a node only while it is visible
(`kAccumulated`, which the feedback still keeps on visible stood-in roots, since a root can have parts the engine
draws) and within its time budget. The GPU advances every member tree every frame.
-   **Measured** (persistent parity's clock-rate histogram): the engine's clock ran at 0 to 1 times the GPU's, by
    the fraction of the interval the tree was in view. The differences are therefore only in the phase of trees out
    of view.
-   **Not replicated:** loop 2's second `dt` for near trees with bones. No such tree was in the test scene to
    validate it.

**Check** (`CS_DCLF_PERSISTENT_PARITY`, every 120th colour epoch, 64 members read back): TreeParams.y and .w exact
against the frame's wind magnitude and the leaf frequency, the fade exact against the record's own distance, the
gust against `FUN_140438950` at the record's timer, and the timers' drift from the engine's own clocks reported.
Results: 0 differences in steady state, holes 0, all parity OK; tree changes in the change log went from 84 a frame
to ~1.

### Trees, landed

`CS_DCLF_TREES=1` brings technique 12 into coverage. The Whiterun exterior goes from ~2900 to **3862
objects**, and `treeAnim` leaves the rejection histogram entirely.

TreeParams and WindTimers are carried per object: a `tables.treeAnim` array parallel to `objects`, two
more `GeometryPatchOffsets` entries for the constant-buffer path, and an `ObjectTreeAnim` at the end of
`BindlessObject` (144 -> 176 bytes) with matching `float4`s in the GPU record, since under bindless the
PerGeometry block is one pair for the whole pipeline. `windTimers` is a `float4` using only `xy` so the
two layouts cannot disagree about padding.

Two defects on the way, both caught by the per-variable histogram within one run each.

**The first version wrote zeroes over every object in the frame.** `tables.treeAnim` is filled for all
objects and `PatchObjectGeometry` wrote it unconditionally, so every *non*-tree lost the TreeParams and
WindTimers its pipeline template had supplied. Parity went from 47,851/83,390 mismatches to
126,942/124,505 — worse, and the samples read `DCLF 0, native 1`, which is what a clobber looks like.
Writing them only under a new `kObjectTreeAnim` object flag took WindTimers to **0** and TreeParams to
**600**.

**The last 600 were the square root.** The engine does not call `sqrtf`: it puts the squared distance
through the `0x5f3759df` fast inverse square root with one Newton step and multiplies back by x.
Substituting `std::sqrt` agreed for every tree that clamps at one end of the fade band and disagreed for
exactly the population inside it — 600 draws a frame, 0.7%. Reproducing the approximation bit for bit
closes it.

**Gates.** Constant-buffer path: capture parity **0 per-geometry mismatches** across four intervals,
against a no-trees control on the same build with identical material numbers (57,300 + ~3,000, both
pre-existing). Bindless path: **443M bindless record components matching**, 20/20 record dedup parity,
21/21 BuildDraws parity, 0 mismatches, no new VUIDs, and trees render correctly.

`CS_DCLF_TREES` is left **off by default**: the gates are clean, but it is a large coverage change and
the project's practice is that a switch defaults on only after someone has looked at it moving.

### A correction to earlier measurements in this document

Several "capture parity 0 mismatched" readings above were taken from the *tail* of a run's log, which is
always the cell the run `coc`s into — Dragonsreach. The Whiterun exterior intervals of those same runs
were not clean: 22,228 material mismatches, all `PS PerMaterial 29` (IBLParams).

This is not caused by the material cache, and the cache is not a regression — with the cache **off** the
same exterior reads **297,589**, five times worse, because nothing then resamples IBLParams at Prepass.
The residue is the sub-frame lag already described: outdoors the value moves far more within a frame
than it does inside Dragonsreach, and one sample at Prepass no longer covers it. `VS PerMaterial 11`
(TexcoordOffset, animated per material) contributes a further ~3,000 and is genuinely uncacheable.

The lesson is the same one the instruments keep teaching: **read every interval, not the tail.** A run
that visits two cells reports two different answers.

## BSEffectShader: investigated, and not worth doing

`not-lighting-shader` is the largest rejection class by tracked count — 3057 in the Whiterun exterior
after trees — so it looked like the obvious next coverage target, and the assumption was that it meant
repeating Steps A-D for a second shader. Three measurements say otherwise, and together they redirect
the work.

**What the class actually is.** Recording the rejected property's RTTI rather than just counting:

| cell | property types |
| --- | --- |
| Whiterun exterior | BSEffectShaderProperty 2701, *no property at all* 343, BSWaterShaderProperty 13 |
| Dragonsreach | BSEffectShaderProperty 235, *no property* 1 |

343 of them have no shader property, so nothing could ever draw them.

**Over half is alpha blended.** 1717 blended against 1340 opaque outdoors, and 208 against 28 inside.
Blended geometry is not drawn in the pass DCLF owns: it goes after the deferred composite, forward and
sorted back to front, while the epoch writes the G-buffer with depth test EQUAL before the composite.
Covering it would mean a **new epoch**, not another shader in the existing one.

**And almost none of it is drawn in the main pass anyway.** The `RegisterPass` probe, counted per report
interval (~300 frames) and split by whether the batch renderer belongs to the main camera:

| shader type | into a main renderer / total | per frame |
| --- | --- | --- |
| **Lighting (6)** | 603,974 / 618,674 | **~2013** |
| **Effect (7)** | 3,900 / 3,900 | **~13** |
| Grass (1) | 3,300 / 6,600 | ~11 |
| Water (3) | 11,700 / 11,700 | ~39 |
| Utility (8) | 0 / 1,537,933 | 0 |

**BSEffectShader registers about thirteen passes a frame.** The 2701 tracked effect objects are
overwhelmingly magic and environment effects that are not currently drawing. A second shader pipeline —
its own stand-in evaluation, constant layout, permutation compilation, pass-capture filter and record
shape — would buy thirteen draws.

### Where the remaining coverage actually is

DCLF claims 1464 of the ~2013 Lighting passes a frame. **The gap is ~549 draws a frame, and all of it is
inside the Lighting shader**, where every piece of machinery already exists. Tracked counts for what is
still rejected in the Whiterun exterior, trees on:

| reason | tracked | what it needs |
| --- | --- | --- |
| projected-uv | 860 | the ProjectedUV constants, per object |
| skinned | 633 | the bone palette in the record |
| unsupported-parent | 630 | scene-graph shapes the tracker declines to follow |
| decal | 601 | blending and depth handling |
| not-trishape | 274 | dynamic tri shapes |
| technique (MTLand, MTLandLODBlend) | 100 | terrain, the per-object constant question trees had |

Trees were worth doing because 1237 tracked objects turned into real draws. These are the classes to
rank next, by how many of their tracked objects the engine actually draws — a number the registration
probe can give per class, and the one that should decide the order rather than the tracked count that
made BSEffectShader look like the prize.

## Ranking the remaining coverage by what the engine actually draws

Tracked count is the wrong measure, and ranking by it is what made BSEffectShader look like the prize.
The right one is: of the objects DCLF rejects, how many did the engine itself register a main-pass
lighting draw for this frame? That is `accumulated != nullptr` at the point of rejection, so it costs a
counter and no new machinery.

Whiterun exterior, trees on, per frame:

| reason | tracked | **drawn** | of tracked |
| --- | --- | --- | --- |
| **decal** | 601 | **181** | 30% |
| **skinned** | 633 | **150** | 24% |
| projected-uv | 860 | 53 | 6% |
| not-trishape | 274 | 44 | 16% |
| technique (MTLand, MTLandLODBlend) | 100 | 33 | 33% |
| actor | 27 | 21 | 78% |
| unsupported-parent | 630 | 7 | 1% |
| **not-lighting-shader** | **3057** | **0** | **0%** |

Dragonsreach agrees: decal 88 -> 35, skinned 52 -> 30, not-trishape 52 -> 26, not-lighting-shader
51 -> **0**.

The order is almost the reverse of the tracked counts. `projected-uv` led on tracked objects (860) and
is sixth on drawn ones; `unsupported-parent` has 630 tracked and draws seven. And
**not-lighting-shader draws nothing at all** — stronger than the ~13 a frame the registration probe
suggested, and a complete answer on BSEffectShader.

Everything still rejected totals ~490 drawn a frame, against the 1464 DCLF claims.

### Feasibility, which does not follow the same order

DCLF's pipeline expresses exactly one piece of fixed-function state, `kRasterTwoSided`; depth test EQUAL
with no write, and blending and depth bias off, are assumptions baked into every pipeline it builds. The
VS layout gives Bones no real storage. So:

-   **decal (181)** needs depth bias and blending in the `PipelineKey`, and an ordering guarantee
    against the base geometry underneath. Structural.
-   **skinned (150)** needs a bone palette per object — a variable-length array, unlike every per-object
    value the record carries today. Structural.
-   **projected-uv (53)** needs the ProjectedUV constants, and `TextureProj` is **already** VS
    PerGeometry variable 6 in the layout. Plausibly the trees pattern again: derive it per object, patch
    it, gate with capture parity.
-   **technique MTLand / MTLandLODBlend (33)** is terrain, and wants per-object `LandBlendParams` —
    VS PerGeometry variable 3, also already in the layout. The same pattern once more.

So the two largest classes are the two that need new architecture, and the two that fit the existing
machinery are worth ~86 draws a frame between them. That is the trade to decide before any of it is
built, and it is not what either the tracked counts or the drawn counts say on their own.

## Decals: a second draw pass, and a single culling phase

The ranking put decals first at 181 drawn a frame in the Whiterun exterior, and the feasibility read said
they needed depth bias and blending in the pipeline key and an ordering guarantee against the geometry
underneath. Both turned out to be true and neither turned out to be large, because the engine's own
decal path answers the question that decides the architecture: **can a decal change the silhouette of
what it sits on?**

It cannot, and `docs/development/skyrim-engine-notes.md` ("Decals") has the decompile that says so. The
main pass draws its two decal groups after every opaque object, under a depth test against them, and
the opaque group's depth write is the host's depth pulled a bias toward the camera. A decal never
occludes anything its host does not. So decals are **occludees only**: they never enter the Z-prepass or
the two-phase replay, they are culled **once**, in the colour segment, against the HZB that the depth
segment rebuilt from this frame's depth, and they are drawn by a **second pass** in the same segment,
after the opaque colour pass, that tests LESS_EQUAL with the engine's bias and writes no depth at all.

### What the probe found before anything was built

`CS_DCLF_DECAL_PROBE=1` records, at every native decal draw, the accumulation hint, the alpha property,
the depth flags and the `RendererShadowState` indices the draw was issued with, and reads the D3D11
description of every distinct state object once. It corrected the decompile-derived plan in one place -
the group index is the hint plus one - and settled two things the decompile could not: which render flags
each group runs with (0x41 for hint 2, so the alpha property is not applied; 0x45 for hint 3, so it is),
and that both blend states the groups use fit BasicRHI's `BlendFactor` without an RHI change.

Whiterun exterior, per frame: 199 native decal draws, 140 in the opaque group and 58 in the blended one;
Dragonsreach 30 (17 / 13). The `RenderPassImmediately` thunks see every one of them (211 offered against
199 drawn), which is the hook-coverage question answered: withholding a decal's pass stops its native
draw.

### The pipeline key carries the engine's state indices

`PipelineKey::rasterFlags` was one bit (two-sided). It now packs the decal group and the engine's own
`rasterStateDepthBiasMode`, `alphaBlendMode`, `alphaBlendAlphaToCoverage`, `alphaBlendWriteMode` and
`alphaBlendModeExtra` (Records.h). Every opaque key keeps all of those at zero, so nothing about an
existing pipeline changed - the pipeline count did not move.

The indices are translated by reading the engine's state objects, not by re-deriving what each index
means: `DrawPipelines::CaptureEngineStates` takes `GetDesc` of the rasterizer and blend states behind
each key's bits (EngineStates.h: the tables at `RelocationID(524748, 411363)` and `(524749, 411364)`) and
keeps them in RHI terms for the asynchronous build. It runs with the main pass's capture, inside the deferred
pass, deliberately: Community Shaders swaps the blend table for its deferred variants between
`StartDeferred` and `ResetBlendStates`, and those - with RT1-2 forced to alpha blend - are what a native
decal in the G-buffer uses. A key whose state has not been captured is simply not ready.

Where the indices come from once DCLF owns the decal and there is no native draw to copy: the bias mode
from `DecalDepthBiasMode` (the `ToggleDepthBias` byte and `DrawWorld::disableSunShadows`, so interiors get
the no-bias modes 7 and 11), the blend mode from the alpha property's functions as `FUN_14150bc80`
chooses it, the write mode as the group function and SetupGeometry leave it (10; 1 with `kZBufferWrite`,
else 11). The probe checks the derivation against every native decal draw it still sees: **0 of 4,800
mismatched in Dragonsreach, 300 of 52,800 in the exterior**, every one of them write mode 11 derived
against 1 observed on a decal without `kZBufferWrite`. That is a leak from Community Shaders' Terrain
Blending, which sets write mode 1 for its own passes from the same hook that precedes the blended decals;
the only difference between the two modes is RT0's alpha mask, and the deferred composite never reads it.

### Ordering, by fixed slots rather than by an atomic append

Overlapping decals are drawn in the engine's order - the opaque group, then the blended one; within a
group the technique buckets ascending, each bucket's five lists, each list's chain - and RegisterPass
prepends, so a chain is drawn in reverse registration order. An atomic append in BuildDraws cannot promise
that order, and two overlapping decals landing in a different order each frame would flicker.

So a decal's sequence goes to a **fixed slot**: SceneStore sorts the frame's decals by that key
(`Tables::decalOrdinal`), the epoch hands each its ordinal in its `DrawInput`, and BuildDrawsCS writes
`sequences[kDecalSequenceBase + group * kMaxDecalDraws + ordinal]` - either the draw, or the same
sequence with an index count of zero when the culling rejected it or the epoch could not build its
record. Every decal input writes its slot, culled or not, so nothing a previous frame left there can be
executed; a guard in the epoch loop pushes a blank input on every path that gives up on a decal. Each
group's draw reads its slot count from its own count word, which the CPU uploads.

### The second pass

`MainOpaquePass::Record` begins a second pass on the same attachments, all loaded, after the opaque
`ExecuteIndirect`, and issues one indirect draw per group from that group's range. The depth segment
never sees a decal: its epoch skips them before the cull-only push, so they get no visibility verdict and
none is needed - the colour segment's BuildDraws tests them itself, frustum and HZB, and the HZB bound
there is the one rebuilt at the end of this frame's depth segment, so it is final. (`SkipNativePass` used
to exempt decals inside the native depth pass, on the belief that a `kZBufferWrite` decal wrote its depth
there. It does not: see "Decal depth" below.)

Two instrument corrections came out of the parity runs. The per-geometry check compared TreeParams and
WindTimers for every object, and they compared clean by accident: SetupGeometry writes them for technique
12 only, the native buffer keeps whatever the last tree left, and no eligible object had been drawn after
a tree in the main range - until decals, which the engine draws after everything. The check now ignores
those two variables for non-trees. And the capped sample list is now two samples per variable, because
the standing PS PerMaterial 29 difference had filled it on its own for three stages running.

### Decal depth: the opaque group's depth before the colour pass

The engine writes decal depth in one place: the main pass's opaque group (hint 2), with depth mode 3 and its bias,
after every opaque object (engine notes, "Decals"). The blended group only tests, and the native depth pass draws no
decal at all, although it is offered a few blended `kZBufferWrite` ones. DCLF's second pass wrote no depth, so the
frame's depth lacked the opaque decals that the native frame has.

The colour segment now runs a **decal depth pass** between DCLF's Z-prepass result and the opaque colour pass
(`CS_DCLF_DECAL_DEPTH`, default on, `=0` off): the opaque group's fixed-slot range, through the depth signature. An
opaque-group key's depth variant carries the decal's bias, tests LESS_EQUAL, writes, and runs the alpha test, so a
transparent texel leaves the host's depth; a blended key's depth variant writes nothing. The frame's depth then matches
the native one, and every host fragment under an opaque decal texel fails the colour pass's EQUAL test before shading:
the decal replaces every target there anyway (blending off, full write masks). The blended group cannot do this: it
blends over its host, which must still be shaded. The decal's own colour draw passes LESS_EQUAL against the depth it
wrote with the same state.

Riverwood (141 opaque and 60 blended decals): colour-pass pixel shader invocations 3.295 M -> 3.262 M a frame, and the
same image (window captures of the same view, on and off, compared over the decal-covered road and ground).

### Explicit DGC preprocessing

Every colour-signature `ExecuteIndirect` (the main draws and the two decal groups) cost NVIDIA's driver a fixed
~170 us of idle GPU when its commands were generated implicitly, inside the call: four extra calls with a zero count
took 0.66-1.06 ms in the colour pass, against ~12 us each for the depth signature's calls in the depth-only pass.
Neither draw order nor pipeline switches were the cause (the decal groups switch pipelines 8 and 4 times; the
unordered layout made no difference).

`CS_DCLF_DGC_PREPROCESS` (default on, `=0` off) gives DCLF's signatures BasicRHI's `explicitPreprocess`: the colour and
depth variants and the shadow signature. Each pass records a `PreprocessIndirect` for every call it will execute before
its first `BeginPass`: the colour segment for its decal depth, main and decal calls, the shadow views' pass and
Skylighting's for every view. `vkCmdPreprocessGeneratedCommandsEXT` must be recorded outside a render pass, but it
snapshots the state it generates for, render pass instance included, from a *state command buffer*. So each recording
site (`PreprocessStates`) keeps, per frame slot, a graphics list that is never submitted: the pass records into it the
same `BeginPass`, heaps, layout, topology and push data that each execution will have, and passes it as the state
source. A site of its own for each pass that may record concurrently with another. The execution must match that state exactly (VUID-vkCmdExecuteGeneratedCommandsEXT-isPreprocessed-11048
and -11049). The first attempt used the executing list itself outside the pass as the state source. The snapshot then
had no depth attachment, and the colour pass ran with no depth test (26.9 M pixel shader invocations instead of 3.27 M).

BasicRHI's side: the layout gets `EXPLICIT_PREPROCESS`, an execution consumes the matching preprocess of the same
list (and fails without one), the preprocess output becomes visible at the list's next `BeginPass` (one barrier for
all of a pass's preprocesses, as NVIDIA's DGC sample advises), and barriers to or from indirect argument reads also
cover the `COMMAND_PREPROCESS` stage when DGC is enabled.

The depth pass (the hybrid Z-prepass, its phase 2, and the depth pass off the hybrid path) keeps implicit
preprocessing, through a second depth signature without `explicitPreprocess` (`IndirectState::depthPassSignature`). It
is a single call over sequences written just before it, so an explicit preprocess can only serialise it: 0.342 against
0.325 ms explicit, 0.316 ms implicit.

Preprocessing does not sort sequences: state sorting stays the application's job, and `UNORDERED_SEQUENCES` is the
only reordering the layout allows (nvpro-samples `vk_device_generated_cmds`).

Riverwood, focused, GPU event timers over 45 s, on against off: the colour pass 1.54 against 2.10 ms, the shadow views
1.08 against 1.24 ms, the depth pass 0.316 against 0.325 ms, and the GPU frame 11.96 against 12.56 ms. The shadow views'
draw counts are unchanged (nvperf, 2660 against 2663).

#### Pipeline set versions

The first validation of all signatures found VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071 three times while
loading: the preprocess size BasicRHI queried at recording was 12452096 bytes, the layer's a moment later 12583168. The
size depends on the execution set's pipelines, and DCLF added pipelines to the sets (`DrawPipelines::Update`, render
thread, mid-frame) while the host's thread recorded epochs that read them. The race predated explicit preprocessing:
implicit calls size their memory the same way.

The sets are now versioned (`DrawPipelines.cpp`, `SetVersion` and `ShadowSetVersion`, three of each). Every frame shape
holds the version its handles belong to (`IndirectState::version`, `ShadowIndirectState::version`) until it is recorded.
`Update` admits finished pipelines to a slot, then writes them only into a version nothing else references, at indices
that version never held, and publishes it; a pipeline's index reaches the builds only once a published version holds it.
So no recording ever reads, or sizes preprocess memory against, a set that is being written, and no entry submitted work
may use is rewritten (VUID-VkWriteIndirectExecutionSetPipelineEXT-index-11029). Versions of an earlier pipeline
generation (a target or shadow format change) are dropped on the render thread once nothing holds them. No locks, and
nothing waits: when every version is held, the admissions wait a frame (`set versions ... waited`, in the pipeline
statistics line).

Validation, full featureset: a Riverwood run and a tour to Whiterun and Dragonsreach, 0 messages about generated commands
or execution sets; 54 main and 50 shadow publishes on the tour, 0 frames waited, every pipeline in its set. Timing
unchanged (colour pass 1.541 against 1.542 ms).

The colour pass alone, focused, nvperf, on against off: the whole colour pass (preprocesses included) 1.58-1.64 against
2.09-2.12 ms, SMs active 86 against 66 %; its main draws 0.97-0.99 against 1.21 ms, the opaque decals 0.048 against
0.25 ms, the blended decals 0.39 against 0.59 ms, and the four zero-count calls 0.006 against 0.66-1.06 ms. GPU event
timers: GPU frame 12.14 against 12.57 ms, World 4.33 against 4.79 ms. Colour-pass pixel shader invocations 3.26 M
(unchanged), the same image, and no validation messages about generated commands.

### Decals, landed (behind `CS_DCLF_DECALS`)

Whiterun exterior on the hybrid path (`CS_DCLF_OWNERSHIP=static`, `CS_DCLF_CULL=occlusion`,
`CS_DCLF_TABLES=tracked`), per frame: **176 decals submitted to the second pass** (136 in the opaque
group, 40 in the blended one), 28 of them culled; `decal ... 0 drawn` left native, down from 181. Claims
1465 -> 1641 with **0 claimed but not drawn**, decal build parity **176 slots, 0 differ**, opaque build
parity unchanged, 0 validation messages. Dragonsreach: 16 submitted, 3 culled, 0 differ.

Cost, profiled, against a control with the switch off at the same place: the table build 4.52 against
4.51 ms (noise), the colour epoch 2.06 against 1.90 ms - 0.16 ms for 176 more draws, most of it the graph
execution (+0.06-0.09) and the epoch prologue (+0.03). The native loop gives up the 176 draws.

**And the image got better, not merely equal.** The control frame is missing the cobblestone road to the
right of the player; the decal frame has it. The road is a blended decal (hint 3), which the engine draws
with depth test only, before `EndDeferred`; DCLF's opaque colour pass then ran at `BeforeDeferredComposite`,
after it, and painted the ground back over it - a standing defect of the hybrid path that nothing had
measured, because capture parity compares constants and the decal's constants were fine. Owning the decal
and drawing it after DCLF's own opaque pass is what restores the order the engine has. Opaque decals
(hint 2) survived before because their depth write wins the host's LESS_EQUAL test.

Left off by default, as trees are, until someone has watched decals in motion (blood, overlapping placed
decals, cell transitions). The switch set that validated it is the one above plus `CS_DCLF_DECALS=1`;
`CS_DCLF_DECAL_PROBE=1` reports the state parity and stays useful whenever ownership is off.

## Skinned objects: the engine's palette, DCLF's buffer

Skinned objects were the second-largest class by drawn count (150 a frame in the exterior, ~100 in
Dragonsreach) and the one the feasibility read called structural, because a bone palette is a
variable-length per-object array and nothing in the record was. The structural part turned out to be
small, because the engine already does the hard half: `docs/development/skyrim-engine-notes.md`
("Skinning") has the decompile. `NiSkinInstance::boneMatrices` holds the whole palette as three float4
rows a bone, in absolute world space, refreshed once a frame by `FUN_140e4ff90` under a frame stamp, with
the previous frame's palette kept beside it. The bone setter that runs from the native draw copies those
rows into b10 and b9 and nothing more. So DCLF has no skeleton, no bind pose and no palette builder. It
runs the same update from `BuildFrame` (the native draw it withholds would otherwise have been the only
caller), copies the rows into `Tables::bones` / `previousBones`, and gives each object an offset.

### What went where

-   **The buffers.** A skinned draw binds `partition->buffData`, the skin partition's own `TriShape`,
    not `geometry->rendererData` (measured: never the same object). The geometry entry for a skinned
    shape is built from the partition, with `partition->vertices` and `partition->triangles`, and it
    resolves through `GpuResources` like any other TriShape. Draw parity confirms the binding on every
    native draw.
-   **The palette buffer.** `DCLFBones`, a `StructuredBuffer<float4>` at VS t126 beside the object
    table, sized 65,536 rows; the pipeline layout's vertex-stage SRV range is now two registers. The
    epoch packs the frame's rows **eye-relative** the way it packs World - each row's translation loses
    the eye's component for that row - current rows first, then the previous frame's relative to the
    previous eye. The shader's pivot is therefore zero, and the depth epoch, which has its own eye, is
    right by construction; `BonesPivot` in the mirrored per-frame buffer (measured equal to `posAdjust`
    on all 62,000 draws checked) is not read at all.
-   **The record.** `BindlessObject` grew a row: `boneOffset`, `previousBoneOffset`, `boneRows`
    (176 -> 192 bytes). Under `DCLF_BINDLESS` the SKINNED vertex path calls
    `Skinned::GetBoneTransformMatrixBindless` / `GetBoneRSMatrixBindless` on `DCLFBones` at the object's
    offsets and declares no b9/b10 at all. The record deduplication is untouched: nothing per object
    entered the binding record.
-   **Eligibility** (`CS_DCLF_SKINNED=1`): a `BSTriShape` with an accumulated pass whose skin instance is
    exactly a `NiSkinInstance` with one partition and at most 80 bones (`skin-shape` counts the rest); the
    `kSkinned` property flag then selects the SKINNED permutation as before.

### Measured

Gate 1, ownership off, capture parity on. **Bone palette parity: 0 of 7,800 palettes differ in the
exterior, 0 of 42,000 in Dragonsreach** - the rows DCLF copies are byte-identical to the ones the native
draw bound at b9 and b10. Draw parity 0 differ, bindless record parity 0 differ over the new row, no new
material mismatches. The one new per-geometry class was World / PreviousWorld on skinned draws, which
SetupGeometry does not write for a skinned pass and the SKINNED vertex shader does not read; the parity
instrument now skips them for skinned objects, as it skips the wind variables for non-trees.

Gate 2, the hybrid path with static ownership and occlusion culling:

| | Dragonsreach | Whiterun exterior |
| --- | --- | --- |
| skinned candidates a frame | 250 | 33 |
| claims (control -> skinned) | 920 -> 990 | 1465 -> 1478 |
| claimed but not drawn | 0 | 0 |
| build parity | OK | OK |
| frustum false negatives | 0 | 0 |
| `skinning` build part | 0.071 ms | 0.009 ms |
| tables, profiled | 2.26 vs 1.83 ms | 4.83 ms |
| colour epoch | 1.36 vs 1.21 ms | 2.20 ms |

The A/B screenshots in Dragonsreach are the same frame; the banners are DCLF's in one and native in the
other. `CS_DCLF_SKINNED` stays off by default until someone has watched skinned objects in motion.

### What the probe put next

The exterior's single-partition class is small (33) because most of its skinned draws are elsewhere:
`skin-shape` leaves 94 a frame native there, 24 in Dragonsreach. The biggest piece is the
three-partition, tree-animated shape (29 a frame, hint 11, technique 12) whose partitions carry LOD bytes
1/2/0: the engine draws the partition the pass's LOD mode enables, through the table at `0x14202a030`,
so covering it means one DCLF object per enabled partition and reading that table. Then
`BSDismemberSkinInstance` (actor parts, one or two draws each, partitions toggled per frame). The
static-pose skip the plan reserved is not worth building: 191 of 201 palettes in the exterior change
every frame.

## ProjectedUV and terrain: the trees pattern, twice

Both classes were the "trees pattern": one per-object PerGeometry value the engine computes in
`SetupGeometry`, derived on the CPU and carried per object. They turned out to be that plus two things
the ranking could not see: ProjectedUV has three per-object *pixel* constants and four pipeline-level
textures as well as its matrix, and terrain is owned by Terrain Blending in this profile.

The per-object values live in the epoch's row buffer, after the bone palettes: `kExtraRows` (seven float4
rows) per object that needs them - LandBlendParams, the three rows of TextureProj, then ProjectedUVParams,
2 and 3 - with `BindlessObject::extraOffset` pointing at them and `DCLFObjects.hlsli`'s statics reading
them under `DCLF_BINDLESS`. The constant-buffer path patches the same variables through
`PatchObjectGeometry`, which is what capture parity compares. `SceneStore::RefreshObjectExtras` fills the
rows at Prepass, where the main camera's `posAdjust` is current, and it calls the engine's own
NiTransform-to-matrix and `D3DXMatrixMultiply` routines so that `TextureProj` is the native value to the
bit. The derivations themselves are in `docs/development/skyrim-engine-notes.md` ("ProjectedUV and
MTLand").

### What the gates found

`CS_DCLF_PROJECTED_UV=1 CS_DCLF_MTLAND=1`, ownership off, capture parity on, Whiterun exterior: 53
projected and 100 terrain candidates a frame; **no mismatches on LandBlendParams, TextureProj or the three
ProjectedUVParams** - the derivations are exact - and `projected-uv` and `technique` gone from the
left-native list. Two other things surfaced and were fixed:

-   **Terrain drew with the wrong vertex stride.** Draw parity reported every terrain draw with stride
    32 against the native 40. CommonLib's `VertexDesc::GetSize` sums the attribute sizes it knows and
    leaves the landscape data out; the engine binds the desc's stride nibble times four, which the engine
    notes had recorded all along. The geometry record now takes the nibble. It never showed before because
    no eligible object had landscape data.
-   **One tree a frame with amplitude 0 against 9.** Shrubs and ferns that are both tree-animated and
    projected became eligible, and one of them (`L2_SwordFern03`) holds an uninitialised negative squared
    distance on its node. The engine's fast square root shifts the bit pattern arithmetically and its
    result clamps to the maximum amplitude; DCLF's shifted logically and clamped to zero. A diagnostic
    that prints the derivation's inputs beside the live node's found it in one run; `FastSqrt` now
    matches the engine's shift.

### Terrain and Terrain Blending

Terrain Blending, on by default here, intercepts every `kMultiTextureLandscape` pass at
`RenderPassImmediately`, holds it, and redraws it after the opaque pass with alpha blending and its own
depth-stencil state (drawing below the surface, against a separate terrain depth). An opaque DCLF draw of
the same terrain would run under that, so `MtLandEnabled()` is false whenever Terrain Blending is loaded
and enabled *and* DCLF is drawing into the frame (the hybrid path). The tables and the parity still
exercise the derivation with the feature on, because its redraws happen inside the deferred pass where the
parity hook sees them; ownership of terrain needs either the feature off or a terrain sub-pass that does
what it does. The hybrid run with every class on reports 0 terrain candidates, as intended.

#### The terrain blended onto an empty G-buffer

With DCLF on, wherever a DCLF object met the terrain (rocks and dirt mounds half-buried in it), the blend
zone showed whitish blue instead of a blend into the object. The order in the main pass was:

1.  The engine's opaque batches, with DCLF's objects withheld.
2.  `Main_RenderWorld_BlendedDecals`: Terrain Blending's held terrain, then the engine's blended decals.
3.  `EndDeferred`: DCLF's colour epoch (`BeforeDeferredComposite`), then the composite.

The terrain is drawn with alpha blending onto the G-buffer, `LESS_EQUAL` and depth writes, and its alpha is
its distance to the depth beneath over 10 units (`Lighting.hlsl`, `blendFactorTerrain`). The depth already
held DCLF's objects from the Z-prepass, so the alpha was right, but the G-buffer did not hold them yet: the
terrain blended onto its clear values. Then the terrain's depth writes made DCLF's `EQUAL` test fail where it
covered the object, so the object never filled the zone in.

**The fix:** the colour epoch runs where the opaque batches end, at the start of the `BlendedDecals` hook
(`DrawcallLimitFix::AfterOpaquePass`), which is the native order. It unbinds the render targets before
the epoch and marks them dirty afterwards, as the Z-prepass does inside the depth pass.
`BeforeDeferredComposite` keeps the rest (the path off hybrid, the claims, the debug view).

Terrain Blending also holds the passes of meshes flagged `kNoTransparencyMultiSample` (a flag the engine does
not use, which marks a mesh the terrain should not blend over) and redraws them after the terrain with
`EQUAL`. DCLF now draws before the terrain, so those stay native while Terrain Blending is on
(`Ineligible::TerrainNoBlend`, "terrain-no-blend").

**Result**, the save by the tree roots: DCLF on and off match in screenshots; capture parity 0 mismatched,
0 untracked eligible; 0 claimed but not drawn. That save has no `kNoTransparencyMultiSample` meshes, so the
exclusion was not exercised.

### Everything on

Hybrid, `CS_DCLF_TREES CS_DCLF_DECALS CS_DCLF_SKINNED CS_DCLF_PROJECTED_UV CS_DCLF_MTLAND`, Whiterun
exterior: claims 1478 -> **1712**, 0 claimed but not drawn, build parity OK, 0 frustum false negatives,
one validation message (the semaphore one). Left native and drawn: skin-shape 89, unsupported-parent 51,
not-trishape 48, technique 35 (terrain, gated), actor 35, alpha-test-state 1.

## Persistent tables: the build that stops re-deriving the frame

`BuildFrame` used to rebuild every table from scratch each frame. In the Whiterun exterior that was 4.3 ms
for 10,000 tracked objects of which ~1,700 are drawn, and profiled by part it was almost all re-derivation
of things that do not change: the static classification (1.2 ms), the per-frame hidden/fade check for
every candidate (0.7), the record (0.95) and the pipeline/material dedup (0.5). Two steps, both behind
gates that read 0, took it to **2.1 ms**.

### A1: nothing for what cannot be drawn

With the draws gated on the engine's visibility (`CS_DCLF_CULL_INPUT=native`, the default) an object the
accumulator does not hold is a culling candidate and nothing more: it needs bounds, `kObjectNoBindings`
and a place in every parallel array. It now gets exactly that, and only that, and the depth segment still
submits it cull-only, so the culling's cross-tabulation is unchanged (5,577 tested before and after).
`=tracked` keeps the full path because it wants records for them.

Whether such an object is a candidate at all is the classification's answer, and that answer is kept on
the tracked entry (`Tracked::candidateReason`) and refreshed every 64 frames rather than recomputed per
frame: once the derived cache below had removed the classification itself, the loop head - the witness
loads and the negative-cache probe for ~8,300 objects - was most of what "classify-static" still measured.
A verdict a few frames old costs at most a diagnostic, because a candidate is tested by the culling and
drawn by nothing. The ineligibility histogram counts the kept verdicts, so it reads as before.

### A2: slot-stable tables and the derived cache

The three shared tables - geometries, pipelines, materials - keep their slots across frames. Each slot has
a `lastUsed` frame and its key; a sweep every 16 frames frees slots idle for 64 and erases their map
entries (since "Persistent draw state", Step 8, liveness is by reference: `SlotTable`); the per-object arrays are still appended per frame (`Tables::ClearFrame`). Everything that reads
a pipeline by index (the epoch's blocks, EarlyPrepass's pipeline requests, `RefreshFrameConstants`) is
restricted to the slots used this frame.

Over that, `Tracked::Derived` caches the positive derivation for an accumulated object: the descriptors,
the static object flags, the pipeline key and the three slots. Its witnesses are the Stage 3 pointers
(TriShape, property, material) plus the fade state, the accumulated technique / sub-pass / hint, the
interior flag, whether the material alpha is below one, the frame's decal bias modes and the table
generation; and the slots are checked against their keys before they are served, so a swept and reused
slot cannot be handed back. A hit skips `ClassifyStatic` and the whole derived section; `ClassifyFrame`,
the alpha-test-state rule, the transforms, shading, palettes and the draw sequence still run.
`CS_DCLF_DERIVED_CACHE=off|on|probe` (`probe` serves the cache and recomputes, comparing slots, flags, key
and descriptors): **0 differ** over 1,712 objects a frame in the exterior, through Dragonsreach, back,
and a save/load, with the sweeps retiring 1,000-1,600 slots at each transition and 0 slot violations.

Three things the persistence surfaced, each of which was a one-frame artefact before and a standing one
after, which is the general lesson of this step - every value in a persistent slot has to be either
fixed or refreshed on purpose:

-   **The technique constants are the frame's** (fog, settings, and the shadow mask's view). They are now
    re-evaluated at Prepass for the used pipelines, with the geometry templates. Serving the slot's first
    evaluation was a parity regression and, at startup, a stale view pointer.
-   **The per-frame drift floats** (PS PerMaterial 29; superseded by "Material records from their
    sources") were learned by re-evaluating a cached material,
    which a persistent slot never does. The rolling validation slice (8 live materials a frame,
    re-evaluated and compared with what their slot serves) now learns them, seeds the patch source, and
    heals a stale record in place; at startup it reports a handful of stale slots as values settle, then
    0. Capture parity with every class on: PerMaterial 29 x91,200 and VS PerMaterial 11 x5,860 over
    523,000 checked draws, against x67,200 / x3,000 over 465,000 before - the same residue (standing debt),
    at a slightly higher rate that belongs with it.
-   **`GpuResources::Resolve` returned a pointer into a map whose storage moves.** Resolving the index
    buffer could reallocate the map and the vertex buffer's pointer then read garbage; rebuilt every frame
    the bad record lasted one frame, kept in a slot it lost the device on the first draw (3 of 14 startups,
    always at the first epoch). `CS_DCLF_SLOT_PROBE=1` - a fresh evaluation and a fresh resolve for every
    used slot, logged against what the slot serves - caught it in one failing run
    (`vb 0xb2104741d736cab1 -> 0x2562278000`, index address correct). `Resolve` returns by value now;
    0 of 6 startups after, 0 probe differences.

Also kept from this: a geometry slot found by address whose buffers no longer match (a TriShape
reallocated at the same address), or whose buffer references were evicted, or which was resolved while
the graph was off, is resolved again in place; the buffer references are touched on the slot's first use
of a frame; a pipeline's template is always a property of an object of the frame; `CheckObjectSlots`
neutralises any object naming a slot not of this frame and counts it (the gate: 0); the teardown paths
reset the maps and bump the generation.

### Measured, and the A3 decision

Whiterun exterior, hybrid, every class on, `CS_DCLF_PROFILE=1`, same place and hour:

| tables, ms      | control | A1   | A1+A2 |
|-----------------|---------|------|-------|
| total           | 4.8     | 4.3  | 2.12  |
| classify-static | 1.29    | 1.15 | 0.21  |
| classify-frame  | 0.68    | 0.69 | 0.19  |
| record          | 0.95    | 0.78 | 0.67  |
| dedup-hit       | 0.53    | 0.18 | 0.10  |
| loop-tail       | 0.38    | 0.36 | 0.36  |
| walk + lookup   | 0.47    | 0.48 | 0.46  |

Claims 1712 -> 1712, 0 claimed but not drawn, build parity OK, 5,578 tested / 0 frustum false negatives,
bone palette parity 0 of 7,800. What remains is per frame by nature: the walk and pass lookup (0.46), the
iteration over 10,000 tracked entries (0.36), and for the drawn objects their transforms, bounds, room
probe, shading and sequence (0.67 for 1,712 plus the 3,225 cull-only pushes). A3 - persistent object
slots - would save the pushes and the `objectIndex` insert, on the order of 0.2-0.3 ms of the 2.1, and is
not built; the next structural gain is Stage 6, where the engine's own per-object main-pass work is.

## Shadow views, step one: what the engine does, and a frame in two halves

### The probe (`CS_DCLF_SHADOW_PROBE=1`)

`DrawcallLimitFix/ShadowProbe.cpp` measures the shadow half of the frame before DCLF owns any of it: it
enumerates the views from the shadow scene node before the shadow maps are drawn, records the render
state at each shadow accumulator's `FinishAccumulatingPreResolveDepth`, samples the Utility draws'
constants, times each light, and cross-checks every Utility registration against the technique DCLF
would derive and the caster rule DCLF would apply. What it found is written up in
`skyrim-engine-notes.md` under "Shadow maps"; the four answers the design needed:

-   **The derivation is exact.** Over ~1M registrations in the exterior and the Bannered Mare, the
    technique derived from the property's flags and the view's render mode differed from the engine's
    `passEnum - 0x2B` **zero** times. For a Utility pass the `techniqueID` given to `RegisterPass` is the
    `passEnum` itself, not a descriptor as it is for Lighting.
-   **The caster rule is exact.** The rejection rule read off `GetRenderPasses_ShadowMapOrMask` rejected
    none of the casters the engine registered. Of the main pass's kept objects with no registration in a
    cascade, all but the alpha-blended ones are simply outside the cascade cull.
-   **Views need a viewport origin.** A paraboloid light's two hemispheres share one array slice as
    `(0,0) 4096x2048` and `(0,2048) 4096x2048`, and a focus shadow is `(0,3547) 549x549`. Half the drawn
    views are sub-rectangles, so `rhi::PassBeginInfo` cannot keep assuming the origin is (0,0).
-   **The eye is the shadow camera's.** Utility's `World` is eye-relative to the shadow camera's
    `posAdjust`, up to 15,000 units from the main camera's, and `VS_PerFrame` c8 holds the transpose of
    that camera's `viewProjMat`. A shadow epoch therefore adds an eye delta per view, as planned.

The cost it measured is the baseline S5 will be judged against: 2.3 ms of render-thread CPU per frame in
the exterior (2.2 of it in cascade 1's 2,569 draws), 0.5 ms for two point lights at night.

### The two-phase frame

Shadow views are drawn *inside* `Main_RenderShadowMaps`, and the main camera's passes are registered by
jobs that run concurrently with them - they are complete only when that call returns. So an object record
a shadow epoch can read has to exist before it, and the accumulator's half of the record cannot.
`SceneStore::BuildFrame` now takes a phase:

-   **`Phase::Scene`**, from `DrawcallLimitFix::BeforeShadowMaps`: the tracked walk, eligibility, the
    geometry slots and their buffer resolve, transforms, bounds, bone palettes, and one object record per
    eligible object - with no pipeline, no material, `kObjectNoBindings` and `kObjectNativeVisible` clear.
    Object indices are fixed for the frame from here.
-   **`Phase::Accumulate`**, at `EarlyPrepass` as before: the capture drain, the pipeline and material
    slots, the per-frame lighting template, shading, light lists, decal order and `kObjectNativeVisible`,
    patched into those records by object index.

Three things this split taught:

-   **The scene phase must not decide anything that needs the pass.** The decal rule is the one such
    rule - a decal's group comes from its accumulated pass's accumulation hint - and rejecting on it
    before the shadow maps left 452 accumulated objects without records and dropped claims from 1712 to
    1531. Decal verdicts are now deferred: the record is built, and the accumulate phase takes the verdict
    again with the pass in hand (`DeferredToAccumulate`).
-   **The per-frame classification still belongs to every frame.** The scene phase's verdict is cached
    for up to 64 frames, which is right for "is this object drawable at all" and wrong for hidden, actor
    and fading. The accumulate phase re-runs `ClassifyFrame` even when the derived cache hits, or an
    object that has just been hidden keeps its bindings.
-   **The second phase should iterate the passes, not the tracked set.** Walking all 10,053 tracked
    objects again to look up 1,700 accumulated ones cost 0.4 ms of pure loop; iterating
    `accumulatedPasses` and finding the tracked entry costs nothing measurable.

Measured against the A1/A2 build at the same place, every main-pass gate holds: claims 1719 (against
1712) with 0 claimed-but-not-drawn, capture parity clean in steady state, draw parity 0 of 13,843,
derived cache 1719 served / 0 differ, 0 slot violations.

| | A1/A2 (one phase) | two phases |
|---|---|---|
| tables CPU per frame | 2.12 ms | 2.77 ms (scene 1.60, accumulate 1.17) |
| object records | 4,937 | 5,358 |
| geometry slots | 645 | 953 |

The extra 0.65 ms is the scene phase giving records, geometry slots and transforms to every eligible
object rather than bounds alone to the ~3,200 the main camera's culling had rejected. That is the work
the shadow epochs exist to consume, and it is paid once for all views.

## Shadow views, step two: the views drawn, and the counters that say so

Step S2 of the shadow plan gives DCLF the sun's cascades - every view the engine draws with render mode
0xD-0xF that is not a focus shadow - behind `CS_DCLF_SHADOWS=1`, with static ownership of their casters
behind `CS_DCLF_SHADOW_OWNERSHIP=static`. Shadow correctness is judged by counters and readbacks alone
for now: Community Shaders' shadows are broken in general at the time of writing, so a screenshot says
nothing about DCLF's part in them.

### What was built

-   **`ShadowViews`** (`DrawcallLimitFix/ShadowViews.{h,cpp}`): the frame's views from the shadow scene
    node's caster array, in the engine's order, keyed by accumulator (what the 0x2A hook is called on)
    and by batch renderer (what the engine registers the view's Utility passes with). Each view also
    carries the accumulator's render mode from its last draw, which is what attributes a registration to
    a mode's claim set. The batch renderers of the non-focus views are published to `PassCapture` per
    frame, as the main camera's are.
-   **The shadow classification** in the scene phase: `ShadowCasterReject` (the engine's rule, 0 false
    rejections over ~1M registrations in step one) sets `kObjectNoShadow`; `ShadowUtilityTechnique`
    gives every object its Utility technique without the mode bits; two-sided and the alpha-test
    threshold are read from the property in the scene phase, because the shadow epoch needs them before
    the accumulate phase runs.
-   **Utility programs and the shadow pipeline set**: `ShaderPrograms::FindShadow` compiles
    `Utility.hlsl` with `DCLF_BINDLESS` per (technique | mode bits); the VS takes `World` from the
    object record plus a per-view eye delta (`DCLFEyeDelta`, b0 c2), bones from the bone rows with the
    same delta, `TreeParams` from the record, and `AlphaTestRefRS` from the record's threshold bits.
    `DrawPipelines::FindShadow` keeps a second pipeline set and indirect signature: depth only, LESS with
    writes, the engine's rasterizer bias for the key's bias mode, culling from the key, the shadow map's
    DSV format (`D16_UNORM`).
-   **Capture, then one epoch.** The 0x2A hook (`BSShaderAccumulator::FinishAccumulatingPreResolveDepth`)
    only *captures* a view after its native draws: target and slice from the renderer state, the
    viewport, `posAdjust`, the PerTechnique block (parabola parameters from the two globals, the eye
    delta) and a copy of `VS_PerFrame` as the engine wrote it for the view. `ExecuteShadowFrame`, at
    `AfterShadowMaps`, then runs *one* graph epoch for every captured view: the object records, bone rows,
    geometry table and binding records once; the draw inputs once per render mode present (a caster with a
    ready pipeline under that mode); per view a slot holding its two blocks at the head of the constant
    arena, its own copy of the binding records naming them, its sequence and count buffers, one
    `BuildDrawsCS` dispatch (frustum only, single phase, no engine-visibility gate) and one
    `ExecuteIndirect` into its slice through a per-slice DSV at the view's viewport origin
    (`rhi::PassBeginInfo::x,y`, added to BasicRHI for this).
-   **The volumetric copy.** The engine draws each cascade twice: into `kSHADOWMAPS_ESRAM` (4096², two
    slices) and again, through the same batch renderer, into `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`
    (512², two slices) for the volumetric lighting. A caster withheld from the renderer is missing from
    both draws, so under ownership DCLF must draw both: target 3 is the third imported shadow depth
    target. Before it was, exactly two views a frame reported "not ready (depth)".
-   **Static shadow ownership.** After a successful epoch the inputs of each mode become that mode's
    claim set (`PassCapture::PublishShadowClaims`); the registration hook withholds a Utility pass whose
    batch renderer belongs to a view of that mode and whose geometry is claimed. The exterior withholds
    ~2,150 of the cascades' registrations a frame with 5,176 claimed; the engine still draws the rest
    natively (actors, grass, LOD, terrain - nothing DCLF has a record for).
-   **Live toggles.** Every non-default feature - hybrid, ownership, the culling mode and input, the
    object classes, the shadow views and their ownership, and the diagnostic modes - is a `Toggles`
    entry seeded from its switch and edited in the DCLF menu. The render thread applies the requested
    set once per frame at `BeforeShadowMaps`; a change to a toggle that enters the classification drops
    every cached verdict and derivation (`SceneStore::InvalidateVerdicts`). The shadow system has its own
    switch there so it can be excluded while everything else is tested.

### The counters

Riverwood exterior, the standard switch set plus shadows and shadow ownership, 120 s:

| counter | value |
|---|---|
| views offered per frame | 5 (two cascades into target 2, the same two into target 3, one focus view) |
| views drawn / not ready / focus left native | 4 / 0 / 1 |
| inputs per mode (0xE) | 5,176 with 0 lacking a pipeline, 0 lacking a texture; 163 binding records |
| GPU culling, cascade 0 (sampled) | 5,176 tested, 111 drawn, 5,065 outside the frustum |
| GPU culling, cascade 1 (sampled) | 5,176 tested, 2,308 drawn, 2,868 outside |
| shadow ownership | 2,149 clamped passes withheld per frame, 5,176 claimed, 0 views not ready |
| main-pass gates | claims 1,712, 0 claimed-but-not-drawn in steady state (one transient of 2 during churn) |
| CPU per frame | 1.65 ms: capture 0.01, prepare 0.38, inputs 0.34, blocks 0.05, graph 0.87, claims 0.14 |

Two lessons the counters taught:

-   **The graph's execution is a per-epoch floor.** One epoch per view cost 4.4-6.0 ms a frame, of
    which 3.5-5.1 ms was the graph's own compile/prepare/record - the main epochs show the same ~0.9 ms
    "graph execute" part. One epoch for all views brought the shadow path to 1.65 ms with the same four
    views drawn. Anything that runs per view has to be a dispatch or a pass inside one epoch, never an
    epoch.
-   **A "not ready" count needs a reason.** The two unexplained not-ready views a frame were the
    volumetric copies; the per-reason counters (`setup`, `pipelines`, `tables`, `depth`, `epoch`,
    `capacity`) said `depth` and a once-per-target log line said target 3.

The engine's own shadow-map CPU with the probe on was 2.7 ms a frame before ownership; what the native
loop saves under ownership is not measured yet (step S5).

### Open, carried to S3-S5

Spot lights (mode 0xD, the clamped permutation) are built but untested for lack of a spot-lit cell in the
runs so far; parabolic lights (S3) and the focus views (S4) stay native; `HighDetailRange` is zero, the
depth bias uses mode 0 only, the samplers are the default wrap/anisotropic; the object records are rebuilt
per frame for the shadow epoch (0.38 ms) although the scene phase has them; the inputs cost two hash
lookups per caster per mode (0.34 ms); the claim set is rebuilt per frame (0.14 ms); the hole detector
counts not-ready views rather than withheld passes per view. The shadow image parity gate from the plan is
deferred until Community Shaders' shadows are themselves correct.

## Payloads built off the render thread

The GPU idle trace (`CS_GPU_IDLE_TRACE`) showed the GPU waiting 2-5 ms a ~24 ms frame for the render
thread, most of it DCLF's own CPU work at points where the GPU had already drained everything submitted:
0.8-1.5 ms before the main-opaque epoch, 0.8-1.4 ms before the Z-prepass, 0.2-0.5 ms before the shadow
epoch and 0.3-0.6 ms for the scene tables at the start of the frame. Reflex keeps the queue deliberately
shallow, so queue depth cannot hide those gaps. The work has to leave the render thread's path between
dependent GPU work. The work is split into phases, each gated on its own. All five have landed.

### The contract: prepare, build, commit

-   **Prepare** (render thread, at the kick): everything the build reads that is not fixed until the join
    is copied into an inputs struct: frame, eye pair, render flags, frame-slot masks, the resources'
    addresses, and the tables' and lookups' generations.
-   **Build** (`BuildMainPayload`, `BuildShadowPayload`): a pure function of the inputs, the tables and
    the lookups. It makes no engine, service or D3D11 call, and it writes only its payload. `probe` runs
    it twice for exactly this reason.
-   **Commit** (render thread, inside the epoch's `beforePrepare`): checks that the job's inputs equal the
    epoch's (`SameInputs`; a mismatch is *stale* and the epoch builds inline), then copies the frame
    blocks into their slots, patches the frame textures, uploads, and hands the frame to the graph.
    `BUFFER_UPLOAD`, the descriptor service and `ConstantEvaluator` stay on this thread.

**Lookups** (`DrawcallLimitFix/Lookups.h`, owned by `SceneStore`) replace what the build used to ask
services for. The render thread fills them where the service is legal: pipelines (set index, constant
tables, register usage per variant) at `EarlyPrepass`, material textures, samplers and projected
textures at the epoch's commit, and shadow pipelines and textures at the shadow commit. Frame constant
blocks moved into `frameConstants`, a buffer of fixed 64 KB slots (`FrameSlotOffset(stage, register)`),
so a record can name a block's address before the block exists.

**The worker** (`DrawcallLimitFix/AsyncWorker.{h,cpp}`) is one dedicated thread, `CS DCLF worker`, with
a small FIFO and a join. A job that fails or is stale falls
back to the inline build, and the worker's payload is dropped. `EndFrame` counts any job still
outstanding at the end of the frame as `leaked`. `SetActive(false)` drains the worker. ORG's task
service and the shader compilation pool were ruled out: the first is what `ExecuteFrame` fans onto while
the render thread waits on it, and the second is saturated exactly when a new cell loads.

### Phase 1 gate: the split, still inline

Every parity line matched the pre-split build: record dedup, bindless record, `BuildDraws` and decal
parity OK; capture parity at its standing material residue (the same windows as before, ±3%);
`0 claimed but not drawn`; shadow `0 views not ready`; `derived cache 0 differ, 0 slot violations`;
live toggle 900:960 clean. The idle trace did not change beyond noise. The shadow epoch's lookup
refresh first cost 0.8-1.0 ms because it walked the shadow textures per view. Collecting the alpha-tested
casters' diffuse textures once in the scene phase (`Tables::shadowTextureSet`) removed that cost.

### Phase 2: the colour job

The colour epoch replays the Z-prepass's eye and constants (the hybrid path), so its inputs are final at
`DrawcallLimitFix::Prepass`, right after `RefreshFrameConstants`, the frame's last writer of the tables.
The job is kicked there (`IndirectDraws::KickColourBuild`) and joined in `RunEpoch(MainOpaque)`. That
leaves 1.5-2 ms of native rendering between the kick and the join. The frame-slot masks the epoch will
supply are predicted from the previous colour epoch and checked like every other input. They change only
when the set of bound frame blocks changes, which happens on the first frame of a cell and on a load.

The join comes before the epoch refreshes the material lookups, and the order matters. The worker reads
the lookups until it finishes, so a refresh that grows them underneath the job is a data race. A refresh
that adds or changes an entry bumps the lookups' generation, which marks the job stale.

Gate (exterior → Dragonsreach → Riverwood, live toggle at 900:960, save and load, ~2,700 frames):

| check | `probe` | `on` |
|---|---|---|
| epochs using the worker's build | 295-300 of 300 per window | 295-300 of 300 per window |
| stale | 1-2 at `coc`/`load` (predicted masks), otherwise 0 | the same |
| late / failed / cancelled / leaked | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| payload comparison | 295-300 compared, **0 differ** | - |
| other parity lines and ownership | unchanged from Phase 1 | `0 claimed but not drawn` in every window |

GPU idle attributed to the main-opaque epoch (`main opaque inputs` + `main opaque` in the
`[GpuIdle] summary`, ms per frame, 300-frame windows): **0.42-1.55 before, 0.04-0.55 after**. The rest
is ORG's prepare/record/submit floor and the join's wait. The Z-prepass epoch is still built inline, and
its share swings by ±0.5 ms from run to run.

### Phase 3: the Z-prepass job, on a predicted eye

Between `EarlyPrepass` and the end of `Main_RenderDepth` nothing writes the tables: the accumulate phase
is their last writer, and `RefreshFrameConstants` runs later, at `Prepass`. The Z-prepass job is kicked
at the end of `EarlyPrepass` (`IndirectDraws::KickZPrepassBuild`) and joined by `RunEpoch(ZPrepass)`.
The only input not yet known at the kick is the eye, which the epoch captures from `posAdjust`. At
`EarlyPrepass`, `posAdjust` still holds the shadow cameras' eye, so the job uses a prediction:
`RE::Main::WorldRootCamera()->world.translate` for the eye, and last frame's captured eye for the
previous eye. The epoch compares both exactly with its capture, and a miss makes the job stale.
The miss count is reported on its own line (`[DCLF] async zprepass eye`). `drewLastFrame`, the gate the
Z-prepass uses without withholding, is snapshotted at the kick. The render thread's colour epoch is the
only writer of `drawnFrame`, and it runs after the join.

Two defects surfaced on the way, and both are fixed:

-   **One job could cancel another.** Dropping a job used `CancelPending()`, which empties the whole
    queue. With two jobs a frame, dropping the colour job could cancel a queued Z-prepass job.
    `AsyncWorker::Cancel(handle)` ends one job: it is dequeued, or waited for if it is already running.
-   **A newly resolved lookup did not make a job stale.** The lookups' generation was bumped only when an
    already-resolved entry changed. A material resolved for the first time in the epoch's refresh left
    the job looking current, yet the job had deferred those draws. The probe caught it right after a
    `coc` (`constants: 4912 vs 82720 bytes`). Under ownership that would be one frame of missing depth
    for the claimed objects. Any change a build can observe now bumps the generation, including an
    entry resolved for the first time. As a result, the Z job goes stale for 2-3 frames after a cell
    change and while textures stream in at startup.

`CS_DCLF_ASYNC_EYE` was dropped: the prediction has not missed once, and `CS_DCLF_ASYNC_JOBS` without
`zprepass` already keeps the Z-prepass inline.

**Refresh after the take (2026-09-30).** With every eligible object a member, the camera turning joins and retires
members every frame, and each joiner's first lookup resolution bumped the generation between a kick and its epoch:
the Z-prepass job went stale on about a fifth of the frames, and the shadow job (whose build reads none of the
material lookups) on a quarter to a third. Now:

-   The main-pass kicks run `RefreshMaterialLookups` in full, outside an epoch (a view never imported is left
    unresolved there, since only an epoch can queue its import). An epoch compares its inputs with the job's first
    and refreshes afterwards; only an inline build refreshes before it builds. What the refresh changes reaches the
    next frame's job. A draw that job defers stays the engine's: admission waits for the colour epoch to have drawn it.
-   The shadow build keys on `Lookups::shadowGeneration`, bumped only by what it reads (shadow textures, slots,
    pipelines, the null texture, the samplers). The shadow kick takes the lookups as the last shadow epoch left them;
    `shadowRefreshDue` (a reset, a recreated pipeline set) makes the epoch refresh before it compares.

-   A full refresh at every kick re-resolved the ~500 character-lit materials whose record version the t11
    alternation renews every frame (0.9 ms a frame, 3,700 registry requests). An entry now keeps every view it already
    holds resolved without asking the registry, and its binding block holds both t11 incarnations, so a swap between
    them does not reseal it (0.23 ms a frame for five refreshes).

Parity run with the camera turning: Z-prepass 300 of 300 epochs on the worker's build (from 226-250), shadow
272-292 (from 188-245; the rest stale on the views' raster states, none on the lookups), holes 0.

**Shadow inputs independent of the frame's views (2026-09-30).** A caster is an input only if its key slot has a
pipeline under every rasterizer state of its class's views, and the kick predicted those states from last frame. The
prediction missed on 3-23 frames in 300. The diagnostic showed why:
-   the sun's cascades alternate their depth-bias states frame by frame (`[5,6]` / `[2,3]`, ShadowmapCascadeRasterizerFix);
-   a local light's view with culling off (state 1) appears for single frames.

Now `Impl::shadowStatesSeen` holds, per mode and caster class, every state its views have drawn with. It grows when a
state first appears (`DrawPipelines`' ids only grow), and the inputs, the claims and `RefreshShadowLookups`' pipelines
are for all of them. Each view's latch row is still its own state's. Result: a job goes stale only when a state first
appears (once or twice at startup), then 0 per 300 frames; holes 0; set parity reports nothing withheld and drawn by
nobody.

Gate (the same route and switches as Phase 2):

| check | `probe` | `on` |
|---|---|---|
| Z-prepass epochs using the worker's build | 296-300 of 300 per window | 285-300 of 300 per window |
| stale | 3 at each `coc` | 2-11 (startup streaming, `coc`, `load`), 0 in steady state |
| predicted eye missed | 0 | 0 |
| late / failed / cancelled / leaked | 0 | 0 |
| payload comparison | 296-300 compared, **0 differ** (colour and Z-prepass) | - |
| other parity lines | record dedup, bindless record, `BuildDraws` OK; derived cache 0 differ, 0 slot violations | - |
| ownership | 0 claimed but not drawn | 3 in the startup window only (the standing transient: every run since the Phase 0 baseline has 1-3) |

GPU idle attributed to the Z-prepass epoch (`Z-prepass inputs` + `Z-prepass`, ms per frame, 300-frame
windows): **0.61-1.40 before, 0.06-0.40 after**. The main-opaque share stays at 0.01-0.43.

### Phase 4: the shadow inputs job

The shadow epoch's inputs are final at `BeforeShadowMaps`, right after the scene phase and
`BeginShadowFrame` (the reference eye). Between there and `AfterShadowMaps` nothing writes the tables:
every writer is in the scene phase (before) or the accumulate phase (after), and `ExecuteShadowView`
and the shadow probe only read. The job is kicked there (`IndirectDraws::KickShadowBuild`) and joined by
`ExecuteShadowFrame`. Its window is the engine's entire shadow-map pass. Two inputs are unknown until
the views are captured: the render modes, and the DSV format the shadow pipelines are looked up for.
The job builds for last frame's modes. The epoch compares the modes with its own set, and a DSV format
change shows up as a changed shadow pipeline lookup, which bumps the generation. Either makes the job
stale. `SharedData` and `FeatureData` are copied at the kick and compared as well.

Gate (same route and switches):

| check | `probe` | `on` |
|---|---|---|
| shadow epochs using the worker's build | 290-300 of 300 per window | 290-300 of 300 per window |
| stale | 6 at startup (lookups resolving), 1-3 at a mode change (`coc` into Dragonsreach's paraboloid views and back) | the same |
| late / failed / cancelled / leaked | 0 | 0 |
| payload comparison | 290-300 compared, **0 differ**, in both the cascades (0xE) and the paraboloid views (0xF) | - |
| shadow ownership | 0 views not ready outside the startup window (the standing 12) | the same |
| join wait | ≤0.001 ms average | ≤0.001 ms average |

Shadow epoch CPU per frame on the render thread: **1.93 ms before, 1.14 ms after** (preparing 0.79 →
0.04 ms). What is left is the graph's own compile/prepare/record (0.95 ms) and publishing the claims
(0.15 ms). GPU idle attributed to the shadow epoch is 0.22-0.34 ms/frame, against 0.16-0.54 before:
the ORG floor.

### Phase 5: the scene walk

The scene phase cost the render thread 1.8-2.3 ms a frame in the exterior (26 ms on the frame after a
load), more than any epoch. `BuildScenePhase` is now split into three parts:

-   **Prologue** (render thread, `BeforeShadowMaps`): the load-screen branch, the frame globals,
    `SweepSlots`, the lighting shader, and the iteration order over `tracked`. When the walk is going to
    the worker, `PrepareSceneJob` also makes the two calls the walk would otherwise make that only the
    render thread may: `GpuResources::Touch` for every geometry slot used last frame, and the engine's
    palette update (`UpdateSkin`, AE `0xe4ff90`) for last frame's skinned objects.
-   **Walk** (`SceneWalk`, on the worker or inline): today's loop, unchanged except for what it may not do
    off the render thread. A geometry slot that is new, changed, or not touched ahead, and a skin the
    prologue did not update, count as *misses* instead. That happens on the frame after a cell change
    (thousands of misses) and when a new skinned object becomes eligible.
-   **Join** (`JoinScenePhase`, render thread): at `AfterShadowMaps`, so the window is the engine's whole
    shadow-map pass. `EarlyPrepass` and Present also call it, in case the frame drew no shadow maps. A
    walk with misses is replaced by an inline walk, which resolves them. The shadow build queued behind the
    walk is first allowed to finish (`AsyncWorker::WaitIdle`), then made stale by `GetSceneRebuilds`.

The window was audited rather than assumed:

-   `tracked` changes only in `ProcessEvents`, at Present.
-   The skip hook returns before `FindObject` for every pass outside the depth and deferred passes, so
    the native shadow draws never read `objectIndex`.
-   `ExecuteShadowView` no longer reads the tables. It only checks whether they are ready, and
    `ExecuteShadowFrame` makes the same check after the join.
-   The shadow probe reads the tables in the window, so the walk stays inline while
    `CS_DCLF_SHADOW_PROBE=1`.
-   The engine data the walk reads (world and previous-world transforms, bounds, runtime data, properties,
    materials, fade nodes, the parent chain, palettes) is the data the render thread read at this same
    point before. Its only calls are side-effect-free virtuals (`GetRTTI`, `GetType`, `GetUserData`,
    `GetFeature`).

`probe` re-walks on the render thread at the join and compares the per-object arrays byte for byte,
including the slots-used vector. A difference would mean either a walk that is not a function of its
inputs, or engine data that changed during the shadow maps.

Gate (same route and switches):

| check | `probe` | `on` |
|---|---|---|
| walks used | 299-300 of 300 per window | 299-300 of 300 per window |
| rebuilt inline | 1 per cell change (2,266-5,349 geometry misses, 0-12 skin misses) | the same |
| slot touches failed ahead | 0 | 0 |
| comparison | 299-300 compared, **0 differ** | - |
| every other job | 0 differ, 0 late, 0 leaked | 0 late, 0 leaked |
| parity and ownership | slot probe 0 differ; bone palettes 0 differ; derived cache 0 differ, 0 slot violations; capture parity at its standing residue; 0 claimed but not drawn | 0 claimed but not drawn except 4 in the startup window (the standing transient) |

Render-thread CPU of the scene phase: **1.8-2.3 ms before, 0.14-0.18 ms after** (the prologue).
Scene-phase GPU idle: 0.06-0.64 ms/frame before, ≤0.04 after.

What moved rather than disappeared: the walk (1.1-1.5 ms on the worker) and the shadow build chained
behind it (0.5-0.7 ms) take longer than the engine's shadow-map pass. The render thread therefore waits
0.6-0.9 ms on average at `AfterShadowMaps` (0.3-0.6 of it for the shadow build). The GPU has the shadow
maps to draw during that wait, so little of it shows as idle.

### All five jobs, `on`

GPU idle per frame, 300-frame windows, the same route (exterior → Dragonsreach → Riverwood, save, load):

| window | idle before (Phase 0) | idle after | `CS DCLF` share before | after |
|---|---|---|---|---|
| exterior, startup | 0.81 | 0.37 | 0.71 | 0.32 |
| Dragonsreach | 2.34-2.98 | 1.48-3.77 | 1.32-2.47 | 0.47-0.83 |
| Riverwood and after save/load | 2.02-4.96 | 0.27-2.04 | 1.66-4.03 | 0.22-1.33 |

The `CS DCLF` share that remains is ORG's own prepare/record/submit per epoch, plus the joins' waits.
In Dragonsreach the total idle did not fall with the DCLF share: what remains there is not DCLF's.
Frame rate is not a usable signal on this route: medians ranged from 36.5 to 46.1 fps across runs with
no trend by phase.

Open:

-   The worker's builds take 0.8-2.0 ms, against ~1.0 ms for the same build inline. The 9800X3D has no
    hybrid cores, so the likely cause is contention with the render thread's native rendering. The colour
    join therefore still waits 0.1-0.5 ms on average. The Z-prepass join barely waits (≤0.02 ms average)
    because its window is longer. Next is a cheaper build: the two main epochs still build their object
    records, bone rows and geometry table separately.
-   The join at `AfterShadowMaps` waits 0.6-0.9 ms. The scene walk and the shadow build need a longer
    window than the shadow-map pass, or a cheaper walk.
-   The standing startup transient of 1-4 claimed-but-not-drawn objects in the first 300 frames: present
    since the Phase 0 baseline, and not yet traced.

## Epoch costs: measured, and the first two cuts

`CS_ORG_EPOCH_STATS=1` now splits each epoch's render-thread time by segment and by phase of ORG's host frame
(`PersistentGraphHost::LastFrameTimings`, `RenderGraph::LastPersistentExecuteTimings`), and counts the queue
submissions the graph hands to DXVK. Baseline after the async work (µs per epoch, exterior):

| epoch | total | inputs (commit + join) | prepare | admission | record | other ORG | submissions |
|---|---|---|---|---|---|---|---|
| LLF light culling | 579 | 6 | 245 | 84 | 110 | 134 | 5 |
| Z-prepass | 822 | 159 | 216 | 86 | 221 | 140 | 5 |
| main opaque | 1,296 | 634 | 175 | 89 | 237 | 161 | 5 |
| shadow views | 1,354 | 677 | 219 | 95 | 213 | 150 | 5 |
| debug view | 300 | 0 | 59 | 63 | 64 | 114 | 3 |

What that showed:

-   **The inputs phase is mostly the join wait.** The colour join waits 0.52 ms and the shadow join 0.57 ms,
    because the worker's builds are slower than their windows. The table work (the next steps) removes this.
-   **ORG costs 0.55-0.65 ms per epoch** whatever the epoch does: prepare, admission and record dominate,
    because every epoch prepares and records every pass of every feature.
-   **`HostWait` is cheap** (25-50 µs). The GPU is not what holds the render thread at an epoch's start.
-   **The debug-view epoch ran every frame** on the hybrid path: its targets are imported there anyway, and their
    presence was the only gate. It is now gated on the debug-view toggle, which removes 0.3 ms of render thread and
    three submissions a frame.
-   **DXVK held the pre-epoch D3D11 work back.** It closes its command list only when the epoch's first
    submission is enqueued, after ORG has prepared and recorded. So the D3D11 commands issued just before an epoch
    sat on the CPU for 0.5-1 ms while the GPU ran dry. `ExecuteEpoch` now flushes the immediate context first
    (`CS_ORG_EARLY_FLUSH=0` restores the old behaviour); the enqueue then finds nothing pending, so the
    submission count is unchanged.

A/B with the same build (Riverwood windows, GPU idle attributed to `CS DCLF`, ms per frame): early flush off
1.23-2.17, on **0.56-1.08**; total idle 2.28-4.70 against 0.97-2.24. What remains is the main-opaque epoch
(ORG 0.26-0.35, the colour join 0.19-0.32); shadow and Z-prepass are down to 0.05-0.23.

## Less work per build: the eye on the GPU, a build cache, no per-frame object map

### The eye moved to the GPU

The object records and bone rows were made eye-relative on the CPU (`StoreRelative`, `PackBoneRows`). That is
why the shadow, Z-prepass and colour builds could not share them, and why the Z-prepass job needed a predicted
eye. Every DCLF draw already receives its epoch's `VS_PerFrame` (b12). A check on every epoch and every shadow view
confirmed, bit for bit, that its `CameraPosAdjust` (c40) is the eye the records were made relative to, and its
`CameraPreviousPosAdjust` (c41) the previous eye: 6,000 main epochs and 13,200 shadow views, 0 different.

-   Records and bone rows are absolute now. `Lighting.hlsl` subtracts `BonesPivot`/`PreviousBonesPivot`
    (c40/c41 of its own `VS_PerFrame`, now declared under `DCLF_BINDLESS` as well as `SKINNED`) with `precise`.
    That is the same single float subtraction the CPU did, so the depth and colour epochs agree to the bit.
-   `Skinned::GetBoneTransformMatrixBindless` takes the pivot and subtracts it per bone before the blend, as the
    engine's own `GetBoneTransformMatrix` does.
-   `Utility.hlsl` subtracts the drawing view's own `CameraPosAdjust`, so `DCLFEyeDelta` is no longer read (left
    zero). The shadow result is now closer to the engine's own, which subtracts the view's eye from the absolute
    matrix, than the old two-step delta was.
-   The bindless parity check makes the absolute record relative with the same subtraction before comparing:
    `bindless record parity OK` over 245M components.
-   A bindless build no longer reads the eye unless the parity checks are on (`BuildReadsEye`), so the eye is
    not an input of the Z-prepass job and cannot make it stale.

Gate: capture parity at its standing residue, `BuildDraws parity OK`, `0 claimed but not drawn` (the Z/colour
EQUAL test holds), every job `probe 0 differ`.

### A build cache for the (material, pipeline) pairs

`BuildCache` (`CS_DCLF_BUILD_CACHE`, default on, `=0` off), one per main epoch kind, keeps each pair's resolved
texture and sampler indices and packed PerMaterial groups across frames, and each pipeline's packed PerTechnique
groups and PerGeometry template. An entry keeps a copy of every input it was derived from and is reused only when
this build's inputs are byte-identical, so it cannot serve a stale value. `CS_DCLF_ASYNC=probe` checks that every
epoch: the worker's build uses the cache and the inline build does not, and their bytes are compared.

Three things the numbers corrected:

-   **The drifting IBL floats.** `RefreshMaterialPatch` writes them into every material each frame, which
    rebuilt every pair every frame (hit rate ~0%). The signature now leaves out the patched positions
    (`MainInputs::materialPatchedFloats`), and a reused PS group gets this frame's values written at their packed
    offsets (`PackedPositionOf`), exactly as `PackConstantGroup` would.
-   **The signature must be cheaper than what it saves.** Copying the 2.3 KB material record into each pair's
    signature cost more than the texture loops it replaced. `Tables::materialVersion`, a session-unique number
    set whenever a slot's record is written, apart from the patch, now stands in for it.
-   **Pipelines still rebuild in the exterior.** Their evaluated PerTechnique and PerGeometry constants change
    every frame (EyePosition, fog). There are only 48 of them, so they are left alone.

Result: 99.5-100% of pairs reused. The colour build takes 1.22 ms instead of 1.50, its join waits 0.21 ms instead
of 0.53, and the GPU idle during `main opaque inputs` fell from 0.19-0.32 to 0.04-0.09 ms per frame.

### No per-frame object map

The scene walk rebuilt a geometry-to-index hash map every frame for about 5,300 objects. The index now lives in
the object's `Tracked` entry (`objectStamp`, `objectId`), valid while it matches the walk's stamp. The accumulate
phase already holds the entry, and `FindObject` looks it up in the persistent tracked map. The accumulate phase got
0.1 ms faster; the walk did not. Its cost is reading scattered engine memory for every object (world and previous
world, bounds, properties, the parent chain), not the tables it writes: the same conclusion as "the transform
witness cannot pay for itself" above. Skipping unchanged objects would still have to read them to know.

### Where the frame's DCLF bubbles stand

After early flush, the debug-view fix, the GPU eye and the build cache, DCLF-attributed GPU idle is about 1 ms per
frame in the exterior, split roughly as follows:

-   the shadow epoch, 0.5-0.7 ms: ORG, plus the wait at `AfterShadowMaps` for the scene walk (1.1-1.6 ms on the
    worker) and the shadow build chained behind it;
-   ORG in the main-opaque epoch, 0.3-0.45 ms;
-   the Z-prepass, 0.1-0.2 ms.

ORG costs 0.55-0.65 ms of render thread per epoch whatever the epoch does, because every epoch prepares, admits
and records every pass of every feature. That is the next lever: epoch variants (plan step O2), and after them
worker-side recording.

## Epochs: each submission point runs only its own passes

Every ORG epoch used to prepare, admit, record and submit every pass of every feature. The shadow epoch prepared
LLF's culling passes and DCLF's colour passes, which then returned empty work. ORG now has host epochs
(`extern/OpenRenderGraph/docs/persistent-epochs.md`).

-   **One epoch-aware compile.** The persistent program compiles once over the whole frame, epochs in frame
    order, so scheduling and transient aliasing see every segment's lifetimes. Transients in different epochs
    can share memory.
-   **Per-epoch executables.** Each epoch also gets an executable compiled from its own passes over the same
    bindings and placements. It is what runs at that epoch, and its entry states come from the admission
    ledger, so an epoch that did not run is harmless.

This was chosen over one compile per epoch because that would have given each epoch its own alias plan (no
memory shared across epochs) and turned every resource crossing epochs into an external one.

CS's side (`CS_ORG_EPOCHS`, default on, `=0` for the old behaviour):

-   Each segment is an epoch, in frame order: shadow views, Z-prepass, light culling, main opaque, debug view.
-   Every pass declares its segment.
-   The Z-prepass has its own `cs.dclf.z.build-draws` and `cs.dclf.z.depth` instances, where before one
    `build-draws`/`main-opaque` pair served both the depth and colour segments.

One thing the first run found: ORG derives hazards in authored (registration) order. Across epochs that order
meant nothing, and the epoch edges then closed a cycle ("Captured dependency graph contains a cycle"). The
program now reassigns authored order by epoch rank, then registration order.

Measured, same build, `CS_ORG_EPOCHS=0` against on. ORG's render-thread µs per epoch, everything but the feature
inputs:

| epoch | before | after |
|---|---|---|
| LLF light culling | 638 | 491 |
| Z-prepass | 712 | 598 |
| main opaque | 675 | 564 |
| shadow views | 689 | 591 |

That is about 0.5 ms of render thread per frame. Admission, recording and commit fell with the pass count. The
rest of each epoch is fixed cost, which the split does not touch:

-   ORG's per-frame polls and statistics setup, about 100 µs;
-   the host upload pass, 44 µs every epoch;
-   retirement and command-list acquisition;
-   five queue submissions per epoch.

ORG's per-pass preparation is only 3-19 µs a pass. The idle trace does not separate the two runs beyond noise.

Gate: probe 0 differ for every job, `BuildDraws parity OK`, capture parity at its residue, `0 claimed but not
drawn`, and save/load and the live toggle clean. ORG's test suite passes in the CS embedding configuration,
including a new epoch test in `PersistentGraphTests`.

What remains per epoch is fixed ORG cost, which the next section addresses.

## Epochs that only submit

The goal: at each epoch point, the render thread takes a ready ticket, writes the epoch's late values, and hands
one submission to DXVK. Everything else runs earlier, on other threads. ORG's side of the contract is in
`extern/OpenRenderGraph/docs/persistent-epochs.md` (closed executions, tickets, latches, staged uploads).
It is on by default since 2026-09-24 (`CS_ORG_ASYNC_EPOCHS=0` returns to the synchronous path, which stays).

Render-thread µs per epoch, feature body included (`CS_ORG_EPOCH_STATS=1`, the "Epoch body" lines). Later
rows are the async path. Steady exterior; later runs vary by about ±20 µs.

| stage | LLF | Z-prepass | main opaque | shadow views |
|---|---|---|---|---|
| before (synchronous) | 197 | 428 | 689 | 1343 |
| one submission per epoch | 163 | 366 | 343 | 1083 |
| latched values (LLF) | 100 | | | |
| tickets | 20 | 163 | 204 | 1044 |
| payloads staged on the workers | 24 | 71 | 135 | 939 |
| scene walk from `Main::Draw` | 26 | 74 | 125 | 158 |
| lookups reused, claims on the worker | 20-22 | 53-64 | 100-126 | 82-120 |

In total, about 2.65 ms of render thread per frame fell to about 0.3 ms. GPU pass times did not change.

### The stages

-   **One submission per epoch.** `PreparedRhiExecutionBatch::Submit` folds its waits and signals into one
    queue submission. A new DXVK export, `dxvkEnqueueInteropSubmissions`, takes several submit infos into one
    `vkQueueSubmit2`. CS batches the hook's submissions inside an epoch (`CS_ORG_BATCH_SUBMIT=0` turns that off).
-   **Latched values.** BuildDraws and LLF read their per-epoch values from an `org::LatchBlock` region:
    counts, cull flags, the view-projection matrix, the dispatch size, and LLF's lights and matrices. A reused
    recording is correct without re-recording. Pass shapes (capacities, which grow to powers of two, and
    generations) replace the per-frame values in the passes' revisions. Invocation reuse went to about 99.9%.
-   **Closed executions.** Each epoch returns everything it touches to its home state, and admission is cached
    per epoch (`CS_ORG_CLOSED`, on with epochs). Vulkan synchronization validation was clean for 60 s.
-   **Tickets.** ORG's host thread prepares each epoch's ticket a frame ahead. The epoch's own cost on the render
    thread is the feature's commit, a revision check, the upload recording and the submit. The feature's
    completed-epoch timings reach the render thread through an atomic inbox.
-   **Staged payloads.** Each DCLF job stages its payload's uploads on the worker, straight into mapped upload
    pages (`StageMainPayload`, `StageShadowPayload`). The shadow job stages the record copies for the number
    of views it expects (last frame's count), and the commit uploads only what depends on the captured views.
    The commits' own small uploads go through one staged batch per commit (`CommitUploads`). The upload pass
    now records nothing in steady state.

A bug the staging found: ORG's `UploadManager` created its upload instance only on the first `UploadData`. Once
every DCLF upload was staged, none came, and the staged calls were silently dropped. DCLF's static objects
disappeared (trees, actors and terrain are native, so they stayed). The manager now creates the instance on the
first staged call too, and keeps the direct-recording setting for instances created later.

A second one, from synchronization validation with async epochs: when a commit staged its batch but its epoch
was never submitted, the next epoch's list carried both batches, and their copies into `cs.dclf.constants`
overlapped with no barrier between them. ORG now orders overlapping copies within a list with a barrier.
Validation is clean for 120 s, cell changes and the live toggle included.

A third one, found by the shadow-map readbacks (2026-09-24): with async epochs, **every shadow draw used pipeline 0**.
The far cascade read a mean depth of 0.02 with nothing clear (native: 0.60, 31% clear), and alpha-tested casters
lost their alpha test.
-   **Why:** a shadow view's inputs name a key slot, and BuildDraws resolves it through the view's row of the
    pipeline map in the latch. The shader reads that row at an offset into the whole latch block, but the commit
    wrote it relative to the slot's region. So every view read slot 0's rows.
-   **Why it only showed with async epochs:** synchronous epochs write slot 0 every few frames, and the map is
    stable, so the stale rows matched. Async epochs hand the ring's slots to epochs in turn, and the shadow epoch
    never took slot 0: its rows stayed zero. Every draw, faces included, got pipeline 0. Faces drawn with a
    one-stream pipeline read their other attributes as positions and covered the cascade.
-   **The fix:** `pipelineMapOffset` carries the slot's base, `LatchBlock::Offset(slot)`, as the dispatch's own
    `LatchOffset` does.
-   **Verification:** a readback of the far cascade's generated sequences, a temporary probe since removed, showed
    24 pipelines in the same draw distribution as synchronous epochs (face draws only on face pipelines).
    With both async switches on, the shadow maps read 0.603 / 0.605 with DCLF on against 0.602 / 0.604 off. The
    face-positions buffer was byte-identical to the snapshots in both modes, so the data was never at fault.

### The scene walk starts at `Main::Draw` (AE)

After the stages above, the largest render-thread cost was the join on the scene walk at `AfterShadowMaps`,
about 0.6 ms. The walk (about 1 ms) was kicked at `BeforeShadowMaps`, and only the shadow-map pass (about 0.4
ms) covered it.

`Main::Draw` (AE 36559, from the Ghidra database) runs, in order:

1.  an `NiUpdateData` update, `FUN_1406452c0`, its first call (`+0xD3`);
2.  the main camera's cull jobs (`DrawWorld_BuildSceneLists`);
3.  `CalculateAndDrawShadowCasterLights` and the first-person cull;
4.  `RenderShadowMaps` (`+0x2EC`, Deferred's hook).

From steps 1 to 4 is about 1.2 ms in the exterior. `DrawcallLimitFix::BeginSceneFrame` now runs from a hook on
the `+0xD3` call, after it returns. There the toggles begin the frame, and SceneStore's scene phase kicks the
walk. `CS_DCLF_EARLY_SCENE=0` restores the old kick point. The walk now overlaps the engine's cull jobs, which
slows it to about 1.2 ms. The join wait fell from 0.57 ms to about 0.02 ms, and whole-frame time did not change.

What the walk reads that changes between the hook and `BeforeShadowMaps`, each found by decompiling:

-   **`BSFadeNode::currentFade`.** Written by `BSFadeNode::OnVisible` in the cull. The walk's eligibility
    verdicts are cached for `kCandidateRefreshFrames` anyway, and the accumulate phase catches a stale one, so a
    frame of lag changes nothing.
-   **A billboard's world rotation.** `NiBillboardNode::OnVisible` turns it to the camera in the cull.
    Geometry under an `NiBillboardNode` now has its own ineligibility reason, `billboard`, and stays native.
    A census through the exterior, Dragonsreach and Riverwood found only effect-shader geometry under
    billboards (flares, fire jets, glow planes), which DCLF does not draw anyway: `billboard` never appears in
    the histogram. Tree LOD is not billboard geometry. It is `BSDistantTreeShader`, whose vertex shader places each
    instance with a fixed rotation about Z from its instance data (no camera facing; `DistantTree.hlsl`), and like all
    LOD it lives outside the loaded cells DCLF tracks.
-   **Animated texture transforms.** `BSLightingShaderPropertyFloatController::Update` moves a material's UV
    offset in that window. The async scene probe found it, one frame behind on `antsMiddle01`. The walk no
    longer stores the texture transform: the shadow build, kicked at `BeforeShadowMaps`, reads it off the
    material.
-   **Hidden bits (found 2026-09-24).** Three engine paths set `kHidden` on nodes the walk classifies, for part
    of this window, and then restore it:
    -   `ShadowSceneNode::OnVisible` hides a portal graph's always-render children and its shared node during
        the room traversal (in the cull jobs);
    -   `TESWaterReflections::Update` (`0x140520570`) hides the player's 3D while a cube-map reflection updates.
        `Main::Draw` calls it on the render thread between the cull jobs' `Begin` and `Finish`;
    -   `Main::Draw` hides the player's first-person skeleton right after the walk's hook, and keeps it hidden
        for every world view.

    The worker read the transient bit on the frames a reflection updated. The player's face shapes, which are
    classified every frame, left the tables for that frame (the "'MaleHeadNord' was drawn last frame but is not
    in this frame's tables" warnings, about 250 a run at Riverwood). `SceneStore::CaptureCullHiddenBits` now
    records those nodes' bits on the render thread before the kick, with the first-person skeleton as hidden,
    and `ClassifyFrame` reads a listed node's bit from there. After the fix: 0 head drops and 0 withheld passes
    handed back. Two `Salmon:0` drops a run remain, also only with the asynchronous walk and not yet traced.
    That is the class of problem "event-driven tables" ([dclf-event-driven-tables.md](./dclf-event-driven-tables.md))
    removes: a worker reading a scene the engine is modifying.

Only plain `BSTriShape` geometry is eligible, so the cull-time LOD selection of `BSLODMultiIndexTriShape` and its
relatives does not matter. The skin palette update keys on `gFrameCounter`, which only `Renderer::End` advances,
so updating from the earlier point stays idempotent within the frame. Every DCLF hook that could fire in the new
window (water reflections are drawn in it) is gated on the main camera's depth or deferred pass, so nothing on
the render thread reads the tables while the worker writes them.

**Since Phase 2 of [dclf-event-driven-tables.md](./dclf-event-driven-tables.md) (2026-09-24) the walk no longer runs
on the worker.** The delta walk evaluates, on the render thread at this hook, only what can have changed, so nothing
reads the scene while the cull modifies it. The fade is taken from events on its writers instead of from the node
in the middle of the cull. `CS_DCLF_SCENE_DELTA=0` restores the worker's walk this section describes.

The shadow-build probe still differs in about 1-6 of 300 frames, always at the alpha-tested texture-transform
block. The scroll also moves during the shadow pass itself, so builds made at different moments read different
offsets. The scene probe used to show the same thing at about 1 in 300 frames.

### Smaller cuts

-   **The ticket's upload list.** Resetting and beginning it (about 6 µs of driver work) moved to the host
    thread, which already waits for the slot. What remains on the render thread is the copies themselves:
    `vkCmdCopyBuffer` measured 0.47 µs each, about 15 per epoch, plus a 2.4 µs barrier.
-   **Material lookups.** `RefreshMaterialLookups` runs in the shadow, Z and colour commits. An unchanged
    material (same views, same written mask, no GpuTextures eviction since) now keeps its indices. It is
    resolved again every `GpuTextures::kRestampFrames` (32) frames, because resolving is what keeps an entry
    from eviction (`kEvictFrames` 600). The cost went from 18 to 5 µs.
-   **Shadow claims.** The shadow job builds each mode's claim set. The epoch publishes it when it drew the
    job's build, and otherwise builds it as before. This took 48 µs per frame off the render thread.
-   **Material rows on events** (added later). The kept build no longer reads every held material (about 800) each
    build. A slot's row is written when the slot is taken, while it waits (texture not resolved, or past the table's
    capacity), and while its material's texture transform moves: the controllers' hook
    (`BSLightingShaderPropertyFloatController::Update`, types above `0x13`) feeds a second queue
    (`MaterialSources::DrainShadowTransformChanges`), and a slot stays watched while its two transform buffers differ or
    for 2 builds after the last event, as the main pass's watch does. A change of the lookups' generation or of the
    capacity writes every slot. The texture owners go to the payload as one shared bundle, rebuilt only when a slot's
    owner changes. Persistent parity compares the rows by value on every input: 0 differ.
-   **A claim stands only while its view is drawn** (added later). A view the epoch cannot draw (setup, tables or
    pipelines not ready) gives its mode's claims back (`GiveBackShadowMode`), so the engine draws that mode's
    casters from the next frame until an epoch draws it again. The report's `<- HOLES` counts only the views whose
    casters the engine withheld that frame (`notReadyWithheld`). The 18 views not ready after a load (the first two
    frames, before any shadow pipeline exists) withhold nothing: a load clears the claims. So they were never holes.

### Not yet at the gate

The target was under 50 µs of render thread per epoch. LLF is there, and the others are not:

-   **Upload recording:** about 20 µs per epoch of driver command recording. Moving it off the render thread
    needs either the producers to record their batches' copies themselves, or a latched copy table run by a
    pre-recorded GPU copy dispatch.
-   **Worker joins:** 0-60 µs, varying by run. The colour and shadow jobs finish just in time on average. Track
    T's work (persistent object slots, GPU-driven candidates) shortens them.
-   **The skip set (`drawnFrame`):** 9 µs per main commit, a geometry-keyed map the render thread updates. It
    has four readers across frames.
-   **ORG's own share:** about 10 µs (checks, releases, submit).

Gates met on this path:

-   probe 0 differ for the colour, Z-prepass and scene jobs;
-   BuildDraws and decal parity OK;
-   capture parity at its known residue;
-   0 claimed but not drawn;
-   save/load and the toggle clean;
-   0 ticket waits in steady state;
-   ORG's tests pass.

### A regression: every ticket stale (found 2026-10-01)

Three passes added later broke the "0 ticket waits" gate:
-   **TreeWind** runs in every drawing epoch. Its revision held `treeCommits`, which every commit bumped.
-   **FadeState** runs in the depth epoch. Its revision held `fadeCommits`, bumped in the same way.
-   **The feedback copy** runs in the colour epoch and had no revision at all, which makes a ticket uncheckable.

Each did this on purpose: its per-frame values (the counts, the scene frame, the tree inputs, the log base, the
feedback slot the colour commit armed) were baked into the prepared invocation. So the shadow, depth and colour tickets
were always stale. The host prepared each one again on the commit and the render thread waited for it
(`ORG.Host.ReprepareStaleTicket`, `ORG.Host.WaitTicket`): about 2.6 ms a frame. DCLF's wait tracer counts only DCLF's
own joins, so it never showed. A Tracy capture found it.

**The rule this restores:** a pass's prepared invocation holds only what its recording depends on. Everything a commit
knows goes into a buffer the commit uploads.
-   **TreeWind** reads `TreeWindFrameRow` (the counts, the frame and its inputs; `cs.dclf.tree-frame`), which every
    commit uploads.
-   **FadeState** reads the root count, scene frame, log base and write-all flag from its `FadeFrame` row, which the
    depth commit now uploads on every commit.
-   **Both** dispatch every slot the buffers hold, and the shaders stop at the row's count. Their revisions are the
    scene buffers' layout (`SceneBuffers::layout`, bumped by every growth, since a growth gives the buffers new views),
    plus the depth shape for FadeState.
-   **The feedback copy** claims a free slot in its own preparation, on the host thread, and so does each ticket
    prepared again. Its revision is the segment and the layout.
    -   Which frame a copy holds is now a stamp FadeStateCS writes into the changes' header (word 1).
    -   The drain reads the stamp. A frame missing between two copies means that frame's changes reached no copy, and
        the next depth commit sends every write-back root again (`fadeCopyMissed`).
    -   An abandoned copy (a discarded ticket) only frees its slot: the ticket prepared in its place claims another.
    -   Growing a slot's staging claims the slot first (`Growing`), so a preparation cannot take it mid-resize.
-   **A latent bug fixed on the way:** the fade-changes growth called `ResizeBytes` on a structured buffer, which
    throws. It had never grown before; the first version of this change abandoned a copy on every preparation that was
    discarded, which made it grow.

Measured (Riverwood, camera turning; d-base against d-tickets, the second a 7 s capture):
-   Tickets prepared again: three of five epochs every frame, down to about 1% of epochs (4-6 in 600).
-   Ticket waits: 0.
-   Render thread in the epochs (`CS.ORG.ExecuteEpoch`): 4.45 down to 1.67 ms a frame.
-   The host thread's preparation and recording: 5.0 down to 2.6 ms a frame.
-   Frame time: mean 17.9 to 12.4 ms, p95 26.5 to 14.9 ms, p99 30.4 to 16.4 ms.
-   Parity unchanged: fade state against the port exact or within rounding, tree wind OK, copy-back 300 of 300
    frames decoded, holes 0.

`CS_ORG_EPOCH_STATS=1` reports the host's count of tickets prepared again ("prepared again after the feature's inputs
changed"). It belongs in every validation that touches a pass.

## Holes on alpha-tested objects

The symptom: alpha-tested objects (roofs, foliage, thickets, ferns) vanished for a frame at a time, now and
then when the camera was still and often while it moved. When DCLF's uploads were broken and it drew nothing,
the same objects instead popped into the native frame. That pointed at ownership (which of the two draws an
object in a given frame) rather than at a draw going wrong.

### The instruments

-   **`CS_DCLF_SET_PARITY=1`, per object, every frame.** BuildDraws keeps two bits in each object's
    visibility word, below a 28-bit stamp: "the depth segment appended a draw" and "the colour segment
    appended a draw". Decals mark their colour draw too. After the colour epoch the words are copied through
    a D3D11 view into a staging ring. A few frames later each object is classified against a snapshot of the
    CPU side: each build's per-object state (`MainPayload::objectState`: drawable, cull-only, or its skip
    reason), and whether the object was claimed and withheld from the native loop. The report counts
    depth-without-colour, colour-without-depth, and withheld-but-drawn-by-nobody, with named samples.
-   **The hole report, always on.** "Claimed but not drawn" used to be a snapshot of the report's last frame,
    so intermittent holes never showed. It now accumulates over the interval and logs samples. Each sample
    gives the object's reason: SceneStore now records the accumulate phase's per-frame verdict
    (`Tracked::accumulateReason`, `SceneStore::ReasonThisFrame`).
-   **`CS_DCLF_TEST_TURN="<start>:<end>:<degrees per frame>"`.** Turns the player every frame, so a
    scripted run has steady camera motion. The standard script had none.

### What they found

With the camera turning (1.5° per frame) in all three scenes:

-   **Depth and colour agree exactly.** Depth without colour and colour without depth were 0 in every
    interval.
-   **Holes were frequent,** and almost all had one cause, `alpha-test-state`:

| scene, turning | holes per 300 frames | frames with holes | reason |
|---|---|---|---|
| exterior | 685 | 192 | `alpha-test-state` 683 |
| Dragonsreach | 397 | 100 | `alpha-test-state` 397 |
| Riverwood | 1061 | 244 | `alpha-test-state` 1061 |

The accumulate phase left a pass native when it sat in an alpha-test batch list (1, 3, 4) and its registered
technique lacked DoAlphaTest. The claims, though, are last frame's colour draws, and withholding happens at
registration. So when an object DCLF drew last frame came in this frame without the bit, the native loop had
already been told to leave it, and DCLF then declined it. That left a hole for a frame, and the reverse
flip let the native path draw it again.

### Why the bit comes and goes

Decompiled (engine notes, batch renderer): `GetRenderPasses` sets DoAlphaTest for alpha-tested geometry only
while the early-Z global is set, or the object is blended, or its `alpha * fade` for the camera being
registered is below 1. It keeps the build on the shared property until that state changes. The flicker was
seen on near objects too, not only at fade distance, so which rebuild decides the bit in a given frame is not
settled. The fix below does not depend on it: what the native frame shows does not depend on the bit, since
the list alone decides alpha testing, and the main pass tests EQUAL against an alpha-tested prepass.

### The fixes

-   **`DrawnPassDescriptor`.** Every pass in lists 1, 3 and 4 is taken with DoAlphaTest. This applies at
    both capture sites, the capture comparison and capture parity's native side. `DO_ALPHA_TEST` adds only
    the discard, so coverage matches the native frame, and these objects are now DCLF's in every frame.
    The `alpha-test-state` reason is gone.
-   **Fading is decided once, at registration.** `PassCapture::FadingAtRegistration` feeds both the
    withholding (a fading object is not withheld) and the accumulate phase's fading verdict
    (`AccumulatedPass::fading`), where the verdict used to re-read the fade node later in the frame.
-   **The hand-back.** At EarlyPrepass, once the tables and pipeline lookups exist and before the native
    depth and main passes draw, every pass withheld this frame whose object DCLF cannot draw goes back to its
    batch renderer through the original `RegisterPass` (`PassCapture::HandBackUndrawable`). That covers no
    bindings this frame, and a pipeline variant still compiling (three such cases in the first run after the
    normalization: `MineOreIron04`, `NorTowerRuinsRamp01`, `DeadSalmon13`). The hole detectors count only
    what was actually withheld and not handed back (`PassCapture::WithheldThisFrame`).

After the fixes, the same turning runs and a probe run with the toggle and save show 0 holes in every
interval, with depth and colour still in exact agreement. The user confirmed the flicker is gone. Capture
parity stays at its known residue.

The shadow-build probe differs more often while turning (up to about 90 of 300 frames). The differing
bytes are always in the alpha-tested casters' texture-transform blocks: animated UVs read at slightly
different moments by the two builds (see "The scene walk starts at `Main::Draw`").

Still open: the hand-back runs a lookup for each withheld pass every frame (about 1700 in the exterior). It
has not been measured against the render-thread budget.

## Trees and actors

The objects DCLF was drawing as "trees" were the TreeAnim technique: shrubs, ferns and other animated
foliage. Real trees and actors still went through the native passes. `CS_DCLF_NATIVE_PROBE=1` found why.
It records every Lighting draw the main pass still issues natively, which under static ownership is exactly
what DCLF does not cover. For each draw it records DCLF's verdict, the base object's form type, the skin
shape and the pass's LODMode, and samples the whole ancestor chain.

Native Lighting draws per frame before this change:

| | exterior | Dragonsreach | Riverwood |
| --- | --- | --- | --- |
| TREE | 190 | - | 83 |
| actors | 17 | 100 | 110 |

### Why they stayed native

-   **Every tree hangs under an `NiSwitchNode`** (`BSTreeNode` > `BSMultiBoundNode 'FadeNode Anim'` >
    `NiSwitchNode` > shapes), and harvestable flora (fish buckets, mushrooms on logs) under another. The
    tracker rejected anything under a switch node as `unsupported-parent`.
-   **Most trees are skins of two or three partitions**, one per LOD level (LOD bytes 1/0, or 1/2/0 with
    TreeAnim). The skinned path took exactly one partition, so the rest were `skin-shape`.
-   **Actor bodies and armour are `BSDismemberSkinInstance`s** of one to three partitions: `skin-shape`.
-   **Anything under an actor was rejected by a blanket Phase 1 rule** (`actor`), recorded as an
    assumption and never measured. Exterior actors also hang under the cell's Actor node, which DCLF did not
    walk.
-   Heads, hair, eyes, mouths and brows are `BSDynamicTriShape`s (`not-trishape`), whose vertices the
    engine rewrites every frame. They stay native.

### What the engine does (engine notes, "Skin partitions" and "Switch nodes")

-   A skinned draw is one `DrawIndexed` per partition the engine draws, each binding that partition's own
    TriShape. All of them use the whole skin's palette. Partition *i* is drawn when a constant table at
    `0x14202a030`, indexed by the pass's `LODMode` and the partition's LOD byte, says so. A
    `BSDismemberSkinInstance` first skips the partitions whose flag is clear.
-   A pass's `LODMode` is the fade node's LOD level (`+0x152 & 0xF`) for `kMeshLOD` geometry and 3 otherwise.
    The main-pass and shadow-pass builders use the same level.
-   While a `kMeshLOD` fade node's LOD state is not settled, `GetRenderPasses` adds a second, single-level
    copy of every pass (hint 10) for the cross-fade.
-   `NiSwitchNode::OnVisible` culls `children[index]` alone, and brings that child up to date there when it
    became the selected one after the last update pass.

### What was built

-   **Switch nodes** (`CS_DCLF_SWITCH_NODES`, reason `switch`). A switch on the path marks the leaf, and
    `ClassifyFrame` checks every frame that each switch selects the leaf's branch and that the branch is
    current (`SceneStore::SwitchSelects`). The scene phase re-takes this verdict every frame instead of
    caching it for 64, because the shadow views draw what that phase admits. The switch's own fields are
    read at their SE/AE offsets (`SceneStore::ReadSwitch`): CommonLib declares them after `NiNode`, whose
    declared size in a multi-runtime build is VR's, so its members read the wrong memory. The first version
    read garbage indices and rejected every tree.
-   **Skins of several partitions** (`CS_DCLF_SKIN_PARTITIONS`, needs `CS_DCLF_SKINNED`). A skin is one
    object whose draw writes one sequence per partition the engine draws:
    -   `Tables::skinPartitions` holds the mask (bit *i* = partition *i*). The scene phase sets it from the fade
        node for the shadow views, and the accumulate phase replaces it from the registered pass
        (`AccumulatedPass::lodRow`).
    -   Every partition's TriShape has a geometry slot, and the slots are linked every frame
        (`GeometryRecord::nextPartition`, packed into `GeometryDraw`'s spare word).
    -   `DrawInput` grew a word for the mask (44 bytes), and BuildDrawsCS walks the links.
    -   An object stays one record, one visibility word and one culling verdict; only its draws multiply.
    -   A mask of 0 (the engine draws none) is `hidden`. Up to eight partitions sharing the first one's
        vertex layout are eligible.
    -   The CPU templates, BuildDraws parity (sorted by object, then index buffer), the draw cap and draw
        parity all follow the same walk. Draw parity takes each native draw's expected partitions from that
        draw's own pass, so the hint-10 cross-fade copies are checked against what they really draw.
-   **The LOD cross-fade is a fade.** `FadingAtRegistration` includes the unsettled `kMeshLOD` state, so an
    object in a cross-fade is neither withheld nor drawn by DCLF until it ends, like any other fade. Before
    this, withholding would have swallowed the hint-10 copies too.
    Superseded by the split ("LOD cross-fades: DCLF keeps the object, the copy stays native"): now only the
    hint-10 copy is native.
-   **Actors** (`CS_DCLF_ACTORS`). The Actor cell node is always walked, and the toggle is `ClassifyFrame`'s
    actor rule, so it stays live. The FacegenRGBTint technique (body skin) is supported: it adds only
    `TintColor`, a PerMaterial constant the material evaluation already captures.

### Results

Same three cells, turning, with `CS_DCLF_ASYNC=probe`, BuildDraws, capture and set parity:

| | exterior | Dragonsreach | Riverwood (steady) |
| --- | --- | --- | --- |
| native TREE draws a frame | 190 -> 18 | - | 188 -> 0 |
| native actor draws a frame | 17 -> 17 | 100 -> 22 | 110 -> 11 |
| claims | ~1465 -> 1546 | 920 -> 1099 | 1614 |

-   **Gates:**
    -   BuildDraws parity OK in every interval;
    -   draw parity 0 differing, and bone palette parity 0 differing;
    -   set parity: no depth/colour disagreement;
    -   0 holes, and 0 claimed but not drawn;
    -   colour and Z-prepass worker builds 0 differing;
    -   capture parity at its known residue (VS PerTechnique 13, one ulp, at the Dragonsreach cell change; the
        pre-change run shows the same 1,342).
-   The shadow-build probe differs in about as many frames as before (110 against 88 in the exterior), in
    the same texture-transform blocks.

What stays native, by name:

-   The dynamic head parts.
-   Actors and trees mid-fade.
-   **The player's own body** (about 6 draws). The engine sets `kHidden` on the player's third-person
    `Skeleton.nif` while `Main::Draw` begins, which is when the scene walk reads it, and clears it before the
    cull. The native loop draws it, and DCLF's verdict is `hidden`. That is correct and leaves no hole; it
    only means those draws are not DCLF's.
-   Distant LOD (`no-ref`, under `LODRoot`).

## Fading objects

An object fading in or out used to go back to the native passes for the whole fade. Most fades do not need
that. The engine draws them in the ordinary opaque groups (hints 0, 11 and 15) with the screen-door mask:
pass descriptor `AdditionalAlphaMask`, with the fade in `MaterialData.z`. `Lighting.hlsl` discards against a
4x4 screen pattern, so the object stays opaque, and the Z-prepass, which keeps the alpha test, dithers the
same pixels.

-   **`CS_DCLF_FADING`** (default on, live toggle) makes those passes eligible. `MaterialData.z` is
    `property.alpha`, which the material evaluation already resamples every frame at Prepass
    (`RefreshFrameConstants`).
-   **What stays native** (`PassCapture::FadingAtRegistration`):
    -   blended fades (accumulation hint 9, drawn with the transparent objects after the composite);
    -   every hint-10 pass: the stencil-dithered fade, and the LOD cross-fade's copy of the old level ("LOD
        cross-fades" below);
    -   fading decals (hints 2 and 3). Keeping these native is a precaution, not a measurement: the decal
        probe's state mismatches turned out not to depend on the fade.
-   Report line: `[DCLF] fading: N screen-door fading objects drawn by DCLF over M frames`.

### The flicker at a tree's LOD distances

Trees (trunks as well as foliage) disappeared for one frame at fixed distances, moving towards them or
away. Culling was not the cause: the flicker stayed with `CS_DCLF_CULL=off`.

`CS_DCLF_TREE_TRACE=1` found it. It follows every geometry of a TREE reference through the frame:
registered, withheld, accumulated, given bindings, drawn natively (`SetupGeometry`), drawn by DCLF. It also
samples the geometry before the scene walk and after the accumulate phase. On a 40-unit-a-frame flight,
100 to 230 accumulated tree geometries per 300 frames were drawn by nobody, always in the first frame of a
LOD cross-fade:

-   The fade node's state went from settled (`a4`) to crossing (`c4`) in the cull. At registration the pass
    was already fading, so it was not withheld and the native loop was meant to draw it.
-   The accumulate phase gave it no bindings (`fading`), so DCLF did not draw it.
-   `SkipNativePass`, the hybrid rule on the `RenderPassImmediately` call sites, then skipped the native
    draw. That rule skips any object the colour epoch drew *last frame* and that has a record this
    frame. It never asked whether DCLF could draw the object *this* frame.

The fix: `SkipNativePass` keeps the native draw of anything `DrawcallLimitFix::DrawableThisFrame` rejects,
meaning no record with bindings, or no built pipeline. That is the same test `HandBackUndrawable` uses, and
the two now share it. The report line is `hybrid (last frame): N native passes were kept because DCLF could
not draw their object this frame`. After the fix, on the same flight:

| per 300 frames | before | after |
| --- | --- | --- |
| accumulated tree geometries drawn by nobody | 98-228 | 0 |
| one-frame gaps of an accumulated tree | 98-228 | 0 |

The trace also rules out the other suspects: 0 switch changes and 0 bone or world moves between the walk
and the draw, and 0 stale record transforms. What it still shows is expected:

-   **Drawn twice**, 270-890 object-frames per interval: the frame an object becomes DCLF's again (its fade
    ends). The claims are last frame's draws, so the native loop draws it once more. It is opaque at the
    same depth, so this costs a draw and shows nothing.
-   **One-frame gaps the engine made** (0-12 per interval): small foliage the engine's own cull did not
    register for a frame (`reg 0`), with nothing withheld.

**Still open:** with occlusion culling on, the set-parity gap detector counts 94-1,572 one-frame phase-2
rejections per 300 frames on the same flight, against none with `CS_DCLF_CULL=frustum`. Whether those
objects were really hidden for that frame is not yet measured.

### LOD cross-fades: DCLF keeps the object, the copy stays native

The flight above is dominated by trees changing LOD level. Until now each one went back to the native loop for
the whole crossing, which is two hand-offs per tree per crossing. A hand-off is where the one-frame flicker
lived, and where the double draws are.

**What the engine does** (engine notes, "Skin partitions", from `BSLightingShader::SetupGeometry`):

-   The object's own pass is untouched. It draws the new level exactly like a settled object.
-   The hint-10 copy of the old level is different in two ways:
    -   it is drawn with a stencil dither: stencil mode `0xB`, reference `int(fade * 31)`;
    -   its `MaterialData.z` is scaled by the fade node's cross-fade factor (`+0x14C`).
-   So the copy cannot be one draw merged with the object's own, but the object's own pass needs nothing
    new.

**The split** (`CS_DCLF_LOD_CROSSFADE`, default on, live):

-   **`PassCapture::FadingAtRegistration`:** every hint-10 pass is native, and a crossing no longer makes the
    object's own pass a fade. That pass is claimed, withheld and drawn by DCLF as before.
-   **`SceneStore::AddAccumulatedPass`:** a hint-10 pass never stands for an object that has a pass of its own,
    so the partition mask is the new level's.
-   **`SkipNativePass`:** never skips a pass `FadingAtRegistration` gives the native loop. Otherwise the copy
    would be dropped, because DCLF drew the object last frame. The rule is general: the skip only applies to
    passes DCLF models.
-   **Capture parity:** does not compare such a pass against the object. It counts it as a "native-only pass
    of a DCLF object".

**Results** on the same flight (tree trace, culling off):

-   Native tree draws: 1,666-10,248 object-frames per 300 frames before; 0-2,758 after, and the 2,758 is the
    first interval, before the claims settle.
-   Tree object-frames with a native copy: 308-3,522 per interval, and every one of them DCLF's otherwise.
-   0 drawn by nobody, 0 holes, 0 claimed but not drawn.

**Gates** (async probe, BuildDraws, capture and set parity, on the flight):

-   BuildDraws parity OK, draw parity 0 differing, bone palettes 0 differing.
-   Set parity: no disagreement, and nothing withheld and drawn by nobody.
-   Capture parity: 29-112 mismatches per interval in `PS PerMaterial 22`, `VS PerMaterial 11` and
    `VS PerGeometry 5`. The same flight with `CS_DCLF_LOD_CROSSFADE=0` shows the same three variables at
    24-95, so this is a residue of the flight, not of the split. It is not yet explained.

**What the copy is** (reverse engineered later; this supersedes "the old level" above):

-   **The level state** (`FUN_14147a430`, the only writer): `+0x152` low nibble is the level L (1-3, 3 nearest), the high
    nibble last call's; `+0x153 & 0x70` is `0x20` when settled. A crossing always pairs L with L+1, and `+0x14C` is L+1's
    weight. Going up, L stays and `+0x14C` rises from 0 to 1, then L increments. Going down, L drops at once and
    `+0x14C` falls from 1 to 0. Not seen for 20 frames (`+0x13c`), or with `DAT_142032dfc` off, the level is set with no
    crossing.
-   **The pass** (`GetRenderPasses`, AE `1414adfb0`): for a `kMeshLOD` geometry whose root is crossing, each of its
    passes gets a copy with the same technique, hint 10 and `+0x1e = L | 0x80`. The object's own pass has `+0x1e = L`.
    As a partition row (`LodRowOf`: index, plus 4 for `0x80`), the object's own pass draws the partitions of LOD byte
    below L, and the copy exactly those of LOD byte L: the detail level L+1 adds, not the old level. For a geometry
    without skin partitions the row selects nothing different, so the copy redraws the same mesh at the same depth,
    with fewer alpha-tested texels.
-   **Its state** (`SetupGeometry`): `MaterialData.z` is the alpha times `+0x14C`, and the depth-stencil state is stencil
    mode `0xB` with reference `int(+0x14C * 31)` (the scale is 31.0 at `1419de7e0`). Mode `0xB` (the state table,
    `FUN_140e657b0`) is stencil on, masks `0xFF`, every op KEEP, function GREATER: the copy draws where its reference is
    above the stencil value. A hint-10 pass without `0x80` (a fade under flag bit 19) uses `currentFade` (`+0x130`)
    instead.
-   **The stencil at that point**: `FUN_1414ccb30` clears depth and stencil to 0 at the start of the main scene. In an
    interior it then draws each room's portal geometry with stencil mode 1 (ALWAYS, REPLACE) and reference room + 1.
    So the test meets 0 in an exterior and the room index in an interior, and it is a bound, not a 32-level dither: the
    visible fade of an alpha-tested copy is the alpha test on the scaled alpha.
-   **The depth pass** draws no copy: `GetRenderPasses` adds one utility (depth) pass per geometry, none for the copy.

**Measured** (Riverwood, 60 s straight at 40 units a frame and 60 s circling, with a temporary census of the level
writer): 2,357 `kMeshLOD` members, none of them a skin with a partition of LOD byte 1 or 2, and 0 level changes and 0
crossings started anywhere, from about 1,000 level-writer calls a frame. Setting the non-tree distances to 3000 and 1500
in memory (`setini`) changed nothing either. Cross-fades look like an unused feature in SE.

**So DCLF keeps crossing roots stood in** (`StandIn` no longer hands them to the engine; the fade service steps them)
and draws no copy. `CS_DCLF_LOD_CROSSFADE` and the `SkipNativePass` / capture-parity rules above stay for the engine's
own objects.

## Material records from their sources

Capture parity on a fast flight still found stale material records: `TexcoordOffset` (VS PerMaterial 11)
on scrolling materials, t11 on character-lit materials, and `ParallaxOccData` on some PBR materials. The
cache assumed a record is fixed apart from a learned set of frame-global PS floats.

A first attempt inferred more of the same by comparing values. It marked a material "volatile" when the
rolling validator happened to find it stale, and adopted a float as frame-global when two materials held
the same value. It adopted a coincidental `ParallaxOccData` 0.7 into every material. After a time-of-day
step it re-evaluated 190-260 materials a frame. It was reverted.

What replaced it is a deterministic account of every input. The engine notes' table ("SetupMaterial: where
every material constant comes from") lists each constant and texture `BSLightingShader::SetupMaterial`
writes, and where it comes from. `MaterialSources` keeps each kind of input current by its own rule:

| Input | Rule | Where |
| --- | --- | --- |
| The material's own fields | re-evaluated when something writes the material | `ProcessMaterialWrites`, end of the accumulate phase |
| `TexcoordOffset` | computed every frame from the material's two texture-transform buffers at the engine's selector | `RefreshTextureTransforms`, end of the accumulate phase (the Z-prepass reads it) |
| Shader object and globals: `IBLParams`, PS 6, `SnowRimLightParameters`, `CharacterLightParams`, `LODTexParams.z`, `LandscapeTexture5to6IsSnow.zw` | one live evaluation per signature (the pass flags that decide which of them are written), copied into every record with that signature | `RefreshFrameMaterials`, Prepass |
| t11 (the character light's render target) | with the frame components; a change gives the record a new version | the same |

**The write events.** Hooks push the written material into a bounded lock-free ring. Every producer is a
hook, on any thread. The accumulate phase drains the ring and handles each written material:

-   a slot drawn this frame is re-evaluated;
-   any other slot of it is dropped, as is its cache entry, so its next use evaluates it afresh.

An overflowing ring treats every material as written. The producers:

-   `BSLightingShaderPropertyFloatController::Update` and `ColorController::Update` (vtable slot 0x27),
    except the types that write the property rather than the material, and the texture-transform types.
    The UShort controller's `Update` is `ret`.
-   `CopyMembers`, `OnLoadTextureSet`, `ClearTextures` and `ReceiveValuesFromRootMaterial` on the 14
    engine material vtables. The vtable IDs were checked against the decompiled functions.
-   CS's own writers, which call `MaterialSources::NoteWritten`: the PBR materials' overrides and their data
    mutators (`ApplyTextureSetData`, `ApplyMaterialObjectData`, `ClearMaterialObjectData`, `LoadBinary`),
    and `TESObjectLAND_SetupMaterial`, which writes the landscape material after `SetMaterial`'s copy.
    `ApplyMaterialObjectData` was the `ParallaxOccData` writer: TruePBR stores the projected material's
    roughness and specular level there.

**The build** repacks the frame-sourced floats into a reused (material, pipeline) group without a new
version: PS as before, and now VS (`materialPatchedVSFloats`) for `TexcoordOffset`. The colour kick and the
Z-prepass kick refresh the material lookups first (`RefreshMaterialLookups` outside an epoch: a view never
imported waits for the epoch's refresh), and the epoch refreshes them only after taking the job, so a changed
t11 index or a joining member does not leave a pre-built job stale.

**The alarm.** The rolling validator (8 records a frame) and `CS_DCLF_MATERIAL_CACHE=probe` compare a
record with a live evaluation outside its frame-sourced components. They only report
(`STALE material record ...: a writer the material events do not cover`). They do not repair: repairing is
what hid the missing events before.

**Found on the way:** the shadow build read the texture transform's buffer 0. It now reads the buffer the
frame reads (`textureTransformCurrentBuffer`), as `BSUtilityShader::SetupMaterial` does.

**Results:**

-   The flight gate: capture parity 0 material mismatches in every interval (before: 29-112 an interval in
    `TexcoordOffset`, t11 and `ParallaxOccData`), 0 stale-material alarms. BuildDraws parity OK, draw parity
    0, holes 0.
-   Z-prepass staleness is back to its level before this work (64-188 inline builds an interval, from the
    290 of 300 the per-frame t11 caused).
-   Three `set gamehour` steps with ownership off, every draw compared: about 483,000 draws checked an
    interval, 0 mismatched.
-   Capture parity's "MISMATCH" label on the flight now comes only from "untracked eligible", which is older
    and separate (see "Scene events before the walk").
-   `WindTimers.y` is not compared: `SetupGeometry` copies a tree node's current timer over its previous one
    after every native draw, and `Lighting.hlsl` never reads the value.

## Scene events before the walk

Capture parity counted 23-1,365 "untracked eligible" draws an interval on the flight, only while cells
loaded. These are native draws of statically eligible geometry under a known category node that DCLF did not
track. The `ObjectLODRoot` trees are counted separately, as "outside the tracked category nodes".

**The trace.** Capture parity now follows each such geometry until it is tracked. It records when it was
first drawn untracked, on how many frames, and what tracked it in the end: the attach event, a category node
the refresh found new, or the rescan after a load. `SceneStore` stamps the frame and the source on every new
tracked entry, and the frame and the refresh's cause on every new category node. On the flight, every one was
drawn untracked on exactly one frame and tracked by its attach event at that frame's Present. None stayed
untracked, and no category node was found late.

**The cause.** The attach events were applied only at Present (`DrawcallLimitFix::Reset`). A cell attached
during the world update was drawn natively on its first frame and joined the tables on the next. The native
loop drew it, so there was no hole, but it was a handoff to native.

**The fix.** `BeginSceneFrame` applies the queued events (`SceneStore::ProcessEvents`) before the walk. The
scene graph is final from `Main::Draw` on. Nothing of DCLF's is in flight there, as after `Reset`. `Reset`
still applies them every Present, in menus too, so the queue cannot grow while the world is not rendered.

**Result:** 0 untracked eligible in every interval of the flight gate, with BuildDraws parity OK, draw
parity 0 and holes 0.

**Found on the way** (the first two are recorded for their PRs in
[bugs-found-by-parity.md](./bugs-found-by-parity.md)):

-   **A loader thread writes materials that are being drawn.** On some runs, the first interval had 8-13
    `ParallaxOccData` mismatches (DCLF 1, native 0.7), each on one frame. TruePBR's `TESBoundObject::Clone3D`
    hook applies a static's MATO in place, on the loader thread, to the materials of the freshly cloned model.
    Those materials are shared with references already on screen, and the previous owner is 0, so they are
    not forked. The write lands between the accumulate phase's drain and the native draw: native reads it
    mid-frame, and DCLF picks it up from the next frame's drain.

    Capture parity follows each mismatched material into the next frames' drains ("mismatched materials: ...
    written after its last mismatch"). A deterministic fix belongs in TruePBR: fork a material another
    property already uses before writing into it, rather than writing into it in place from a loader thread.
-   **`IsBeastRace` in the permutation buffer is sticky natively.** Subsurface Scattering's `SetupGeometry`
    hook sets or clears it only for face draws, so every later draw (bodies, hands, flora) inherits the last
    face's value. That is capture parity's occasional "permutation parity ... extra 4". DCLF's 0 is the
    intended value for those draws; the fix belongs in Subsurface Scattering (clear the bit for every other
    deferred lighting draw).
-   The materials line counts the last frame only, like the other "(last frame)" lines, and now says so.

## First person: the Z-prepass drew the world with the first-person camera

In first person nothing DCLF draws showed. Terrain, water, grass, distant trees and the first-person model
were drawn, and the objects DCLF draws were missing. Their shadows were drawn (the shadow epoch has its own
cameras). Every gate passed: capture parity, draw parity, 0 holes. So DCLF's draws executed and put nothing
on screen.

**The cause.** The Z-prepass ran where the engine's depth pass returns (`Main_RenderDepth`, `Main::Draw`
`+0x395`). In first person, the depth pass ends by drawing the first-person model with the first-person
camera and does not restore the world camera (engine notes: "The depth pass and first person").

-   **The Z-prepass captured that camera.** Its eye was `(0, 0, 120.5)` against the main pass's
    `(17120, -47226, 9.7)`, and its VS_PerFrame projection had the first-person near plane. It drew every
    object relative to the player's head, off screen.
-   **The colour epoch replays the Z-prepass's vertex inputs** (its eye and VS constants), so that both
    epochs rasterise to the same depth, and so it drew nothing either.

In third person the depth pass has no first-person block and the camera at the return is the world's.

**The fix.** On AE, the Z-prepass runs inside the depth pass, after the world's depth draws
(`Main_RenderDepth_WorldDrawn`, the call at `Main::RenderDepth` `+0x1AA`):

-   **The world camera is current there** in both views.
-   **It captures the engine's `kMAIN` depth** rather than the bound target, because Terrain Blending
    alternates the bound target with its own terrain depth while terrain draws. Afterwards it marks the render
    targets dirty, so the engine rebinds its own for the rest of the pass.
-   **The engine's own `kPOST_ZPREPASS_COPY`, at the end of the pass, now includes DCLF's objects**, so the
    copy `RefreshDepthConsumers` made after the pass is not needed on this path. Terrain Blending's blended
    depth is still built after the pass returns.
-   **The first-person model's depth is drawn after the world's**, which is the native order.
-   **The HZB, built in the Z-prepass segment**, leaves out the first-person model and the rooms' stencil
    draws. That means fewer occluders, never more.

On SE the offset inside `Main::RenderDepth` is unverified, so the Z-prepass stays at the end of the pass. In
first person it logs a warning once and still draws with the wrong camera.

**Result:**

-   The first-person save renders DCLF's objects: screenshots with DCLF on and off match.
-   Third person, reached by scrolling the camera back, is unchanged.
-   The gates stay clean: capture parity OK, draw parity OK, 0 holes, the colour and Z-prepass probes 0
    differing.

**Also noted:** the G-buffer probe's "after the z-prepass" depth slot reads `000000` whatever is drawn, so it
does not measure the prepass.

## Every DCLF draw sampled through an empty sampler descriptor

**Symptom:** the road by the large tree was lit as if in direct sunlight with DCLF on, much brighter than
native (screenshot mean 175.6 against 110.3). It looked like broken shadowing.

**Tracing it back.** Every shadow input matched native at that pixel (the shadow maps with DCLF's shadow
views off, the engine's shadow mask, terrain and cloud shadows, the detailed shadow), and so did the direct
light. The G-buffer, averaged over a 64 x 64 block (`CS_DCLF_TARGET_PROBE`), did not: DCLF's albedo was 1.8
times native, and everything scaled by it followed. With the Lighting shader writing its intermediates
(`CS_DCLF_SHADOW_DEBUG_OUTPUT`), the road's BC7 sRGB base colour read 0.310 in DCLF's draw and 0.099 in the
native one: DCLF's sample was not decoded from sRGB.

What decided it, in DCLF's draw:

| The same texel | Value |
| --- | --- |
| `SampleLevel(..., 0)` through the sampler | 0.308 (not decoded) |
| `Load` (no sampler) | 0.099 (decoded) |
| The shader's biased `SampleBias` | 0.308, identical to mip 0 |

The texture descriptor was right: the heap slot `GpuTextures` wrote differed from a UNORM view's by exactly
the sRGB bit, nothing rewrote it, and a swizzle forced on it showed in the draw. The sampler was not: a
biased sample identical to mip 0 is a sampler that neither filters nor selects mips, and on NVIDIA the sRGB
conversion is enabled in the sampler descriptor as well.

**The cause.** `GpuTextures::SamplerOf` passes the engine's D3D11 sampler with `BorderPreset::Custom` (it
copies the border colour). BasicRHI's Vulkan `CreateSampler` returned `Unsupported` for any custom border
colour without writing the slot, and ORG's `DescriptorHeapManager::CreateIndexedSampler` ignored the result
and returned the slot index. All 20 of DCLF's samplers were empty heap slots, so every DCLF draw sampled
without filtering, without mips and without the sRGB decode. Only sRGB textures (TruePBR's) changed
brightness; everything else lost filtering and mips.

**The fix, in the shared components:**

-   **BasicRHI (Vulkan):** a custom border colour is accepted when no address mode is Border (the colour
    is then never read) or when it equals one of the three built-in colours exactly
    (`VkCustomBorderIsExpressible`). A custom colour with border addressing still needs
    `VK_EXT_custom_border_color`, and says so in the log.
-   **ORG:** `CreateIndexedSampler` checks the backend's result; on failure it releases the slot, logs and
    throws instead of handing out an empty descriptor.
-   **DCLF:** `GpuTextures::Sampler` catches that, logs it once per engine sampler and returns no index, so
    the draws that need it are skipped rather than drawn through nothing.

**Result**, the road save, DCLF on against off:

| | Before | After | Native |
| --- | --- | --- | --- |
| Road (screenshot mean) | 175.6 | 111.6 | 110.3 |
| Albedo, G-buffer block mean | 0.388 | 0.240 | 0.216 |
| Diffuse, G-buffer block mean | 0.866 | 0.556 | 0.472 |

The trunk, rock and player regions are back to their native values too: they were darker only because the
exposure adapted to the bright road. The G-buffer block means still differ by about 10 %; the final image
does not show it.

## The far cascade: DCLF drew the wrong set of casters

**Symptom:** shadows on distant objects were wrong with DCLF's shadow views on. The sun's far cascade (slice
1 of `kSHADOWMAPS_ESRAM`) was 20-25 % clear against 46 % natively (`CS_DCLF_SHADOWMAP_PROBE=1`), and
`CS_DCLF_SHADOWS=0` matched native.

**Tracing it back.** `CS_DCLF_CASCADE_PROBE=1` compares, per shadow view, the casters DCLF's cull keeps (a CPU
replica of `BuildDrawsCS`'s `Culled` against the view's latch matrix; it agrees with the GPU counters) with
the ones the engine registered into that view's batch renderers, recorded at `PassCapture::Withhold`. At the
road save's far cascade, DCLF kept 1146 casters and the engine registered 660; only 396 were in both. The
difference had four causes, each confirmed against the engine and then in the decompile.

**1. The engine culls against a caster volume, not the orthographic box.** `BSShadowDirectionalLight::UpdateCamera`
(`0x141512230`) builds six planes from the main camera frustum's corners and the light direction into each
cascade's culling process (`NiCullingProcess::customCullPlanes`, `doCustomCullPlanes`), and the accumulation's
cull (`FUN_1414f0920`, via `FUN_1414bf9f0`) tests them on top of the shadow camera. DCLF tested only the
view-projection box, which covers the whole slice. The planes rejected 0 of the 396 casters both drew and 683
of DCLF's 750 extras - large, distant rock and mountain meshes no visible receiver can see a shadow from.
The descriptor's own `clipPlanes` are not the cull volume (four of its six planes are zero).

**2. The near plane rejected casters that clamped views pancake.** In a clamped shadow view (mode `0xE`),
`Utility.hlsl` writes `positionCS.z = max(0, positionCS.z)` (`RENDER_SHADOWMAP_CLAMPED`): a caster between the
light and the near plane still writes depth, at 0. `Culled` rejected every caster wholly in front of the near
plane. With ownership, those were claimed and withheld too, so 228 of the engine's far-cascade casters were
drawn by nobody - rock shelves and cliffs above the scene towards the sun.

**3. Casters without `kCastShadows` belong to the volumetric copy only.** In
`GetRenderPasses_ShadowMapOrMask` (`0x1414af030`), a property without `kCastShadows` in a shadow mode gets no
pass when the global byte at `0x142033498` is 1, and when it is 2 (volumetric lighting, as here) a clamped
view whose accumulator has the volumetric flag (`+0x12E`, which the directional light's `Accumulate` always
sets) puts it in `volumetricShadowUtilityPasses` only: drawn into the volumetric lighting copy, never into the
cascades. DCLF's rule modelled only the first case and drew these 804 objects into every cascade; 89 of the
far cascade's remaining extras were among them, mountains in front of the near plane that now wrote depth 0.

**4. The sun's and the spot lights' accumulators do not register decals.** The accumulator registers a
culled geometry through a table indexed by its render mode (`FUN_1414b2140`, table at `0x14332b020` filled by
`FUN_14147e2c0`); modes `0xC`-`0x11` go to `FUN_1414b2a60`, which asks the property for its passes
(`GetRenderPasses_ShadowMapOrMask`, vtable `+0x158`) only when the property has neither `kDecal` (26) nor
`kDynamicDecal` (27), or the accumulator's `drawDecals` (`+0x12C`) is set, or its `+0x12D` is set and the
property has `kZBufferWrite` (32) and bit 18 and the geometry's alpha blends. The constructor sets
`drawDecals` to 1, but `BSShadowDirectionalLight::UpdateCamera` and `BSShadowFrustumLight::UpdateCamera`
(`0x14151ac50`) set it to 0 and `+0x12D` to 1 on the accumulators they create. DCLF's rule modelled only the
bit-18 case, which `GetRenderPasses_ShadowMapOrMask` repeats, so it drew the decal overlays without bit 18 -
the `:8` sub-shapes of rock and cliff meshes, 35 in the far cascade and about 250 shadow inputs in all.

**The fix:**

-   **The engine's caster volume in the latch.** `BuildDrawsLatch` grew a six-plane block and a plane mask
    (the former padding word; 0 for the main camera). `ExecuteShadowView` copies the descriptor's culling
    process's `customCullPlanes` when `doCustomCullPlanes` is set, and `Culled` rejects a bound wholly
    outside any active plane.
-   **No near plane for clamped views:** latch flag `kCullNoNearPlane` (`CullFlags` bit 9), set for mode
    `0xE`.
-   **`ShadowReject::VolumetricOnly`:** `kCastShadows` clear while the global is 2. Such an object is not a
    DCLF caster, so it is not claimed, and the engine keeps drawing it wherever it does: the volumetric copy,
    and point lights, where it casts normally.
-   **`ShadowReject::DecalNoZWrite` covers every decal** outside the registration's exception (bits 32 and
    18, blended). The verdict is one per object for every view, and a paraboloid light keeps `drawDecals`
    and casts the decals without bit 18; left unclaimed, the engine still draws them there.

**Result**, far cascade at the road save:

| | Before | Planes and near plane | 1-3 | All four | Native |
| --- | --- | --- | --- | --- | --- |
| Casters both draw | 396 | 618 | 618 | 577-618 | 619-660 registered |
| DCLF only | 750 | 124 | 35 | 0-3 | |
| Engine's, withheld and not drawn by DCLF | 228 | 6 | 6 | 6-7 | |
| Slice 1 clear | 20-25 % | 9.5 % | 43 % | 45.6 % | 45.8 % |
| Slice 1 mean depth | 0.26-0.31 | 0.14 | 0.48 | 0.510 | 0.514 |

The last two columns are the readings either side of a live toggle (`CS_DCLF_TEST_TOGGLE=3000:99999`); the
scene drifts by about as much between two readings (the sun moves). The middle column is why the first three
are one fix: keeping the pancaked casters without the volumetric rule drew mountains at depth 0 over most of
the slice. The near cascade went from 53 extras to 0-1.

The engine's casters DCLF withholds and does not draw are not holes. They are one actor's parts (`WarAxe:0`,
`_Cuirass_1`, `MaleUnderwearBodyArmor`, its head parts) and a grass shape in the far cascade, and two parts
in the near one, and every one lies wholly outside the view's box: the actor at NDC y -1.02 to -1.04, about
130 units past the slice's edge, with its bones (the skin palette's translations) in the same range as the
bound. The engine registers them because its cull is the caster volume, which is wider than the box, and the
rasterizer clips them; DCLF's x/y test drops them first. What is left is in
[dclf-open-defects.md](./dclf-open-defects.md).

## Shadow views draw with the view's rasterizer state

**Symptom:** after the caster sets were fixed, the near cascade still stepped at a live toggle (slice 0 mean
depth 0.2678 with DCLF against 0.2699 native) while its caster sets agreed. DCLF drew the same casters
closer to the light.

**Cause.** Community Shaders' `ShadowmapCascadeRasterizerFix` (`src/EngineFixes`) swaps the engine's global
rasterizer table (`[fill][cull][depth bias][scissor]`) for per-cascade copies around each cascade's draw:
DepthBias 160, slope 3.2, clamp 0.015 for the first; 100, 3.8, 0.015 for the second. It hooks only the cascade
loop of `BSShadowDirectionalLight::Render`, so the volumetric lighting copy draws with the engine's own states,
which there cull nothing. Read off the context after each native view (`CS_DCLF_CASCADE_PROBE=1`):

| View | DepthBias | Slope | Clamp | Cull |
| --- | --- | --- | --- | --- |
| Cascade 0 (`kSHADOWMAPS_ESRAM` slice 0) | 160 | 3.2 | 0.015 | back |
| Cascade 1 (slice 1) | 100 | 3.8 | 0.015 | back |
| Volumetric copy, both slices | 0 | 0 | - | none |

DCLF's shadow pipelines had no bias at all (a shadow key carried only the two-sided bit, so no state was ever
read for it) and culled back faces everywhere.

**The fix: pipeline variants per view rasterizer state.**

-   `ExecuteShadowView` runs inside the view's draw (from `FinishAccumulating`), so it reads the state the
    engine binds for the view: `EngineRasterStates()` at the renderer's fill, cull, bias and scissor modes,
    which is the cascade fix's copy while it is swapped in. `DrawPipelines::ShadowRasterStateId` registers the
    distinct states (bias, clamp, slope, cull) and names each with an id; the exterior has three.
-   A shadow pipeline key carries the id (`kRasterShadowStateShift`), and `BuildShadow` takes the state from
    it. A two-sided caster still draws without culling. The scissor is not part of the state: the pass is
    bounded by the view's viewport.
-   The views of a render mode keep sharing one input list, but an input names its caster's **key slot**
    (`Lookups::shadowSlots`, append-only), not a pipeline. `RefreshShadowLookups` resolves every slot under
    each state its mode's views use and keeps `Lookups::shadowMapRows[state][slot]`. Each view's latch
    carries the byte offset of its state's row (`BuildDrawsLatch::pipelineMapOffset`, the latch is now 208
    bytes), the rows are copied into the shadow latch block after the views' latches, and `BuildDrawsCS`
    resolves the draw's pipeline through the row. The main camera's offset is 0: its inputs name pipelines.
-   A caster is an input only when its pipeline is ready under every state its mode's views use: the claim
    withholds the engine's pass from all of them.
-   The winding is the state's too (`FrontCounterClockwise`, set in every engine state); see "Front faces:
    BasicRHI's `frontCCW` has D3D's meaning" below.

**Result** at the road save, the readings either side of a live toggle:

| | DCLF on, before | DCLF on, after | Native |
| --- | --- | --- | --- |
| Slice 0 mean depth | 0.2678 | 0.2690 | 0.2688 |
| Slice 1 clear | 45.6 % | 45.6 % | 45.8 % |
| Slice 1 mean depth | 0.510 | 0.510 | 0.513 |

The slice-1 differences are within the drift between two readings. 0 claimed but not drawn; 1200 of 1500
views drawn (the 300 focus views are left native).

## Front faces: BasicRHI's `frontCCW` has D3D's meaning

Every rasterizer state of the engine has `FrontCounterClockwise` set. DXVK draws the engine's passes with a
y-flipped viewport (negative `VkViewport::height`) and maps that flag straight to
`VK_FRONT_FACE_COUNTER_CLOCKWISE` (`d3d11_rasterizer.cpp`, `d3d11_context.cpp`). BasicRHI's Vulkan backend
uses the same flip, but mapped `frontCCW` to the opposite `VkFrontFace` (BasicRHI `fc214cd`, "frontCCW
parity"): DCLF's main pass pipelines passed `false`, which under the direct mapping culled the engine's front
faces, and the inversion made that `false` right instead of the caller. It also made BasicRHI's Vulkan
backend disagree with its D3D12 backend, which passes the flag through.

The contract is now D3D's on both backends: `frontCCW` is the winding of the triangle as it lands on the
render target, clip space y-up, and the Vulkan backend maps it to the `VkFrontFace` of the same name
(BasicRHI `rhi.h`, `rhi_vulkan.cpp`, README "Backend-independent conventions"). DCLF passes the engine's own
winding: the main pass pipelines read it once from the engine's rasterizer table
(`DrawPipelines::Impl::EngineFrontCCW`), the shadow pipelines from their view's state. At the road save the
G-buffer after DCLF's colour pass matches native (`CS_DCLF_TARGET_PROBE`), and the shadow maps are
unchanged. SARP and BasicRenderer set `frontCCW` true for glTF content and have not been run under Vulkan
since; their D3D12 behaviour is unchanged.

## The volumetric lighting copy: only the volumetric-only casters

**Symptom:** Volumetric Shadows' copy of the near cascade (its mip 1, the nearer of the cascade and the
volumetric lighting copy per texel) stepped at a live toggle, 0.2565 with DCLF against 0.2609, after both
cascades matched.

**Cause.** `BSShadowDirectionalLight::Render` draws each cascade twice: into
`kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` with flag 0x100 while the shadow global is 2, then into
`kSHADOWMAPS_ESRAM`. Both go through the descriptor's accumulator and batch renderer, but the mode's draw
(`FUN_1414b44f0`, mode 0xE's entry in the table at `0x14332b120`) branches on the flag: without it, the
Utility technique ranges and batch groups 1 and 9; with it, batch group 15 alone. Group 15 is where the
registration puts accumulation hint 8 (`FUN_1414b2a60`), which only `GetRenderPasses_ShadowMapOrMask`'s
volumetric-only passes carry. So the volumetric copy holds the volumetric-only casters and nothing else, and
those are not DCLF's (`ShadowReject::VolumetricOnly`, unclaimed). DCLF drew its whole caster set into both
volumetric views, filling far more of the copy than the engine does:

| Volumetric lighting copy | DCLF drawing it | Native |
| --- | --- | --- |
| Slice 0: clear / mean depth | 23.7 % / 0.389 | 44.0 % / 0.594 |
| Slice 1: clear / mean depth | 23.0 % / 0.252 | 33.1 % / 0.346 |

**The fix:** `ExecuteShadowView` leaves a view whose target is the volumetric lighting copy to the engine
(`ShadowStats::volumetricSkipped`). Across a toggle afterwards the copy's slices, both cascades and both
Volumetric Shadows mips match within the drift between readings (slice 0 of the copy 0.4996 against 0.5006,
37.1 % clear on both; VSM mip 1 0.2091 against 0.2094). DCLF now draws two views a frame instead of four.

**Taking the copy over: measured, not done.** `CS_DCLF_VOLUMETRIC_PROBE=1` (TEMP, `VolumetricProbe.cpp`) counts
every Utility draw the engine issues inside each shadow view (`BSUtilityShader::SetupGeometry`) and times the
sun's CPU work. At the road save, Riverwood, Ivarstead and Winterhold:

-   The copy is 71-93 geometries a frame, one draw each (75-94 draws), all hint 8, all static `STAT`
    `BSTriShape` with `BSLightingShaderProperty` and no `kCastShadows`: mountains, cliffs, road pieces. None
    is also drawn into a cascade. The engine's draw of both views costs 0.025-0.039 ms a frame.
-   Their passes never reach `BSBatchRenderer::RegisterPass` (the `PassCapture` hook). `FUN_1414b2a60`, the
    shadow modes' registration, inserts hints 11, 7, 3 and 8 straight into batch groups 9, 1, 4 and 15
    with `FUN_1414f5090(batch, pass, group)`, a direct call; only the rest go through the vfunc. Claiming
    them needs a thunk at that call.
-   The sun's CPU, native against DCLF: cascade draws 0.29 / 0.76 ms (road / Riverwood, 690 / 2150 draws)
    against 0.025 / 0.075 ms; `Accumulate` (the cascade culls and registration, `FUN_1414f0920` per
    cascade) 0.17 / 0.44 ms against 0.22 / 0.54 ms; the full-frustum cull (`FUN_141511f30`) 0.04-0.05 ms.
-   With DCLF on, the engine still draws 94 casters a frame into the cascades at Riverwood, all eligible
    by the caster rule but not DCLF's yet: 49 `BSTriShape`, 45 `BSDynamicTriShape`.

So the copy is about 80 draws and 0.03 ms. Taking it over pays only as part of retiring the sun's
`Accumulate`, and that also needs the cascade residue above.

**DCLF draws the copy (2026-09-23).** This is the first step towards retiring the sun's native
accumulation.

-   **Tables.** A `ShadowReject::VolumetricOnly` object becomes a shadow caster with
    `kObjectVolumetricOnly` (`Records.h`), instead of `kObjectNoShadow`. It gets a Utility technique and a
    pipeline key like any other caster.
-   **One input list, two caster classes.** A render mode's inputs hold both classes.
    -   A copy view's latch sets `kCullVolumetricOnly` (cull flag `0x800`) and draws the flagged inputs
        alone.
    -   Every other shadow view sets `kCullCastersOnly` (`0x400`) and skips them.
    -   `BuildDrawsCS` drops the other class before any culling.
-   **Pipeline readiness per class.** The mode's `modeRasterStates` word keeps the ordinary views' states
    in bits 0-15 and the copy views' states in bits 16-31. A caster must be ready only in the states of the
    views that draw its class. A volumetric-only caster with no copy view that frame is no input, and stays
    the engine's.
-   **Claims.** A volumetric-only pass never reaches `RegisterPass`. `PassCapture::VolumetricGroupHook`
    thunks the shadow modes' direct insertion into group 15 (AE `FUN_1414b2a60` + 0xFF, `0x1414b2b5f`) and
    withholds it when the mode's claim set holds the geometry.
    -   The claim set stays one per mode. Its ordinary casters' passes reach only `RegisterPass`, and its
        volumetric-only casters' passes reach only the group-15 call.
    -   SE and VR: the call's offset is unverified, so `VolumetricClaimsAvailable()` is false there and
        the copy stays the engine's, as before.

**Result** at the road save across a live toggle, with 0 views not ready, all four sun views drawn a
frame, and 77-79 volumetric-only passes withheld a frame. The engine's draws into the copy went from
about 78 a frame to 0 with DCLF on.

| | DCLF on | Native |
| --- | --- | --- |
| Copy slice 0: mean / clear | 0.5930 / 44.0 % | 0.5936 / 44.0 % |
| Copy slice 1: mean / clear | 0.3440 / 33.0 % | 0.3454 / 33.1 % |
| Volumetric Shadows mip 1 | 0.2620 | 0.2617 |

## Shadow-only casters, and the sun's entry rule

Goal: the engine draws nothing of the sun's own (see "DCLF draws the copy"). With DCLF on, the engine still
drew these casters into the cascades, all accepted by the caster rule but missing from DCLF's tables, which
held only main-pass-eligible objects:

| Class | Road | Riverwood |
| --- | --- | --- |
| Terrain blocks (`Block (x, y)`, `Ineligible::Technique`) | 37 | 41 |
| NPC heads, mouths, hair (`BSDynamicTriShape`, hints 0 and 3) | 6 | 45 |
| Hay under a `BSOrderedNode` (`Ineligible::UnsupportedParent`) | 0 | 7 |

(`CS_DCLF_VOLUMETRIC_PROBE`, the engine's cascade draws with DCLF on, by the scene store's verdict.)

**Shadow-only objects.** A walked object that fails main-pass eligibility only for a reason the shadow views
do not care about becomes a table object with `kObjectShadowOnly` (plus `kObjectNoBindings`), when the caster
rule accepts it (`SceneStore.cpp`: `ShadowOnlyReason`, `ShadowOnlyCaster`; `CS_DCLF_SHADOW_ONLY=0` turns it
off).

-   **Which reasons:**
    -   `Technique`: the Utility technique does not depend on the lighting technique.
    -   `UnsupportedParent`: a `BSOrderedNode` only orders blended draws.
    -   Not billboards: `NiBillboardNode` turns to the culling camera, which for a shadow view is the light's.
-   **Where it is skipped:** the accumulate phase and the main epochs skip it (not even a culling candidate),
    and so does the `CS_DCLF_ONLY_ELIGIBLE` parity mode.
-   **Claims:** the shadow claim set covers it like any caster.
-   **Result:** with DCLF on, terrain and hay no longer appear in the engine's cascade draws. At Riverwood the
    only class left is the NPC `BSDynamicTriShape`.

**The sun's entry rule.** The cascade culls walk only the entries of the full-frustum culling processes'
`objectArray` (engine notes, "The sun's accumulation"), so an object whose entry the full-frustum cull left
out is never a candidate, however its own bound tests. With terrain added, this mattered: 11 DCLF-only far
cascade casters at Riverwood, among them a volumetric-only `MountainCliff01` with radius 7,359 and a terrain
block.

-   **The rule, measured with `CS_DCLF_CASCADE_PROBE`:** it now also records the engine's hint-8
    registrations. Every process rejects with the same plane (2), at the reference root. The entry is:
    -   for a static reference, its reference root (the topmost ancestor carrying the geometry's
        `userData`);
    -   for an actor, the cell's container, never tested;
    -   for a geometry without a reference (terrain), its nearest `BSMultiBoundNode`.
-   **Implementation:**
    -   `SceneStore::SunEntryOf` caches the entry node per tracked geometry, and the tables carry its bound
        (`Tables::sunEntry`).
    -   `PrepareShadowInputs` copies every full-frustum process's planes (render thread, after the
        full-frustum cull).
    -   The build flags an input whose entry is outside every process (`kInputOutsideSunEntry`), and only
        the sun's views skip flagged inputs (`kCullSunEntry`), because spot lights share mode `0xE`'s
        inputs.
-   **Result at Riverwood's far cascade:** DCLF-only went from 11 to 2 (a dagger carried by an actor, and one
    more of that kind, pruned by the engine at a node below the entry), and engine-only is 43 (the NPC
    shapes, plus one volumetric-only mountain whose bound lies outside the view in x). The near cascade has
    DCLF-only 0.
-   **Readbacks** across four live toggles at Riverwood (off, on, off, on) match within the drift between
    samples, for example far cascade 30.9 % against 30.8 % clear and copy slice 1 60.1 % against 60.4 %.
    The far slices step twice during the run, at about the same time in every run and in native periods as
    well: a scene change, not DCLF.

## NPC faces: positions published by the engine's own writer

The last class the engine cast into the sun's views was NPC face shapes (`BSDynamicTriShape` under a
`BSFaceGenNiNode`): 6 a frame at the road, 45 at Riverwood. Their positions are `dynamicData`, written by the
face morphing jobs (skyrim-engine-notes.md, "Face morphing"). DCLF never reads it or takes its lock.

-   **Capture in the writer** (`FaceSnapshots`, AE only, `CS_DCLF_FACEGEN=0` turns it off). A thunk on each
    head's morph call (`0x1404334f3`) copies, on the job's thread, every face shape of that head into a
    snapshot as soon as the job has morphed it. A thunk after the stage's `JobList::Finish` (`0x1406d36fd`),
    when no morph job runs, captures the heads that want a first snapshot (new, rebuilt, never animated).
-   **One head, one slot.** A head's snapshots are a lock-free triple buffer: the writer fills its slot and
    exchanges it into `latest`; the walk takes the newest at its start. Every shape of a head is read from one
    slot, so from one job run: a head is never drawn from two updates. Under the engine's schedule the walk
    reads what the engine's draws read (frame N-1's morphs); if the schedule changes, it reads the newest
    whole head.
-   **Lifetime.** The walk alone creates, rebuilds (a head's shapes changed) and retires records. Writers find
    them through a fixed open-addressed table keyed by the head; a retired record is freed once a morph stage
    has completed after it, since no job spans a stage's join.
-   **In the tables.** A face shape was first `Ineligible::FaceGen`, shadow-only; it is now classified like any
    shape, with its positions as a property of the object (see "NPC face parts in the main pass"). Its
    positions go to a region of the epoch's positions buffer (a float4 a vertex, kept per shape while walked),
    uploaded only when the head's snapshot generation changed (about 3 regions a frame at Riverwood).
-   **The draw.** Shadow sequences are `ShadowDrawSequence` (84 bytes, a second vertex buffer view after the
    first); `BuildDrawsCS` writes them for the shadow dispatches (`kPhaseBitsShadowSequences`), with the input's
    `streamIndex` naming a `GeometryDraw` appended after the geometry slots. The shadow pipelines declare
    binding 1 when their layout reads it. The shadow pass declares the buffer as a vertex buffer
    (`BindVertexBuffer`, added to ORG's `RenderPassBuilder`), so the graph orders the uploads before the draws.
-   **Claims.** Face parts register at hints 0 and 3. Hints 11, 7 and 3 bypass `RegisterPass` as hint 8 does
    (`FUN_1414b2a60`); the same claim test now withholds them at their direct calls (`0x1414b2b29`, `b3b`,
    `b4d`). Before this about 540 passes a frame of claimed casters with those hints were drawn by the engine
    as well as by DCLF.

**The defect found on the way.** The first build broke the far cascade (0 % clear against 31 % native).
Bisecting showed it with face shapes dropped before any snapshot, slot or palette: face parts with the decal
flag (brows, hairlines) got `Ineligible::Decal` from the derivation, and `Decal` is the verdict the walk defers
to the accumulate phase with a record. So those dynamic shapes became ordinary casters without their positions,
and the second stream fell back to their own buffer: UVs and weights drawn as positions. A face shape is now
`FaceGen` or rejected, never deferred, and an input whose layout reads its position from stream 1 must have a
face stream.

**Result** at Riverwood across three live toggles: far cascade 31.3 % / 30.9 % clear with DCLF against 31.4 % /
31.0 % native, VSM mip 0 0.4996 against 0.4984. With DCLF on, the engine draws nothing into the cascades
or the volumetric copy; the only engine Utility draws left are the focus view's 9 a frame. Snapshot parity with
`dynamicData` at the walk (TEMP, `CS_DCLF_VOLUMETRIC_PROBE`): 27,600 of 27,600 equal, 0 layout mismatches.

## NPC face parts in the main pass

Face parts are DCLF's in the main pass too. Measured at Riverwood (TEMP, both pass sources agree, so every
face pass reaches `RegisterPass`):

| Face part | Technique | Pass |
| --- | --- | --- |
| heads | Facegen (4) | hint 0, list 0 |
| mouths | 0, alpha-tested | hint 0, list 1 |
| hair, beards | Hair (6), alpha-tested | hint 0, list 1 |
| hairlines, brows, some hair and beards | Hair (6), decal-flagged, blended | hint 3 (the blended decal group) |

-   **A face shape is a property of the object, not a verdict.** `Ineligible::FaceGen` is gone. A face shape
    (`Tracked::faceShape`, from its type and parent) is classified like any shape, every frame, and the walk
    gives every record of one its positions stream, whatever the verdict, a deferred decal included. Without a
    snapshot or a region it gets no record, and the engine keeps it. This is the contract that the shadow-only
    shortcut broke (the decal-flagged parts drawn without positions, above).
-   **One sequence layout.** `DrawSequence` is 84 bytes everywhere: a second vertex buffer view (slot 1) after
    the first, a face shape's positions or its own buffer again. Both main signatures and the shadow signature
    have the slot-1 argument; `BuildInputLayout` declares binding 1 when a layout reads it. `ShadowDrawSequence`
    is gone.
-   **Positions for every epoch.** The main epochs have their own positions buffer (`cs.dclf.face-positions`),
    with the same per-region generations as the shadow epoch's, uploaded at their commit on the render thread.
    The main payload appends the stream `GeometryDraw`s after the geometry slots (`AppendFaceStreams`), and an
    input that is a face shape, or whose pipeline reads its position from stream 1, is drawn only with them.
    The interior (no sun epoch) works from the main epoch's uploads alone.
-   **Techniques.** Facegen, Hair and Eye join FacegenRGBTint under `CS_DCLF_ACTORS`. Their constants and
    textures needed nothing new: the material evaluation runs the engine's own `SetupMaterial` (the tint and
    detail maps, the hair tint, the eye centres).
-   **Hair with ProjectedUV** is drawn without the projection: the engine reads another object's snow there
    ([bugs-found-by-parity.md](./bugs-found-by-parity.md), "The engine").
-   **Extended Translucency's material model** is per geometry: its `SetupGeometry` hook sets "use default" for
    blended skinned geometry, an explicit model from `AnisotropicAlphaMaterial`, or "disabled". DCLF had
    assumed "disabled" for every pipeline, which held until it drew blended skinned geometry (the hair decals).
    The rule is now `ExtendedTranslucency::MaterialModelOf`, called by the hook and by DCLF, whose main keys
    carry the model (`kRasterTranslucencyShift`, left out of `RasterStateBits`) and whose permutation is built
    from it.

**Advanced Skin** was left for its own step; see "Advanced Skin in DCLF's draws" below.

**Result** at Riverwood:
-   Capture parity (`CS_DCLF_OWNERSHIP=0`): OK, about 18,000 draws checked per 300 frames, 0 mismatched.
    Permutation, draw, light data and bone palette parity OK.
-   With ownership: 0 native passes kept, 0 holes, `CS_DCLF_BUILD_PARITY` OK in every report (sequences with
    streams included).
-   Shadow maps across a live toggle unchanged; 0 engine draws into the cascades or the volumetric copy.
-   Snapshot parity 35,400 of 35,400 equal.
-   The Sleeping Giant Inn (interior): 0 holes, BuildDraws parity OK, 0 native passes kept.

## Advanced Skin in DCLF's draws

Community Shaders' Advanced Skin (`src/Features/Skin.cpp`) binds four things for Lighting draws, none of them
through the engine's state. DCLF used to take all four from the frame capture, so every DCLF draw got whatever
the capture happened to see: one material's t71/t74, one actor's wetness.

| Binding | Set by | Varies per | DCLF |
| --- | --- | --- | --- |
| t72, the skin detail normal map | `Prepass` | frame | the frame capture, as before |
| t71 (RFAOS), t74 (wetness texture) | its `SetupMaterial` hook, bound at `State::Draw` | material (FaceGen and FaceGenRGBTint only) | the material record |
| b7 `SkinPerGeometry` | its `SetupGeometry` hook, every Lighting draw | owning actor, per frame | per object |
| t75 | nothing | - | declared by `Lighting.hlsl`, never sampled |

-   **t71/t74: one rule, two callers.** The hook's logic is now `Skin::MaterialTexturesOf(material)`: nullopt for
    a material that isn't FaceGen or FaceGenRGBTint, otherwise its extra textures or the default black texture
    for both. The hook binds what it returns, and `ConstantEvaluator::EvaluateMaterial` stores it in the material
    record (`MaterialRecord::featureTextures`, registers `kFeatureMaterialRegisters`). The lookups resolve those
    views like the engine's t0-t15 (`Lookups::Material::featureIndex`), and the binding record names them for a
    pipeline that reads them. One change of behaviour in Skin: a FaceGen material without a hash key now binds
    black, as a material without extra textures does. Before, it left the previous draw's textures.
-   **The stand-in no longer leaks Skin's binding.** DCLF's stand-in `SetupMaterial` runs Skin's hook, which
    leaves the evaluated material's textures pending for the next `State::Draw`. A native draw that skips
    `SetupMaterial` (the same material as the draw before it) would bind them. `RunStandIn` saves and restores
    the pending binding (`Skin::GetPendingTextures` / `SetPendingTextures`), as it already does for the
    permutation data.
-   **b7: the actor's wetness, per object.** `Skin::GetWetness(geometry)` returns the owning actor's sweat,
    water wetness, height and water depth: zero unless the geometry's user data is an `ActorCharacter`. It keeps
    a fading state per actor and computes once a frame; later calls in the frame return the cached value.
    -   The walk resolves ownership once per tracked geometry (`Tracked::actorOwned`).
    -   `RefreshFrameConstants` (Prepass, the same frame as the main pass) calls `GetWetness` for every
        actor-owned object in the tables (`Tables::actorObjects`, `Tables::skinWetness`), while Skin is enabled.
        Every actor's fade therefore advances once a frame whether or not the engine draws it. Natively it
        advanced only on frames where the actor was drawn.
    -   With `DCLF_BINDLESS_DRAW` it is a field of the object record (`BindlessObject::skinPerGeometry`,
        `DCLFObjectRecord::DCLFSkinPerGeometry`), which `Skin.hlsli` reads in place of its b7 buffer, so the
        binding record stays one per (material, pipeline). Without it, b7 is a per-draw block deduplicated by
        value, like Linear Lighting's b8.

**Parity.** Capture parity's `skin parity` line checks every native draw it compares:

-   t71 and t74 as bound at the draw, against the material record (draws of FaceGen materials);
-   Skin's uploaded wetness against `Tables::skinWetness`;
-   the geometry's actor ownership against `Tables::actorObjects`.

Bindless record parity checks the record's `SkinPerGeometry` against the tables.

**Result** at Riverwood, with `player.damageav stamina 1000` at frame 1500 so that the player sweats and then
dries:

-   Capture parity (`CS_DCLF_OWNERSHIP=0`) OK in every report: 0 mismatched. Skin parity OK: up to 215 draws' t71/t74
    and about 4,200-8,600 draws' wetness per 300 frames, 0 differ, 0 ownership differences. The wet draws (sweat
    or water) went from 0 to 1,200 per report after the drain and back to 0 as the player recovered, and the
    fade matched throughout.
-   With ownership: 0 native passes kept, 0 holes, BuildDraws parity OK, bindless record parity OK, all 42 SPIR-V
    programs ready. The same with `CS_DCLF_BINDLESS_DRAW=0` (b7 as a per-draw block, no missing pixel constants).
-   Every FaceGen material bound the same 16x16 view at t71 and t74, below the shader's 32-texel threshold, so
    the RFAOS and wetness-texture paths never run natively either. That is a Skin bug
    ([bugs-found-by-parity.md](./bugs-found-by-parity.md)); DCLF binds what Skin binds.
-   Feature binding parity still reports t26, t55, t81-91 and VS b7 differing within some frames. Those are other
    features, not Advanced Skin, and were reported before this work.

## The sun without the engine's registration (stage 3, M1)

DCLF draws every sun caster, but the engine still registered them. `BSShadowDirectionalLight::Accumulate` culled
each cascade and handed every geometry to the accumulator's registration (`FUN_1414b2140`). That built its shadow
passes, and `PassCapture` then withheld those passes one by one. The registration also sets the geometry's
`activeLightMask` bit for the cascade, which the main pass reads to decide whether the object samples the sun's
shadow ("The sun's accumulation", engine notes), so it can't simply be dropped.

**`SunAccumulation`** (`SunAccumulation.cpp`, AE only):

-   **The hooks.** It hooks `Accumulate` (vtable slot 9) and `FUN_140e28af0`'s two calls of the registration
    (`0x140e28bc3`, `0x140e28c89`). Each call site is verified before patching.
-   **Per sun `Accumulate`.** On its thread, it records the cascades' accumulators. For each one it takes the
    claim set `PassCapture` would withhold that accumulator's passes by: its batch renderer's render mode's
    (`PassCapture::ShadowClaimsForBatch`, the same test as `Withhold`).
-   **A registration for one of those accumulators, of a claimed geometry,** runs only the registration's
    early-outs and its mask write (`WriteMaskOnly`). No pass is built.
-   **Everything else calls the original,** so an unclaimed caster is registered and drawn by the engine as
    before, and M1 needs no frame-wide verdict. Under capture parity (`CS_DCLF_OWNERSHIP=0`) nothing is claimed,
    so everything registers.
-   **What else stays covered:**
    -   The volumetric copy's passes come from the same accumulators. A claimed geometry's copy is DCLF's too
        ("DCLF draws the copy"). M1 requires `PassCapture::VolumetricClaimsAvailable`, so the hint 8/11/7/3
        withholding it replaces exists.
    -   `PassCapture`'s per-pass withholding stays, as the backstop.
-   **Switches:**
    -   The menu's "Skip the engine's sun shadow culling and registration" (`CS_DCLF_SUN_SKIP`, default on) needs
        static shadow ownership.
    -   The diagnostics that need the engine's sun registrations (`CS_DCLF_CASCADE_PROBE`,
        `CS_DCLF_VOLUMETRIC_PROBE`, `CS_DCLF_SHADOW_PROBE`) keep it off for the run.
    -   `CS_DCLF_SUN_TIMING=1` times the full-frustum cull, `Accumulate` and the registrations.
-   **Open:** a skipped geometry misses `FUN_1414b2a60`'s `property->lastAccumulatedFrameCount` write, which has
    no reader found yet.

**Result** (`CS_DCLF_SUN_TIMING`, render thread per frame):

| | Registered by the engine | Mask only | `Accumulate` | … registration |
| --- | --- | --- | --- | --- |
| Riverwood, M1 off | 2,580 | 0 | 0.69 ms (max 1.25) | 0.41 ms |
| Riverwood, M1 on | 330 | 2,260 | 0.47 ms (max 0.81) | 0.19 ms |
| Road save, M1 on | 200 | 780 | 0.20 ms (max 0.66) | |

-   The geometries still registered are the unclaimed ones, the same set the engine registered before.
-   The shadow-map and volumetric readbacks match the M1-off run within the drift between samples (far cascade
    29.2 % clear in both).
-   Main-pass objects with the sun's shadow mask are the same share of the engine-kept objects with M1 off and on
    (1,475-1,504 of 1,910-1,933).
-   0 holes, 0 claimed but not drawn, BuildDraws parity OK, over the road save and two teleports.

**M2**, a CPU replica of the engine's culls, was not built. The next section takes DCLF's objects out of the cascade
culls instead, which needs no replica.

## The sun's cascades without DCLF's objects (stage 3, entry exclusion)

M1 left the engine culling every caster DCLF draws, cascade by cascade, only to skip the registration at the end. At
Riverwood that was still about 0.6 ms a frame of the render thread in `BSShadowDirectionalLight::Accumulate`. The
exclusion takes DCLF's objects out of the cascade culls altogether ([dclf-gpu-driven-frame.md](./dclf-gpu-driven-frame.md),
"The principle").

**Where to cut.** The cascade culls walk only the entries of the full-frustum culling processes' `objectArray` (engine
notes, "The sun's accumulation"): a `BSTArray<NiPointer<NiAVObject>>` the full-frustum cull rebuilds every frame, which
nothing but `Accumulate` reads (DCLF reads only the processes' planes). An entry removed from it after the full-frustum
cull is never traversed by any cascade, so its whole subtree skips the culling, `OnVisible` and the registration. The
cascade processes set neither `cameraRelatedUpdates` nor `updateAccumulateFlag`, so no fade, tree clock or
`kAccumulated` state is lost with them.

**Which entries: `SunCandidates`** (`SceneStore::UpdateSunCandidates`, on the render thread at the end of the delta
walk). An entry (a static reference's root, or a terrain block's multibound node: `SunEntryOf`) is a candidate when
every tracked geometry under it (`rootDependents`) either:

-   is a table object, whose shadow the shadow epoch draws, or which the caster rule rejects; or
-   builds no shadow pass in the engine either: not a Lighting geometry, hidden, alpha-blended, fading, or an unselected
    switch child (`SunEntryAllows`).

Everything else (decals, billboards, non-`BSTriShape`, LOD, and so on) keeps its entry in the culls. The status is
judged again only for the entries a change touched: a dependent attached, detached, or gaining or losing its record or
its verdict. Any change bumps a generation at once. The immutable snapshot (entry and geometry indices) is rebuilt on
the next walk that changes nothing, so a cell load does not rebuild it every frame.

**Which of those, each frame: `SunExclusion`** (`BuildSunExclusion`, with the cascades' claims, on the worker when the
shadow build runs there). A candidate is excluded unless one of its table objects casts (no `kObjectNoShadow`) and is
not an input of the cascades' mode, so is not claimed. The exclusion is published with the claims and used once, by the
next frame's full-frustum cull, and only if the candidates' generation is still the one it was built for. Otherwise the
engine culls everything that frame (`stale`), which is also what happens during a cell load.

**The filter** (`SunAccumulation::ExcludeEntries`, a thunk on `CalculateAndDrawShadowCasterLights`' call of the
full-frustum cull). It compacts each process's `objectArray`, releasing the removed pointers. Each process keeps at
least one entry: the traversal's first entry sets the cascade's planes up (vfunc `0xB8`), and an empty array leaves the
process with the previous cascade's.

**The bits.** Removing the registration also removes its mask write: the sun's bits in `activeLightMask`, which the
main pass reads for ShadowDir and DefShadow. DCLF writes them where they are read:

-   When the main camera registers a geometry under a removed entry (the same `FUN_140e28af0` call sites M1 thunks, on
    the registration jobs), DCLF ORs in the bit of every cascade its world bound meets, before `GetRenderPasses` runs.
-   The planes are the engine's own for that cascade: copied from the cascade culling process right after its cull
    (a thunk on `FUN_1414bf320`'s call of `FUN_140e305c0`), with the custom planes when it has them. The cascade and
    its bit come from a thunk on `Accumulate`'s call of `FUN_1414f0920`.
-   **The main registration reads the mask and then clears it** (the main accumulators' `+0x160` is `0xFFFF`), so every
    later registration in the frame, such as the reflections and the depth accumulations, reads 0. DCLF follows that:
    it writes a geometry's bits until the geometry's first `0xFFFF` registration (a per-geometry stamp), and only from
    the end of the sun's `Accumulate` until `Main::Draw`'s mask clear (`FUN_1414cb640`) returns. Without the stamp, a
    fifth of the comparisons below differed.

This is the Geometric rule (stage 3's decisions): each geometry's own bound against the cascade, where the engine
tests the nodes above it as well.

**The dry run** (`CS_DCLF_SUN_EXCLUDE=probe`): the exclusion is built and the entries marked, but nothing is removed.
The engine's bits at the main registration are compared with DCLF's, and every cascade registration the engine makes
under a would-be-removed entry is counted by whether it built a pass (`PassCapture::PassesOnThisThread`).

| Scene | Bits compared per frame | Agree | Engine only | DCLF only | Unclaimed registrations that built a pass |
| --- | --- | --- | --- | --- | --- |
| Riverwood | about 2,500 | 99.99 % | 0.2-0.4 | 0.03 | 0 (205 a frame built none) |
| Whiterun | about 750 | 99.8 % | 0.5-1.0 | 0.3-0.6 | 0 (85 a frame built none) |

The differences, by class:

-   **Clouds** (`CloudDistant*`): effect-shader geometry, so nothing reads the bits.
-   **Distant cliff pieces, engine only:** accepted through a node the cascade tests as fully inside, whose plane state
    then spares the geometry its own test; its own bound is outside.
-   **Small clutter (fish, buckets, crabs), DCLF only:** in the far cascade's volume but not given its bit by the engine.
    DCLF's bit makes them sample the screen-space shadow mask, which is the more correct result.

**Result** (full featureset, `CS_DCLF_SUN_TIMING`, render thread per frame, SkyrimEngineTelemetry's coarse zones in both):

| Riverwood | M1 (exclusion dry) | Exclusion |
| --- | --- | --- |
| `Accumulate` | 0.59-0.65 ms | **0.10-0.11 ms** |
| … of which registration | 0.26-0.30 ms | 0.04-0.05 ms |
| Cascade registrations: skipped (claimed) / engine | 2,300 / 330 | 250 / 125 |
| The filter | - | 0.037 ms (1,358 of 1,450 entries removed; 4,494 candidates, all excluded) |
| Main registrations given DCLF's bits | - | about 2,330 |

-   The main pass's objects with the sun's shadow mask are the same share of the engine-kept ones: 1,501 of 1,908
    (78.7 %) with the exclusion, 1,485 of 1,893 (78.4 %) with the engine's bits.
-   Riverwood, `coc Whiterun`, `coc WhiterunDragonsreach`, `coc Riverwood`: 0 holes, 0 claimed but not drawn, 0 cascade
    registrations under a removed entry, 0 registrations before the cascades were known. The Whiterun load stood the
    exclusion down for 47 frames (`stale`).
-   What the cascades still walk: the actors (an actor's entry is its cell's container, never removed), the entries with
    a native caster, and the unclaimed ones. The full-frustum cull itself (0.044 ms, mostly jobs) still runs.
-   Switches: the menu's "Take DCLF's objects out of the engine's sun culls" (`CS_DCLF_SUN_EXCLUDE`, default on) needs
    M1's switch. Needs the delta walk (`rootDependents`).

**A crash this run found, not of its making.** `coc Whiterun` crashed in the engine's material database on a loader
thread, with or without the exclusion, and not with DCLF off. The material cache held its materials through a
`BSTSmartPointer`, whose release deletes the material directly. The engine's own release (`FUN_1414f7a40`, the
manager at `0x143187758`) takes the database lock and removes the material from the database at zero. So when the
cache held the last reference, the database kept a pointer to freed memory, and the next load whose material hashed to
it called into it. The cache now holds its references through `SceneStore::MaterialReference`, which releases through
the engine's manager (AE only; the cache is off on SE and VR).

## Skylighting's occlusion map, drawn by DCLF

Community Shaders' Skylighting renders a sky occlusion height map every exterior frame, through the engine's
precipitation occlusion machinery: `Precipitation::SetupMask` culls and registers the scene from an orthographic
camera in a new sky direction each frame, and `RenderMask` draws it (engine notes, "The occlusion maps"). At
Riverwood that was 0.43 ms and 0.34 ms of the render thread a frame, the largest native view left after the sun.

**No ownership exception is needed.** The map is Skylighting's own texture (`texOcclusion`), and nothing but
Skylighting's `Prepass` compute reads it. So when both features run, DCLF can draw the whole map instead of sharing
it with the engine: there is no residue to withhold anything from, and no claims.

-   **Coverage, measured first** (`CS_DCLF_SKYLIGHT_PROBE`, TEMP): every geometry that built a pass in this view was
    a DCLF table object, about 600 a frame at Riverwood and 240 in Whiterun; none tracked without a record, none
    untracked.
-   **One rule.** Skylighting's pass rule (its replacement of the Lighting property's vfunc `0x2D`) is now the static
    `Skylighting::OcclusionTechnique`, which its hook and DCLF's scene phase both call: the scene phase gives every
    table object its Utility technique for the map (`Tables::skyTechnique`, the keys in `skyKeysUsed`).
-   **The build.** The frame's shadow build lists the occluders as a fourth input list (`kSkyMode`, no mode bits: the
    technique already carries `RenderDepth`), with records for the alpha-tested ones even when they cast no shadow,
    and pipelines for the map's own depth format. No claims are built for it.
-   **The view.** In Skylighting's `RenderOcclusion`, when `DrawcallLimitFix::SkyOcclusionReady` (this frame's
    shadow commit uploaded every occluder, none left out for a pipeline or a texture not yet resolved), `SetupMask` is
    skipped. `RenderMask` still runs: with an empty accumulator it only sets the camera, clears the map and computes
    the projection Skylighting samples with. DCLF takes the view at its `FinishAccumulating` hook (render mode `0x1C`,
    the precipitation accumulator, `inOcclusion`): the matrices from `VS_PerFrame`, the viewport, the target (import
    of target 10 as Skylighting has swapped it in).
-   **The epoch.** `IndirectDraws::ExecuteSkyOcclusion`, right after `RenderMask`, in its own segment (`SkyOcclusion`,
    between the Z-prepass and Light Limit Fix's culling): the shadow passes over one reserved view slot (`kSkySlot`),
    frustum-culled on the GPU, into the map. Everything but the slot's blocks, records and latch was uploaded by the
    frame's shadow commit.
-   **The fallback** is the engine: a frame DCLF cannot draw (the first frames, DCLF off, the toggle off, an interior)
    runs `SetupMask` as before.

**The rasterizer state.** The first parity runs had DCLF nearer over up to 65% of the map on some frames, never
farther. The Utility shader sets the cull mode per pass (0 for a two-sided property, 1 otherwise), so the state left at
`FinishAccumulating` is the last pass's, which was two-sided: DCLF drew every occluder without culling, and the back
faces of large terrain and cliff pieces showed at low sun directions. The view's state is now back-face culling at the
renderer's fill, bias and scissor modes; a two-sided occluder draws without culling, as a two-sided caster does.

**Parity** (`CS_DCLF_SKYLIGHT_PARITY=1`): every 120th map is rendered both ways in the same frame, the engine's first
(`SetupMask` and `RenderMask`, copied), then `RenderMask` again and DCLF's (copied), and the two are compared texel by
texel. The same frame lists the occluders in DCLF's frustum that the engine did not register.

| Riverwood and Whiterun, 13 comparisons | Result |
| --- | --- |
| Occluder sets | identical, but one NPC's held axe (`AnimObjectAxe`) now and then |
| Texels where DCLF is farther (an occluder missing) | 0 in every comparison |
| Texels where DCLF is nearer | 30 to 900 of 262,144 (0.01-0.3 %), most by one step (1.5e-5); 0 to 290 by more than 1/256 |

The differences above 1/256 are isolated single texels in tree canopies: alpha-tested, wind-animated leaves.

**Result** (Riverwood, Tracy coarse zones, render thread per frame):

| | Engine | DCLF |
| --- | --- | --- |
| `Precipitation::SetupMask` | 0.43 ms | not called |
| `Precipitation::RenderMask` | 0.34 ms | 0.022 ms (the camera, the clear, nothing to draw) |
| DCLF's epoch | - | 0.025-0.035 ms |
| `Main::RenderPlayerView` (inclusive) | 7.49 ms | 6.79 ms |

-   DCLF drew 300 maps of 300 in steady state (4,911 occluder inputs at Riverwood, 3,946 in Whiterun); the first 36
    of a session are the engine's, until the pipelines exist. 0 holes, and the live toggle hands the map back to the
    engine and takes it again.
-   The engine's precipitation mask (when there is precipitation) is still the engine's.
-   Switches: the menu's "Draw Skylighting's occlusion map" (`CS_DCLF_SKYLIGHT`, default on) needs DCLF's shadow
    views.

**The fade roots (2026-10-03).** On the teleport route the parity had DCLF nearer over up to 98% of the map, never
farther, at every teleported location. The dumped maps showed every tree, and some rocks, in DCLF's map and none in the
engine's. `SetupMask`'s cull runs without `cameraRelatedUpdates`, and `BSFadeNode::OnVisible` (`0x141479f50`) then does
not update the fade. With fades on, it goes into a node that is not settled only while `currentFade > 0` and
`fadeAmount != 0`. So the engine's map leaves out every root the main camera has faded out, and every root of a new cell
the main camera has not seen yet (its fade starts at 0).

DCLF's view now applies that rule (`kCullFadeOnVisible`, `BuildDrawsCS`'s `FadedOutOfOcclusion`, in place of the shadow
views' stood-in fading test). It uses `FadeStateCS`'s state, which only a root DCLF services has (`kFadeRootOwned`). An
occluder under any other fade node is not ready for the occluder phases (`IndirectDraws::PhaseReady`). The engine draws
it with its own `OnVisible`, and a change of ownership is a readiness event (`fadeOwnershipSerial`).

-   **With the stand-in off** (`CS_DCLF_PRIMARY_EXCLUDE=0`) every comparison is exact, because no root is owned and the
    engine draws every fade-node occluder.
-   **With the stand-in on**, the reference cull reads a stood-in root's node, which the fade write-back keeps at the
    GPU's milestones (faded out, fading, faded in: "The fade write-back"), and those decide the rule. The occlusion views
    read the GPU's states on every frame, parity frames included. The fade update itself is checked by
    `CS_DCLF_FADE_PARITY`. (Before the write-back, the node kept the fade the engine last left there, and parity frames
    read the nodes' states instead, through a buffer of their own.)

    The nodes take a milestone a few frames after the GPU (the readback's frame slots, then the next frame's job), so
    the reference lags while many roots start fading at once. On the teleport route (2026-10-04, two runs), each map
    was within 15 texels by more than 1/256 except in the first 17 s after the last teleport, while the camera turned
    newly loaded roots into view: there, a few maps had 2,500-19,000 texels, DCLF nearer. That is the
    reference's latency, not DCLF's map, which reads the current state.
-   **The cost.** The engine runs `SetupMask` on every frame that has an occluder under a root DCLF does not service,
    which is most frames.

**The rule was never applied (2026-10-04).** The occlusion views' latch never named a states buffer (`fadeStatesIndex`
was left 0), and their build (`ShadowBuildDrawsPass` with `sky`) bound no fade roots (`FadeRows` was `!sky`, from when
the occlusion views followed no fades), so `BuildDrawsCS` skipped the test (`FadeRootsIndex != 0`). Both are set now.
A probe that took the engine's registrations against the CPU's evaluation of the same rule found them in agreement
while the maps differed by up to 70% of their texels; after the fix no map differs by more than 0.8%.

**The depth pass's alpha reference.** What was left was single texels in foliage, DCLF always nearer. Under
`RENDER_DEPTH` with `ALPHA_TEST`, `Utility.hlsl` also discards below `PerGeometry AlphaTestRef.x`, which the shadow
maps (`RENDER_SHADOWMAP`) do not test and DCLF's shadow layout binds to the zero block. `BSUtilityShader::SetupGeometry`
(`0x1414fae40`) writes it from the Lighting property's alpha property: `0x3F7EFEFF` (about 0.996) while it blends, else
`threshold * (1/255) + 0x3B80802C` (just under one more step), and another `1/255` for a threshold of 4. The depth pass
now computes it from the object record (`DCLFDepthAlphaTestRef`: the threshold from `AlphaTestRef`, the blend from
`kRecordAlphaBlended`, which `kObjectAlphaBlended` sets). On the teleport route no map then has more than 10 texels
apart by more than 1/256; the rest differ by one 16-bit step (vertex transform precision).

**Skylighting's quadrants.** Skylighting's `SetViewFrustum` hook narrows the occlusion camera to one quarter of the
square each frame (`frameCount % 4`), before the projection is built, so the map covers that quarter at twice the
resolution, and `UpdateProbesCS` updates only the probes it covers (the others keep their accumulated values). The
captured view projection is that quarter's, so DCLF culls the same quarter with no planes of its own.

## The primary's cull without DCLF's objects (Phase 1 of the GPU-driven frame)

The main camera culled and registered every object DCLF draws, only for `PassCapture` to capture the passes and
withhold them. At Riverwood that was about 1.0 ms of cull and 0.9 ms of registration on job threads, and a 0.4-0.7 ms
wait on the render thread (`PreparePlayerView`). `PrimaryCull` takes DCLF's references out of that cull
([dclf-gpu-driven-frame.md](./dclf-gpu-driven-frame.md), "Phase 1, step 1" for the measurements it rests on).

**Where to cut.** The list processes' `Process1` (`BSGeometryListCullingProcess`, vtable slot `0x16`, AE `0x140e28390`)
is called by each list job for every entry after its first, and by every node's `OnVisible` for its children. It is
overridden, and filtered by process, because the vtable is shared with the sun's full-frustum processes and every
other list process. For an eligible entry it does, on the job's own thread, what the cull would have done, and the
entry's subtree is never traversed. That covers entries nested in multibounds too. Entry 0 of each list goes
through `Process2`, so it stays the engine's. (The first version filtered the lists on the render thread and put them
back after `Finish`; the decision, the fade calls and the list edits cost 0.24 ms of render thread, which the job-side
version reduces to about 0.02 ms.)

**Which entries** (per snapshot, `PrimaryCull::PlanOf`; the snapshot is the sun's candidates):

-   Nodes whose `OnVisible` is the plain recursion: `NiNode`, `BSMultiBoundNode` and `NiSwitchNode`, under a root that
    may also be a `BSFadeNode`, `BSLeafAnimNode` or `BSTreeNode`.
-   Geometries whose `OnVisible` is `BSGeometry`'s (the append to the process), at least one of which DCLF draws. Each
    geometry is DCLF's when the snapshot's per-geometry verdict says so: a main-pass table object that is neither a
    decal nor alpha-blended (`SceneStore::PrimaryEntryAllows`, `SunCandidates::primaryGeometry`; a change of any
    verdict bumps the generation). **Every other geometry is the engine's**, and is handed to its registration.
-   Admitted: the colour epoch drew every shown geometry of DCLF's in a frame the cull reached the entry in view
    (`Admit`, after the epoch, from `drawnFrame`). An admitted entry stays admitted, by node, across snapshots.
-   Frame preconditions: the sun's entry exclusion is live (its cascades are captured), and no local light cast shadows
    last frame (a synthetic pass has no point-light shadow). Otherwise, and whenever the candidates are stale, the
    engine culls everything.

**What stands in for the cull** (`StandIn`, per entry, on the job thread):

-   **The engine's `Process1` instead, for the frame:**
    -   a root that is fading or cross-fading LOD (`currentFade` or `fadeAmount` below 1, or `+0x153 & 0x70` not
        `0x20`: `GetRenderPasses` then adds the old level's hint-10 copy);
    -   a switch whose selected child is out of date (`childRevID[index] != revID`: `NiSwitchNode::OnVisible` brings it
        up to date with `UpdateDownwardPass` before culling it);
    -   an entry not yet admitted.
-   **The bound test** against the job's own planes, and the root's `kAccumulated` bit set or cleared, as `Process1`
    does (the tree clock reads it).
-   **The root's `OnVisible` update:**
    -   `BSFadeNode`: `FUN_14147a160`, or its own branch for LOD type 6;
    -   `BSLeafAnimNode`: `FUN_14147b110` and `FUN_14147a430` first;
    -   `BSTreeNode`: its height test (above the limit: nothing, and no recursion), the leaf update, and its LOD fix-up.
    -   A root that faded out draws nothing. One that starts to fade hands all of its geometries to the engine.
-   **Each geometry**, when not app-culled up to the root and when every switch above it selects its path:
    -   the engine's: its bound tested, then the process's `AppendVirtual`, in the traversal's order, so the
        registration jobs register it (decals keep their capture, order and native depth; effects and blended objects
        stay native);
    -   DCLF's: collected for a synthetic pass.
-   **After `Finish`**, on the render thread: the jobs' outputs are gathered, and DCLF's geometries have their
    `activeLightMask` cleared, as the main registration's `0xFFFF` would have. Not in the jobs, because the sun's
    `Accumulate` writes masks while they run.
-   **The synthetic passes** (`SyntheticPass`) are built on DCLF's worker (`primary synthetic passes`, about 0.09 ms)
    and joined at the accumulate phase, which takes them as if the engine had registered them:
    -   the derived pass descriptor;
    -   the sun's bits from the cascade test (`SunShadowBits`);
    -   the batch list, the hint and the LOD row.
-   A synthetic pass the colour epoch did not draw is a hole.

**The gates.**

-   A dry run (`CS_DCLF_PRIMARY_EXCLUDE=probe`) compared the synthetic pass with the engine's registered one for every
    object under a listed candidate. At Riverwood, 1,689 of 1,693 a frame agreed in every field; the rest are 2-3
    cascade-edge objects (the sun's bits, as in the sun's exclusion) and one blended decal, which is now the engine's.
-   The hole test covers the synthetic passes only. **The count of main-pass objects** (the tables report's "the
    engine also kept", which includes the synthetic ones) must match a run without the cut. It is what caught a
    defect the hole test could not see: CommonLib's `NiSwitchNode::index` is not AE's, every tree's switch read as
    selecting nothing, and 100 objects a frame went undrawn (1,793 against 1,894). The switch state is now read at the
    engine's offsets (`SceneStore::ReadSwitch`).

**Result** (full featureset, Riverwood, SkyrimEngineTelemetry's coarse zones, 20 s captures, ms per frame):

| | Off | Lists filtered on the render thread | In the jobs, with engine members, trees and switches |
| --- | --- | --- | --- |
| Primary cull, job threads | 1.00 | 0.66 | 0.80 |
| Primary registration, job threads | 0.93 | 0.53 | 0.31 |
| Render thread, waiting on the primary's jobs | 0.69 | 0.45 | 0.30 |
| Render thread, DCLF's share | - | 0.24 | 0.02 |

-   Riverwood: 827 entries stood in for, 1,432 synthetic passes, and 153 of the engine's geometries a frame handed to
    its registration. The captured registrations fall from 2,360 to about 580.
-   Whiterun: about 700 entries, 1,176 synthetic passes, and the captured registrations fall from about 685 to 282.
-   0 holes and 0 not modelled at Riverwood, Whiterun and Dragonsreach (an interior: the preconditions keep everything
    in).
-   Main-pass objects 1,902 against 1,894 without the cut: the 8 are members outside the frustum under an entry in
    view, which the engine would have rejected and the GPU culls.
-   Sun-mask share unchanged, capture parity OK.
-   What still keeps entries in view with the engine, Riverwood (`CS_DCLF_PRIMARY_REASONS=1`, TEMP):
    -   516 geometries a frame under entries with nothing DCLF draws (effects), which the engine keeps;
    -   13 under `BSValueNode` and 12 under `BSOrderedNode` (in Whiterun, 388 under `BSOrderedNode`).
-   Switches: the menu's "Take DCLF's objects out of the engine's main camera cull" (`CS_DCLF_PRIMARY_EXCLUDE`, default
    on) needs the sun's entry exclusion and static ownership. `probe` runs the census and the comparison instead.

**Open:**

-   `BSOrderedNode`: it starts its own alpha groups, so its geometries' `AppendVirtual` group index is not the entry's.
-   Local shadow lights: the synthetic pass needs the shadow-light part of the light selection (`FUN_1414fcf80`).
-   Actors (one entry holds them all) and portal interiors (rooms are the entries, and their cull uses the compound
    frustum).
-   The entries not yet admitted because they have not been in view (about 2,250 at Riverwood) are still tested by the
    engine at their root bound.

## The sun's bits on the GPU (culling-job elimination, phase 1)

The synthetic pass was rebuilt every frame for one input: ShadowDir and DefShadow (descriptor bits 13 and 14) depend
on this frame's cascades (`PrimaryCull::SunShadowBits`). That input is now the GPU's
([dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md), phase 1).

-   **The static rule.** `SunShadowStatic` is the bits an object takes when its bound meets a cascade: the
    accumulator's deferred flag, the alpha and fade conditions, the shadow passes and the global. A synthetic pass
    carries them, and `kObjectSunTest` (object flag bit 26) marks it for the test.
-   **The cascades.** The colour epoch's latch (`BuildDrawsLatch`, now 1,024 bytes) holds `sunState` (on, and the count)
    and up to four cascades' planes and custom planes, as `SunAccumulation` captured them from the engine's own cascade
    culls (`GpuCascades`).
-   **The test.** `BuildDrawsCS` tests every flagged colour input's bound against them (SunAccumulation's sphere test).
    On a miss it sets the top bit of the draw's object-index word (`kObjectSunMiss`).
-   **The shader.** `DCLFObjects.hlsli` reads the push word as `DCLFObjectWord` and derives `DCLFObjectIndex` (masked)
    and `DCLFSunMiss`. `Lighting.hlsl` reads its pass descriptor through `PixelDescriptor()`, which drops DefShadow and
    ShadowDir on a miss. The vertex descriptor never carried the two bits, so a draw is exactly what a pipeline without
    them would draw.
-   **One key for both sources.** An engine-registered pass of a geometry PrimaryCull draws synthetically takes the
    same static bits and test (`UnifySunBits`, on a frame the cut applies).
    -   Without it, admission was proven on the engine's pipeline (no bits outside the cascades), while the synthetic
        pass needed the sun-capable one, possibly not yet compiled.
    -   The first run found that: 175 holes over 89 frames after the shader cache was rebuilt.
-   **The gate.** At the sampled frame, the colour dispatch counts its flagged inputs and misses, and the CPU counts the
    same inputs against the same uploaded planes. The report reads `sun on the GPU ... <- OK`. They agreed exactly at
    Riverwood, in Whiterun, and through dusk (`set gamehour to 19.5`) and night (23.5):

| | Flagged draws | Missed every cascade (GPU = CPU) |
| --- | --- | --- |
| Riverwood, day | 1,537 | 341-343 |
| Riverwood, dusk | 1,537 | 280 |
| Riverwood, night | 1,537 | 285 |
| Whiterun | 456-460 | 112 |

**Results:**

-   0 holes.
-   Main-pass objects 1,901 at Riverwood (1,894 without the cut) and 555-563 in Whiterun (555). Capture parity OK.
-   Pipelines 72-74 at Riverwood, against 88-90: the variants that differed only in bits 13 and 14 are one now.
-   The synthetic job takes 0.03-0.09 ms on the worker. The tables report's "with the sun's shadow mask" now counts
    sun-capable pipelines (1,838); less the GPU's misses, that is about 1,496 against the engine's 1,487.
-   Switch: `CS_DCLF_SUN_GPU=0` keeps the per-frame CPU rule in the synthetic pass.
-   The synthetic pass now depends only on the object and the frame's globals. Making it table state, updated on
    events, is phase 4.

## Visibility feedback: the stood-in roots' state from the GPU (culling-job elimination, phase 2)

The primary's stand-in updated each stood-in root's engine state inside the list jobs: the fade, the leaf and tree LOD
(`ServiceFade`, `ServiceTree`) and the tree clock's `kAccumulated`. That state now comes from the GPU's own
visibility, a frame or more later, on DCLF's worker
([dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md), phase 2). What the frame draws stays in the jobs:
the bound test that picks the synthetic passes, the tree's height test, and handing the engine's members over.

**The GPU side.** BuildDraws' depth phase 1 writes, for every candidate whose bound is inside the main camera's frustum,
the frame's stamp into a per-object buffer (`cs.dclf.frustum`, `BuildDrawsConstants::frustumIndex`; 0 in every other
dispatch). This is the frustum alone: occlusion does not stop the engine's `OnVisible`. It is never cleared, because a
stale stamp is simply not the frame's.

**The readback, asynchronous** (as BasicRenderer's `CLodStreamingSystem` streaming readback):

-   A ring of readback-heap slots (at least 4, at least the frames in flight), each Free, Recording, Submitted or
    Decoding.
-   A timeline of DCLF's own.
-   The colour commit arms a Free slot for the frame (`ArmFeedback`), tagged with the frame's stood-in entries.
-   A copy pass after the main opaque draws (`FeedbackPass`) copies the stamps into it and reserves the slot's fence
    value (`ExternalSignalReservation`), which the framework signals after the copy. A copy abandoned before
    submission frees its slot.
-   With no Free slot the frame is dropped, never waited for.
-   A worker job polls the timeline and decodes every completed slot in fence order (`DrainVisibilityFeedback`).
    Nothing assumes one frame in flight.

**The decode** (`PrimaryCull::ConsumeFeedback`). For each entry stood in for in that frame:

-   **In view** (any of its synthetic members' stamps is the frame's):
    -   the root's update, `ServiceFade`, or `ServiceTreeState` for a tree;
    -   `kAccumulated` set, with an atomic OR (other threads touch other bits).
-   **Out of view:** `kAccumulated` cleared.
-   A root that starts to fade is seen by the next frame's stand-in (not settled), which leaves the entry to the engine.
    Between the two, one frame draws the synthetic passes: the latency the design allows.

**Two defects found on the way:**

-   **Freed nodes.** The first version serviced roots through the snapshot's raw pointers a frame later. On `coc
    Whiterun` a cell unload freed them first, and a later registration crashed on the damage. The tag now holds the
    roots (`NiPointer`), taken while the list jobs had just traversed them, and releases them on the render thread
    after the join, never on the worker.
-   **Wrong types.** `FUN_14147b110`'s LOD output is a float, and `FUN_14147a430` takes it in `XMM1`
    (`BSLeafAnimNode::OnVisible`, `0x14147ca3e`). DCLF passed an integer since leaf support began, so the leaf LOD step
    read a garbage level. Fixed for both the stand-in and the decode.

**Timing and placement.** Kicked right after the list jobs, the decode overlapped the registration jobs and the render
thread's wait grew by about 0.1 ms. It is now kicked after the synthetic passes' join (after registration) and joined
at Present (`PrimaryCull::EndFrame`), before the next frame's `Main::Update` reads the tree bits.

**Results** (Riverwood, full featureset):

-   Every frame armed and decoded, 0 dropped, 0 abandoned. Per frame, 827 stood-in entries, 826-827 of them in view by
    the GPU (the jobs' CPU test said 827). The decode takes 0.06 ms on the worker.
-   The tree clock: 39 stood-in trees in view a frame; 97-99 % of decodes see `+0x164` advanced since the last. The
    manager is time-budgeted, as it is with the engine's own cull.
-   0 holes; main-pass objects 1,916 (1,894 without the cut) at Riverwood and 555 (555) in Whiterun. No crash over
    Riverwood, Whiterun and Dragonsreach.
-   Render thread's wait on the primary's jobs, alternating captures on the same build: 0.395 and 0.376 ms with the
    feedback off, 0.399 and 0.375 ms with it on.
-   Switches: `CS_DCLF_FEEDBACK=0` keeps the services in the list jobs; `CS_DCLF_FEEDBACK_PROBE=1` (TEMP) reports the
    tree clock.
-   **Not built:** the fade parity probe of the plan (the engine's update against DCLF's on the same nodes). The decode
    calls the engine's own functions, one frame later; a parity of the state machines themselves would need a node
    both update, which the stand-in rules out by design.

## Fades on the GPU

The feedback above keeps the stood-in roots' fade and LOD state on the CPU: the render thread joins a worker's decode,
which runs the engine's fade functions per root a frame late. The engine's functions read only the node and globals and
write only the node ([skyrim-engine-notes.md](./skyrim-engine-notes.md), "Fade state: inputs, outputs and their other
readers"), so the same state machine runs on the GPU instead, in the frame it is for. Step E1 builds it beside the CPU's,
which stays authoritative until the GPU's agrees.

**The tables** (`SceneStore::Tables::fadeRoots`, `Records.h`, `FadeRootStatic`). A slot per fade node a member draws
under, listed with the member (`ListFadeRoot`, beside the tree slots) and released with its last one:

-   The static row is read from the node when it is listed (`FadeState::StaticOf`): its state as `initial`, its bound
    radius, `fadeAmount`, near and far, `+0x109`, its plan by its `OnVisible` (fade, leaf, tree or another class), and the
    LOD step's distance scale, `powf`/`logf` of the radius computed with the engine's own CRT (the GPU cannot round them
    the same).
-   A tree's LOD switch selecting past child 0 (`BSTreeNode::OnVisible`'s LOD fix-up) is an input the tree manager
    changes: the switch events refresh it (`RefreshFadeRootSwitch`).
-   The root's centre is a member's record (the fade node row, which placements keep current), chosen again when
    membership changes.

**The pass** (`FadeStateCS.hlsl`, `cs.dclf.z.fade-state`), first in the depth segment's epoch, one thread per root:

-   A state row whose generation is not its static row's starts from `initial`.
-   Once a frame, a root the main camera's cull would visit gets its class's `OnVisible`: the tree height test, the leaf
    LOD step, the fade update, the tree's LOD fix-up. The verdict records what it saw and whether `OnVisible` would go on
    into the children. Which roots the cull visits is `Process1`'s test, compound frustum included ("Roots with
    engine-drawn parts", below); it was the depth segment's frustum alone until F2.
-   The frame's inputs (`FadeFrame`: the cull's camera and the fade globals) are a one-row buffer the depth commit writes.

**The port** (`Scene/FadeState.cpp`) is the same state machine in C++, instruction for instruction (operation order,
`MINSS`/`MAXSS`, the comparisons' unordered cases). `CS_DCLF_FADE_PARITY` checks both sides of it:

-   the port against the engine's functions, run on a copy of the node with the fade watch muted (`FadeState::CheckPort`,
    64 snapshot roots every 30 frames at `AfterFullFrustum`);
-   the pass against the port: every 30 frames the pass logs 64 roots' state before and after, read back three frames
    later and made again by the port with the GPU's frustum verdict (`ReadFadeLog`). The GPU's division and square root
    may round differently, so equal integers and bits with floats within 1e-5 count apart, as rounding.

Measured (e1-parity, Riverwood, camera turning): the port against the engine 1,216 roots, 0 differ; the pass against
the port 1,280 updates, 0 differ (96 within rounding). The pass takes 0.006 ms of GPU.

**The members follow it (E2a).** A root is *owned* (`kFadeRootOwned`) while PrimaryCull stands in for its admitted entry
(`SyncFadeOwnership`, on a new snapshot or an admission). Only then does nothing but DCLF run its `OnVisible`:

-   The draw input names the root (`DrawInput::fadeRoot`), and the depth segment's first phase drops a member while its
    owned root's latest serviced `OnVisible` stops (`kFadeVerdictServiced` without `kFadeVerdictDrawn`), as a final
    verdict. The pass runs just before it in the same epoch, so the cut is the frame's own, as the engine's cull is.
-   A root's state is seeded again from its node when it becomes owned, and every owned root after frames the cut did not
    apply (`ReseedOwnedFadeRoots`): the engine's `OnVisible` ran on them meanwhile.
-   Members of a root DCLF does not own (an actor's, an entry not admitted) keep the distance test (`kObjectFadeTest`).
-   `kObjectFadedOut`, `Tables::fadedOut`, `SetFadedOut` and `ApplyFadeChanges` are gone. The decode still services the
    nodes for the engine's own readers (the next step's subject); its verdict is only counted ("faded out" in the
    feedback report).

Measured (e2a-parity): holes 0, every parity OK (the Shield race apart); per sampled frame 32-151 members dropped by
their owned root's state.

**The nodes follow it (E2b).** The engine still reads an owned root's node: the other views' culls (the point lights'
shadow culls, the sun's cascade residue, the precipitation mask when the engine draws it; not the water reflections,
whose cube map culls only LOD and the sky: "Which native views touch DCLF's roots") run `BSFadeNode::OnVisible` on it, and
`GetRenderPasses` and DCLF's own classification (the shadow Faded rule, the skin's LOD row) read its fade and level. So:

-   An owned root without engine-drawn parts is **written back** (`kFadeRootWriteBack`). When an update changes what
    those readers see (the fade, its target bit, the levels and transition, the blend, the `fadeAmount` fade), the pass
    appends the state (`FadeChange`) to a buffer the depth commit zeroes. The visibility feedback copies it with the
    stamps; the decode takes the changes, and after the join the render thread writes them onto the nodes
    (`SceneStore::ApplyFadeChanges`, checked against the row's generation) and reports a changed fade or level to the
    fade watch, as the engine's writers are. The LOD metric and the last-visible frame ride along with a change, not
    every frame: the other views' LOD fades read a metric from the last change.
-   Lost changes are sent again whole: past the buffer (it grows to the appends counted), or a frame whose changes no
    feedback copy took (no colour epoch, no free slot, an abandoned copy). The next update appends every write-back root.
-   Such a root's `kAccumulated` is cleared once: its trees are TreeWindCS's, so the tree clock need not advance them.
-   The decode services only roots with engine-drawn parts (`Cut::decode`): the stand-in lists no other entry.

Measured (e2b-parity, Riverwood, camera turning): holes 0, every parity OK (the Shield race apart). The decode went
from 8,287-9,128 stood-in entries a frame to 520-577, and from 0.37-0.55 ms to 0.08-0.10 ms on the worker. Write-back:
0-1,856 changes per 300 frames (341 reported to the fade watch); no frame lost.

The write-back is gone since R3 ("No fade write-back").

## The shadow build kicked at the end of the scene phase

With the fade cut on the GPU nothing in the primary's cull writes the tables, so the shadow build's inputs are final at
the end of the scene phase (1.2 ms into the frame) rather than at BeforeShadowMaps (5.85 ms):

-   `KickShadowBuildEarly` kicks it there, behind the placement job on the worker. `KeepEarlyShadowBuild` keeps it at
    BeforeShadowMaps when nothing it read moved: the change logs it takes (`SceneStore::ShadowInputsWitness`: the notes of
    `kShadowChangeCauses` and the geometry slots written) and the inputs the epoch compares. Otherwise it is kicked again
    there, as before, and the cause is counted (`async shadow early`).
-   The placement join writes the tables the build reads, so before either join (BeforeShadowMaps, or EarlyPrepass in a
    frame without shadow maps) the early build is finished first (`BeforePlacementJoin`: waited for up to the async
    budget, or dropped). It has ~4.6 ms of slack.
-   Two inputs became the epoch's, not the build's. The sun's full-frustum planes (the cull that sets them runs after
    the scene phase) were only ever the epoch's latch's. CS's SharedData and FeatureData, which the water reflections'
    prepasses refresh every frame after the scene phase: the build places their blocks, and the shadow commit writes the
    frame's bytes over them. The epoch compares only their sizes.

Measured (e3-parity2, e3-waits; Riverwood, camera turning): every parity OK (the Shield race apart), holes 0. The early
build is kept on 291-300 of 300 frames (the rest: the views' raster states moved). Render-thread waits 0.010-0.027
ms/frame (from ~0.10), with no shadow join among them.

## Owned members' light masks, without a clear

The main registration reads a geometry's `activeLightMask` and clears it, so every later registration in the frame (the
reflections, the other views) reads 0. An owned member (`PrimaryCull::Owned`: a member the colour build draws) has no
main registration, so the render thread used to clear the masks of every member in view after the list jobs: per
member, per frame. The writers now leave them 0 instead:

-   `SunAccumulation`'s registration thunk (every `FUN_140e28af0` registration): a shadow light's accumulation of an
    owned geometry (`+0x160` neither 0 nor `0xFFFF`), and a sun cascade's registration of an unclaimed caster, run as
    before and then leave its mask 0 (`ClearOwnedMask`); a claimed caster's mask write (`WriteMaskOnly`) becomes the
    same; no registration of one takes the sun's bits (`ApplySunBits`).
-   A geometry the colour build starts drawing has its mask cleared once (`PrimaryCull::Admit`, the newly drawn): bits
    written while it was the engine's or not yet drawn (out of view inside a cascade, say) have no reader to clear them.
-   `CS_DCLF_PERSISTENT_PARITY` checks it after the registration jobs (`CheckLightMasks`): owned members in view with any
    bit, by the sun's and other lights'.

Measured (e4-parity5, e4-plain; Riverwood, camera turning): light masks 0 of 0.95-2.0 million checked per window, every
other parity OK (the Shield race apart), holes 0. The render thread's work after the list jobs 0.14-0.27 -> 0.011 ms a
frame. The first version left the sun's bits on a few geometries (the unclaimed cascade path, then bits from before the
geometry was drawn), which the check named.

## The stand-in without per-member work

With fades, light masks and the decode off it, the stand-in's walk of an admitted entry (its bound test, then each
member's hidden, switch and binding test) only fed the hole check, unless the entry has something for the registration.
So the walk is per entry, by event (`Cut::walk`):

-   Walked: an entry with engine-drawn members (handed to the registration every frame), or with a member shown but not
    bound (handed over until it is). Every other admitted entry is left to the GPU whole: one lookup in the list job.
-   Kept by events (`RefreshWalk`): a new snapshot; a member's residency dropped (`NoteMemberLost`, from
    `DropResidentSlot`: walked until the next frame says whether a shown member is unbound, or hidden or unselected
    after all); a member newly drawn; every residency ended (`NoteAllMembersLost`).
-   Hidden members and switch selections need nothing here: they leave membership through classification (the hidden
    stores and switch events).
-   `CS_DCLF_PERSISTENT_PARITY` walks every entry, for the hole check and the light masks, and counts what an entry the
    walk would skip handed to the registration (`stand-in walk parity`, 0).

The decode is no longer joined (`PollFeedback`): the job's last store publishes it (`decodeReady`), and the render
thread applies a finished one at the full-frustum cull and at Present; one still running waits for the next poll. Its
service of the mixed roots is gone since F2 ("Roots with engine-drawn parts"); it takes the write-back changes alone.

Measured (e5a-parity, e5a-plain, e5b-parity; Riverwood, camera turning): every parity OK (the Shield race apart), holes
0, stand-in walk parity 0. Entries walked a frame 9,128 -> ~577 (the mixed ones). The decode: 300 kicked, 0 joined,
every frame decoded.

## The occlusion maps, drawn by DCLF

The engine's precipitation occlusion mask (where rain and snow stop) is a depth view from above: `Precipitation::SetupMask`
culls and registers every scene list (`FUN_1414bf320(ctx, 1)` over `0x14338c870`), `Precipitation::RenderMask` renders
it into depth target 10. Skylighting hooks the call (`Main_Precipitation_RenderOcclusion`) and renders it first, then its
own sky occlusion map with the same machinery, which DCLF already drew natively. Both are now one kind of view, an
*occlusion view* (`Records.h`, `kOcclusionViews`: `kOcclusionSky`, `kOcclusionPrecipitation`):

-   Per view: its pass rule (`Skylighting::OcclusionTechnique` with `a_skylighting` true or false: the precipitation mask
    takes no skinned object, no multi-texture landscape, and none of the sky map's BSX filter), technique column and
    pipeline keys (`Tables::occlusionTechnique`, `occlusionKeysUsed`), shadow-build mode (`OcclusionModeOf`), view slot
    (`OcclusionSlot`), depth target (`OcclusionDepthTarget`; the sky map's is target 10 while Skylighting swaps its own
    texture in, the mask's target 10 itself), capture (`CaptureOcclusion`, at FinishAccumulating for render mode 0x1C:
    the sky map while Skylighting's `inOcclusion`, else the mask) and readiness (`OcclusionReady`).
-   Skylighting asks for each map (`DrawcallLimitFix::OcclusionReady`); when DCLF is ready it skips `SetupMask` (sets the
    projection and the accumulator's camera itself), and `RenderMask` only clears. `DrawOcclusion`, after the sky map's
    `RenderMask`, draws every map asked for in one epoch (`ExecuteOcclusion`); nothing reads the mask in between.
-   `CS_DCLF_SKYLIGHT_PARITY` compares either map texel by texel (the engine's render kept, the map cleared, DCLF's).
    The mask's texture is taken when its engine render is kept, before Skylighting swaps target 10.
-   `CS_DCLF_PRECIPITATION=0` leaves the mask to the engine; without Skylighting the engine draws both.

Measured (p1-parity, p2-rain with `CS_DCLF_TEST_COMMANDS='100:fw 10a241'`, Riverwood, camera turning): the sky map's
parity as before the change; the mask drawn by DCLF 300 of 300 frames once it rains (18,700 occluders); its parity
~99.6% of texels equal, DCLF never farther, nearer on ~1,000 texels (the sky map's pattern); every other parity OK.

## The scene lists without DCLF's roots

`DrawWorld_BuildSceneLists` (`0x14064bc20`, a job of `Main::Draw`'s, finished before the shadow lights) deals every
reference root of the loaded cells round robin into the six scene lists (`skyrim-engine-notes.md`, "The primary's cull:
the scene lists"). Three readers walk every entry: the main camera's list jobs, the sun's full-frustum cull, and
`Precipitation::SetupMask`. For a root DCLF draws whole, all three only produced results DCLF threw away: the stand-in's
lookup, the full-frustum cull followed by the exclusion filter that took the root back out of its `objectArray`, and an
occlusion map DCLF draws itself.

**The filter** (`CS_DCLF_LIST_FILTER`, on unless `0`; `PrimaryCull::PublishListFilter`, `FilterSceneLists`):

-   At the scene phase's end, before `Main::Draw` queues the build, the render thread publishes the roots this frame's
    lists leave out. A root qualifies when its entry is admitted, has nothing for the registration (`Cut::walk` clear: no
    engine-drawn part, every member bound), and the exclusion the coming full-frustum cull applies takes it out of the
    sun's cascades (the pending `SunExclusion`, for the same snapshot). Both occlusion maps must be DCLF's, and the decal
    order must not be the lists' (`CS_DCLF_DECAL_ORDER=engine`, and its probe). The set is rebuilt only when an entry's
    verdict changes; Present withdraws it.
-   A detour on `BuildSceneLists` removes those roots after the engine's build, on the build's own job. Each list keeps
    an entry: its job sets the process's frustum up from the first.
-   **Put back** (`RestoreSceneLists`): when the engine draws an occlusion map after all (DCLF's not ready, or a texel
    parity frame's reference render), the left-out roots are appended to the lists before `SetupMask` culls them.
    Nothing reads the lists between the list jobs and `ClearLists` but the occlusion maps.
-   **Checks:** a parity frame (`CS_DCLF_PERSISTENT_PARITY`, every 60th) leaves the lists whole, and the stand-in counts
    any root the filter would have left out that it did not skip (not admitted, walked, or a geometry handed to the
    registration). The report also counts filtered frames whose full-frustum cull applied no exclusion, and members whose
    binding was lost while their root was out of the lists. All must be 0.

Measured (f1a-plain and f1a-parity2, Riverwood, camera turning, rain from frame 100):
-   About 8,500 of 11,100 entries are left out per frame.
-   The stand-in sees 777 entries a frame (9,128 before), and the sun's `objectArray` holds 539 (2,163 before).
-   The full-frustum cull went from about 0.10 to 0.045 ms on the render thread, and its exclusion filter from 0.089
    to 0.046 ms.
-   Every check above 0, holes 0, and both occlusion parities as before.

**Kept across frames** (`Engine/SceneLists.cpp`; `PrimaryCull::ListMode`). The engine's build walks the object root
(`ShadowSceneNode` child 3: its two whole entries, then every cell's listed category nodes and their roots) the same
way every frame in an exterior. Only what follows the camera changes: a portal graph's rooms and occlusion planes, and
the extra list. So the object root's part of the lists is DCLF's:
-   **Keep:** when nothing it reads moved, the lists stay as the last frame left them. `ClearLists` skips the scene
    lists, and the build trims each list back to its kept part.
-   **Rebuild:** when something moved, DCLF builds that part itself on the build's job (`EnumerateSceneLists`, less
    the filter's roots), in about 0.25 ms for about 3,950 entries.
-   **Either way the engine's build still runs**, for the extra list and the camera's part. Its `AsNode` call on the
    object root (`0x14064be5a`, `mov rax,[rcx]; call [rax+0x18]`) is patched to return null, so it skips the walk.
-   **Engine:** the engine's own build, filtered as above. Used in an interior or with no unbound space (its other
    branches), and on parity frames. The lists are cleared first: `Main::Draw` clears them only at the frame's end,
    after the occlusion maps, so after a kept frame they still hold DCLF's build. Missing this put every root in
    twice and hung the render thread.

What a rebuild is triggered by:
-   **Hidden roots stay in kept lists.** The readers do not skip a hidden entry: `Process1` has no hidden test, and the
    list cull (`FUN_140e28f70`) skips one only when its context's `+0x61` is set, which `FUN_1414bf2b0` leaves 0. A
    detour on the list cull sets that flag for the scene lists and the extra list (`CullList`). That is exact: the
    engine's build leaves hidden roots out, and the cull skips hidden children itself. Each list starts with an empty
    node that is never hidden, so its cull's `Process2` still sets the frustum up.
-   **Structure:** an attach or detach of a cell or a category node, but not under the two whole entries (the actors).
    The attach/detach detours report it (`NoteListStructure`). A root attached under or detached from a category node
    is not a rebuild: it goes into a queue (`listEvents`, in event order), and the next build applies it to the kept
    lists (`ApplyListEvents`). An attach appends the root to the next list in the round robin, unless the category node
    is not listed, the root is DCLF's, or it is category node 6's first child. A detach swap-removes it, by the root's
    place (`listPositions`). An attach holds its root until applied, and the job releases nothing itself: everything
    goes to Present.
-   **A hidden bit** written on one of those nodes (`NoteHiddenKey`, from SceneStore's hidden events).
-   **A new filter.** The filter is built again only when the cut's admissions or walks move (`cutVersion`), or the
    exclusion's content does (`SunExclusion::version`, carried over by a reuse).
-   **The build's globals:** the list count, the worldspace flag that skips category node 2, and the object root.
-   **The lists not being DCLF's:** after an engine frame, or after roots were put back for an occlusion map.

Two rules keep this safe:
-   Roots the build's job lets go of are released at Present (`listGraveyard`), where the engine unloads. A kept list
    may hold a detached cell's last reference.
-   **Child indices run to `_freeIdx`** (`NiTArray::free_idx()`, null entries included), as the engine's do. CommonLib's
    `size()` counts only the non-null entries, so a node with holes lost its last children. The list parity found this.
    The same fix went to `SceneTracker`'s detach by index, the stand-in's switch children, the category lookup and the
    decal order's child index.

**Check:** on a parity frame, the engine's lists are compared with DCLF's enumeration. Every root of the engine's must
be in it, and every other root must be hidden.

Measured (f1c-parity; Riverwood, camera turning, rain from frame 100):
-   285 of 300 frames keep the lists. The 10 rebuilds a report follow the parity's own frames (the engine's build,
    and roots put back for the occlusion parity).
-   About 1,000-1,500 root events a report, of which about 30 change a list.
-   The list parity: 0 of the engine's roots missing, 0 extra roots not hidden.
-   Holes 0, and every other parity check as before.

## Roots with engine-drawn parts, culled by the engine

An admitted entry with engine-drawn parts (`Cut::mixed`) was stood in like any other. Its node, which the engine's own
draws of those parts read, was serviced by the visibility feedback's decode: the frustum stamps were read back each frame,
and the engine's fade functions were run on a copy of the node a frame late. Now the engine culls such an entry whole:
-   **The stand-in passes it to the engine's `Process1`.** Its `OnVisible` updates the node the engine's parts are drawn
    from. DCLF's members under it are kept from the registration by the `AppendVirtual` hook, and FadeStateCS runs the same
    update for them as an owned root without write-back.
-   **Gone:** the decode's service (`ConsumeFeedback`, `ServiceWrite`, `Cut::decode`, the feedback tag), and the frustum
    stamps' copy and its staging buffers. The feedback ring carries FadeStateCS's changes for the write-back roots and
    nothing else.

**The engine's cull test, on the GPU.** Two copies of one state machine stay equal only on equal inputs. A new
`CS_DCLF_FADE_PARITY` check snapshots each logged mixed root's node at the depth commit (after the list jobs' `OnVisible`)
and compares it with FadeStateCS's state that frame. It differed on most roots: the engine left `lastVisible` and the
LOD metric at 0 while the GPU's moved. The list processes cull through a compound frustum, the portal graph's portals
and occlusion planes: at Riverwood about 46 operators over 22 plane sets, rejecting about 90% of the mixed roots the
frustum lets through. `Process1` (`0x140e28390`, list processes recurse to geometry) runs `OnVisible` when:
-   the node is `kAlwaysDraw` (flag bit 11), or the cull mode is 1;
-   the node is pre-processed (bit 12) and the process does not ignore pre-processing (and is not in cull mode 4): then
    unless bit 20 is set, with no test;
-   a compound frustum is active (operators present, cull mode not 3): the view test (unless `skipViewFrustum`), then
    `BSCompoundFrustum::Process` (`0x140e320b0`);
-   otherwise: the view test.

`BSCompoundFrustum::Process` is a small program:
-   Each operator is 12 bytes: {type, next if true, next if false}. Type 2 accepts and type 3 rejects.
-   Type 7 passes unless the bound is wholly outside an active plane of its set. Type 8 passes unless the bound is wholly
    inside every active plane of its set (an occluder's volume). For both, the plane set index is the next record's
    first word.
-   The planes it deactivates on the way (a bound wholly inside one) change no outcome within a call, and `Process1`
    restores them around each node.

So FadeStateCS repeats that test (`EngineInView`, `CompoundVisible`):
-   **The block** (`Records.h`, `kFadeVisibility*`) holds the process's cull mode and flags, then the operator records
    and the plane sets, whole. The program references records past `freeOp` and sets past `freePlane`, so copying only
    the in-use counts broke the test.
-   **When it is sampled:** the compound frustum exists only while a list job culls through it. The first list job to
    see it samples the block (`PrimaryCull::StandIn`); without one, the render thread samples after the jobs. It is
    uploaded with the depth commit.
-   **The root's flags** (always-draw, pre-processed, bit 20) join its static row (`kFadeRootAlwaysDraw`,
    `kFadeRootPreprocessed`, `kFadeRootPreprocessHidden`).
-   **The view test** uses the process's own planes and their active mask, carried in the same block
    (`kFadeVisibilityViewPlanes`), as its sphere test (`FUN_140d3ff10`) reads them. Planes taken from the depth latch's
    view-projection disagreed with the engine at the frustum's edge, about one root in thirty, in the LOD metric.
-   **The bound's radius** is the record's sun entry row, which the placements keep current. A member's sun entry node is
    its reference root, which is the fade root. The listed radius is 0 for a dynamic reference listed before its first
    bound.
-   **Placement snaps:** the cell attach's snap of a fade node (`FUN_14147aa20`: its fade, faded-in flag, snap radius
    and last visible frame) is an event (`fadeSnapEvents`). The root's row is taken from the node again, at a new
    generation (`ReseedFadeRoot`): a dynamic reference can be listed before its snap.

This corrects DCLF's fade semantics for every owned root, not only the mixed ones. Since E1 the GPU had been stepping
the fade of roots behind an occlusion plane, which the engine leaves alone. The older parities could not see it: they
replay the GPU's own in-view verdict.

Measured (f2-parity4; Riverwood, camera turning, rain from frame 100):
-   577 mixed entries a frame culled by the engine.
-   FadeStateCS against the engine's node: 381 checks, all exact or within rounding.
-   The pass against the port as before; holes 0; every parity OK (the Shield race apart).
-   The compound frustum never outgrew the block.

## Owned trees off the tree manager's animation list

`BSTreeManager` (`0x1420F6A18`) has two lists ([skyrim-engine-notes.md](./skyrim-engine-notes.md), "The tree
manager"):
-   **The tree list** (`+0x20`) holds `BSTreeNode`s. Its loop (`FUN_140437e50`) sets the LOD switch and animates the
    bones of the near trees.
-   **The animation list** (`+0x50`) holds `BSLeafAnimNode`s. Its loop (`FUN_1404381e0`) runs every frame in
    `Main::Update` and advances each node's wind clock and gust when the node's `kAccumulated` bit is set.

At Riverwood, in this load order, the tree list is empty and the animation list holds 2,562 nodes. A root DCLF stands
in has its `kAccumulated` cleared and its wind on the GPU (TreeWindCS). So the loop only tested those roots, but it
still visited every one of them each frame.

**What changes** (`Engine/TreeAnimation.cpp`, `CS_DCLF_TREE_LIST`, on by default):
-   **Taking roots off.** `PrimaryCull::SyncFadeOwnership` passes the owned roots without engine-drawn parts to
    `TreeAnimation::SetOwned`. That runs on a new snapshot or an admission, never per frame. The roots are taken off
    the animation list in one pass, and roots no longer owned are put back. DCLF keeps the list's reference to each
    node it took off.
-   **The lock.** Every edit runs under the manager's own lock (`+0x18`, a `BSSpinLock`), the one the engine's add,
    remove and update take. The set of nodes taken off is guarded by the same lock.
-   **The engine's edits are detoured**, so its list membership stays exact:
    -   A remove (`FUN_140437840`) of a node that is off the list forgets it and drops the list's reference, as the
        engine's remove would have.
    -   An add (`FUN_1404376f0`) pushes an event. The render thread takes the node off again if it is owned.
    -   A clear (`FUN_140437ae0`, from `TES` and the manager's teardown) forgets every node that is off.
-   **Parity** (with `CS_DCLF_PERSISTENT_PARITY`): the report scans the list for owned roots still on it.

**Native views.** Native draws of an owned tree (the native point-light casters; not the water cube map, which draws only
LOD and the sky) read the node's wind clock, and so did
before this change: `kAccumulated` was already clear, so the clock did not advance.

**Measured** (Riverwood, camera turning): 2,173 roots off the list, 389 left on it. The manager's own measure of its
update (`+0x7C`) went from 0.13-0.24 ms to 0.05-0.07 ms a frame on the main thread (`CS_DCLF_TREE_LIST=0` against on).
Parity: 0 owned roots on the list, holes 0, every other check as before.

## Switch selection by event (culling-job elimination, phase 3)

Which child an `NiSwitchNode` draws was a per-frame test in two places:
-   the scene walk, which re-read every entry under a switch every frame (the `kTraitSwitch` light path);
-   the primary's stand-in, which read every switch on a member's path in the list jobs, and left the entry to the
    engine when the selected child was out of date.

Both now follow events ([dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md), phase 3).

**The writers** (AE 1.6.1170; every `mov [reg+0x12C]` in `.text`, [skyrim-engine-notes.md](./skyrim-engine-notes.md),
"Switch nodes"):

-   **`BSTreeManager`'s LOD selection**, in `Main::Update` (`FUN_140437e50`, five stores; `FUN_140438840`, two), and
    the local map's (`FUN_140438580`, one), on a tree's LOD switch (`BSTreeNode` `+0x180`). They leave the new child
    as it was, for `NiSwitchNode::OnVisible` to bring up to date in the cull. The manager can store twice in one pass:
    a level, then the far level.
-   **Harvesting** (`FUN_1401e8ef0`, from `TESObjectTREE::Activate` and the flora's) and **a harvestable's 3D setup**
    (`FUN_1401e9450`), on the switch under the reference's root. Both update the switch right after.
-   **`NiSwitchNode`'s own `AttachChild`, `DetachChild` and `SetAt`** reset `revID` to 1. That leaves the selected
    child out of date with no store to the index, and `DetachChild` clears the index when the selected slot empties.
-   Construction, cloning and loading happen before the node is in the scene; the attach covers them.

**The hooks** (`SceneStore::InstallSceneEvents`, `CS_DCLF_SWITCH_EVENTS`, default on with the scene delta):

-   The ten stores are patched with a call to an Xbyak stub, after every site's bytes are checked; if any site
    differs, none is patched and the switch events stay off. The stub saves the volatile registers, the flags (a store
    sets none, so the code after it may test flags set before it) and `xmm0-5`, and calls `SwitchIndexStore`. That
    makes the store itself and pushes an event carrying the index before it, when the value changed.
-   `NiSwitchNode`'s child edits are detoured (its vtable's implementations of slots `0x35`, `0x37`-`0x3C`) and push a
    structural event.
-   Only the render thread's writes make events (`switchEventThread`). A loader thread's are left out: its subtree is not
    in the scene yet, and its attach brings the switches up to date. Holding a reference to a node a loader is still
    assembling is not safe ("Resident entries", the crash).
-   The events go on a lock-free stack from the writer's thread. `ProcessEvents` drains them into one pending entry
    per switch, which keeps the index before the oldest event. A load screen discards them; the rescan's walk covers
    that.

**Applying them** (`SceneStore::ApplySwitchEvents`, render thread, before the walk's first round):

-   A switch in the scene whose index differs from its oldest event's value, or that had a structural event, is
    brought up to date. `CatchUpSwitch` is `NiSwitchNode::OnVisible`'s catch-up outside the cull: when
    `childRevID[index] != revID`, `childRevID.SetAt(index, revID)` (`FUN_140d29990`), then the child's
    `UpdateDownwardPass` (vtable slot `0x2C`) with `{ savedTime, flags bit 1 }`.
-   Every tracked entry under the switch is classified again.
-   The switch is listed for `PrimaryCull` (`TakeSwitchChanges`).
-   The attach and rescan walk (`AddSubtree`) brings every switch it passes up to date the same way. A newly attached
    switch's selected child is out of date until its first cull.

So a selected child is never out of date when the walk classifies it or the list jobs reach it. An entry under a switch
is no longer evaluated every frame for its selection: it drops the switch trait, and an unselected child no longer counts
as able to get a record (`PerFrameOf`).

**The primary's stand-in** keeps a per-member bit, `memberLive`: every switch on the member's path selects it.
-   It is read from the switches for a new snapshot, after a resync (a full walk or dropped events), and after a frame
    the cut skipped.
-   Otherwise it is read again only for the entries a switch event names (`switchEntry`).
-   The list jobs read the bit, and the stale-child fallback is gone.
-   `CS_DCLF_SWITCH_EVENTS=0` restores both per-frame tests.

**Validation** (full featureset, Riverwood, then `coc` to Whiterun, Dragonsreach and back):

-   Walk parity OK in every window (0 stale verdicts, 0 stale traits) and 0 holes.
-   `CS_DCLF_SWITCH_PROBE=1` (TEMP) reads the switches in the list jobs, as the old test did:
    -   0 members whose `memberLive` differs from their switches;
    -   0 selected children out of date.
    -   A few sightings in 300 frames are animated switches (fish buckets) whose own per-frame update runs alongside
        the list jobs, caught between its `revID` bump and its child's update (`revID` one ahead). The probe counts
        them apart: not stale.
-   The tree manager: 3.2 events a frame after the `coc` to Whiterun, 0.48 net changes, 1.7 entries classified again;
    none at rest.
-   `CS_DCLF_TEST_HARVEST=<frame>` (TEMP): the player harvests the flora and trees within 4000 units whose produce is
    switched.
    -   Eight harvests (mushrooms, egg nests, a fish): each an event, a net change and a reclassification, with parity
        OK.
    -   150 frames later the harness sets the same switches back to child 0 through the stub's handler, with no update
        pass, as the tree manager leaves a switch: the walk caught up all eight (`CatchUpSwitch`), with parity OK and
        the probe clean.
    -   The passes handed back to the native loop in those windows (147, 91) match a run with the events off (143, 84):
        records made again after a switch change, not holes.

**Cost** (Riverwood, four alternating 45 s runs):
-   The per-frame set is 1,057 entries, against 1,707 without the events (the light path kept 10, against 633).
-   The scene phase took 0.61-0.69 ms, against 0.66-0.84 ms without the events.
-   The event drain is unchanged (0.05-0.06 ms).
-   The ten stubs take 285 bytes of the SKSE trampoline, which went from 2 to 4 KiB.

## Scene membership (bindings by events)

An eligible object's main-pass binding (pipeline slot, material slot, shading, lights' room) is decided when the scene
phase writes its record, not by what the engine registered this frame. The camera no longer adds or drops bindings.

-   **Binding.** `WriteObject` queues every eligible, non-shadow-only record it writes (`bindQueue`), whether the object
    entered the scene or an event rewrote it. `BindByMembership` builds the object's pass from the object alone
    (`PrimaryCull::MembershipPass`), and the accumulate phase patches it once, as a member (`kObjectMember`, the
    resident records). The queue waits until the engine's Lighting shader exists, because the material records are its
    `SetupMaterial`.
-   **Rewrites keep the binding.** A member written again (a skin, a face or a mover, every frame; or an event) keeps
    its slot, its patch and its membership. It is bound again only when what the binding reads changed
    (`MemberBindingStands`: the derivation cache's witnesses, meaning property, material, geometry slot, fade state,
    alpha and interior). Animated shading and the extras rows (projected UV, land blend) are per-frame inputs that the
    watch resamples (`RefreshFrameConstants`), so members take them too.
-   **Fade-out.** The GPU's fade test measures from the object's own fade node (`Tables::lodFade`, which is now written
    for every object with a fade node; `kLodFadeHeld` marks the ones without the draw's LOD fades), not from its entry
    root. So an object whose fade node is not its entry root can be a member too.
-   **Moves are not exits.** The engine moves physics objects between a cell's dynamic and static nodes as they wake and
    sleep. Each move is a detach followed by an attach. A detached geometry that is back under a tracked category node
    by the end of the event batch keeps its entry (`detachMoves`; the attach has evaluated it again). About 3,000-10,000
    moves happen per 5 s in Riverwood, and before this change every one of them released a member and its material.
-   **Sun candidates.** The per-root signature is a set hash, so a moved geometry, which is listed again at the end of
    its root's dependents, does not rebuild the snapshot.

**Lifetimes are usage counts.** Geometry, pipeline and material slots are all freed at their last reference, after
the journal batch (`SlotTable::DrainUnreferenced`), with no idle-frame grace. A game buffer's `GpuResources` entry lives
exactly as long as a lease on it does: the geometry slots hold the leases, the registry holds entries weakly, and the
last lease's release queues the removal. There is no touch and no eviction age, and rejections are not cached. A
written material is re-evaluated in place while its slot is referenced. The category-node set is rebuilt only on a
signature change or a detach (no 30-frame backstop).

Fading objects, skins whose LOD row selects no partition, and decals became members later ("Every eligible object a
member", below). No eligible object is on the engine's registered path any more.

Measured (Riverwood, camera turning, 5 s intervals):
-   Material slots retired on last reference: 46 / 207 / 123 / 113 per interval. Before membership it was about 600
    per 6 s, and with membership but before the rewrite and move fixes it was 310-520.
-   About 3,050 material slots are alive, up from about 2,100, because every eligible object in the scene is now bound.
-   Scene phase 1.3-1.8 ms and accumulate phase 0.8-1.1 ms, the same range as before.
-   Resident parity: 0 of about 99,000 records differ. Holes: 0.

## The primary's cull with scene membership

Members (above) are drawn from their records whenever the GPU finds them. What is left of the engine's main-camera cull
for DCLF's objects is only what the engine itself would do to their nodes.

-   **The stand-in** (`PrimaryCull::StandIn`, the list processes' `Process1`) returns for an admitted entry without
    traversing it. It builds no passes. Its members in view are recorded for the light-mask clear and the hole check.
    A DCLF geometry not bound yet, like an engine member (an effect, a blended object), is handed to the registration
    (`AppendVirtual`).
-   **Admission by readiness** (`Admit`): an entry is admitted once the colour build draws every DCLF member it shows,
    in view or not. The check runs when a member's drawn mark turns on, and once per entry of a new snapshot. It only
    runs against a current snapshot, and skips members no longer tracked: during a cell load a stale snapshot's
    geometry may already be released.
-   **Fades** go through the feedback, not the engine's cull:
    -   A fading root stays stood in. The decode steps its fade with the engine's own functions (`ServiceFade`), from
        the GPU's frustum stamps, one frame late as the engine's 2-frame window allows. A LOD cross-fade stays stood in too (none occur
        in SE's settings: "LOD cross-fades").
    -   The engine draws a fading opaque object as a plain opaque pass (no screen-door and no blend at Riverwood, measured).
    -   So a member only needs the cutoff: a root whose fade service says `OnVisible` stops (`currentFade` or
        `fadeAmount` 0) marks its members `kObjectFadedOut` (`Tables::fadedOut`, on a change only), and BuildDraws
        drops them while still stamping them, so the root can fade back in.
    -   The service's own writes of `currentFade` are reported to the fade watch (`SceneStore::NoteFadeChanged`).
    -   A member's `MaterialData.z` is the material's alpha, not the property's `alpha`, which `GetRenderPasses`
        leaves multiplied by whichever camera's fade it last saw.
-   **Local shadow lights** no longer stop the stand-in: a member's are the GPU's (`LocalShadowLights`).
-   **Leaf exclusion** (`PrimaryCull::Owned`, the list processes' `AppendVirtual`): wherever the engine still culls
    (the actor entry, an entry not admitted, a stale snapshot), an owned geometry is not handed to the
    registration. Owned means a member that the colour build draws (the claims).
-   The entry residency below (joins, probation, evictions) is gone: membership replaced it.

**Found along the way: character-lit materials lost t11 every other frame.** The character light's t11 ping-pongs
between two render targets, and the material lookups keep the other incarnation's binding
(`alternateCharacterLight`). Since imports became asynchronous, that binding could have been kept while pending (no
index), and was then reused forever on that parity. Every actor part's draw dropped on alternate frames: 9,000-26,000
holes per 300 frames. An alternate is now reused only with its index.

Measured (Riverwood, camera turning):
-   9,128 of 9,130 entries stood in for.
-   0 passes of members registered by the engine after load. The engine's registrations went from about 750 to 520-630 a frame.
-   Holes 0, resident parity 0 of about 99,000.
-   Scene phase 1.2-1.3 ms (from 1.8-2.0), accumulate phase 0.6-0.8 ms (from 0.9-1.1).

## Every eligible object a member

The last three cases that still needed the engine's main-camera registration are members, and the registered path is
gone. What the engine registers for the main camera is read for diagnostics only.

-   **Fading objects.** `SyntheticPass` judges the object's settled state (the material's alpha, the alpha property's
    blending), not its current fade. `GetRenderPasses` (AE `1414adfb0`) draws a screen-door fade of an unblended, fully
    opaque material as the plain opaque pass with `alpha = materialAlpha`. It takes hint 9 (blended, sorted) when
    screen-door is not possible or the object is blended, and hint 10 for a LOD cross-fade's old-level copy (flag bit 19).
    A member's fade is the feedback's (`kObjectFadedOut`).
-   **Skins whose LOD row selects no partition.** They stay members that draw nothing, instead of being released as
    Hidden and bound again when the row changes (the NPC shields and weapons that rebuilt their materials over and
    over). `Tables::skinPartitions` is 16 bits with `kNoPartitions` (bit 8) for "none"; `SkinPartitionsOf` feeds the walk
    and `KeepSkin`, `PartitionDraws` counts the draws for every reader, and BuildDraws returns early on `kNoPartitions`.
    The row is the fade node's LOD level (`+0x152 & 0xF`), which `GetRenderPasses` also reads, so there is no per-pass
    mask. Its only writer, `FUN_14147a430` (called from `FUN_14147a160`, `BSLeafAnimNode::OnVisible` and `ServiceFade`),
    is detoured into the fade watch, which rewrites the skin when the level changes.
-   **Decals.** A decal's group comes from the settled state as `GetRenderPasses` computes it:
    `2 + (materialAlpha < 1 or blended)`. Every decal without `kMultiIndexSnow` takes DoAlphaTest. `PrimaryEntryAllows`
    stands decals in (both groups) while `CS_DCLF_DECALS` is on. Their depth is DCLF's decal depth pass ("Decal depth").

### Decal order

The engine draws a decal chain (a group's technique bucket and sub-pass list) in reverse registration order, so
overlapping decals need the engine's registration order, reconstructed without the registration
(`Scene/SceneStore/DecalOrder.cpp`):

-   `DoSceneListAccumRegisterJob` is one main-accumulator job. It runs `ProcessJob` (`140e28af0`, under a global lock)
    over the list processes in order.
-   Each process registers its plain appends (`+0x128`) in traversal order, then its alpha and `BSOrderedNode` groups
    (`+0x301d8`, queued by `AppendVirtual`, `140e288e0`, for a group other than -1).
-   `BGSDecalNode::OnVisible` visits its decals' `Get3D()` last to first.
-   The first job culls list 0, then the extra list (`0x338c888`).

The key is (list, ordered group, position in the list, child path from the entry; a `BGSDecalNode`'s children by
reverse index). `CS_DCLF_DECAL_ORDER_PROBE` checks it against the captured registrations while the engine still
registers decals (for example with `CS_DCLF_DECALS=0`): 0 of about 24.5 million pairs out of order.

The engine's own order is not stable. `BuildSceneLists` (`14064bc20`) deals the roots round robin into the lists and
skips hidden ones, so showing or hiding any root earlier in the scene reorders the decal roots after it: on 14-24% of
frames while the camera turns. So there are two modes (`CS_DCLF_DECAL_ORDER`):

-   `stable` (default): the scene's order (the key from the top of the scene), sorted again only when the member decals
    change (`memberDecalsChanged`).
-   `engine`: the frame's scene lists, every frame, with the extra list after list 0. It reproduces the engine's
    swaps.

`OrderDecals` sorts by chain ascending, then the key descending, then the object, and writes `Tables::decalOrdinal`
and `decalCount`. Both are kept with the objects across walks and cleared only when the objects are.

### The registered path, retired

-   The accumulate phase's pass table holds the frame's membership joins alone. The registered fill
    (`CollectAccumulatedPasses`, `CS_DCLF_PASS_PARITY`), `LapseAccumulated`, the main pass's `Withhold` and
    `HandBackUndrawable` are gone. `DrainCapture` feeds the diagnostics, including "registrations of eligible objects
    not bound".
-   Every record with bindings is a member (`kObjectMember`); every other record is a culling candidate
    (`kObjectNoBindings`). `kObjectNativeVisible`, BuildDraws' `RequireNativeVisible` gate and the counters measured
    against the engine's cull (engine-culled, false negatives, rescued, occluded-visible) went with it.
-   Leaf exclusion is live whenever ownership is on; the stand-in keeps its own switch. The hole detector covers what was
    left out of the engine's cull (`StoodInMembers`), and set parity's "withheld" bit means the same.
-   Admission carried across snapshots is keyed by the root's member set (`MemberSignature`), so attaching an object
    to an admitted root checks it again.
-   A pipeline's per-frame constants are evaluated from a member's property with the frame's Lighting pass
    (`TemplatePassOf`), since no member has a registered pass of its own.

**Material slots follow their references.** `ProcessMaterialWrites` first counts the references from the frame's
joins (`UpdateSlotReferences`), then frees only unreferenced slots. A referenced slot whose evaluation fails keeps its
last record and is evaluated again next frame (`materialEvaluationsPending`, `materialsHeld`). Before this, a written
material that members still used could be freed, and `Permanent Decal` objects lost their draw during loads. Under
`CS_DCLF_PERSISTENT_PARITY`, `DrainUnreferenced` reports a freed material that a bound object still references.

The engine still draws a member for one frame when DCLF first draws it (its claim is published after the colour
epoch). That is the same pixels twice, not a hole: about 1,300 objects during a load, about 2 per 300 frames after.

Measured (Riverwood, camera turning):
-   Skins: joins that failed on the frame's verdict 44 -> 7, material slots retired 565 -> 354 per interval.
-   Decals: 2,202 member decals; decal parity 0 differ in both order modes; 9,128 of 9,128 entries stood in.
-   Holes 0, including loads and `CS_DCLF_PRIMARY_EXCLUDE=0`; constants parity 690 of 690 pipelines; resident, build,
    persistent and Skylighting parity clean.

## Resident entries (culling-job elimination, phase 4)

Superseded by scene membership (above); kept for the reasoning.


A stood-in entry still took its list job every frame: the frustum test picked which synthetic passes were built, and
each synthetic pass went through the accumulate phase, which restored it at the next walk. A **resident** entry needs
neither ([dclf-cull-job-elimination.md](./dclf-cull-job-elimination.md), "Phase 4 in detail").

**Its records persist** (`SceneStore`'s resident records):
-   Each DCLF member's record is patched once, from its synthetic pass, through the accumulate phase's own patch
    (`AccumulatedPass::resident`). It is not in `accumulatePatched`, so no walk restores it.
-   It keeps `kObjectNativeVisible` (since removed: every bound record is a member), so BuildDraws draws it whenever the
    GPU's cull finds it. There is no CPU visibility for it at all.
-   Each frame the accumulate phase only takes its pipeline's lighting template again (`KeepResidentsAlive`); the used
    sets change only with membership (since the used-set cleanup). `RefreshFrameConstants` resamples its shading
    with every other bound record's.

**Its list job returns at once** (`StandIn`, a lookup by root). The feedback decode services its root with the stood-in
entries (fade, LOD, `kAccumulated`).

**Joining** (`JoinEntry`, render thread, at most 256 a frame of each kind): entries the stand-in reached admitted and
settled join in `PrepareFrame`; entries not yet admitted join on probation after the list jobs (below). Either way:
-   any plan but `Rejected`: plain, fade, leaf and tree roots;
-   a fade root settled and not fading out whatever its distance (`+0x109` bit 0);
-   every member the switches select DCLF's (the engine's members keep the entry in the stand-in until phase 5), shown
    (nothing app-culled up to the root) and `ResidentCapable`: a record written only by events. That excludes faces,
    actors, animated shading and records written in full every frame. A kept skin qualifies: `AppendKeptSkin` writes
    its palette rows and partition mask every frame, from the same LOD row the synthetic pass reads, and a skin it
    cannot keep is written in full, which ends the residency;
-   every selected member's synthetic pass built at the join, without extras rows (projected UV, land blending), and no
    decal under the GPU's fade or height test (decals are never in the depth segment, whose first phase makes both).

Members no switch selects are left out of the resident's members; a switch change under the root ends the residency.

`ResidentRefusal` says why an entry was refused, and the report counts it by cause.

A refused entry is not offered again for 120 frames. The list jobs skip offering it (`joinBlocked`), so the render
thread's cost before the jobs is back to 0.003 ms.

**Leaving:**
-   the walk rewrites or releases a record (`WriteObject`, `ReleaseObjectSlot`, the slot check);
-   something is attached under the root or detached from it (the walk's dirty sun entry nodes);
-   a switch under the root changes its selection (`TakeSwitchChanges`; a resync compares every resident's selection,
    `SelectionSame`);
-   the decode finds the root fading, or its passes stale (below);
-   the new snapshot's plan or selected members differ;
-   a patch fails;
-   a probation join the build did not draw in full.

**Every resident leaves** on a frame the cut does not apply (a local shadow light, the sun's exclusion not live, the
menu toggle), and when the frame globals the static sun bits read change. A stale snapshot does not end residency.

**The walk parity** compares a resident record as the walk left it (its accumulated half reset).

**Defects found:**
-   **Joiners' passes replayed.** A frame that failed its preconditions returned before clearing the last frame's
    joiners' passes. The accumulate phase then patched them in a frame the engine culled everything, and the engine's
    pass won: about 2,000 failed joins at the `coc`. `PrepareFrame` now clears them first, and the accumulate phase
    takes them only on a frame whose residents are live.
-   **A crash while loading** (`rab7-off`, with residency off) on a `QueuedTree` background thread, in Havok's
    collision setup (`FUN_140ea5cc0`): a child with a null vtable under a tree's `FadeNode Anim`, whose child is the
    tree's LOD switch. The switch events took references to switches from any thread, including loaders assembling a
    subtree. They are now taken on the render thread only (`switchEventThread`). A subtree built on a loader is not in
    the scene until its attach, which brings its switches up to date (`AddSubtree`). It was the first crash with that
    signature, so this cause is likely, not proven.

**Validation** (full featureset, the tour Riverwood, Whiterun, Dragonsreach):
-   `CS_DCLF_RESIDENT_PARITY=1`: every 60 frames each resident's synthetic pass is built from scratch and compared with
    the one it was patched with, and its record with the patch. 0 differ in every window.
-   Walk parity OK and 0 holes in every window.
-   Main-pass objects: in Whiterun 303 kept by the engine plus 252 resident, against 555 without the cut. At Riverwood
    about 1,110 plus about 800 resident, against 1,894-1,917 in other runs.
-   Riverwood: about 580 resident entries (800 records) of 827 stood in for. The rest are trees (39), entries with the
    engine's members, and records that need extras rows. Whiterun: about 180 resident entries, 252 records.
-   Whiterun's local shadow lights fail the preconditions a few times a window, and every resident leaves and joins again
    (about 1,300 joins per 300 frames). Lifting that precondition is phase 6.
-   Walk parity also caught a guard's shield or symbol kept hidden in Whiterun, with residency on and off alike. It is
    the actor-equipment class the scene-delta work met before, and is left open.

**Cost** (Riverwood, four alternating 20 s Tracy captures, ms per frame):

| | Off | On |
| --- | --- | --- |
| DCLF's tables, render thread | 1.09-1.13 | 0.98-1.00 |
| of it, the accumulate phase | 0.43-0.45 | 0.30 |
| Render thread, waiting on the primary's jobs | 0.36, 0.43 | 0.38, 0.41 |
| The primary's list jobs (cull), job threads | 0.94, 0.98 | 0.93, 0.94 |

-   Synthetic passes per frame fall from 1,432 to about 630.
-   The list jobs cost the same: their time goes to the entries the engine still culls (effects, trees, entries with the
    engine's members, entries not yet admitted).
-   An earlier build offered every refused entry again every frame. That cost 0.02 ms before the jobs were queued and
    pushed their start back 11-20 µs.
-   Switches: `CS_DCLF_RESIDENT=0` turns residency off; `CS_DCLF_RESIDENT_PARITY=1` runs the check.

### Step 2: admission from readiness (probation)

An entry the engine never showed was never admitted, so it stayed in its list job for good. Now it joins on probation.

**The join.** The stand-in runs the engine's `Process1` on a non-admitted entry, as before. When that leaves the root
without `kAccumulated` (out of view, or culled by the engine's occlusion), the entry is offered for probation.
`AfterListJobs` joins those offers the same frame: their records are patched in this frame's accumulate phase, while
nothing draws them. The root is outside the engine's view, and the GPU's frustum is the engine's, so no draw depends on
them yet.

**The admission.** After the colour epoch, `Admit` checks each probation join: when the build drew every member (its
pipeline, material and record were ready), the entry is admitted. Otherwise the next `PrepareFrame` ends it, before any
list job can return at once for it, and it is not offered again for 120 frames.

**The fade distance on the GPU.** Nothing on the CPU services a resident while it is out of view. When it comes into
view past its fade-out distance, BSFadeNode::OnVisible would snap its fade to 0 (FUN_14147a160: a root not visible last
frame does not fade gradually), but its kept record would draw it for the frame or two the feedback takes. So the depth
segment's first phase tests it:
-   **The distance** (`FadeDistanceOf`, engine notes "Fade distance"). The fade value falls below the fade-out
    threshold (`0x142032e38`) where the root's distance times the camera's LOD factor passes
    `(n + (1 - threshold)(f - n)) * divisor`. Here `n` and `f` are the node's near and far distances (`+0x128`,
    `+0x12C`) times `0x142032e48`, and the divisor is the LOD type's (`0x142032e00[type]`). Types 6 and 8, and the
    fades-off and LOD-updates-off globals, give no test.
-   **The row.** A resident fade root's objects carry the distance (`Tables::fadeDistance`, `kObjectFadeTest`), and
    the depth inputs carry it with the entry root's centre (`Tables::sunEntry`, which the walk keeps current when the
    root moves). The latch carries the main camera's position and LOD factor (`PrimaryCull::FadeEye`). `DrawInput` grew
    to 64 bytes for the row.
-   **The rule.** Past the distance and not in view last frame, the draw is dropped as a final verdict, so the colour
    segment and phase 2 follow it. The frustum stamp's bit 31 (`kFrustumFadeHidden`) remembers the drop, and a dropped
    object stays dropped while it is past the distance. One that was in view and drawn is drawn on: the engine fades it
    out over frames, and the feedback hands it over within a frame or two, the latency every stood-in root has.
-   **Validation.** A TEMP probe compared the CPU form of the distance with the engine's own servicing: 947 residents in
    view past it were all fading after the servicing, and none within it was. To get objects past their distance inside
    the loaded cells, a TEMP switch (`CS_DCLF_TEST_FADE_DIVISOR`) lowered the objects' divisor from this INI's 30 to 1.5.
    With `CS_DCLF_TEST_MOVE` and `CS_DCLF_TEST_TURN` carrying the player 12,000 units and turning back, the GPU dropped
    12 and then 119 residents in view. The witness ended every resident when the divisor changed, and holes and set
    parity stayed at 0.

**Stale passes.** The decode checks every resident fade root in every decoded frame, in view or not, and whether or not
its tag's snapshot is current (the tag holds the roots). A root that is not settled, whose LOD level (`+0x152`, the LOD
row) changed, or whose LOD metric crossed the specular or envmap fade end (`LodFadeStateOf`, when a member's derivation
reads it) goes back to the stand-in (`FadeWitnessOf`). The decode's own servicing is not the only writer: the LOD rows
that went stale in testing belonged to residents out of DCLF's view, which another view's `OnVisible` had updated.

**The frame globals** the passes and the rows read (`ResidentWitness`: the static sun bits, the fade globals, the type
divisors) end every resident when they change.

### Step 3: trees

-   **The height test on the GPU.** `BSTreeNode::OnVisible` draws nothing of a tree whose root is above the frame's
    height limit. A resident tree's objects carry `kObjectHeightTest`, the latch carries the base and the limit
    (`PrimaryCull::TreeHeightTest`; +infinity when the list processes' test is off), and the depth segment's first phase
    drops them as a final verdict. The decode does not service a tree above the limit, as `OnVisible` would not.
-   **The animation.** A tree's wind state is the tree manager's (`FUN_1404381e0`: it advances `+0x164` and derives
    `+0x15C` for the trees whose `kAccumulated` bit is set, which the feedback keeps). `KeepResidentsAlive` takes a
    resident tree's `TreeParams` and `WindTimers` from the node every frame (`DeriveTreeAnim`), as the accumulate phase
    does for every other drawn tree. Replacing the tree manager itself on the GPU is not attempted: its update is
    time-budgeted and runs under a mutex on the engine's side, and nothing else needs the cull for it.
-   **Skins.** Trees' LOD partitions are skins, and skins were not resident before. A kept skin's per-frame work is the
    scene half only (above). A defect this uncovered: the accumulate phase added every skinned object to
    `accumulatePatched` when it set the partition mask, including a joining resident. The next walk then restored the
    record while the slot stayed resident, which left it drawn by nobody. Resident parity caught 2,285 such records.

### Results (steps 2 and 3)

Full featureset, Riverwood and the tour, `CS_DCLF_RESIDENT_PARITY=1`, `CS_DCLF_SET_PARITY=1`:
-   **Resident parity: 0 differ** in every window. A root fading at the moment of the check is not compared, and is
    counted separately: the next decode ends it.
-   **0 holes, and set parity 0** in every window: no depth without colour, and no colour without depth.
-   **Riverwood: about 2,150 resident entries a frame** (about 930 fade, 1,000 leaf and 220 tree roots; 2,990 records),
    against about 580 before step 2. Whiterun: about 1,200.
-   **The main pass's object count no longer matches a run without the cut, by design.** Probation joins entries the
    engine culled by occlusion as well as by the frustum. Those are culled by the GPU (frustum and HZB) instead of the
    engine: at Riverwood about 2,650 resident records are inside the frustum a frame, while the engine kept about 925
    of them. The engine keeps about 985 objects of its own, against 1,910 without the cut.
-   **Refusals** are almost all entries with an engine member (1,100-1,700 a window at Riverwood, phase 5), and passes
    the synthetic pass cannot build (about 300).

**Cost** (Riverwood, 20 s Tracy captures, ms per frame, render thread unless noted):

| | Residency off | Probation off | Probation on |
| --- | --- | --- | --- |
| `Main::RenderPlayerView` | 7.07, 7.40 | 6.96, 7.28 | 7.95, 7.71 |
| Waiting on the primary's list jobs | 0.42, 0.47 | 0.38, 0.43 | 0.35, 0.32 |
| The list jobs' cull (job threads) | 0.92, 0.99 | 0.89, 0.93 | 0.74, 0.70 |
| `RenderBatches` (the colour epoch's join) | 2.24, 2.34 | 2.28, 2.35 | 2.79, 2.74 |
| `Main::RenderDepth` (the z-prepass epoch's join) | 0.56, 0.58 | 0.57, 0.62 | 0.89, 0.84 |

-   Probation takes about 0.2 ms of cull off the list jobs and 0.1 ms of waiting off the render thread.
-   **It costs more than that in DCLF's own build.** Both epochs' builds grow from about 1,830 drawable candidates to
    about 4,230. The worker builds take about 0.4 ms longer each, and the render thread waits on them (about +0.5 ms in
    `RenderBatches`, +0.3 ms in `RenderDepth`). The CPU build is per drawable candidate per frame, however few the GPU
    then draws. The fix is DCLF's, not the engine's: resident draws that persist across frames instead of being built
    every frame.
-   Switches: `CS_DCLF_RESIDENT_PROBATION=0` turns probation off (residents are then admitted from a draw, as in
    step 1).
-   The build's cost is fixed by the persistent resident draws (next section).

## Persistent resident draws

The main epochs' builds wrote a draw input, a sequence template and a drawn mark for every drawable candidate every frame.
With probation that was about 4,230 candidates, and the render thread waited on it. A resident's draw input changes
only when the resident does, so each main segment now keeps its residents' inputs across frames.

**The region** (`ResidentRegion`, one per segment in its `BuildCache`):
-   **The inputs.** A dense array of the resident records' draw inputs, at the head of the segment's input buffer. The
    Z-prepass now has an input buffer of its own (`Resources::inputsDepth`), since each segment's region must survive
    the other's upload. The frame's own inputs follow the region, and BuildDraws reads both as one list.
-   **What it holds.** A resident record the per-frame loop would draw as one input with its pair's record. Decals
    (their slot is a per-frame ordinal), face shapes and second-stream pipelines stay with the loop, and so does
    anything past half of the input, draw or record capacity. The loop skips the region's objects.
-   **The records.** Each (material, pipeline) pair the region uses has a stable record slot (`records[0, slotCount)`).
    Its record is assembled there once per build, which is once per pair, not per draw. The per-object loop's record
    assembly became a lambda for this (`assembleRecord`). A per-frame draw of the same pair reuses the slot.
-   **The upload.** When the region's version is not the one the segment's buffer holds (`Resources::residentUploaded`,
    written by the commit that uploads it), the commit uploads only the entries changed since that version (`dirty`).
    After a resync it uploads the whole region.
-   **The rest of the build's output.** The region adds its sequence count to the draw capacity and its inputs to the
    dispatch count. Its drawable entries go into the drawn set and the per-object states, so the native skip set, the
    Z-prepass rule and set parity see them as before. The sun's CPU check covers its inputs too.

**What changes it**, and nothing else:
-   **SceneStore's change log** (`Tables::changeLog`, append-only; "Persistent draw state" below). The region takes the
    entries whose causes a draw input carries (placement, bindings, skin, geometry, membership) for the slots
    `Tables::residentSlot` says are resident. Each region reads the log from its own position. One that fell behind the
    trimmed head, or whose tables generation changed, reads every resident slot again (a resync).
-   **A pipeline's set index changing**, or **a pair's record failing or recovering**: that pipeline's or pair's
    entries are written again (drawable only while both are good).
-   **The Z-prepass waits** for the colour epoch's first draw of a joiner (`drewLastFrame`) before its depth entry
    exists (`pendingAdds`): depth only for what colour drew last frame, as the loop's rule has it.

**Defects found:**
-   **The first change feed lost changes.** It was a per-frame list, cleared when the scene phase began, so a change
    noted outside the window between that point and the builds never reached the regions. Stale entries then drew a
    freed slot's old geometry at the slot's new object's transform: the player drawn as a rock and a plant, and
    mountains where the village should be. The region parity showed 3,788 entries against 2,297 resident slots. The
    append-only log fixed it: the regions then matched the resident slots exactly.
-   **The first build lost the device.** It is the same stale-entry defect, as far as can be told: it did not recur
    once the log replaced the list.
-   **Every change re-uploaded the whole region.** About 33 residents a frame move (their bound really changes), so the
    whole region (190 KB) went up every frame. Only the changed entries go now.
-   **A quadratic pending check.** The Z-prepass's pending joins were searched per log entry; they are marked per slot
    now.

**Checks** (`CS_DCLF_RESIDENT_DRAW_PARITY=1`, every 60 frames): each region entry is written again from the tables and
compared byte for byte, and every resident the region should hold is looked for. The tour (Riverwood, Whiterun,
Dragonsreach) gave 0 differences over 148,787 entry checks and 0 missing, with resident parity, set parity and holes all
at 0.

**Cost** (Riverwood, 20 s Tracy captures after `coc` at frame 300, probation on, ms per frame):

| | Region off | Region on |
| --- | --- | --- |
| `Main::RenderPlayerView` | 7.18, 6.44 | 6.33, 5.94 |
| `RenderBatches` (the colour epoch's join) | 2.40, 2.29 | 2.02, 2.05 |
| `Main::RenderDepth` (the Z-prepass epoch's join) | 0.72, 0.69 | 0.41, 0.45 |
| Colour build on the worker | 1.20, 1.13 | 0.89, 0.90 |

-   About 2,990 persistent inputs (3,410 sequences) behind 240 pairs at Riverwood; about 2,000 in Whiterun.
-   Both joins are now below their cost with no residency at all (2.34 and 0.58 ms), so probation's cost is gone.
-   **Still per frame, O(residents):**
    -   the drawn set and the per-object states the region hands the commit;
    -   the per-object records (`BindlessObject`) the build writes for every object, resident or not;
    -   the constants arena.
-   Switches: `CS_DCLF_RESIDENT_DRAWS=0` turns the region off; `CS_DCLF_RESIDENT_DRAW_PARITY=1` runs the check.

## Persistent draw state

The resident draws showed the model; this moves everything DCLF builds from the tables to it. Every per-object value has
one writer, which notes a change only when the value differs, and every structure built from the tables reads those
notes from its own position and uploads only what changed. The plan, by step: the change log (done), stable offsets,
one persistent object-record buffer, persistent constants and binding records, the whole scene's draw inputs with the
drawn set, the shadow build, and the remaining scans.

The CPU storage path retains up to two spare vectors per `KeptArray`, reusing a vector only after all
snapshot readers release it. Copies still copy every element, including commit-time patches outside
the journal. Object-record updates deduplicate repeated log entries for the same final table row;
resident inputs are journalled only when their bytes differ. Dense dirty tables use bitmap range
coalescing, with the original sorter retained for sparse byte arenas. `CS_DCLF_CPU_STORAGE_REUSE=0`
selects the allocation/per-event/unconditional-input-write/sort control path for correctness and timing
comparisons; it does not change scheduling. Standalone regression tests live in
`tests/DrawcallLimitFixCPU` (configure with CMake, build, then run CTest).

### Step 1: the change log covers every record

**The log** (`Tables::changeLog`): an entry is a slot and what changed (`ChangeCause`): placement, bindings (flags,
material, pipeline, fade distance), shading (with the emissive multiplier and wetness), lights, tree animation, skin
(partitions, bone offset and rows), extras, shadow columns, geometry, membership (residency). A reader skips entries
whose causes it does not read. The log keeps its tail (it is halved past 128k entries); a reader behind its base, and a
full walk, a reset or a load (`InvalidateChangeLog`), make every reader read every slot again.

**The writers:**
-   **A write in full** (the delta walk's `WriteObject`, a reset, a patch, a lapse) is noted by what it changed: the
    slot's columns are taken before it (`Tables::ColumnsOf`) and compared after (`Tables::CausesBetween`, `NoteWrite`).
-   **The targeted writers** compare their own column: the placement (`MoveObject`), the skin's mask and bone offset
    (`AppendKeptSkin`, the accumulate phase), the tree animation (`KeepResidentsAlive`), and at Prepass
    (`RefreshFrameConstants`) the shading, the extras rows and the wetness.

**The accumulated half persists.** It used to be reset for every patched record at the next walk
(`RestoreAccumulated`) and written again by the accumulate phase, about 1,100 records a frame at Riverwood. Now:
-   a patch the accumulate phase gives again is renewed, and changes nothing unless the pass does;
-   a patch it does not renew lapses at the end of that phase (`LapseAccumulated`: the slots patched last frame and not
    this one, residents apart);
-   a record the walk writes again for the same object keeps its patch (`WriteObject`'s `keepHalf`), so an entry written
    every frame (a face, an actor's part) does not lose it and take it back every frame;
-   the extras rows are a persistent block per record (`AllocateExtras`, `FreeExtras`), not a per-walk list;
-   the shadow build, which runs between the walk and the accumulate phase, now sees last frame's patch on the records
    that keep one, as it already did on residents. Nothing it reads is the patch's (its flags are the scene's, and its
    culling ignores the main pass's bits);
-   walk parity compares the scene half alone, since any record may keep a patch.

**The check** (`CS_DCLF_CHANGE_LOG_PARITY=1`): every 60 frames each slot's columns are kept at the scene phase's start,
and a frame later every slot whose columns changed must be in that frame's log with the causes that changed. Tour
(Riverwood, Whiterun, Dragonsreach): 0 changed slots missing from the log, over 3,100-6,100 changed slots per report,
with walk, resident, region and set parity and holes at 0.

**What changes, measured** (changes a frame, one cause counted once per slot; Riverwood with the camera standing):

| Cause | Before the kept patch | After |
| --- | --- | --- |
| tree animation | 890-930 | 958 |
| shading | 460 | 185-195 |
| placement | 270 | 281 |
| bindings | 170-190 | 1 |
| everything else | 5 | 5 |

-   **Tree animation** is every tree the frame draws: the wind timers advance every frame.
-   **Shading** is the specular LOD fade (`MaterialData.y` and `SSRParams.w`) of the objects inside its fade band, by
    about 0.001 a frame: it follows the camera's distance, which moves by the idle sway.
-   **Placement** is the movers (actors, animated furniture, physics).
-   So all three are values that change every frame; they are the candidates for derivation on the GPU (the tree clock,
    the LOD fade from the distance) rather than for more events.
-   Whiterun: 1,000-1,100 a frame, of which tree animation 410-430, shading 315-320 and placement 240-270.

### Step 2: stable offsets

A record addresses its bone palette and its extras rows by offsets into the epoch's row buffer. Both used to move with
everything else: the palettes were packed end to end by every walk, and the previous rows and the extras were placed
after all of them, so one skin appearing moved every skinned and extras record.

-   **Palette blocks.** Each skin has a block of its own for as long as its slot keeps a palette of that size
    (`Tables::PlaceBones`, `FreeBones`; freed blocks are reused by size). The walk writes the rows in place. The arrays
    are a capacity, grown in 4,096-row steps (`kBoneGrowRows`), with the previous rows one capacity further and the
    extras after both. A growth moves every previous palette and every extras block, so it notes all of them.
-   **Extras blocks** are the record's own (Step 1).
-   **The row buffer** grew to 131,072 rows (2 MB), for the holes. Riverwood: 18,222 rows in use in a 20,480-row
    capacity, 41,373 rows uploaded; the capacity stays at its high-water mark.
-   **Render flag 0x10** (the previous transform is the current one) cannot move every record: nothing sets
    `mainPassRenderFlags`, so it is always 0.
-   **Results:** steady-state skin changes fell from 25 a frame to 0.2 (the partition masks). Tour: walk parity,
    change log parity, resident and set parity and holes at 0; the bindless record parity (which checks the offsets)
    matched 24.5 million components. Walk parity no longer compares the palette arrays' sizes (the slot tables' have
    holes), only each object's rows.

### Step 3: one persistent store of object records per buffer

The per-object records (`BindlessObject`, 208 bytes) were built for every object by every build and uploaded whole:
6,215 objects, 1.29 MB, three times a frame (the Z-prepass, the colour epoch, the shadow epoch).

-   **The stores** (`ObjectRecordStore`): one for the main epochs' objects buffer, which both main segments bind, and
    one for the shadow epoch's. A store keeps the records across frames and reads the change log from its own
    position. It rebuilds a record only for an entry whose causes a record is built from (placement, bindings,
    shading, lights, tree, skin, extras), and keeps it only when the bytes differ.
-   **The upload**: the records changed since the version the buffer holds (`Resources::objectsUploaded`, written by
    the commit that uploads it), as runs; the whole store after a resync or on new buffers.
-   **One writer at a time.** The stores' builds run in frame order: the shadow build, then the Z-prepass, then the
    colour build. Each is joined, or cancelled (which waits for a running job), before the next is kicked, on the
    one worker or inline. A collision counter checks it: 0 on the tour.
-   **Results.** A main store rewrites 340-600 records an update, mostly the trees' wind timers and the movers; the
    shadow store 520-1,060 (it sees a frame's changes at once). Every object record parity check (every record
    against one built from the tables, every 60 frames) matched. The colour epoch's upload fell from 2.2 MB to 1.2 MB.
-   Switches: `CS_DCLF_PERSISTENT_OBJECTS=0` builds the records whole every build, as before;
    `CS_DCLF_PERSISTENT_PARITY=1` runs the checks (this step's and the next's).

### Step 4: persistent constant blocks and binding records

**The state** (`PersistentBindings`, one per main segment in its build cache):
-   **Each segment has its own buffers.** The Z-prepass now has its own constants and records
    (`Resources::constantsDepth`, `recordsDepth`), because both segments used to upload into the same buffers
    from offset 0. BuildDraws' depth phases write the depth records' addresses.
-   **Blocks.** Every pipeline's technique, PerGeometry template and permutation blocks, and every (material,
    pipeline) pair's PerMaterial blocks, keep one address in the segment's constants buffer. They are 256-byte
    aligned, and freed blocks are reused by size. A block is written only when its bytes differ.
-   **Records.** Every pair keeps one record slot in the segment's records buffer while the build cache holds it.
    A record is written only when it differs.
-   **The upload**: the ranges written since the versions the buffers hold (`Resources::constantsUploaded`,
    `recordsUploaded`).

**Frame textures.** The frame textures (t16 and up) were patched into every colour record by every commit, about
5,400 patches an epoch.
-   **Their indices are stable.** A probe over the tour saw them change only in the first one or two epochs after a
    load.
-   **So the build writes last epoch's.** It takes the indices the last commit resolved
    (`Resources::committedFrameTextures`).
-   **The commit patches only on a change.** It patches, and uploads again, only the records reading a register
    whose index changed.
-   **Tour: none in steady state.**

**What is clean is not looked at.** The first version of this step still re-derived every pair's and every
pipeline's source signature on every build, which cost as much as the packing it saved. Now the writers version
what they write, only when the value changes:
-   **Pipelines** (`Tables::pipelineConstantsVersion`, `pipelineBindingVersion`, written by `RefreshFrameConstants`
    and the accumulate phase):
    -   the constants version covers the technique and PerGeometry floats, and gates repacking the pipeline's
        blocks;
    -   the binding version covers the key, the technique's filter modes and shadow mask, and the permutation, and
        gates its pairs' records.
-   **Material frame floats** (`Tables::materialFrameVersion`): `RefreshFrameMaterials` and
    `RefreshTextureTransforms` version them, and `MaterialSources` now reports whether a float changed.
-   **Lookup entries**: a version per lookup pipeline and material entry, and one shared by the null texture, the
    samplers and the projected textures (`Lookups::NextVersion`).
-   **What a build skips.** A pipeline whose versions are the ones its blocks were written from takes their
    addresses. A pair whose versions are unchanged is its record slot, and nothing else about it is looked at.
    -   The pair's versions are its material's, its frame floats', its lookup entries', its pipeline's binding
        version, and its pipeline's block moves.
    -   They also include the frame textures' version and the frame masks.
-   **What stays dirty.** At Riverwood 65 of 77 pipelines change their constants every frame (fog, the sun), so their
    blocks are repacked and rewritten. Of about 490 pairs, 390 are clean and about 100 are looked at.
    -   About 85% of those are character-lit materials: their t11 alternates between two render targets every frame
        (the character light), so the record really changes.
    -   The rest are frame floats (IBLParams, scrolling UVs).
    -   A stable alias descriptor for the character light's t11, rewritten once a frame, would remove most of the
        rest.

**Checks** (`CS_DCLF_PERSISTENT_PARITY=1`, every 60 frames): the same build is made the per-frame way, and for every
object either draws, the kept record must bind the same textures and samplers, and constant buffers holding the same
bytes (a frame slot's address must be the same address). Tour: 0 differences over 11,000-40,000 draws a report, in
both segments, with set parity, change log parity and holes at 0.

**Cost** (Riverwood, a 45 s run each; the report's means; "join" is the render thread's wait on the build):

| | Steps 3-4 off | Steps 3-4 on |
| --- | --- | --- |
| Colour build on the worker | 1.13 ms | 0.81 ms |
| Colour join | 0.94 ms | 0.63 ms |
| Z-prepass build | 1.02 ms | 0.85 ms |
| Z-prepass join | 0.41 ms | 0.25 ms |
| Shadow build | 0.84 ms | 0.64 ms |
| Shadow join | 0.66 ms | 0.45 ms |
| Colour epoch's upload | 2.2 MB | 0.5 MB |
| The build's pair and pipeline parts (textures and samplers, constant groups) | 0.36 ms | 0.16 ms |

-   Switches: `CS_DCLF_PERSISTENT_BINDINGS=0` packs the blocks and records into a per-build arena, as before.

### Step 5: the whole scene's draw inputs, and the drawn set as changes

**The drawn set** (5a). What the colour epoch draws is the native loop's skip set and the claims.
-   **Before:** the build listed every drawn object, the commit wrote a hash entry for each of them
    (`drawnFrame`), the claims were rebuilt from that map every frame, and the Z-prepass's gate (without withholding)
    looked every object up in it.
-   **The colour build keeps a mark per slot** (`DrawnMarks`: drawn or not, and as which geometry). It sends the
    slots whose mark changed since the version the render thread applied: the uploads' dirty protocol, with a full
    send after a resync.
-   **The render thread keeps its state from those changes alone:**
    -   per slot and per geometry, drawn now or the last frame it was (`SlotDrawn`, `drawnGeometry`);
    -   the claims (`Impl::claimSet`), republished only when they changed, or when someone else published over them
        (a load publishes an empty set);
    -   dropping a claim a frame after the last draw, the native skip's one-frame tolerance.
-   **A geometry that moves between slots** is undrawn only by the slot that draws it (`DrawnGeometry::slot`). The
    first version undrew it after its new slot had drawn it, and the geometry then read as undrawn from then on:
    1,165 holes in 300 frames at Riverwood, on clutter that changes slots. It is 0 since.
-   **Result:** the commit's drawn-set part fell from about 14 µs to 0.1-1.7 µs an epoch.

**The whole scene's inputs** (5b). The region (`ResidentRegion`) holds every object the loop would draw as one input
with its pair's record, not only the residents, when the bindings are kept (Step 4).
-   **The depth segment's cull-only candidates are entries too**: an object without bindings, with its bounds and no
    draw.
-   **Every other live slot is classified from the change log**: the per-frame loop's (`loopList`: decals, faces,
    second-stream shapes, and what does not fit, with 2,048 inputs and draws kept for it) or nobody's (a candidate
    the segment does not submit, counted for the report).
-   **The loop visits only its own list**, not every object.
-   **The Z-prepass keeps its rule where its gate is the colour epoch's last frame** (no withholding): there only
    residents are the region's, and their joins wait for the colour epoch's first draw, as before. With withholding
    (the default) the whole scene is the region's.
-   **The per-object states** (set parity's) are built only when set parity runs.
-   Riverwood: about 5,900 inputs kept by the colour segment, against 2,990 residents before.

**Checks.** Tour: region parity 0 differences over 256,000 entry checks and 0 missing, persistent bindings parity 0,
set parity and holes 0, and claims churn 0.

**Cost** (Riverwood, a 45 s run each):

| | Steps 3-5 off | Steps 3-4 | Steps 3-5 |
| --- | --- | --- | --- |
| Colour build on the worker | 1.12 ms | 0.77 ms | 0.77 ms |
| Colour join | 0.91 ms | 0.57 ms | 0.57 ms |
| Z-prepass build | 1.02 ms | 0.81 ms | 0.77 ms |
| Z-prepass join | 0.40 ms | 0.19 ms | 0.12 ms |
| Shadow join | 0.61 ms | 0.45 ms | 0.46 ms |
| Render thread, the main epochs' commit (µs an epoch) | 658 | 380 | 347 |
| Colour epoch's upload | 2.2 MB | 0.5 MB | 0.3 MB |

-   The per-draw parts of the colour build are now about 0.1 ms: record push and the per-draw tail are 0.002 ms each.
-   What remains per frame is what changes every frame:
    -   the pipelines' constants, 65 of 77 at Riverwood;
    -   the character-lit records;
    -   the movers' records;
    -   the bone palettes, which each main and shadow epoch uploads whole (current and previous rows, about
        0.66 MB).
-   Switches: `CS_DCLF_WHOLE_SCENE_REGION=0` keeps the region to the residents.

**The bone palettes** (5c). Every main and shadow build copied the whole palette arrays and extras rows into its
payload and uploaded them, about 0.66 MB each, three times a frame.
-   **The writers note a palette whose rows differ** (`kChangePalette`: the walk's `WriteObject` and `AppendKeptSkin`
    compare the rows before they copy them).
-   **A store per bones buffer** (`BonesStore`) collects from the log the rows changed since the version the buffer
    holds (`Resources::bonesUploaded`):
    -   a palette's rows or place: current and previous;
    -   an extras block.
-   **The uploads read the tables' arrays directly** (no copy); a new capacity is a resync.
-   **The check** (`CS_DCLF_PERSISTENT_PARITY`): each store keeps a shadow of the rows it uploaded, compared with the
    tables every 60 frames. It found 0 differences on the tour.
-   **Result:** at Riverwood about 250 of 669 skins change their palette each frame. The epoch now uploads about
    12,700 rows (200 KB) instead of 41,000, the colour build fell from 0.77 to 0.67 ms, its join from 0.57 to
    0.49 ms, and the main epochs' commit on the render thread to 292 µs.

### Step 6: the shadow epoch

**The sun's entry rule on the GPU** (6a). A caster of the sun's views is an input only while its entry is inside one of
the sun's full-frustum culling processes.
-   **Before:** the build tested every caster's entry sphere against the frame's planes and wrote the verdict into
    the input (`kInputOutsideSunEntry`), so every caster's input could change every frame.
-   **Now** the input carries the sphere (its fade row, `SetSunEntryRow`), each sun view's latch carries the
    processes (`BuildDrawsLatch::sunEntryPlanes`), and BuildDraws tests them (`OutsideSunEntry`).
-   **The latch grew to 2 KB.** The sun has six processes at Riverwood, more than the four cascade plane sets the
    latch already held, and the rule is a union over processes, where a cascade's plane sets intersect.
-   **Above eight processes** the CPU's flag is used, as before.
-   **Since 2026-09-29** the processes are no longer in each view's latch: one region per frame slot holds any number
    of them, and the CPU flag is gone (dclf-architecture.md, "The sun's plane sets and the view rasterizer states").
-   **Check** (`CS_DCLF_PERSISTENT_PARITY`): the latch's test, run on the CPU over every sun input, against the old
    verdict. Tour: 0 differences over about 100,000 inputs a window. The sampled views' GPU counts (tested, drawn,
    rejected) are the same as the CPU flag's.

**The inputs, the records and the claims** (6b, 6d; `ShadowKept`).
-   **Records.** The plain record and one per alpha-tested material, at slots the materials keep. Their texcoord
    blocks sit at fixed offsets in the arena, which is still written every build (a few KB), so a record changes only
    when its texture does.
-   **Inputs.** Per render mode, a region of the casters' inputs, changed only by:
    -   the change log's shadow causes (the technique and reason, flags, placement, geometry, skin, membership);
    -   a mode's views changing their rasterizer states (the mode is read again);
    -   what waited (a pipeline, or a diffuse texture) becoming ready. The waiting list is read again every build.
-   **The frame's own list** is only the face shapes (their positions are the walk's).
-   **Uploads.** Every entry and record carries the version it last changed at. A mode's input buffer and each view
    slot's copy of the records receive what is newer than the version they hold (`ShadowResources::inputsUploaded`,
    `recordsUploaded`): the four view slots' 115 records are no longer staged every frame.
-   **Claims.** Each mode's claims are kept with the region, each geometry with the object that owns its claim: a
    geometry that moves between slots is claimed by its new slot before its old one's entry goes. The first version
    dropped such a claim, 2-5 geometries in Whiterun. A new set is published only when the claims or the faces
    changed.
-   **Check** (`CS_DCLF_PERSISTENT_PARITY`): the same build made the per-frame way, and per used mode:
    -   the same casters with the same inputs, the record's number apart;
    -   records binding the same textures and samplers with the same texcoord values;
    -   the same claims.
-   **Result:** tour 0 differences over 42,000-53,000 inputs a window, with holes and set parity at 0. At Riverwood
    about 270 entries a build are written again (the movers, in two modes), and no records.

**Cost** (Riverwood, 45 s): the shadow job on the worker fell from 0.51 to 0.28 ms (its build part 0.29 → 0.12 ms,
the claim sets 0.05 → 0), and the render thread's wait for it from 0.33 to 0.08 ms. What its staging still sends
(0.13 ms) is mostly its own copies of the object records and bone rows, which the main epochs upload too.
-   Switches: `CS_DCLF_PERSISTENT_SHADOW=0` builds the shadow epoch's inputs and records every frame, as before.

### Step 7: what still walked every object each frame

These scans were measured one by one, then made event-driven in cost order. Riverwood render thread, µs a frame:

| Scan | Before | After |
| --- | --- | --- |
| Shading resample | 130 | 5 |
| Pipeline constants | 64 | 20 |
| Material frame components | 30 | 14 |
| Texture transforms | 11 | 1 |
| Delta walk's stale-slot scan | 27 | 2 |
| Slot check | 16 | 0 |
| Hole detector's claim walk | 30 | 10 |
| Geometry table packing (worker, per build, 3 builds) | 11 | 1 |
| Sun exclusion (worker) | 30-37 | 10 |

In all, the render thread's scans went from about 350 to about 105 µs a frame. Every check below is
`CS_DCLF_PERSISTENT_PARITY`, and was 0 over the tour.

**Shading** (`RefreshFrameConstants`). The resample reads only what an event names (since F4; before it, every slot
with a controller and every actor slot was resampled every frame, about 2,000 a frame at Riverwood, now about 300):
-   **Controllers.** The float and colour controller hooks report the emissive colour and multiplier they write, and a
    material field a float controller writes (a member's shading takes the material's alpha).
-   **The alpha `GetRenderPasses` leaves on the property** (materialAlpha times the fade, for whichever camera registered
    it last; an actor's fade): its detour compares it with the LOD fades.
-   **Extras.** ProjectedUV and land blend follow the eye and a clock (`kWatchExtras`, set by the patch): still watched.
-   **The LOD fades.** `GetRenderPasses` writes them into the property (`specularLODFade`, `envmapLODFade`)
    whenever any view registers the object. A detour on its vtable slot (0x2A) pushes the property when either value
    moved. It fires about once a frame, and the property's dependents are resampled.
-   **External emittance.** A property with `kExternalEmittance` reads a colour it does not own: its reference's
    light, its region's `emittanceColor` (blended from the region's weather), or one of the sky's colours. The weather
    rewrites that colour in place, with no event on the property (found as a MISSED resample after `fw`, on Riverwood's
    mill fan). Both writers go through `Sky::SetColor`; its detour pushes the colour when it moved
    (`emittanceEvents`), the colour is listed as a key in `propertyDependents`, and its dependents are resampled.
    Pointing a property at another colour is a property event (`skyrim-engine-notes.md`, "External emittance").
-   **The render flags** changing resamples every slot.
-   **A bug the watch exposed.** A hit in the derivation cache (`Tracked::Derived`) reused the cached LOD fades, and
    the old per-frame resample corrected them at Prepass. Every object in the fade band changed twice a frame
    (about 90 objects at Riverwood). A hit now takes the fades from the property, as the uncached path does.
-   **Check:** every slot sampled against the tables every 60 frames ("shading resample").

**Pipeline constants.**
-   **PerGeometry blocks.** What of a pipeline's block changes after its first evaluation is either written over per
    object (`ObjectGeometryConstants`) or one of the frame's globals: `DirLightDirection`, `DirLightColor`,
    `DirectionalAmbient`, `AmbientSpecularTintAndFresnelPower` and `AmbientColor` (PS 3, 4, 5, 6, 18). All
    pipelines that write them share them. So:
    -   a pipeline is evaluated in full once, and again when the render flags change;
    -   otherwise one sample a frame is copied into it, compared and written in place.
-   **`EyePosition`** (VS 2). `SetupGeometry` (0x1414dd040) writes it for Envmap, Eye and technique 0x10 and for any
    pass with descriptor bit 9 or one of `0x21c00` (`WritesEyePosition`; engine notes), the same for all of them: the
    camera less `posAdjust`, in world space. Those pipelines take it from the frame's one evaluation of such a pipeline.
    Every other pass leaves whatever the constant buffer last held, which no draw of it reads, so the check skips it
    there. Until 2026-10-02 the rule named the three techniques alone, so a hair or specular pipeline kept the eye of
    its first evaluation; complete capture parity (`CS_DCLF_NATIVE_SKIP=0`) showed it on every technique. Lighting.hlsl
    never reads it, so nothing drawn changed.
-   **Frame lighting** (`Tables::frameLighting`). The sun's direction moves every frame, so while it sat in every
    pipeline's PerGeometry block about 70 blocks were rewritten each frame.
    -   **Its own block.** The `DCLF_BINDLESS` pixel stage now reads `DirLightDirection`, `DirLightColor`,
        `DirectionalAmbient` and `AmbientSpecularTintAndFresnelPower` from `DCLFFrameLighting` at PS b13, a register
        no shader used. It is a DCLF frame slot (`kFrameSlotLighting`) that the commit uploads only when its version
        changes.
    -   **Merged per component.** The rows are merged from the frame's evaluations, each component from whichever
        evaluation writes it, over what was last published. Techniques differ in what they write: Eye leaves
        `AmbientSpecularTintAndFresnelPower.w` unwritten where technique 0 writes 1. So taking the rows from a
        single evaluation made the result depend on pipeline order, which the check caught.
    -   **What the block no longer carries.** The bindless PerGeometry block packs as zero everything those draws never
        read from it (`kVSBindlessGeometryUnread`, `kPSBindlessGeometryUnread`, `PackGeometryTemplate`): the frame
        lighting, the values each object's record or extras rows supply, and `EyePosition` and `AmbientColor`, which
        Lighting.hlsl never reads. Only a change in what they do read versions a pipeline (`SameBindlessGeometry`). That
        includes the eye pipeline's full evaluation, whose template actor's World moved every frame.
    -   **What still reads the full values.** `geometryConstants` keeps them for the constant-buffer path and the
        checks.
    -   **Riverwood, before and after:**
        -   pipelines changed a frame: about 70 → 0;
        -   pipelines clean in a colour build: 5 → 77 of 77;
        -   blocks written a build: colour 93 → 19, Z-prepass 82 → 9;
        -   worker build: colour 0.73 → 0.61 ms, Z-prepass 0.79 → 0.65 ms.
-   **PerTechnique blocks.** `EvaluateTechnique` reads only the technique and whether the shadow mask is bound
    (`TechniqueKey`). It runs once a frame per key and is versioned when it changes. Since Step 8, the value itself is
    one shared row per key (`Tables::TechniqueRow`).
-   **Template pass.** It is looked up only to evaluate.
-   **Pipelines whose template has no lighting pass this frame** now take the frame's globals too. They used to be
    skipped and kept a stale sun direction.
-   **Check:** every pipeline's block against a full evaluation, in what no object writes over; its PerTechnique
    block against `EvaluateTechnique`; and the frame lighting against each evaluation, where that evaluation writes
    it ("pipeline constants").

**Material frame components** (`MaterialSources`: IBL, snow rim, character light, LOD texture, landscape snow, t11).
-   **By signature** (`Tables::frameSignatures`). Each signature keeps its slots, listed when keyed and dropped as
    the list is walked. One live sample a signature is taken from a slot drawn this frame, and applied to the whole
    list only when it differs from the last. Slots keyed or rewritten since take it regardless
    (`materialFramePending`). At Riverwood one signature a frame applies: the character light's, whose t11
    alternates.
-   **TexcoordOffset.** A material's two texture-transform buffers change only by a write, which is an event. A slot
    is watched (`transformWatch`) from its keying or its material's write until two frames have passed and both
    buffers agree.
-   **Check:** every slot drawn this frame against its signature's sample and its material's transform ("material
    frame components").

**Geometry slot liveness.**
-   **References.** A kept object no longer renews its geometry slot every frame. A slot an object draws from,
    including a skin's partition chain, is kept; only the others are freed once idle. Since Step 8, this is counted
    from the logs (`SlotTable`) rather than by a sweep over every object.
-   **Buffer touches.** These are staggered over the slots, every 64 frames each, against `GpuResources`'
    eviction.
-   **Stale slots are events:** a failed touch; every unresolved slot once buffers start resolving; a slot
    `ResolveGeometrySlot` freed. Only then are the objects drawing them found and written again.
-   **The slot check** (`CheckObjectSlots`) runs only on a frame some slot was freed (a sweep, a material drop, a
    geometry slot that could not be resolved again), or under the parity switch.
-   **Counts.** The slot counts for the reports are taken every 16 frames. The decal ordinals are reset from last
    frame's list, not across the whole column.

**The geometry table** (`GeometryStore`, one for the main epochs' buffer and one for the shadow epoch's).
-   **Packing.** The geometry slots' draws are kept and repacked from the tables' geometry log (`Tables::geometryLog`:
    a record resolved, a slot added, a partition link changed).
-   **Uploads.** Only the slots changed since the version the buffer holds are uploaded (`geometriesUploaded`). The
    face streams' draws follow the slots and are sent every build.
-   **Result:** at Riverwood almost no slot changes, where all of them (1,400 to 2,700) were packed and uploaded by
    every build.
-   **Check:** every slot against one packed from the tables ("persistent geometry table").

**The hole detector** (`PublishClaims`). A hole was withheld, and only a registration is withheld, so the detector
walks this frame's withheld registrations (`PassCapture::LastDrain`) rather than every claim. The `claimed` count,
which only the reports read, is taken every 16th frame.

**The sun exclusion** (`BuildSunExclusion`, `SunExclusionCache`). The exclusion reads four things, and while none of
them has changed the last verdicts are reused (only the per-frame stamps are new):
-   the candidates snapshot;
-   the tables' generation;
-   the sun mode's membership (`ShadowKept::Mode::membership`: the build at which an object last joined or left its
    inputs or faces);
-   every object's flags and geometry (the change log's bindings and geometry causes).

It is reused on 85-90% of builds at Riverwood, and 33-70% in Whiterun's crowds, where walking NPCs keep changing their
bindings. Check: a reuse is built in full every 60 frames and compared ("sun exclusion").

**Left** (µs a frame at Riverwood):
-   **Render thread:**
    -   material validation, 15 (a check, 8 evaluations a frame);
    -   wetness, 11 (per actor, per frame by nature);
    -   the synthetic passes' hole test, 10;
    -   the three live material samples, 13.

### Step 8: one implementation of each mechanism

Steps 1 to 7 grew several copies of the same few mechanisms, each with its own data. Each mechanism now has one
implementation, which every kept structure uses:

| Mechanism | Used by | Replaces |
| --- | --- | --- |
| `EventLog`, `LogCursor` (KeptState.h) | `Tables::changeLog`, `Tables::geometryLog`, and every reader of them | Two logs, each with its own base, trim and invalidation, and seven hand-written "fell behind" tests |
| `ChangeJournal`, `KeptArray`, `KeptView` (KeptState.h) | Object records, bone rows, the geometry table, resident regions, persistent constants and binding records, shadow records and inputs, drawn marks | Two change protocols (dirty-since-base lists, and per-entry versions in the shadow state), and six hand-written coalescing uploads |
| `TablesHeld` | Resources, ShadowResources, and the build inputs | Nine `*Uploaded` fields and their copies |
| `KeptRegion` | `ResidentRegion`, `ShadowKept::Mode` | Two swap-removals |
| `SlotTable` (SlotTable.h), `Tables::GeometryColumns`/`PipelineColumns`/`MaterialColumns` | Geometries, pipelines, materials | Three free lists, three allocators, the idle sweep and its walk over every object |
| `Tables::TechniqueRow` | PerTechnique values | The per-pipeline copies, the memo, and `techniqueVersionHeld` |
| `EventQueue` (EventQueue.h) | Fade, property, node, LOD fade and switch events; material writes | Five Treiber stacks allocating per event, and MaterialSources' own ring |
| `ParityCounter`, `ParityDue` | The stores' parity checks | Per-store counters and verdict strings |

-   **The journal.**
    -   **Versions.** Each build that changes a kept array is one version. The journal keeps (version, range) entries
        after its floor. A buffer holding version H is sent the runs changed since H, coalesced. When H is below the
        floor it is sent everything; a new buffer holds 0.
    -   **Holders.** There can be one holder, such as a main buffer, or the render thread for the drawn marks. There
        can also be several: each of the shadow epoch's view slots holds its own copy of the records. `BeginBuild`
        trims at the oldest holder.
    -   **Resets.** A reset keeps the journal, so versions keep counting and never repeat one a buffer already holds.
    -   **Bone rows.** These journal row ranges. A run is sent one section at a time (current, previous, extras); the
        old coalescing could join two sections at the capacity boundary.
-   **Slot liveness.**
    -   **What counts as a reference:**
        -   an object references its geometry slot while it has a record;
        -   it references its pipeline and material slots while it is bound;
        -   a skin partition is referenced by the partition before it.
    -   **Counting.** `UpdateSlotReferences` counts them from the change and geometry logs. Each count records the
        slot's generation, since a geometry or material slot can be freed while referenced.
    -   **Freeing.** A slot is freed 64 frames after its last reference goes. That expiry is an event, not a sweep.
    -   **The used sets are the members' slots.** `usedPipelineBits` / `usedMaterialBits` certify the raw engine pointers
        a slot holds (a pipeline's lighting template, a material key) for evaluation. A joining member marks its slots,
        `KeepResidentsAlive` sets them again from the residents when membership changed, and a drained slot leaves them
        at once; nothing re-marks them per frame. The template is taken from a resident's property every frame (a
        property swap rewrites a member only when an event names it). `CheckObjectSlots` holds every bound record to
        the sets; persistent parity compares them with the slots the bound records name.
-   **Technique rows.**
    -   **Rows.** There is one row per `TechniqueKey`, evaluated once a frame. Its floats and its bindings are
        versioned separately.
    -   **Versions.** A pipeline's kept blocks check the row's constants version, and a pair's record its binding
        version.
    -   **Blocks stay per pipeline.** Each shader permutation's constant table lays the group out differently.
-   **The event queue.** A bounded ring with a sequence per cell, which allocates nothing per push while it has room.
    When it is full it spills onto a lock-free stack rather than dropping the event: the scene consumers have no way
    to rebuild what a lost event would have told them. Drains are oldest first.
-   **Not added:** a `Versioned<T>` helper. Only one site writes and versions a whole value (the frame lighting); the
    others write parts of a record.
-   **Validation:**
    -   the full parity tour after each stage, 0 throughout;
    -   timings unchanged within noise (Riverwood colour build 0.61 ms, Z-prepass 0.65-0.67 ms).

## The resident region's pairs by witness (2026-09-30)

`MainBuild::UpdateRegionPairs` resolved every (material, pipeline) pair of the resident region on every build, ~3,500
pairs twice a frame, to learn whether each could still draw: 1.86 ms a frame on the worker, the render thread's
critical path.
-   **A witness per pair.** Each pair now keeps a witness of what its resolution reads:
    -   the pipeline's half, once per build (`PipelineWitness`: the pipeline row's key and flags, the set, the
        technique's bindings, the constant tables);
    -   the material's three versions (record, frame components, lookup entry; `Lookups::materialVersions` is a
        compact mirror of the entries' versions);
    -   the region's frame-slot masks and segment.

    Only a pair whose witness moved is resolved again.
-   **One owner bundle.** The pairs' binding owners are one bundle held by the region, made again when a pair changes,
    so each build no longer copies ~7,000 owners.

Results: `Region.Pairs` 1.86 -> ~1.1 ms a frame, colour jobs late 0 of 300 (from 72-96), render-thread wait on the
worker 3.0 -> 2.3 ms. Parity (build, persistent, resident draw, set) clean.

**The character light's t11 is the frame's.** About 500 character-lit materials' records used to change every frame,
because `TexCharacterLightProjNoiseSampler` (t11) is a ping-pong render target that the frame components wrote into
each material. Their pairs were resolved again, and their rows rewritten and uploaded, twice a frame.
-   **Why not a second layout:** an indirect pipeline set requires one layout for all its pipelines (BasicRHI
    `IndirectPipelineSetDesc`), so a layout of its own would split the colour pass's command streams.
-   **A register of its own:** under `DCLF_BINDLESS`, `Lighting.hlsl` reads the character light's noise from
    `DCLFCharacterLightNoise` at t125 (`kCharacterLightRegister`), a frame register, in both of t11's uses when the pass
    descriptor has `CharacterLight` (the character light, and a projected technique's noise). t11 keeps its other
    meanings (`TexLandNormal5Sampler`, a projected technique's noise texture).
-   **The view:** the frame components take it from a character-light signature's live sample
    (`MaterialSources::CharacterLightView`, `Tables::characterLightView`). The colour commit resolves it into the frame
    record. The Z-prepass's frame record binds the null texture there: the noise only shades.
-   **The records:** a character-light pass's record keeps t11's sampler modes and no view, and its row binds the null
    texture at t11. The material frame components went from ~1.0 applications to ~1,016 slots a frame to 0.

## No per-object loop in the main builds; pairs by event (2026-10-01)

After the region took the whole scene (Step 5), each build still visited two kinds of object every frame, through
`MainBuild::RunObjectLoop`, assembling each one's input again:
-   **decals:** about 2,300, colour only;
-   **face shapes:** 281, both segments.

The loop cost 0.84 ms a frame on the worker. Neither kind changes from frame to frame:
-   a decal's ordinal changes only when the member decals do (`OrderDecals`);
-   a face's stream changes only when its head's stream or the geometry table does.

**Decals in the region.**
-   **Eligibility:** in the colour segment, with an ordinal in its group's range.
-   **The entry:** carries the ordinal. A decal it cannot draw is still an entry: BuildDraws writes its slot as a
    zero-count draw, the loop's blank. That includes a decal of several partitions, which its group's one-sequence slot
    cannot hold (103 at Riverwood).
-   **The draw count:** a drawable decal's entry is tagged `kRegionDecal` in `drawsOf`. It counts as drawn for the marks,
    but not toward the draws' range: its sequence is in its group's range. Counting it there would overrun the draws'
    bound.
-   **Ordinal changes:** `OrderDecals` logs a `kChangeBindings` change for every decal whose ordinal moved, or that left
    the order, so the region takes those entries again from the change log.

**Faces in the region.**
-   **The entry:** an object that needs its positions' stream (a face shape, or a pipeline reading its position from
    the second stream) is an entry with its stream index (`FaceStreamGeometry`).
-   **What the index counts from:** the geometry slots' count. When that count moves, or the positions buffer comes or
    goes, the region takes every face entry again (`ResidentRegion::faceBase`). A face's own stream change is already
    a `kChangeGeometry` change.

**Pairs by event.** With decals, the colour region holds about 3,580 pairs (from 2,750). `UpdateRegionPairs` still
scanned every pair's witness on every build, and remade the owner bundle on any resolution: 0.73 ms a frame.
-   **Which pairs are checked:**
    -   those of the material slots named by two new logs since the region's last build: `Tables::materialLog`, pushed
        wherever a material's record or frame version moves, and `Lookups::materialLog`, pushed when a lookup entry's
        version moves or its slot is retired;
    -   the pairs new since that build;
    -   every pair, when a pipeline's half of the witness moved, the frame witness moved, or a log cannot be read on (a
        trimmed log, new tables, another `Lookups` instance: each instance has its own log generation).
-   **Unchanged:** the witness comparison still decides what is resolved.
-   **The skips** the report counts every build come from per-reason counts the region keeps, not from a visit to
    every pair.
-   **The owner bundle** is remade only when pairs come or go, or a resolution finds other owners or frame textures
    than the bundle took for the pair.
-   **Parity** (`CS_DCLF_RESIDENT_DRAW_PARITY`): every pair's witness is computed again and compared with the one it
    holds. A difference is a change no event named.

**Results** (Riverwood, camera turning):
-   Both segments' loops are empty.
-   Region parity: 0 differ and 0 missing over about 2 million entries.
-   Pairs: 0 resolved with no event over 327,000 checks.
-   2,202 decals submitted, as before. Set parity, BuildDraws parity (with the region off) and holes 0.

Worker, ms a frame (d-tickets against d-pairs-t; different captures, so compare the parts, not the totals):

| | before | after |
| --- | --- | --- |
| `BuildMain.ObjectLoop` | 0.84 | 0.00 |
| `BuildMain.Region.Pairs` | 0.32 | 0.52 |
| `BuildMainPayload` | 1.24 | 0.61 |

What `Region.Pairs` still spends:
-   **Resolutions:** about 21 pairs a build, materials whose frame values changed (animated texture transforms). Their
    rows are written once, but each segment's region resolves the pair: about 0.2 ms a build.
-   **The owner bundle:** remade on most builds, about 0.06 ms.
-   **The pipelines' witnesses:** about 0.004 ms.

## Which native views touch DCLF's roots (2026-10-01)

The premise that the water reflections and cubemaps cull and register DCLF's objects was wrong
([skyrim-engine-notes.md](./skyrim-engine-notes.md), "Water reflections: the cube map"). A census settled it
(`Engine/ViewCensus.cpp`, `CS_DCLF_VIEW_CENSUS=1`, removed after R3):
-   `BSFadeNode::OnVisible` on owned roots, by culling process instance and class (MSVC RTTI);
-   `GetRenderPasses` on geometry under owned roots, by the accumulator's render mode (`+0x150`);
-   the cube map's roots (`FUN_1405275e0`, its add-root);
-   a sampled stack for the shared static `BSCullingProcess` (`0x14332bda0`).

Riverwood (camera turning, rain) and the Bannered Mare, per 300 frames:

| Who | Owned roots reached | Notes |
| --- | --- | --- |
| Point lights (`BSParabolicCullingProcess`, static `0x14335bfa0`) | 40,000-110,000 outside, 47,000-95,000 inside | mostly stood in |
| The sun's cascades (`BSShadowDirectionalLight::Accumulate`, through the static `BSCullingProcess`) | 1,300-14,500 | mostly stood in: the exclusion's residue |
| `Precipitation::SetupMask` (same static process) | on the frames the engine draws the mask | |
| The main list processes | 8,000-13,000 | mixed roots only, by design |
| The water cube map | none | its roots are the Sky, LOD trees and two LOD roots, twice a frame |
| `GetRenderPasses` | mode 0 only, under mixed roots | shadow modes register without it |

So the point lights' culls were the write-back's main reader, then the sun's residue.

## Point lights' shadow culls without DCLF's entries (2026-10-01)

`BSShadowParabolicLight::Accumulate` (`0x14151b960`) culls through `BSCullingProcess::Process` (`FUN_1414bf320`). Its
descriptor's flag at `+0x5C` selects the static `BSParabolicCullingProcess`. With no root list of its own
(`sceneAccumArray`), the light passes the scene's object root (`ShadowSceneNode` child 3) as the root, cull mode 3 or 4.
So every point light walked every loaded reference each frame, ran `OnVisible` on DCLF's roots, and registered their
casters, which `PassCapture` then withheld: DCLF's paraboloid views draw them.

**The exclusion** is the sun's (`BuildSunExclusion`) for the paraboloid mode:
-   An entry is excluded when every object under it that the engine would cast into a point light is an input of that
    mode.
-   **The engine's rule there:** every caster the scene phase found, volumetric-only ones included. A point light
    registers those through `RegisterPass` like any other: the parity found `Rug12` that way.
-   **Decals without z-write and bit 18** (`ShadowReject::DecalPointLight`, split from `DecalNoZWrite`): reading
    `FUN_1414b2a60`, a paraboloid light would cast them. Measured, it never does. With the culls whole on parity frames
    and every insertion watched, Riverwood's 2,099 and the Bannered Mare's 80 gave no paraboloid pass. They are no
    caster of any mode, and the parity reports one if a scene ever registers one.
-   **Where it is built and published:** on the shadow worker beside the sun's, with its own cache, and published after
    the shadow epoch that drew the paraboloid views (`LocalLightCull::Publish`).

**The skip** (`Engine/LocalLightCull.cpp`, `CS_DCLF_LIGHT_EXCLUDE`, on by default; `=probe` skips nothing):
-   **The hook:** `BSParabolicCullingProcess::Process1` (vtable slot `0x16`, checked against `0x141519a90`) returns at
    an excluded entry.
-   **The frame's exclusion** is chosen at the scene frame's start, after the frame's claims (`SelectFrame`). It applies
    only while the paraboloid claims are live and the exclusion was built for the current candidates.
-   **Parity frames** (`CS_DCLF_PERSISTENT_PARITY`, every 60th) skip nothing. Every paraboloid pass the registration did
    not withhold under an excluded entry is counted as one the skip would lose, through `RegisterPass` and the direct
    group inserts alike.

**Results** (camera turning):
-   Outside: 4.3-5.3 of 5.8-7.3 million `Process1` calls skipped per 300 frames.
-   Inside: 0.17-0.34 of 0.29-0.51 million.
-   Parity: 0 passes lost; resident, set, fade and skylight parity OK; holes 0.
-   **Volumetric-only casters are ordinary casters in the paraboloid mode.** Before, they were the volumetric copy's
    class in every mode (`VolumetricClass`, `InputFlagsOf`), so point lights drew them natively and they kept their
    entries in. Paraboloid claims went from 19,806 to 20,182, and the parity's non-withheld paraboloid passes from
    84-130 to 0.
-   **Result:** outside parity frames, no point-light cull reaches an owned root.

**Cost** (`BSShadowParabolicLight::Accumulate`, timed, `CS_DCLF_LIGHT_EXCLUDE=0` against on; plain runs):

| Scene | Off, per light | On, per light |
| --- | --- | --- |
| Riverwood | 1.4-1.5 ms | 0.45-0.53 ms |
| Bannered Mare | 0.34-0.42 ms | 0.06 ms |

With 1.5-2 shadowed point lights a frame at Riverwood, that is 1.5-2 ms a frame of main thread. What remains per light
is the walk through what is not excluded: cells, actors, native objects.

**The sun's cascades, the same way.**
-   **The residue was all excluded entries.** The census found the sun's cascade culls reaching owned roots, every one
    of them an excluded entry. The exclusion takes entries out of the full-frustum `objectArray`, but a cascade reaches
    an entry anyway through a node above it that stays.
-   **The skip:** `BSCullingProcess::Process1` (vtable slot `0x16`) returns at an excluded entry, but only inside the
    sun's `Accumulate` on its own thread (`currentCall` with a cascade). It also stamps the entry as removed, so that a
    native geometry of it the main camera registers takes DCLF's sun bits (`ApplySunBits`).
-   **Parity frames** skip nothing, and count an unclaimed registration under an excluded entry that built a pass.
-   **Results:** about 135 entries skipped a frame, parity 0 lost, holes 0. `Accumulate`'s time is unchanged within
    noise; the point is that no sun cull touches an owned root.

## No fade write-back (2026-10-01)

Superseded in part: the engine's tree LOD reads the node every frame, so the milestones are written back again ("The fade
write-back", below).

With the point lights' and the sun's culls skipping DCLF's entries ("Point lights' shadow culls without DCLF's entries"),
nothing reads a stood-in root's node in steady state. So the write-back (E2b), the last functional readback, is deleted:
`FadeChange` and FadeStateCS's append, `FeedbackPass`, the feedback ring and its timeline, `DrainVisibilityFeedback`,
the decode job (`KickFeedbackDecode`, `PollFeedback`, `TakeFadeChanges`), `ApplyFadeChanges` and `fadeCopyMissed`.
`kFadeRootWriteBack` is now `kFadeRootStoodIn`: the root's state is FadeStateCS's alone, and its node keeps what the
engine last left there.

**DCLF's own readers of the node:**
-   **The shadow Faded rule.** `ShadowCasterReject` takes the material's alpha alone for a stood-in root
    (`SceneStore::FadeOnGpu`). A shadow view's BuildDraws drops a caster under a stood-in root while the GPU's state is
    not fully faded in. Each shadow input now carries its fade root (`FadeRootOf`), and a member's root changing is
    noted (`kChangeBindings`). A root whose stood-in state changes is reported to the fade watch, so its dependents'
    verdicts are taken again.
-   **The synthetic pass's sun bits.** These are the settled state's (`StaticShadowBits(..., true)`), as the rest of the
    synthetic pass already was. With screen-door fades the engine gives a fading pass the settled bits anyway.
-   **The skin's LOD row** (`SkinPartitionsOf`, `LodRowOf`) reads the node's level. A stood-in member with a
    `kMeshLOD` skin would keep the partitions of the level it had when stood in. They are counted at each ownership
    change ("fade roots: ... skins with LOD levels" in the report).
-   **`PassCapture::FadingAtRegistration`** reads the node only for registrations, which reach a stood-in root's
    geometry only on the frames the engine culls it (below), right after its own `OnVisible` updated the node.
-   **The resident parity** skips a fading root's pass only when the engine updates the node.

**The frames the engine culls a stood-in root again:**
-   when the cut does not apply (start-up, a stale snapshot);
-   when the precipitation mask falls back to the engine, or on its texel parity frames;
-   in the parity frames of the point lights and the sun;
-   while the local map menu is open.

`OnVisible` then starts from the node as the engine last left it. Its `lastVisible` is that old, so the engine treats
the root as long unseen (`LongUnseen`, more than 20 frames): the LOD level snaps with no cross-fade, and the fade snaps
in or out by distance. That is the state the engine gives any object coming into view. The only difference from the
GPU's state is a root caught mid-fade, which snaps. After a frame the cut did not apply, every owned root is seeded
again from its node (`ReseedOwnedFadeRoots`), as before. No readback is needed.

R2's other views:
-   **The focus view** reached no owned root in the census.
-   **The local map** culls only while its menu is open, and is covered by the snap above.

**Measured** (r3-out: Riverwood, camera turning, rain, every parity; r3-in: the Bannered Mare, without build parity):
-   Holes 0 in both. Every check OK, the Shield race apart (one MISSED outside).
-   The point lights' and the sun's parities lost no pass. Fade state parity: 640 of 640 exact.
-   8,551 stood-in roots outside and 612 inside. None has a `kMeshLOD` skin among its members.
-   Stale tickets: 6 of 600 epochs outside, 0 inside, as before.
-   Gone: the feedback copy (`cs.dclf.feedback`, about 7 us a preparation), the decode job and the node writes.

## The fade write-back (2026-10-04)

"No fade write-back" missed a reader: the tree LOD. `BGSDistantTreeBlock::UpdateBlockVisibility` (`0x140503FF0`) gives a
tree's LOD instance the alpha `1 - currentFade` of the full tree's node, hidden at 0 (dclf-lod.md, "Tree instance
hiding"). A stood-in tree's node kept the fade the engine last wrote (often 0 from the load), so its LOD stayed drawn
over the tree DCLF had faded in. On the teleport route about 200 trees stayed overlapped to the end of the run. The
occlusion maps' engine cull reads the node as well ("The fade roots").

**What is written.** Only milestones, not every step: 0 faded out, 1 fading, 2 faded in (`currentFade` at or above 1).
For each of these, the node gets the GPU's `currentFade` and the fade bits of its flags (`kFadeFlagMask`, atomically:
the engine owns the other bits). That is what the engine's readers decide by: the tree LOD hides at fade 1, and the
occlusion cull goes in while the fade is above 0, or the root is settled at 1. The tree LOD does not crossfade: its
instance stays at alpha 1 while the tree fades in, and goes when the tree reaches 1. A root's node, mid-fade, keeps the
value of the step where it started fading.

**How.**
-   `FadeStateCS` appends a `FadeEvent` (root, generation, flags, fade) to the event list (`cs.dclf.fade-events`, a count
    word then the events) when a stood-in root's milestone differs from the one it last reported
    (`cs.dclf.fade-reported`: per root, generation and milestone). Any other root's node is the engine's, so its
    reported milestone just follows. A root whose event does not fit reports nothing and tries again next frame.
-   Every depth commit zeroes the count, and the pass stamps the list with its scene frame. `FadeEventReadbackPass`
    (`cs.dclf.z.fade-events`) copies the list to the frame slot's host buffer, after first reading what that buffer held
    from the slot's last execution (finished by then). The events go to the host in batches, through a lock-free stack
    (`FadeWriteBack`).
-   The job writes the batches in frame order. A root never takes an event older than the last one written to it,
    whatever order the recordings finish in.
-   The list grows to the largest count a readback saw past it (`wanted`, `ReserveSceneTables`). Each slot takes its
    larger host buffer at its next recording, after reading the old one. The shader appends at most the fewest any slot
    holds (`capacity`), so no execution's events are lost to a growth.
-   **The "fade write-back" job** (`IndirectDraws::KickFadeWriteBack`, at the end of the scene tables) writes them onto
    the nodes on the worker:
    -   an event whose root was listed again, or is no longer stood in, is stale and dropped;
    -   it is joined (`JoinFadeWriteBack`) at BeforeShadowMaps, the accumulate phase, and before the next frame's events;
    -   between the kick and the join the tables are the frame's and no node is freed;
    -   a late join stops the job, and its remaining events go to the next one, not to the render thread.

    The fade watch is not told: a stood-in root's dependents read its fade from the GPU's state.

**Measured** (teleport route, `CS_DCLF_TREE_LOD_AUDIT`): at the last location, the trees shown over their LOD went from
about 200, all to the end of the run, to 3 within 15 s of the camera turning to them. That is what the engine does with
the stand-in off. The report: some tens to a few hundred milestones written every few seconds, a few dozen stale after a
teleport, no join late, the job about a microsecond.

## Every root's fade from FadeStateCS (T1a, 2026-10-08)

Under the standing policy that DCLF does not rely on the engine's culling (dclf-architecture.md, "Standing policies"),
DCLF's fade state is authoritative for every root it draws under, not only the roots it owns:
-   **Every record lists its fade root** (`ListFadeRoot` at the walk's record write, `UnlistFadeRoot` when the slot is
    released), not only residents: actors' and other per-frame members and shadow-only casters get FadeStateCS state.
    The root's centre is any record that holds it (`fadeRootMembers`, kept by the listing itself).
-   **Phase 1** drops an input by its root's serviced verdict (`kFadeVerdictServiced` without `kFadeVerdictDrawn`) for
    every root. Where the published state did not service the root (out of view on that update, or listed since), the
    fade-out distance test (`kObjectFadeTest`) applies, owned roots included.
-   **Shadow views** drop a caster while its root fades (`RootFading`, was `StoodInFading`), any root; the CPU's
    `ShadowCasterReject` takes the material's alpha alone and reads no node. The report's counter is "under a fading root".
-   **The engine comparison** ("fade state of roots with engine-drawn parts") is informational: the engine's node differs
    where its own cull visits a root differently (an actor's animated bound, a root its cull never reaches).

Measured (m173: motion, every parity; e34: equip and fight): fade state parity against the port 0 differ; set, view-mask,
commit and revision parities OK; the flags left (STALE MATERIAL, extras DIFFER, FADE VISIBILITY, RENDER THREAD) as in
m172 and e33. Roots listed 3,971 to 3,978 in equip and fight (the actors'), and the engine comparison closer there
(151 checked, 145 exact, 0 differ, against 153, 134, 4).

## LOD skins' partitions from FadeStateCS's level (T1b, 2026-10-08)

A `kMeshLOD` skin draws the partitions its fade node's LOD level selects (the engine's table row: `+0x152 & 0xF`, written
only by `FUN_14147a430`; "LOD cross-fades" above). The walk read that level from the node and rewrote the record when the
fade watch saw it change. Now the level is the GPU's:
-   **The record** (`Tables::skinLodPartitions`, `SkinLodPartitionsOf`): the skin's mask for each level 0-3, a byte each.
    `skinPartitions` holds row 3, every level's partitions (the table is cumulative), so draw counts and capacities cover
    any level. Written once with the record, never at a level change.
-   **The draw** (`DrawInput::lodPartitions`, was `inputReserved`; `LodPartitions` in BuildDrawsCS): every drawing
    dispatch picks the byte by the level in its root's FadeStateCS state, the one the frame's builds read. The depth
    phases, the colour segment (its latch now names the same published states) and the shadow views pick the same one;
    a dispatch without the fade buffers (the reflection faces) draws row 3. Every BuildDraws pass declares the fade roots.
-   **The fade watch's dependents** are listed only while DCLF's fades are off (`CS_DCLF_FADING=0`, whose
    `Ineligible::Fading` reads the node): with T1a and T1b nothing in the walk reads a node's fade or level.
-   The report's fade roots line counts the records with LOD masks ("LOD skins"; was a warning for stood-in ones).

Measured (m175 motion, every parity; e35 equip and fight; forest-t1b): parities as in m173 and e34, set parity OK, the
DCLF draw counts unchanged. Coverage is thin: this load order has almost no LOD skins (2 records in one m175 window,
none in the others).

A first build declared the fade roots only for the Z-prepass's first phase: every other phase's capture was a late-bound
slot, every reflection recording failed, and with it every scene revision (m174: 144 draws).

## Fades drawn by DCLF (T1c, 2026-10-08)

**Untested.** In the measured load order and settings no LOD cross-fade, blended fade or fading decal occurs (m175, e35,
forest-t1b, m176: 0 cross-fade copies and 0 fades DCLF did not model, summed over every frame), and only 2 LOD skins were
ever seen. The parts below compile and pass every parity, and the scenes drawn look as before (forest-t1c, bridge y78),
but nothing exercised the new draws. Built at the user's request (2026-10-08) so that no fade depends on the engine's
registrations (policy 1); revisit with a load order or settings that produce them (LOD cross-fades on,
screen-door off).

What it replaces: the passes `PassCapture::FadingAtRegistration` gave the engine (every hint-10 pass; a fading decal; a
blended fade), noticed only when the engine registered a member (`TakeUnmodelledFades`, `setFadeHeld`), so a stood-in
member never met them. Now, while `CS_DCLF_FADING` and `CS_DCLF_LOD_CROSSFADE` are on:
-   **Every draw's alpha takes its root's fade.** `GetRenderPasses` leaves `materialAlpha * currentFade` on the property and
    `SetupGeometry` puts it in `MaterialData.z`. A member's shading row holds the material's alpha, and the Lighting
    shader multiplies the root's FadeStateCS fade in (`DCLFAlphaFade`): the object record names the root
    (`recordFlags` bits 8-31, `RecordFlagsOf`), the frame record binds the states the frame's builds read (t120,
    `kFadeStatesRegister`) and the roots' static rows (t119, `kFadeRootsRegister`); a row of generation 0 (none bound, or
    freed) is no fade. So a fading decal blends or tests with the faded alpha, and an alpha-tested member thins as the
    engine's does. The Z-prepass reads the same row, so it tests the same alpha.
-   **The cross-fade copy** (hint 10 with the single-level bit): while the root crosses (`+0x153 & 0x70` not `0x20` in
    the state's `levels`), BuildDraws adds LOD byte L's partitions (the cumulative row L+1 less row L, from
    `lodPartitions`) to a main view's draws, each marked in the object word (`kObjectCrossfadeCopy`, bit 30), whose alpha
    is the cross-fade factor (`+0x14C`, the state's `blend`) instead of the fade.
    -   The stencil test (mode `0xB`: reference `int(blend * 31)` GREATER than the stencil, 0 outside and a room's index + 1
        inside) is taken as outside's: the copy is drawn while `int(blend * 31) > 0`. Inside it may draw where the engine's
        would not.
    -   The engine draws the copy in the G-buffer pass only (group 10, `FUN_1414b3390`, depth state not established); DCLF
        draws it in both the Z-prepass and colour, so it writes depth where it covers.
    -   A main member's hint-10 passes are withheld (`WithholdMain`).
-   **Blended fades** (hint 9: drawn blended after the composite when the screen-door fade is off) are drawn in their
    opaque group with the faded alpha. An approximation: DCLF has no blended pass.
-   With either toggle off, the engine's passes and the set's hold (`setFadeHeld`) are as before.
-   The report's main camera line sums the cross-fade copies and unmodelled fades the registrations met since the last
    report (`PassCapture::TakeFadeTotals`).

Measured (m177 every parity, e36, forest-t1c, y78): DCLF's draws and every parity as before (record parity 0 of ~48,000
differ, the fade root bits included); SPIR-V programs 99 of 99 built.

## Shadow views and lights from DCLF (T2a, T3a, T3b, 2026-10-08/09)

The shadow views' cameras and the choice of shadow lights are DCLF's: nothing reads a cull or the renderer's state at a
view's draw for them. The engine's values are the parity's alone (`CS_DCLF_SET_PARITY` or `CS_DCLF_PERSISTENT_PARITY`).

-   **The sun's views (T2a, `Engine/SunViews`).** Read at `BSShadowDirectionalLight::UpdateCamera`'s return: each cascade's
    camera and caster volume, and the full-frustum camera. DCLF derives the view, projection and view-projection
    (`SetCameraData`'s arithmetic), the eye, the viewport (the port times the target), and the planes each culling process
    tests: `BSCullingProcess::Process2` leaves SetFrustum's planes, replaced whole by `customCullPlanes` when
    `doCustomCullPlanes` (`SunViews::CullPlanes`). Used by the shadow epochs, the sun bits' cascades (`SunAccumulation`) and
    the entry rule (`sunEntryProcesses`, one per full-frustum process; the build ahead takes the last frame's).
    Parity `sun views ... <- SUN VIEW`. The viewport parity compares the camera state's depth range (`0x14202b130`): the
    renderer's holds the last draw's, less its depth-bias step.
-   **The local lights' views (T3a, `Engine/LightViews`).** The same arithmetic per descriptor of each spot and point light
    (a point light's second hemisphere: descriptor 1). A point light's culling process (`BSParabolicCullingProcess`) culls
    without planes (they stay zero), so its `cull` is none. Parity `light views ... <- LIGHT VIEW`.
-   **Which lights (T3b, `Engine/LightSelection`).** `CalculateActiveShadowCasterLights` (`0x1414cc570`) ported, by detour:
    -   It walks `activeShadowLights` (ShadowSceneNode `+0x148`, insertion order) and keeps a light while fewer than 4 are
        kept (the sun counts when it draws: `0x14338c911` clear), its UpdateCamera's verdict is visible, and descriptor 0's
        culling process shares a room with the world camera's list process.
    -   The verdict is DCLF's: the NiLight not hidden; a spot light's frustum intersecting the world camera's (`FUN_14150aba0`:
        a corner of either inside the other's six planes, or one of 8 edges crossing a plane at a point inside the other
        five); a point light's sphere inside the camera's planes (`BSMultiBoundSphere::Func41`); a directional light always;
        then the lodFade cutoff (`0x1433dcfac <= (d^2 - r^2) * lodAdjust`). Each in the engine's operation order.
    -   **The room test is a transitional engine read** (the user's choice): `FUN_140e14300` on the portal-graph entries
        (`+0x30190`), sampled at the function's entry. The light's rooms are its last accumulation's. Removed with the portal
        walk (T5).
    -   A kept light takes `shadowLightsAccum[slot]` and `maskIndex` (`+0x520`) from the 64-bit counter at `0x14338c904`
        (low: slot; high: mask index; the sun takes mask 0 and a slot per cascade). Each Accumulate advances the slot: 1 for a
        spot light, 2 for a two-hemisphere point light (its second slot is not written).
    -   Used by LightViews (built for the kept lights at the return, from the cameras their UpdateCamera set up),
        `ShadowViews::Rebuild` (DCLF's slots) and `LocalShadowLights` (DCLF's lights and mask indices, LightViews' cull
        planes). Parities `light selection ... <- LIGHT SELECTION` and `local shadow volumes ... <- LOCAL SHADOW VOLUME`
        (LightViews' planes against the processes' after the cull, a zero plane inactive).
-   **Still the engine's:** the focus views (T3c), the scissor rectangle (`+0x544..`; no DCLF reader), the depth target,
    slice and raster state at the draw (T7), and UpdateCamera itself (T2b, deferred to T7).

Measured: T2a m183/m186 0 differences (3,600 full-frustum processes per 300 frames); T3a l1-l10 0 of ~1,200 local views per
300 frames; T3b s2 (inn and Dragonsreach, point lights, no sun) and m189 (exterior motion, sun with local lights) 0 frames
differ in slots, mask indices and counts, 0 of 3,000 volumes differ, 600 of 600 shadow views drawn. No spot light was met.

## Point lights' culls: the category filter, and the light candidates (2026-10-01)

**The first attempt, a lent list** (replaced). A light that was not portal-strict was lent a list of the object root's
entries less the excluded ones, through the engine's list path (`BSShadowLight::sceneAccumArray`, `FUN_140e28f70`). That
saved 0.89 -> 0.46 ms a frame outside, but portal-strict lights (every interior light) could not take it:
-   under cull mode 4, a cell's `BSMultiBoundNode::OnVisible` (`0x140e2d710`) tests its multibound, and with `kAllPass`
    culls its children with no tests at all;
-   `BSParabolicCullingProcess::Process1` tests every node against the portals' compound frustum.

**The category filter** (`Engine/LocalLightCull.cpp`, `CS_DCLF_LIGHT_LIST`, on with the skip). `NiNode::OnVisible`
(`0x140d1e2c0`, `VTABLE_NiNode` slot 0x34) is trivial. If the bound radius is not 0 or the node is `kAlwaysDraw`, it calls
`NiAVObject::Cull` (`0x140d1c570`: skip a hidden child, else `Process1`) on each child. Category nodes are exact
`NiNode`s; `BSFadeNode` and the others call it directly, not through the vtable.
-   **The override.** Inside a point light's `Accumulate` (a thread-local filter), a category node that is the parent of
    excluded entries culls its children less those entries, with the same test. Everything above it stays the engine's:
    cells, multibound tests, `kAllPass`, portals. So every light takes it, portal-strict ones included.
-   **The build.** The filter is an immutable snapshot, built at the selection for a new exclusion. A node a child is
    attached to or detached from (`NoteStructure`) is walked natively until the next selection builds it again. Retired
    snapshots are released two Presents later.
-   **Empty children.** A tracked category node's child that holds no geometry at all (blood-spray decal placeholders,
    markers: about 660 at Riverwood) is cut too. A light entry appearing (`GetLightEntriesAppeared`) has those nodes
    judged again.
-   **Parity frames** filter nothing. Every paraboloid registration under a filtered node must hang from a child it
    keeps, or from an excluded entry (any ancestor).

**The light candidates** (`SceneStore::UpdateLightCandidates`, `GetLightCandidates`). The paraboloid exclusion is built
from them, not from the sun's candidates.
-   **A bug since R1.** A cell's multibound node is a sun entry for a geometry with no reference under it. Excluded, the
    skip left out the whole cell, actors, creeks and terrain blocks included, and their engine-drawn Lighting geometry
    lost the light's mask bit (below). The first mask parity measured 21-47 lost per report outside.
-   **Light entries** (`LightEntryOf`, `lightDependents`, `Tracked::lightRoot`). An actor's geometry has no sun entry: its
    light entry is its actor's root (its category node's child whose reference is an actor), carried items included.
    Any other geometry's is its sun entry, unless that is a category node (a cell's or a room's), so entries cannot nest.
-   **The rule** (`LightEntryAllows`). An entry is a candidate when every tracked geometry under it is a table object, or
    gives the light's registration no pass: what the sun's rule allows (hidden, alpha-blended, fading, an unselected switch
    child: the caster rule is the same function for the paraboloid mode), or no Lighting property. The exclusion
    (`BuildSunExclusion`) then asks that every table object that casts be the mode's input.
-   **Actors' churn** was the reason they stayed the engine's in the first version: the equipment's hidden bits flip with
    no event of their own. With the rule above, a hidden part and a shown one both allow the entry, so the flips change
    nothing: 1 stale frame in 300 at Riverwood.

**The lights' bits** (`LocalLightCull::NoteMainRegistration`, `NoteMaskClear`). A point light's registration ORs its bit
into the `activeLightMask` of every geometry its cull reaches, caster or not (`FUN_1414b2140`; skyrim-engine-notes.md,
"the sun's accumulation"), and the main pass reads it (`GetRenderPasses`: the shadow lights). DCLF's members take theirs
on the GPU (`LocalShadowLights`). The engine's main passes under a cut entry (terrain, actors' hair, technique-blocked
parts) take them from DCLF, as the sun's do (`ApplySunBits`):
-   **The test.** `FUN_14151a1e0`, the process's bound test, reduces to the light's sphere: visible when
    `(|c - centre| - r) - radius < 0` (skyrim-engine-notes.md, "Point lights' shadow culls").
-   **Per light**, in the `Accumulate` thunk: its ordinal in the frame (from the selection; past 32, a light culls
    everything), its bit (its accumulators' `+0x164`), and the sphere, read from the static process during its cull. Not
    from Main::Draw's mask clear (`FUN_1414cb640`): Main::Draw calls it only while the player's third-person 3D is drawn,
    so in first person the lights accumulate with no clear before them. Keyed on it, the list kept the earlier frames'
    lights and their reach, and the parity found bits given from them (the interiors tour, High Hrothgar).
-   **Reach**, recorded in the walk: a filtered category node the light reaches (one stamped store per node per light), or
    an entry its `Process1` skips (stamped words per candidate, `SunExclusion::lightReach`), by the process's state there:
    untested (cull mode 1, under a `kAllPass` multibound), the sphere, or the sphere and the portals (mode 4 with a
    compound frustum, which DCLF does not test; DCLF's members' GPU rule does not either).
-   **The write.** At a geometry's main registration (`+0x160 == 0xFFFF`, not owned), before the original call: per light
    that reached its entry, the light's bit when untested or its bound is in the sphere. Once a frame per geometry, as the
    engine's main registration reads the mask and clears it.
-   **Parity** (persistent-parity frames cut nothing): DCLF's bits against the engine's, where a cut would have taken the
    geometry out ("point lights' bits"). Riverwood: 570-960 compared a report, all agree but for 2 hair shapes in about
    8,000, which the engine gave a bit 3 units outside the sphere (its skinned bound moves between the light's cull and the
    main registration; at the radius the light contributes nothing). Bannered Mare: all agree, portal-strict lights
    included.

**Measured** (Riverwood with rain, camera turning; the Bannered Mare):
-   Every check OK, the Shield race apart. Light filter parity 0, light exclusion parity 0, holes 0.
-   Entries a light still walks outside: 5 actors (a skin-shape part), 79 technique-blocked (streams, campfires), 126
    under an unsupported parent (hay), a few particles and water: about 219 children culled a light, 12,516 cut. Inside: 3.
-   Point lights' `Accumulate`, without the parity switches: 0.10-0.11 ms a light outside (about 2.3 lights a frame), against
    0.56-0.70 ms with the filter off (`CS_DCLF_LIGHT_LIST=0`: the per-entry skip only) and 0.23-0.26 before the actors and
    the bits. Inside: 0.024 ms a light (0.06 before).
-   Filter builds 0.17-0.25 ms, about 35 per 300 frames.

**What is left:** the blocked entries above, each a paraboloid coverage expansion (technique, unsupported parent, skin
shape). Once those are covered, a light walks only the cells and the category nodes.

### Properties no shadow view takes, and the tour of the map (2026-10-01)

**The census** (`CS_DCLF_COVERAGE_PROBE`, `SceneStore::CoverageCensus`, every report): every tracked geometry the main pass
leaves to the engine for its technique, by verdict, technique, shadow verdict and record; and the light entries kept in the
point lights' culls, by the first geometry that blocks them. Two teleport tours with it and the parity switches: the hold
cities (Whiterun, Solitude, Windhelm, Riften, Markarth) and the interiors (Dragonsreach, Breezehome, the Blue Palace, the
Palace of the Kings, Understone Keep, the Ratway, Bleak Falls Barrow, High Hrothgar, Sky Haven Temple, Mzinchaleft,
Nchuand-Zel).

**What kept light entries in** (most at a location): 126 hay (unsupported parent, decals no shadow view takes), 77
refraction (a fire's heat haze, streams), 53 cave walls and boulders (not tri-shapes, real casters), 5 actors (hair: a
skin-shape verdict, alpha-blended), 2 parallax-occlusion river decals.

**The rule** (`CastsNoShadow`, Engine/ShadowViews.cpp; `SunEntryAllows`): a Lighting property whose own flags give it no
shadow pass in any view, whatever the frame, lets its entry go: the decals `ShadowCasterReject` rejects, refraction
(`GetRenderPasses_ShadowMapOrMask` rejects the flags themselves), alpha blending outside the decal exception, and a Utility
declaration of 0. Not a fade (it changes with no event) or the shadow global. The sun's entries take it too, through the
shared rule. Only the masks are wanted from such a geometry, and DCLF writes them (the lights' bits; the sun's,
`ApplySunBits`).

**Results.** Riverwood: 9 children culled a light (219 before), 0.07-0.10 ms a light with the parity frames. Every light entry
on the tour is a candidate but for the cave geometry (not tri-shapes). The main pass's techniques left to the engine across
the map: terrain (`MTLand`, `MTLandLODBlend`: Terrain Blending draws it after the opaque pass), multilayer parallax (ice
floes, a few clutter pieces), parallax occlusion (river decals) and refraction.

**Two defects the tours found:**
-   **The frame's lights.** Main::Draw calls its mask clear (`FUN_1414cb640`, `0x140644d7e`) only inside a block that draws the
    player's third-person 3D (it unhides it first). In first person the lights accumulate with no clear before them, and the
    light list was keyed on it: it kept earlier frames' lights and their reach, and a main registration took bits from them
    (High Hrothgar: 163 differences, every light the same light). The list now starts at the selection, every frame.
-   **A DXVK hang** (entering the Palace of the Kings): the main thread waited in `DxvkCsThread::injectChunk` (the ORG interop's
    `GetResourceInfo`, a buffer's address for `GpuResources::Acquire`) while the CS thread waited for work. The CS thread
    woke one waiter (`notify_one`) per synchronized chunk; upstream has one synchronizing thread, but the interop lets
    several wait at once, each for its own sequence number, so the wrong one could be woken and the right one never. It
    wakes all of them now (`extern/dxvk/src/dxvk/dxvk_cs.cpp`).

**The bits' parity on the tour:** all agree but for the kinds known: the hair at the radius edge, a terrain block once
(Mzinchaleft, its bound 2,750 units outside the radius: the light gives it nothing), and portal-strict lights' bits DCLF
gives without the portal test (a cobweb, a spider web: DCLF only, as its members' rule).

### Multi-index shapes in the shadow views; parallax occlusion and multilayer parallax in the main pass (2026-10-01)

**Multi-index shapes** (`Ineligible::MultiIndex`). The cave walls and boulders the tour found keeping light entries in are
`BSMultiIndexTriShape`s (geometry type 7): a tri-shape plus a second index list and a second shader property, a snow layer
(`altIndexBuffer`, `additionalShaderProperty`, `useAdditionalTriList`).
-   **The draw** (`FUN_1414f2ad0`, the pass draw): type 7 draws like a tri-shape (its renderer data, its triangle count), but
    a pass with accumulation hint 12 (the additional property's main-pass layer) draws the second index list.
-   **The shadow modes** register the main property's passes alone (`FUN_1414b2a60`: `GetRenderPasses` of the geometry's
    property), and `FUN_1414b2140` writes the light mask on the main property only.
-   **So** a multi-index shape is classified as a tri-shape (`ClassifyStatic`), and an eligible one becomes `MultiIndex`: a
    shadow-only caster (`ShadowOnlyReason`), its main pass the engine's (the snow layer).
-   **Measured** (Mzinchaleft, Bleak Falls Barrow, Nchuand-Zel; full parity): 95 multi-index shapes shadow-only; no light
    entry left blocked; holes 0; every check OK, the Shield race apart.

**Parallax occlusion (7) and multilayer parallax (11)** join the main pass's techniques (`IsSupportedTechnique`).
`SetupMaterial` writes their constants and textures (`ParallaxOccData`, `MultiLayerParallaxData`, the layer and envmap
maps), which the material records take from the engine's own evaluation. What `SetupTechnique` adds is ported:
-   **The remap** (`SetupTechniqueDescriptor`): technique 7 draws as 0 unless the parallax-occlusion global
    (`0x142035500`) is set, and 18 as 9 unless the land-noise global (`0x142032fdb`) is. The vertex and pixel descriptors,
    `EvaluateTechnique` and the stored technique `SetupGeometry` reads (`BSLightingShader+0x94`) all take the remapped one.
-   **The samplers:** technique 7 as parallax (slot 3), 11 slots 4, 5 and 8 (the cube map, its mask, the inner layer).
-   **Capture parity by technique** (`CS_DCLF_CAPTURE_PARITY`, new: checked and mismatched draws per technique, and the
    first mismatch of each): at Riften, multilayer parallax 3 of 9 and 6 of 18 mismatched, all the specular bit (`0x200`)
    the engine fades out by distance and DCLF keeps (its specular fades in the draw, `LodFadeFrame`): the same difference
    technique 0 shows. Material and technique mismatches 0. Parallax occlusion (two river decals on the tour) was never drawn
    in view on these runs, so its capture is unverified.
-   **With ownership** (Riften, Mzinchaleft; full parity): holes 0, every check OK, the Shield race apart.

**The "STALE material record" warnings on cell changes** were the validation, not a material writer. Every one was a
character-light pass whose live sample lacked t11 (`written 1807->1007`, `803->3`): `SetupMaterial` binds the character
light's target only while its index (`0x142033db0`) is not negative, and it is -1 while a cell loads. t11 is the frame's
(`MaterialSources`), but `CopyFrameComponents` copied it only when the source had written it, so a load frame's sample
compared unequal. It now copies t11 whole, written or not. A Bleak Falls Barrow load and the three-cave tour: 0 stale.

### Multi-index shapes in the main pass: pass layers (2026-10-02)

A `BSMultiIndexTriShape` with an additional property has two main passes, and the main camera's claims are by geometry,
so DCLF takes both or neither (engine notes, "The main modes' registration"). The second is a **pass layer**: an object
of its own, held by the base's tracked entry (`Tracked::layerSlot`; `Tables::layerBase` / `layerOf` link the two slots).
This is the general shape for any geometry with more than one main pass: each further pass is a layer object with its own
pipeline, material and geometry, so the decal ordering, culling, rows and kept stores see an ordinary object.
-   **Classification.** A multi-index shape is classified as a tri-shape; its layer (`LayerPropertyOf`, `ClassifyLayer`) by
    `DeriveLightingDescriptors(..., a_layer)`: decal group 3, blend 0, write mode 10, the alpha property not applied (render
    flags 0x41). A shape whose layer DCLF cannot draw stays `MultiIndex` (shadow-only, both passes the engine's), and so
    does every one with `CS_DCLF_LAYERS=0` (a live toggle, under Decals). A shape with no additional property is a plain
    tri-shape. Technique 14 (`multiIndexSparkle`) is supported with the layers.
-   **The record** (`WriteLayer`, after the base's in `WriteObject`): the base's placement, a geometry slot for the second
    index list (`ResolveLayerGeometrySlot`, keyed by the `altIndexBuffer`, `Tables::geometryLayerKey`), no shadow
    (`ShadowReject::Layer`), its property's LOD fade node, journaled like any write. Light-path placements and root moves
    write the layer with the base (`TakePlacement`, `TakeRoot`); release, the slot sweep and the device reset take it too.
    The layer property is listed in `propertyDependents`, so its events classify the entry again.
-   **Membership.** `BindByMembership` binds a layer slot with `PrimaryCull::MembershipLayerPass` (hint 12); the
    accumulate phase's join runs the same code for both (`layer`), with the layer's own derived cache. A base and its layer
    are members together: `DropResidentSlot` drops the partner, and a frame where one joined without the other drops both
    (`layers or bases unpaired` in the membership line). A layer's drawn mark claims nothing: the base's claims the geometry.
-   **The pass.** No shadow bits: a layer's passes never carry ShadowDir or DefShadow (measured), since its property's light
    mask never names the sun (the registrations write masks on the main property alone). ProjectedUV extras of both
    objects come from the shape (`materialProjection`, `materialParams`, `normalDampener`, `materialScale`).
-   **The draw.** Decal group 3, drawn by the second pass between the opaque and blended groups (`kDecalDrawOrder`; its slot
    count at count word 27), testing LESS_EQUAL with bias mode `8 + b` and writing depth from its colour draw, after its
    host is shaded; it has no part in the decal depth pass.
-   **Where the engine draws them.** The sparkle layer is drawn where its shapes are in view and the scene is snowy: the
    Yngvild exterior's icebergs and ice piles, 149 layers, about 4,000-25,000 native layer draws per report with DCLF
    off for them. Bleak Falls Barrow's 95 cave pieces are behind portals from the spawn point, and the engine drew none of
    their passes there.
-   **Measured.** Capture parity at Yngvild (`CS_DCLF_OWNERSHIP=off`, `CS_DCLF_PRIMARY_EXCLUDE=0`, and the new
    `CS_DCLF_NATIVE_SKIP=0`, which keeps the native passes of what DCLF drew so that every draw is compared): 19,695
    layer draws compared; pass and shader descriptors, material, technique and per-geometry constants match (`EyePosition` aside, which
    every technique showed in this mode until `WritesEyePosition` took the engine's whole rule); draw parity OK over 180,618 draws (the second index
    list's buffer and count). `ProjectedUVParams2.zw` of a multi-index shape is unwritten by the engine and not compared.
    With ownership (Yngvild, Bleak Falls Barrow; full parity): holes 0, no base or layer unpaired, every check OK.
-   **Found along the way.** Capture parity sees only the native passes the skip leaves, which in a settled scene is
    hardly any (hence the earlier counts of 9 and 18 multilayer draws): `CS_DCLF_NATIVE_SKIP=0` is the switch for a
    complete comparison. In Bleak Falls Barrow, about 5,000 native draws a report were of geometry whose root fade node
    has no parent (`NorChamberRitual01` and others), outside every category node and so never tracked. Every such root
    is in the portal graph's `alwaysRenderChildren`; see "Parentless roots the portal graph draws".

## Parentless roots the portal graph draws (2026-10-02)

-   **What.** An interior's portal graph (`loadedData->portalGraph`) draws the references in `alwaysRenderChildren`
    (`+0x58`) whatever room the camera is in. Their roots have no parent, so no walk up from them reaches a category
    node, and DCLF never tracked them: in Bleak Falls Barrow 500-5,000 native draws a report (the ritual chamber, stairs,
    wall pieces), drawn every frame by the engine.
-   **How they are tracked.** `RefreshCategoryNodes` files each listed parentless root under its graph's shared portal
    node, a category node already (`alwaysRenderRoots`), and `FindCategoryNode` returns that node for anything whose
    walk up ends at such a root. Every walk that stops at the category node runs out of parents first, so the root
    and its reference stay inside the walks: the move key is the reference's, and hidden bits and switches under the
    root count as anywhere else. `CaptureCullHiddenBits` already took these roots' hidden bits for the walk.
-   **Membership by events.** `CategorySignature` folds in each graph's list (its size, every root and its parent), so
    a root added or removed runs the refresh: a new root is walked (`AddSubtree`), and what was tracked under a root
    that left is dropped. The map holds each root, so its subtree is still there to walk when that happens. Attaches
    under a listed root are found through `FindCategoryNode` like any other.
-   **Measured.** Capture parity in Bleak Falls Barrow: geometry drawn outside the tracked category nodes 4,969 a report
    to 0, untracked eligible 0, and the roots' draws compare clean (per-geometry: the candle flicker alone).

## Members whose pipeline is still compiling (2026-10-02)

-   **The holes.** Every tour had 48-52 holes in two frames right after `coc BleakFallsBarrow01`: skeevers' and bandits'
    parts, members of stood-in entries, drawn the frame before and then by nobody. Set parity: "withheld natively, drawn by
    nobody ... depth build skipped: pipeline, colour build skipped: pipeline". Their pipelines had a current key and no
    set index: compiling.
-   **Why.** The cell turns the sun's shadows on (`kNoSunShadowDir`, part of `PrimaryCull::MembershipWitness`), so
    `BindByMembership` binds every resident again in that frame's accumulate phase. The actors' passes gain ShadowDir
    (`0x40`), a new pipeline compiled in the background for a frame or two. The cull that frame had already stood in for
    their entries (they were drawn in full the frame before), so the engine registered nothing, and the native skip, which
    leaves to the engine any pass DCLF cannot draw (`DrawableThisFrame`), had no pass to leave. The `Lookups` contract (a
    pipeline still compiling delays a draw, never makes a hole) held for the native loop and not for the stand-in.
-   **The fix: a member DCLF cannot draw this frame is the engine's registration's.**
    -   `SceneStore::PipelineDrawable` / `ObjectDrawable`: compiled into the set for the slot's current key
        (`PackPipelines`' rule), and bindings. `DrawableThisFrame` uses it, so the native skip and the builds agree.
    -   `PrimaryCull::PrepareFrame`, before the list jobs, samples which used pipelines are drawable and the membership
        witness. The stand-in's walk hands a bound member that is not drawable to the registration instead of DCLF's
        visible list, and so does `Owned` (the leaf exclusion's test, which the hand-off goes through as well). A witness
        that differs from the residents' (`rebindAll`) walks every entry and hands every member over, since this frame's
        accumulate phase binds them all again; `BindByMembership` reads the same sample, so the two always agree on the
        frame.
    -   A join onto a pipeline still compiling marks its entry walked (`NoteMemberUndrawable`), and the entry is walked
        again once the pipeline is drawable (`undrawableMembers`), so it stops walking.
    -   The engine registers those members that frame, and the native skip draws each pass natively while DCLF cannot;
        as soon as the pipeline is in the set DCLF draws it and the skip withholds the native pass.
-   **Measured.** Ownership tour (Bleak Falls Barrow, then Yngvild; full parity): holes 0 in every report, every parity
    check OK. The report's primary exclusion line counts "bound but not drawable this frame" and the rebinding frames.

## On-demand builds are logged (2026-10-02)

The goal is no shader or pipeline compiled at runtime: one that is means the precompile missed it. Until then every such
build is a warning, once, when the request that starts it is made, with what is needed to write a precompile rule:

-   **SPIR-V stages** (`ShaderPrograms::Find`, `FindShadow`): a stage a runtime request asks for before any precompile
    task did (`kOnDemand*`). "on-demand SPIR-V compile: Lighting|Utility <stages> ..." names the descriptors or
    technique, whether Community Shaders' compile workers were still busy (the request beat the precompile) or idle (the
    precompile never asked), the pipeline key, and the objects that use it. A second line, "on-demand SPIR-V ... done
    after N ms", says which stages DXC compiled and which the disk cache served.
-   **Pipelines** (`DrawPipelines::Find`, `FindShadow`; `DrawcallLimitFix::RequestLightingPipeline`,
    `RequestShadowProgram`, `RequestShadowPipeline`): every pipeline is built at runtime today (no precompile, no disk
    cache). "on-demand pipeline build: Lighting|shadow ..." gives the key, its technique, the objects or casters that need
    it, and how it differs from the nearest pipeline already requested (`NearestKey`: the fields and bits), or the
    recreation that left none (a target or shadow-format change).
-   **First reading** (Bleak Falls Barrow tour): Lighting SPIR-V 0 misses; Utility SPIR-V 40 stages, all the shadow
    techniques with DCLF's mode bits (`0xC000`, `0x14000`), which no Community Shaders task compiles, all served from the
    disk cache; 210 Lighting and 224 shadow pipelines, every one. Many Lighting keys differ from an existing one only in
    the pass descriptor's shadow bits (`0x2040`, ShadowDir and DefShadow).

## The GPU-bound gap: shadow views run with late Z (2026-10-02)

At a save where the frame is GPU-bound (Save5, a Riverwood exterior, 2560 x 1440 internal under DLSS Quality at 4K),
DCLF made the GPU frame slower: 12.7 ms against 11.75 ms native (`CS_GPU_EVENT_TIMERS=300` across a live toggle).

**Where, by segment** (nvperf, `CS_NVPERF_RANGES` over DCLF's graph passes and the engine's events; ms per frame):

| Segment | Native | DCLF (its passes and the engine's residual draws) | Gap |
| --- | --- | --- | --- |
| Shadow maps (sun cascades, volumetric copy, point light) | 1.58 | 2.14 | +0.56 |
| Depth (Z-prepass) | 0.66 | about 0.95 | +0.29 |
| World (main pass) | 4.36 | 4.37 | 0 |
| Skylighting occlusion | 0.09 | 0.12 | +0.03 |

In the depth segment the draws match (0.73 against 0.66 ms). The extra is the compute around them: HZB 0.10, phase 2
0.04, fade states 0.03, builds and sorts 0.05. Every upkeep compute pass together is about 0.35 ms, each 0.006-0.04 ms,
and the range profiler inflates small ranges.

**The shadow views shade every fragment before testing it.** The same 2.31 M input primitives cost the engine's mode-0xE
accumulator 1.19 ms and DCLF's cascades 1.66 ms. DCLF launched 6.05 M pixel-shader warps against 4.2 M. The early-Z
counters (`prop__earlyz_input_samples`, `prop__latez_*`) explain it: the engine's casters send 51 M samples through
early Z, and DCLF's send none, in every shadow view. DCLF's Z-prepass gets little early Z too (3.8 M samples against the
engine's 14.5 M). The main colour pass, which writes no depth, gets it normally.

Ruled out, each with a run: depth bias (none at all: still late Z), cull mode (the engine honours `kTwoSided` in shadow
maps as DCLF does: two-sided passes draw with cull 0, the rest with cull 1, about 28% two-sided), a pixel shader that
discards or exports depth (DCLF's opaque cascade technique `0xC000` has neither), and the attachment layout (the
Z-prepass imports its depth the same way).

**Cause.** An indirect execution set holds both kinds of pipeline: opaque casters, and alpha-tested ones whose pixel
stage discards while depth writes are on. With device-generated commands, the driver puts the whole set on late Z.
Compiling every DCLF shadow pixel stage empty (a temporary experiment, so no pipeline in the set discards) gave early Z
in every view (92 M samples in cascade 0, 52 M in cascade 1), 0 pixel-shader warps (an empty stage under early Z is
skipped), and 1.39 ms for the views against 1.94 ms. With no pixel stage at all (also an experiment: alpha-tested
casters then shadow solid) the views took 0.79 ms. An indirect execution set needs one stage layout, so a pipeline
cannot simply drop its pixel stage: `UpdateIndirectPipelineSet` rejects it.

**The split (done; its two sets and second range were replaced by plain draws, below, which keep its class order).** Each
shadow view drew its casters in two calls, each with its own indirect execution set:

-   **Classes.** `DrawPipelines::BuildShadow` scans the pixel SPIR-V (`FragmentDefersDepth`: `OpKill`,
    `OpTerminateInvocation`, `OpDemoteToHelperInvocation`, a depth or sample-mask export, image writes, atomics). A
    pipeline that can defer the depth test is in the discarding class (`kShadowDiscards`), the rest in the depth-only
    class (`kShadowDepthOnly`). `ShadowSetVersion` holds a set and a signature per class over one index space: a
    pipeline is written only into its class's set, at its own index (`ApplyClassToSet`), and a set is created with the
    first pipeline of its class.
-   **Routing.** `UseShadowMapRow` writes a discarding pipeline's map entry with `kShadowMapSecondRange` (bit 30).
    BuildDrawsCS sends such a draw to the view's second range of sequences (from `PhaseTwoBase`, the slot's
    `sequenceDraws`), counted in the phase-2 word, as phase 2 does for the depth segment. A slot's sequence buffer holds
    both ranges.
-   **Recording.** `ShadowViewPass` preprocesses and executes, per view, the depth-only call and then the discarding
    one. A class with no pipeline in the set yet has no call. The culling statistics add the two count words.
-   **The empty stage.** Utility.hlsl compiles DCLF's depth-only techniques without an alpha test (`RENDER_SHADOWMAP` or
    `RENDER_DEPTH`, no `ALPHA_TEST`, `ADDITIONAL_ALPHA_MASK` or `RENDER_SHADOWMAP_PB`) with an empty pixel `main`.
    `TEXTURE` is allowed: its test compares an alpha of 1 with a reference of at most 1, so it never discards, but its
    `discard` instruction alone put every textured opaque caster (`0xC01B` and the like) in the discarding class. Before
    that, 180 of 184 shadow pipelines were discarding; after it, 69 are depth-only. The point lights' paraboloid techniques
    (`RENDER_SHADOWMAP_PB`) discard by design and stay late-Z.

**Result**, the same save, GPU event timers over the frames after 3000 (the passes' SPIR-V and pipelines compile during
the first frames of gameplay, so a measurement starts after them), two runs each:

| | GPU frame | Shadow views |
| --- | --- | --- |
| No split | 13.13 / 13.39 ms | 2.67 / 2.90 ms |
| Split | 12.87 / 12.86 ms | 2.24 / 2.25 ms |
| Native, same frames (live toggle) | about 11.85 ms | 2.01 ms |

nvperf at the same frames: the views' early Z went from 0 to 47 M samples and their pixel-shader warps from 7.7 M to
5.9 M, the engine's figures being 53 M and 6.2 M. The cascades' draws went from 2.2 to 1.5 ms. The shadow maps and the
image match native across a live toggle. Cascade 0's mean depth steps by about 0.0035 at a toggle, with or without the
split: a small difference that predates it.

**What is left in the shadow views: device-generated commands.** With the same fragment work as the engine, the views
still take about 0.5 ms more than its shadow maps, and the front end issues 4.3 times the operations per draw (about 42
against 9). nvperf's per-call ranges put the preprocesses at 0.2-0.9 ms a frame, mostly cascade 1's two calls, but short
ranges vary a lot between runs under the range profiler's drains. Tried and ruled out, each against a run at the same
frames: implicit preprocessing (2.75 against 2.63 ms), sorting the inputs by pipeline (front-end operations 145 k to
106 k, time unchanged), and a quarter of the max count (preprocess time unchanged).

### The same split for the Z-prepass: measured, not kept

The Z-prepass's late Z looked like the same cause: 4.1 M early-Z samples against 10.1 M late, 8.4 M of them rejected
after shading. The split was built the same way:

-   the depth variants classed by `FragmentDefersDepth` (49 of 99 cannot discard; DXC compiles `discard` to
    `OpDemoteToHelperInvocation`);
-   a set of the opaque ones;
-   a per-pipeline class bitset in the depth segment's latch;
-   phase 1's alpha-tested draws in a third range of the sequence buffer, with their own count word;
-   two calls.

Per call (nvperf, Save5, frames 3300+):

| Call | Draws | Early-Z samples | Late-Z samples | PS warps | Time |
| --- | --- | --- | --- | --- | --- |
| Opaque depth variants | 1,277 | 3.68 M | 0.02 M | 0 | 0.15 ms |
| Alpha-tested depth variants | 753 | 0 | 10.68 M | 433 K | 0.31 ms |

The opaque draws were already on early Z: the baseline's 4.1 M early samples are theirs. Unlike the shadow views, the
depth pass's mixed set does not put every draw on late Z. The late samples are the alpha-tested foliage's, which must be
shaded before its depth can be written. The engine's own alpha-tested depth draws behave the same way: 4.6 M of their 6.3 M
late samples are rejected after shading. So the split only added a generated-commands call (0.46-0.47 ms against 0.35),
and it was reverted.

Why the shadow views behave differently is not known. Their pipelines carry a depth bias and the views' rasterizer
states; the depth pass's do not.

### Plain indirect draws for the shadow views (done)

The shadow views (and Skylighting's occlusion map, which uses the same passes) no longer use device-generated commands.
Each view records one plain `vkCmdDrawIndexedIndirectCount` per pipeline it uses; the vertex stage fetches its own vertices
and per-draw words.

**The gate.** Before building this, the views were run twice with their generated commands intact but nothing to draw:

| Shadow views, Save5, frames 3000+ | Time |
| --- | --- |
| As drawn (the split) | 2.25 ms |
| Every sequence with `instanceCount = 0` (commands processed, no geometry) | 1.00 ms |
| No sequences at all (count 0) | 0.63 ms |

So about 1 ms of the views was the generated commands, their preprocesses and the passes' setup, against the engine's
1.58 ms for its whole sun shadow maps.

**How it works.**

-   **Buckets.** A view rasterizer state's map row now names, per key slot, a *bucket*: the distinct pipelines the row uses,
    the depth-only class first (`DrawPipelines::ShadowDiscards`), each in index order (`ShadowEpochs.cpp`, `BucketsOfRow`).
    A plain call's argument offset is fixed on the CPU, so each view sizes its buckets (`SizeShadowBuckets`) from
    `ShadowPayload::keySlotDraws`, the draws each key slot's inputs can produce, counted in the same walk as `modeDraws`.
    A bucket keeps its capacity while its draws fit and grows to a power of two past it, so the recorded calls change only
    when one grows (the slot's sequence buffer holds twice the scene's draw bound, as for the split). The view's bucket
    table (first, capacity) is in the shadow latch (`ShadowLatchLayout::BucketOffset`, `BuildDrawsLatch::bucketTableOffset`).
-   **BuildDrawsCS.** A shadow draw takes the next slot of its bucket from the slot's bucket counts
    (`ShadowResources::bucketCounts`, a word per bucket, zeroed by each commit) and writes its sequence there
    (`StoreShadowSequence`): the usual `DrawSequence`, whose tail is an indexed draw from the index pool, with the bucket
    slot as its first instance.
-   **The index pool** (`ShadowIndexPool`). A plain draw cannot bind an index buffer of its own, so every geometry slot's
    index buffer is copied into one DCLF buffer, which each view binds once. The shadow commit follows the scene's
    geometry log with a cursor of its own (`UpdateIndexPool`): a slot the log names lets go of its range and takes its
    buffer's, a buffer with no range yet gets one (ranges are shared by address, freed with their last slot, and reused
    first-fit) and a copy. `IndexPoolCS` runs the copies, a group each, from the buffers' device addresses, ahead of the
    epoch's draws; its dispatch is in the latch. A full pool doubles and lays every range out again. The slots' first
    indices are a buffer BuildDrawsCS reads. Save5 fits the initial 16 MB.
-   **The vertex stage** (`Utility.hlsl`, `DCLF_PULLED`, defined for DCLF's Utility builds). Its draw's sequence is at the
    call's first sequence (pushed per call in the draw push range, `kShadowDrawPushSequences`) plus its instance. From it
    come the object word, the material row (`TexcoordOffset`) and the geometry's vertex buffer and second stream. The
    vertex index is the geometry's own (the pool draw has no vertex offset); the attributes are loaded at it and decoded as
    the engine's input layout for the pipeline's vertex layout has them (`VertexInput.cpp`; the layout is pushed per call).
    The pixel stage gets the diffuse's descriptor and the object word as `nointerpolation` outputs, and samples the diffuse
    through `ResourceDescriptorHeap`.
-   **Recording** (`ShadowViewPass`). Per view: begin the pass, push the view's words, bind the pool, then per bucket with
    a capacity bind its pipeline, push its words and record the draw over its range with its count word. No preprocess.
    The shadow pipelines are no longer in execution sets; `ShadowIndirectState` holds them by index (with their vertex
    layouts and classes), and the published version keeps them alive.

**Result**, Save5, GPU event timers over the frames after 3000:

| | GPU frame | Shadow views |
| --- | --- | --- |
| The split (DGC) | 12.83 ms | 2.30 ms |
| Plain draws, the vertex stage reading indices itself (no pool) | 12.52 ms | 2.15 ms |
| Plain draws with the index pool | 12.48 / 12.49 ms | 1.97 / 1.97 ms |
| Native, same session (live toggle) | 11.94 ms | 1.98 ms (sun 1.56, point lights 0.42) |

The split's row here is the same timer read the same way as the others (`gt.py`, every block after the first three); the
table above gives 2.24-2.25 ms for the later blocks alone.

nvperf (`cs.dclf.shadow.view`, frames 3300+): front-end operations 106 k (DGC) to 22 k for 3,354 draws, the engine's
ratio; vertex-shader warps 233 k without the pool (an invocation per index) to 108 k with it; pixel-shader warps and early Z
unchanged (5.95 M, 45 M samples). The shadow maps' means match the DGC path's to about 0.0005 and native's across a live
toggle, the image is unchanged, and a run with the validation layers (`VK_LOADER_LAYERS_ENABLE=*validation`) reports
nothing from DCLF: its only messages are Streamline's (a barrier to an undefined layout at startup, and swapchain
semaphore reuse in the present path).

The shadow views are at parity with the engine's shadow maps. The rest of the GPU frame gap (about 0.55 ms at Save5) is
elsewhere.

### Next options

1.  **Upkeep compute on the async queue** (fade states, tree wind, builds, sorts; the passes already name their queue
    with `PreferQueue`): at most about 0.2-0.3 ms, less the cross-queue waits.
2.  **The HZB and phase 2** (0.14 ms) pay off only when occlusion culling removes more than it costs, which at a
    GPU-bound exterior is worth measuring with `CS_DCLF_CULL=frustum`.
3.  **Plain draws for the Z-prepass and the colour pass**, if their generated commands cost as the shadow views' did
    (the same gate measures it). The two test EQUAL against each other, so they would have to move together, and the
    colour pass binds far more per draw (the rows' blocks and textures through the layout's indirect ranges).

### The rest of the gap: DCLF's compute, and async compute (investigation)

**The gap by segment** (Save5, GPU event timers, DCLF on against off across a live toggle in one run; 12.35 against
11.77 ms):

| Segment | On | Off | Gap |
| --- | --- | --- | --- |
| Depth (DCLF's Z-prepass segment and the engine's residual depth draws) | 1.03 | 0.67 | +0.36 |
| World (the colour segment) | 4.50 | 4.37 | +0.13 |
| Shadow maps | 2.08 | 1.99 | +0.09 |
| Skylighting occlusion | | | about 0 |

**DCLF's compute passes** (nvperf, frames 3300+, 5 captures; the range profiler drains around each range, so a small pass
reads high, and in the frame each costs its time plus the drain its barrier forces):

| Segment | Pass (us) |
| --- | --- |
| Depth | HZB 98, fade state 33, build-draws 18, sort scan 18 and scatter 9, tree wind 7 and 6, phase-2 build 13; phase-2 depth draw 46 (draws next to nothing: its cost is the pass) |
| Colour | build-draws 23, sort scan 17 and scatter 8, tree wind 7 and 6 |
| Shadow | build-draws 23, tree wind 8 and 7, index pool 4 |
| Sky | build-draws 15 |

The depth segment's draws (0.36 ms, and the engine's residual 0.38 against its native 0.67) are close to native; its
extra is the compute and the phase-2 pass, about 0.25 ms. The colour segment's compute is about 0.06 ms, the shadow
epoch's about 0.04.

**What async compute needs here.** CS has no second queue to give ORG today:

-   DXVK creates graphics, transfer and sparse queues (`dxvk_adapter.cpp`, `DxvkDeviceQueueSet`), and the interop
    (`DxvkOrgInterop.h`) hands BasicRHI the graphics queue alone (`adopt.queues[0]`), so BasicRHI's compute queue aliases it.
    ORG itself schedules compute passes on a compute queue (a pass's `PreferQueue(QueueKind::Compute)`) and synchronises
    the queues with timeline semaphores.
-   The DXVK fork would create a queue of a compute-only family (on NVIDIA, family 2) and export it with its family and
    index, and the host would adopt it as `queues[1]`.
-   The host's submission hook sends everything into DXVK's stream (`dxvkEnqueueInteropSubmission`), which submits on the
    graphics queue in D3D11 order. A compute queue's submissions would be submitted directly, under a lock of their own.
    Their waits on graphics values still in DXVK's stream are legal (timeline semaphores allow a wait before its signal).
-   DXVK's own D3D11 work is not in the graph, so an async pass may touch DCLF's buffers only, never an engine resource
    (the HZB's read of the engine depth stays on the graphics queue). Buffers both queues use need concurrent sharing
    (BasicRHI's `QueueSharing::Concurrent`) or ownership transfers.

**Where async compute can overlap anything.** Inside an epoch every pass feeds the next (build, sort, draw), so moving a
pass to the compute queue only adds a cross-queue wait. Overlap needs work whose consumer is not right behind it:

1.  **Built early, drawn later.** The Z-prepass's culling (build-draws and sort, about 45 us) reads the inputs and the
    predicted eye ready at EarlyPrepass (its job is already kicked there) and the previous frame's HZB: issued on the
    compute queue at the shadow epoch, it runs beside the shadow maps. The colour segment's build and sort (about 50 us)
    read the depth segment's visibility, final after phase 2: issued at Prepass, they run beside the G-buffer draws. The
    shadow epoch's build could start at BeforeShadowMaps, beside the engine's own casters.
2.  **A frame behind, ping-ponged.** Fade state (33 us) and tree wind's clocks need not be this frame's: computed into one
    of two buffers while this frame's builds and draws read the other, they overlap whatever runs then. Tree wind's record
    pass runs in every epoch only because each epoch's record uploads put back the tree words a member joined with; once
    the uploads leave them alone it runs once a frame, and frame-behind like the clocks. ORG has no history resource: the
    two buffers would be declared through resolvers (per-frame resolution, `PassBuilder`'s resolver declarations), and
    whether ORG orders a next-frame reader after a compute writer from the frame before (its per-queue batch history)
    must be checked before relying on it.

**Not async, but cheaper.**

-   **The HZB** is a dispatch per mip with a full barrier between each (about 12 drains for 98 us). A single-pass
    downsampler (one dispatch, the last group per tile carrying on to the coarser mips) would do it in one, likely 20-30 us.
    It reads the engine's depth and phase 2 waits on it, so it stays on the graphics queue.
-   **Phase 2's depth pass** costs about 46 us drawing next to nothing: a depth pass begun and ended for the rescues. Its
    cost is fixed; drawing the rescues inside the phase-1 pass is not possible (they need the HZB built from it), but the
    pass could load and store less (its region), or the rescues could be rare enough to skip when the build counts none
    (an indirect count already gates the draws; the pass itself is what costs).

**Order proposed:** the single-pass HZB first (graphics queue, no infrastructure, about 70 us); then the compute queue
(DXVK and host, the largest piece); then the early builds (about 0.1 ms of overlap); then the frame-behind upkeep. Together
these address roughly 0.2-0.3 ms of the 0.58; the rest is the passes' fixed costs (each epoch's render pass begins,
uploads and the residual engine draws).

### The HZB in one dispatch (done)

`HzbCS.hlsl` is now AMD's single-pass downsampler (FidelityFX SPD 2.0, `ffx_a.h` and `ffx_spd.h` vendored unchanged
from BasicRenderer into `Shaders/DrawcallLimitFix/FidelityFX/`): one dispatch of a group per 64x64 tile of the source
writes mips 0-5 of its tile, and the last group to finish (a global counter, `cs.dclf.hzb-counter`) carries on from mip 5
to the end.

SPD wants a source whose every level halves evenly. The depth is not one, and nothing is padded or copied for it: the HZB
was already defined over a power-of-two domain (mip 0 is half the next power of two of the depth), and the callbacks
answer for the texels the depth does not have. Inside the domain but past the rendered area they read the far plane, as
before. Outside the domain, or outside a level that is no longer square once its short side is one texel, they read 0,
the max reduction's identity, since such a texel covers no area. Stores outside a level are dropped. The old per-mip
build answered the far plane there too, which made the last levels of a non-square chain all far; they are now exact.

A domain wider than 4096 (a render target wider than 4096) is more than one dispatch can finish: its mip 5 is wider than
the last group's 64x64 tile. `HzbPass` then splits the chain into dispatches of six levels until the rest fits, each
dispatch reading the previous one's last level.

The counter is zeroed once by the first depth commit (`hzbCounterZeroed`, like the sort's counts); after that the
last group of each dispatch zeroes it. The pass runs only with a published depth frame, so that commit has happened.

**Result** (Save5): nvperf `cs.dclf.hzb` 97-98 us to 44 us over five samples. The GPU event timers' depth segment went
from 1.033 to 0.986 ms and the Z-prepass event from 0.655 to 0.609 ms (17 blocks against 18); the whole-frame difference
is inside the run-to-run noise (about 0.05 ms). Culling is unchanged: 2993 of 8972 occluded against 2919 of 8927, and
phase 2 brought back 18 objects against 4.

### An async compute queue, and tree wind a frame ahead (done)

**The queue.** Four layers, each fixed where its contract was:

-   **DXVK** (`dxvk_device_info.cpp`, `enableQueues`) creates a queue of the compute-only family (on NVIDIA family 2), the next
    queue of it when the transfer queue fell back to that family, and never submits to it. The interop exports it
    (`DxvkOrgInteropDeviceInfo::computeQueue`, interop version 3; both headers bumped together).
-   **The host** (`RenderGraphRuntime.cpp`) adopts it as BasicRHI's compute queue (`CS_ORG_COMPUTE_QUEUE=0` leaves it
    aliasing graphics) and submits to it directly under a lock of its own. ORG orders an execution against DXVK's work on the
    shared queue with barriers (`ExternalQueueBoundary`); a queue the host does not share is the host's to order, and it now
    does: before an epoch's first compute submission DXVK's stream signals a host timeline where it stands (after every D3D11
    command so far) and that submission waits for it (`EnqueueStreamPoint`); when the epoch ends the stream waits for the
    compute queue's last signals (`CloseComputeEpoch`, in the epoch's batch). `ExternalQueueBoundary`'s comment states this.
-   **ORG** kept every pass on the graphics queue on Vulkan ("until their queue-family ownership policy is captured"). A pass
    may now leave it when nothing it touches needs a queue-family ownership transfer: every resource
    `QueueSharing::Concurrent` (both lowerings, `RenderGraphPersistent.cpp` and `RenderGraphCompile.cpp`). Every graph buffer
    is created concurrent (`GpuBufferBacking`); `BufferBase::TryGetRHIResourceDesc` now says so. A closed execution may use
    a buffer from several queues (relative waits order it inside, the host's boundary outside); a texture stays on one.
-   **BasicRHI** clipped a barrier's stages to the recording queue's family but not its accesses: the closed entry's full
    barrier on the compute queue named colour-attachment access (VUID 02815/02816). `VkAccessForQueue` clips them too.

Validation (`VK_LOADER_LAYERS_ENABLE=*validation`) is clean with the compute queue in use: only Streamline's messages.

**In-epoch compute is a loss.** Fade state moved to the compute queue inside the Z-prepass epoch (its consumer, the build,
right behind it) put two cross-queue hops into the depth chain: the depth segment 0.986 to 1.110 ms. Work on the compute
queue has to be off the chain, which upkeep can be when it runs a frame ahead.

**Tree wind a frame ahead.** Every TreeParams and WindTimers value is the tree's, not the member's (its clock, position and
leaf frequency, and the frame's camera and wind). `TreeWindCS` is now one pass on the compute queue in the Z-prepass epoch,
a thread per entry (entry 0 the nodeless trees', entry s + 1 tree slot s): it brings the tree's clock to the frame and
writes the tree's values, three rows an entry (TreeParams, WindTimers with the distance and amplitude, the listing's
generation), into the frame's wind buffer of two (`SceneBuffers::treeWindRows`, by the frame's parity). Every draw reads
the other one, the frame before's, at t124 (`kTreeWindRegister`, `DCLFTreeWind`; the frame record names it,
`TreeWindReadIndex`): so the Z-prepass and colour passes of a frame read the same values, as their EQUAL test needs. A
record carries its tree slot and that listing's generation in WindTimers.zw (`LightingConstants.cpp`; a slot cannot be
relisted while a member holds it), and draws the entry only when the generation matches, otherwise the values it joined
with: a new tree draws its joined wind for its first frame. The buffers are zeroed once per backing (a zero generation is
no listing's). Nothing in any epoch reads what the pass writes, so nothing waits for it, and the readers do not declare the
buffers: the one they read was finished a frame ago, and the host's boundary orders the writer against both frames. The
per-epoch clock and record passes in the shadow, Z-prepass and colour epochs are gone, and the records no longer take GPU
writes (the uploads used to put back what those passes wrote, which is why they ran in every epoch).

The tree wind parity check (`CS_DCLF_PERSISTENT_PARITY`, now reading the wind buffer) reports 0 of 64 differing, as before.

**Result** (Save5). ORG's pass timestamps: shadow epoch 1.799 to 1.792 ms, Z-prepass 0.542 to 0.537, colour 1.197 to 1.181;
the GPU frame 12.446 to 12.425 ms with DCLF on (17 blocks each), inside the noise. The per-epoch tree wind passes were 7 us
each in ORG's own timestamps: nvperf's 13-15 us per epoch were mostly its drains.

**What this says about the rest.** In ORG's timestamps DCLF's upkeep is small: fade state 30 us, the builds and sorts
10-20 us each, the HZB 42-50 us. The epochs are their draw passes (Z-prepass 0.36, colour 1.14, shadow views 1.71-1.77 ms).

### Fade state a frame ahead (done)

`FadeStateCS` runs on the compute queue in the Z-prepass epoch, like tree wind. Its state rows (`fadeStates`) are its own
alone, so a frame without the pass never rolls a root back; after each root's update it publishes the root's state into
the frame's output of two (`SceneBuffers::fadeStatesOut`, by the scene frame's parity), every root every run. The builds
that read the state (the Z-prepass's phase 1, an owned root's members; the shadow views, a stood-in root's casters) read
the other output, the frame before's: its descriptor depends on the frame, so it is the latch's
(`BuildDrawsLatch::fadeStatesIndex`, at +240; `FadeStatesReadIndex`), and the builds no longer declare the states. Both
already took a state only when its generation was the root's, so a zeroed output (a new backing, zeroed by whichever epoch
commits first: `ZeroFrameAheadOutputs`) reads as the root's listed state. The verdicts lag a frame: an owned root's members
follow its OnVisible of the frame before.

**ORG: two reads of a buffer need no order.** The first measurement lost 0.09 ms in the depth segment (1.068 against 0.976):
fade state reads the object records, and so do the Z-prepass's build and draws, and ORG's scheduler chained every pair of
consecutive users of a resource, reads included ("deliberately serializes all uses... until state planning can prove
which reads may overlap"), so the graphics work waited for the compute queue. For a buffer it is provable: a buffer has no
layout, and a graph buffer is concurrent, so the cross-queue barrier pair is dropped and two reads have nothing between
them. The scheduler and its validation now order each use after the resource's last write, and a write after every use
since (`ExperimentalGraphCompiler.cpp`, `OrderUse`); a texture's uses stay serialized. Validation stays clean.

**Result** (Save5, 17 blocks DCLF on): depth segment 0.952 ms (0.976 with tree wind alone, 0.986 before either), the
Z-prepass event 0.591 (0.612, 0.609); ORG's Z-prepass span 0.508 ms (0.537, 0.542). Fade parity (`CS_DCLF_FADE_PARITY`):
0 differ against the port and the engine's node; no holes.

### The draw passes against the native draws they replace (investigation)

Save5, nvperf, three captures a run, one run with DCLF on and one with it off from frame 2000 (`CS_DCLF_TEST_TOGGLE`).

**The draws are not slower.** Measured against the native batches they replace, each counted directly with DCLF off:

| | DCLF | Native batches replaced |
| --- | --- | --- |
| Colour (`cs.dclf.main-opaque` against `FinishAccumulatingDispatch [0] <65>`'s opaque share) | 1132 us, 2987 draws | about 1206 us |
| Z-prepass (`cs.dclf.z.depth` against `RenderBatches (2031)`, `(2046)`, `(20C6)`, `(40020C6)`) | 364 us, 1936 draws | 387 us, about 1610 draws |
| Shadow maps (GPU event timers) | 2.02 ms | 2.08 ms |

The Z-prepass's fragment work is the native pass's: its alpha-tested draws launch 444 K pixel-shader warps and 10.9 M
late-Z samples, the native `20C6` and `40020C6` batches 438 K and 10.8 M; its opaque draws launch none (the driver drops the
depth variant's empty pixel stage, as the native pass has none). An earlier reading of 1.7 times the native warps came from
subtracting the DCLF-on residual from the DCLF-off pass: 8 grass batches (`5C00005C`, 228 K warps, 6.8 M late-Z samples) stay
native and do not cost the same in both. Neither drawing the opaque pipelines first (a depth-class sort key) nor the
alpha-tested draws front to back changed the counters beyond 10%, so neither was kept.

Device-generated commands' own cost, sequences written with no instances (nothing rasterized): `z.depth` 161 us for 3245
sequences, `main-opaque` 318 us for 4291, about 50 and 74 ns a sequence. Raster hides most of it (both passes still beat the
native draws), but it is a fixed cost the plain draws would not have. An empty `depth-phase2` costs 12 us; its usual 45-54 us
is its draws.

**Where the gap is** (GPU event timers, 16 blocks on, 4 off, by the toggle's timestamps):

| Segment | On | Off | Gap |
| --- | --- | --- | --- |
| Depth | 0.953 | 0.653 | +0.30 |
| World | 4.578 | 4.442 | +0.14 |
| Water effects (the reflection cubemap) | 0.657 | 0.577 | +0.08 |
| Shadow maps | 2.02 | 2.08 | -0.06 |

-   **Depth:** the Z-prepass event is 0.58 ms against the native draws' 0.39: DCLF's compute and second phase (HZB 45 us,
    phase-2 draw 50, builds and sorts about 50, phase-2 build 14) and about 0.07 ms between ORG's span of the passes (0.51)
    and the event.
-   **World:** the colour event is 1.26 ms against its pass's 1.13: builds and sorts about 0.05, and the same kind of 0.07 ms
    outside the passes.
-   **The cubemap** (`RenderPersistentPassList <8>`): 0.38-0.49 ms with DCLF on in every run since the first, 0.340-0.343
    off. Its work is the same (nvperf: 354 against 361 us, 190 against 197 draws, the same warps and samples), the GPU is never
    idle (`CS_GPU_IDLE_TRACE`), and it is neither the compute queue (it predates it) nor the early flush (`CS_ORG_EARLY_FLUSH=0`
    leaves it). Not yet explained.

Tools for this: the per-view nvperf ranges (a sub-range per view inside `cs.dclf.shadow.view`, selected by exact name,
since a prefix that also matches the pass takes the pass instead) were a temporary change; the engine's own events
are selectable by name (`BSShaderAccumulator::FinishAccumulatingDispatch [14]*`).

### Plain indirect draws for the Z-prepass (done)

The Z-prepass now draws the way the shadow views do. Its two phases are plain `DrawIndexedIndirectCount` calls, one per
pipeline slot (a bucket), so device-generated commands are left to the colour segment.

-   **Buckets.** The depth segment's builds (phase 1 and phase 2) count each draw into its pipeline slot's bucket
    (`input.y >> 20`). The slot is the draw's instance.
    -   Each phase has its own table in the main latch, and its own counts (`Resources::zBucketCounts[2]`). Phase 2's
        table follows phase 1's sequences (`BuildDrawsLatch::phaseTwoBucketTableOffset`, +244).
    -   `ReserveMainSequences` sizes a bucket for every draw its slot's objects can produce (`SceneDrawBound`, per
        pipeline). The capacity is sticky, at a power of two.
    -   The draws need no order, so the Z-prepass's sort is gone.
-   **The index pool is the scene's.** The pool the shadow views built (`IndexPool`: each geometry's indices copied into
    one R16 buffer) is now shared. Both the shadow commit and the depth commit keep it current (`UpdateIndexPool`). The
    depth segment copies what its commit gave out in `cs.dclf.z.index-pool`.
-   **Vertex pulling in Lighting (`DCLF_PULLED`).** The vertex stage finds its sequence by `SV_InstanceID`. It decodes
    the attributes from the vertex layout, as Utility does: position, UV, normal/bitangent, colour, land blend weights,
    skin weights and the eye parameter.
    -   The material row is read in the shader. `PulledMaterialSource` rewrites the Lighting source when the pulled stages
        are compiled:
        -   the material textures and samplers become descriptor-heap reads through the row (`DCLFMaterialTextureIndex`,
            `DCLFMaterialSamplerIndex`);
        -   the `PerMaterial` members become statics, assigned from the row (`DCLFPerMaterialStatics`).
    -   The pixel stage, which only the alpha-tested pipelines have, gets the draw's words through `DCLFDraw`
        (`TEXCOORD15`).
    -   `viewPos` is `precise` (`DCLF_PRECISE`) in both the pulled build and the colour pass's, so the colour pass's EQUAL
        test holds.
-   **The call.** The push range is the call's first sequence, the pipeline row and the vertex layout
    (`kZDrawPushSequences`, `kZDrawPushPipelineRow`, `kZDrawPushVertexLayout`).
    -   The pulled depth pipeline has no input layout and no material ranges. `CheckPulledBindings` rejects one that would
        need them.
    -   The depth variant's implicit-preprocess signature (`depthPassSignature`) is gone.

**Result** (Save5, the same camera path as above):

| | Before | After |
| --- | --- | --- |
| Depth segment | 0.953 ms | 0.906 ms |
| Z-prepass event | 0.591 ms | 0.526 ms |
| ORG's Z-prepass span | 0.508 ms | 0.46 ms |

There were no holes over 300 frames.

Phase 2 still costs about 45 us. Most of that is probably its fixed cost: about 99 bucket calls, mostly with a count of
zero, each binding its pipeline.

### Z-prepass buckets by depth pipeline (done)

The Z-prepass's buckets are now its depth pipelines' rather than the colour pipeline slots'.

-   **The depth pipeline's key** (`DrawPipelines.cpp`, `ZPipelineKey`) holds the pulled stages' SPIR-V (by hash and size)
    and the depth variant's cull, depth write, compare and bias.
    -   The pixel stage is left out where the depth pixel stage cannot defer the depth test (`FragmentDefersDepth`).
    -   Pipelines are grouped as they are admitted (`DrawPipelines::Update`). A set version carries each pipeline's group and
        each group's pipeline (`IndirectState::zGroups`, `zPipelines`).
-   **Buckets.** The depth commit groups the pipeline slots by their pipeline's group.
    -   Each bucket's range is its slots' ranges together (`Resources::zBucketCapacity`), so the sequences' reserve holds it
        unchanged.
    -   The slots' map to the buckets is in the latch (`BuildDrawsLatch::bucketMapOffset`, +248). BuildDraws reads it in both
        phases.
    -   A slot without a published pipeline maps to none, and its draws are dropped.
-   **Every row input is the draw's own.** A call binds once for many pipeline slots, so the pulled Lighting stages read
    everything a row gives through their sequence (`PulledLightingSource`, generalised from `PulledMaterialSource`):
    -   the PerTechnique (b0), PerMaterial (b1) and PerGeometry (b2) blocks become statics assigned from the rows, including
        arrays and row-major matrices;
    -   the permutation block (b4) is `Permutation.hlsli`'s statics under `DCLF_PULLED_ROWS`;
    -   the shadow mask (t14, s14) is read from the pipeline row;
    -   the vertex layout is in the pipeline row's header (`PipelineRowHeader::vertexLayout`).

    The Z layout has no row ranges, and `CheckPulledBindings` rejects any row binding. A call pushes only its first
    sequence's address. The pixel stage gets both rows' addresses (`DCLFDraw`) and the object word (`DCLFDrawObject`).

**Result** (95 main pipelines): 58 depth pipelines, 16 of them without a pixel stage.

| Auto-runner camera, GPU event timers | Before | After |
| --- | --- | --- |
| ORG's Z-prepass span | 0.421 ms | 0.408 ms |
| `cs.dclf.depth-phase2` | 32 us | 23-24 us |
| `cs.dclf.z.depth` | 0.316 ms | 0.310 ms |

There were no holes.

**Why the groups did not converge.** Every admitted pipeline's key was logged next to its depth key:

-   **Shader debug info** (`CS_DCLF_SHADER_DEBUG=1`, set in `CommunityShaders-DCLF.ini` beside the log) was the main cause.
    Its SPIR-V embeds each stage's defines and source, so modules with identical code still differ by their bytes.
    -   Offline, the depth pixel stages of `00100011` and `00100211` (Specular) compile to identical bytes.
    -   With it off: 35 depth pipelines, the pixel stage 7 modules from 38 descriptors, and `depth-phase2` 15 us.
-   **Decals** had buckets and calls they never filled. The depth segment is not given decals (BuildDrawsCS), but
    `SceneDrawBound` sized a bucket for every object's slot, decals and multi-index layers included. Their slots now get
    no Z-prepass capacity, so no call either: `depth-phase2` 15 to 13 us.
-   **What remains** (non-decal pipelines: 27 depth pipelines, 15 with a pixel stage):
    -   The vertex stage: 9 modules (VC, skinned, model-space normals, projected UV, the TreeAnim technique), different
        code. Merged, there would be 9 groups.
    -   Cull: two-sided or back. With cull merged as well, 7 groups.

## Striped foliage: the pulled Z-prepass's UNORM decode (fixed, 2026-10-02)

**Symptom.** Alpha-tested foliage showed bluish-white stripes on some frames, much like Z-fighting, more often while the camera
or the wind moved. It was not seen with DCLF off.

**Cause.** The pulled Z-prepass's vertex stage decoded the 8-bit UNORM attributes (normal, bitangent, vertex colour) as
`x / 255.0`. That compiles to a reciprocal and a multiply, which is an ulp off the input assembler's correctly rounded value for
126 of the 256 inputs. The colour pass still reads them through the input assembler, and a tree's vertex moves along its
normal, scaled by its vertex colour (`GetTreeShiftVector`). So the two passes' positions differed by a hair, and the colour
pass's EQUAL depth test failed in stripes across the leaves. Those pixels were never shaded and kept what the G-buffer held.

**Fix.** `DCLFUnorm4` (`Common/DCLFObjects.hlsli`) now computes c / 255 correctly rounded: the product by 1/255 corrected by
its fused remainder (`precise` `mad`, which DXC emits as `Fma`). It is exact for all 256 values, and costs nothing measurable
(the Z-prepass draw is 0.186 ms either way). A 256-entry table was just as exact but cost 21 us. The shadow views' pulled
Utility stages use the same decode.

**Result** (the fern scene, live wind): pixels whose depth an alpha-tested Z-prepass fragment owns but the colour pass left
unshaded fell from 240-3,500 per frame (flagged on 136-300 of every 300 frames) to about 0.5 (none flagged).

### The instrument: `CS_DCLF_FOLIAGE_PARITY`

`1` freezes tree wind; `wind` keeps it running.

-   **What the shaders record.**
    -   The colour pass's alpha-tested stages (`DCLF_FOLIAGE_PARITY`, with `[earlydepthstencil]` so only the visible
        fragment writes) record each pixel's object, albedo, diffuse, specular and motion vector. The motion vector also
        carries its error against a static object's.
    -   The Z-prepass's alpha-tested stages record each pixel's owner: the closest fragment their alpha test kept, as
        depth and object, by a 64-bit atomic max.
-   **What the compare pass checks** (`FoliageParityCS.hlsl`, after the colour pass). The counters and samples are read
    back asynchronously per frame slot and reported every 300 frames.
    -   **Within the frame, whatever the camera does:**
        -   owned pixels the colour pass left unshaded, or shaded with another object;
        -   foliage whose motion vector is more than a pixel off a static object's (with the wind frozen);
        -   near-white albedo or diffuse.
    -   **Against the frame before, reprojected through the motion vectors:** colour or brightness jumps over a
        running average.
-   **How it gets per-frame data.** The frame's buffer parity and tag are read on the GPU from the frame-lighting block (PS
    b13, c14), because a pass's prepared constants only follow the frame's shape.

**Readings worth knowing.** With the fix, coverage reads 0 to 1 pixel per frame. Motion vectors are within a pixel apart
from a few dozen pixels per frame. Near white stays at the scene's own baseline. The reprojected comparison is about 1% of
foliage pixels per frame while turning, at canopy edges.

## Junk geometry after cell loads: the index pool's ranges were shared by address (fixed, 2026-10-02)

**Symptom.** After cells loaded, broken geometry sometimes covered parts of the screen without shading. Every existing
parity check passed.

**Cause.** The index pool (`IndexPool`, which the shadow views and, since the plain Z-prepass draws, the depth segment
draw from) shared one range between every geometry slot that named the same index buffer address. An address is no
identity: DXVK gives a freed buffer's address to the next buffer it creates, and a cell's load creates new meshes while
the unloaded cell's slots still hold their ranges. Newly appended slots were also acquired before the log's slots were
released. A new mesh at a reused address therefore took the old mesh's range with no copy, so its draws read the old
mesh's indices, or ran past the range into its neighbour's, against its own vertices.

In the depth segment that is junk depth, and the colour pass's EQUAL test leaves those pixels unshaded. The colour pass
binds the engine's own index buffers, which is why the geometry rendered unshaded. A flight across six cells
(`CS_DCLF_TEST_MOVE='300:5000:5'`) logged 20 aliased slots, all at the cell loads. Most had a different size from the
range they took, for example 32,208 bytes against 17,856.

**Fix.** A range is a slot's own. Every slot the geometry log names gives back its old range, and only after all of
those are released are new ranges given out and copied. A regrow copies each slot's range from the buffer recorded for
it. Through the same flight the pool holds 4.6M to 5.2M of its 8.4M indices, without growing.

**Check.** `CS_DCLF_BUILD_PARITY` now also compares every 300th pool update against the tables: each slot's range against
the buffer address and size its geometry row has now. Its verdict is `index pool parity: ... <- OK`, and a slot whose
buffer changed without a log entry reads `<- INDEX POOL`.

**Open.** BuildDraws parity reports MISMATCH on every depth-segment check. Its CPU templates predate the plain Z-prepass
draws: those draws carry pipeline 0, the pool's first index and the bucket slot as their instance. The check is blind for
that segment until the templates learn the pool's firsts.

## Shadows flickering in motion: the colour pass's sun test lost its cascades (fixed, 2026-10-03)

**Symptom.** With DCLF on, shadows flickered while the camera moved and settled when it stopped. With only DCLF's shadow
views off, the lighting was wrong outright: walls lit differently from both full DCLF and DCLF off.

**Tracing.** Each stage got a check, run on a flight that moves, turns, and then stands still:
- the claims (casters withheld from the engine that DCLF's epoch did not draw): none;
- each view's captured b12 (its eye against the renderer's): consistent;
- each sun view's culling against the engine's own cascade culls of the same frame: consistent, apart from the residual
  below;
- the engine's shadow mask, frame to frame (`CS_DCLF_SHADOWMASK_PROBE=flicker`): the same with DCLF on and off.

So DCLF's shadow maps and the mask were right, and the defect was in the colour pass.

**Cause.** The colour pass's sun test (BuildDrawsCS `InSunCascades`) clears `ShadowDir` on a pass whose bound meets no
cascade (`kObjectSunMiss`). It reads the frame's cascades from `SunAccumulation::GpuCascades`, but those were captured,
and returned, only on frames whose full-frustum cull applied the sun entry exclusion.

The exclusion is skipped:
- on a stale frame, when the sun candidates changed since it was built, which happens all the time in motion (13 to 41
  frames in 300 on a flight);
- with DCLF's shadow views off.

The colour commit still set `kSunTestOn` on those frames. With zero cascades, every pass with the sun's bits drew
unshadowed. In motion that is single frames of full sunlight. With the shadow views off it is every frame.

**Fix.** The cascades are captured on every frame the sun accumulates, independent of the exclusion; only the bits path
(`ApplySunBits`) still needs the exclusion. The capture is stamped with its scene frame, so a frame where the sun did not
accumulate gives no cascades, as the engine then gives no pass the sun's bits. The colour commit reports
`colour sun test: N of 300 frames without the sun's cascades`. It reads 0 through stale exclusions, and 0 with the shadow
views off.

**Residual.** About 0.2% of the casters the engine's cascade culls reach are rejected by DCLF's view test (the cascade
culling parity under `CS_DCLF_SET_PARITY`, for example 'Spout', 'BlacksmithForge01:0', mountain pieces). Those are drawn
by nobody for that frame. Not yet explained.

**New instruments:**
- shadow claim holes (always on with static shadow ownership);
- the shadow view capture check;
- cascade culling parity (`CS_DCLF_SET_PARITY`);
- per-reason shadow culling counters;
- `CS_DCLF_SHADOWMASK_PROBE=flicker`;
- the shadow map probe's probe-to-probe diff.

## Black terrain: trees gusted by a field the engine never reads (fixed, 2026-10-03)

**Symptom.** In one area, with DCLF on, everything was in shadow; looking at the ground, the terrain was completely black.
With DCLF off the area was lit.

**Tracing.** The checks below ran on a save at that spot, with DCLF toggled off and on:
- Set parity was clean (every depth draw had its colour draw), and the G-buffer barely differed.
- The engine's shadow mask read 00 (shadowed) against FF.
- DCLF's two sun cascades were covered almost entirely at the light's near plane: means 0.0049 and 0.0031 with 0% of
  texels clear, against the engine's 0.12 and 0.32.
- `CS_DCLF_TREES=0` restored the maps, and so did zeroing `TreeParams.z` in Utility's pulled vertex stage (a temporary
  bisect). So a tree's amplitude was flinging its leaves across the cascades.
- A scan of both tree wind buffers found 8 entries with amplitudes of about 1e36, against the engine's 1 for the same
  nodes.

**Cause.** `TreeStaticOf` marked every tree node with a model as animated, and `TreeWindCS` then scaled the gust by the
model's `+0xB0`. The engine's manager advances a `BSTreeNode` only in loop 2 of `FUN_1404381e0`, which runs over the near
list that `FUN_140437e50` builds every frame. A node gets onto that list:
- within the manager's range (`+0x80`);
- only if it passes `FUN_14147d640(node)`'s check: that function's result, then `+0x128`, then `+0x18`, then an int at
  `+0x10` equal to 1;
- sorted nearest first and capped;

and loop 2 then gusts only nodes whose model has bones (`+0xB8`).

Loop 1, the one the earlier doc section describes as scaling by `+0xB0`, walks the animation list (`+0x50`), which holds
`BSLeafAnimNode`s, not trees. For these trees' model, `+0xB0` reads 2.29e36. The engine never reads it, so their leaves
stayed put natively.

**Fix.** A tree is animated (its clock advances, and its gust is scaled by `+0xB0`) only when its model has bones and the
near list's check selects it. The check is evaluated when the tree is listed, by calling `FUN_14147d640`.

Not replicated: the near list's cap on its count. More selectable trees within range than the cap would all be gusted by
DCLF, where the engine gusts only the nearest. The earlier claim that every member tree advances is superseded for
`BSTreeNode`s.

**Check.** The tree wind readback now also scans both wind buffers whole, every 120th epoch, for every listed tree:
- non-finite values, or amplitudes over 1000, as bad entries;
- generations that don't match their listing;
- the largest amplitude, against the listing's, the engine's live `+0x15C` and the engine's near list.

It reads `0 bad, largest amplitude 4.3` at the spot.

**Also new:** a cascade cover check on sun views (`CS_DCLF_SET_PARITY`). It logs drawn casters whose bound covers over a
quarter of the map at the near plane, objects whose world matrix is far from their bound, and draws whose geometry row
no longer matches their TriShape's buffers. It found 'leaves' (a near canopy, legitimately covering 30% of cascade 0)
and four decals with their world origin off their bound (DirtDecal, harmless). It found no stale buffers.

## Dark terrain: TruePBR's landscape layers bound by the previous draw (fixed in TruePBR, 2026-10-03)

**Symptom.** At one spot the terrain was much darker with DCLF on than off, and with DCLF off one terrain quadrant still
looked different from its neighbours.

**Tracing.** The target probe (`CS_DCLF_TARGET_PROBE`) reads the targets at four stages:
1. before DCLF's colour epoch;
2. after it;
3. after Terrain Blending's replayed landscape passes;
4. after the deferred composite.

The probe found:
- The G-buffer matched after DCLF's epoch, and so did the shadow mask.
- The difference first appeared after Terrain Blending's replay of the engine's own landscape passes. With DCLF on, rt0
  was 0.71 and the glossiness (rt2.z) was 0; with DCLF off they were 0.78 and 0.21.
- `CS_DCLF_PRIMARY_EXCLUDE=0` made the two agree, so taking DCLF's objects out of the engine's main cull changed those
  native draws.
- A per-pass probe of the replay (`ProbeTerrainPassState`) recorded each pass's state: its textures, PS b1 and b2, VS b2,
  and the pipeline state. Per material everything matched except TruePBR's per-tile slots: displacement t80-t85 and
  RMAOS t86-t91.

**Cause.** TruePBR's landscape `SetupMaterial` bound a tile's textures only when the material's pointer was non-null.
Its extended slots are flushed by dirty bit, so a skipped tile kept whatever the previous landscape draw had bound:
null, or the default white RMAOS. The result therefore depended on the order the passes were drawn in.
- DCLF's stand-in walk (`PrimaryCull::StandIn`) registers the engine's members in its own order.
- The native order varies from frame to frame.
- White RMAOS (rough and metallic) is dark; null reads as zero.

The pointers were null because the 11 vanilla landscape texture sets that TruePBR's configs mark as PBR have empty
displacement and RMAOS paths in the loaded records (LandscapeFieldGrass01, LandscapeTundra01, and others). The likely
cause is a plugin later in the load order overriding the PBR mod's texture-set records.

**Fix.** The landscape `SetupMaterial` binds every tile's four slots on every call. A missing texture gets the default
the material would otherwise have been given (`ReceiveValuesFromRootMaterial`): black base colour and displacement, the
default normal map, and white RMAOS. DCLF on and off now agree (rt0 0.72, glossiness 0).

They agree on the dark look: that is what TruePBR draws for a PBR tile with no RMAOS. The look is right only once the
texture sets carry their RMAOS again, which is a load-order matter.

## The pre-existing parity verdicts, under the camera traversal (2026-10-03)

The runs use the camera traversal (`CS_DCLF_TEST_MOVE=300:1000000:40`, `CS_DCLF_TEST_TURN=300:1000000:0.2`) with every
parity check on. Rotation alone reaches few of these.

**MISSING (change log parity): `lodFade` counted as shading.** `Tables::CausesBetween` put the LOD fade node row under both
placement and shading. Every writer of that row (the placement takers, `TakeRoot`) notes it as placement. The log was
right and the diff wasn't: `lodFade` is now placement only, as `ChangeCause`'s comment says.

**TREE WIND at startup: the check read the buffer being written.** Wind is computed a frame ahead on the compute queue
(`TreeWindCS` writes `treeWindRows[frame & 1]`), and the draws read the other buffer (`TreeWindReadIndex`). The check
copied this frame's buffer, which nothing orders after the compute write. It now reads the buffer the draws read, against
the inputs of the frame that wrote it (`previousTreeInputs`). It skips a frame where no earlier frame ran the pass, and one
where the tree rows changed since. What remains is inherent to the frame-ahead design:
- the first frame's draws read the zeroed buffer, so trees are still for one frame;
- a tree whose row changes keeps the old wind for one frame.

**ENGINE FADE: two engine paths FadeStateCS did not model.**
- *A visibility block per list process.* Each scene list's culling process has its own view planes and compound frustum
  (set up from its list's first entry). FadeStateCS tested every root against one sampled process.
  - `PrimaryCull` now samples one block per list job slot (`kFadeVisibilityLists`), on each job's first stand-in call.
  - It records each entry's slot (`Cut::entrySlot`).
  - FadeStateCS reads each root's block through `fadeRootLists`, built by `PrimaryCull::FadeRootLists` and uploaded when
    the roots or a slot change.
  - A CPU port of the shader's test (`VisibilityPort`), run on the stand-in's engine-culled entries, agrees with the
    engine every time (the "fade visibility parity" line).
- *The animation job's fade update.* `UpdateAnimationJob` calls `FUN_1402cff60`, which runs the fade update
  (`FUN_14147a160`) from the world root camera when an animated reference's fade root wasn't reached by the cull. That
  covers doors, traps, puzzle pillars and ambient effects. It runs beside the render thread, between one cull and the
  next, sometimes twice per root.
  - `Hooks::AnimatedFade` (the call at `0x1402d0044`) records the nodes and the inputs (camera, lodAdjust, fade counter,
    frame time).
  - At the list jobs' end the batch is complete. The depth commit stamps its roots in `fadeAnimated` with the frame and
    the update count (`FadeAnimatedWord`), and puts the inputs in `FadeFrame::anim*`.
  - FadeStateCS applies `FadeUpdate` that many times, with those inputs, before its cull test (`kFadeVerdictAnimated`).
  - The port does the same (`FadeState::AnimatedUpdate`).
  - The engine-node check now reads the nodes as the list jobs left them (`PrimaryCull::NodeSnapshot`), not at the depth
    commit, where they race with the animation job.

  What remains: an animated root's update landing on the other side of the list jobs, so one update more or fewer, or
  `lastVisible` off by one. That is the engine's own race between its animation job and its cull.

**CASCADE CULL: casters outside the cascade's viewport.** The rejections were all by the view's clip box on x or y: the
bound's box lies wholly past one side of the view's own projection. The engine's looser sphere cull registers such a
caster, but its draw writes no texel either. They are now counted as "outside the view's viewport", not as a verdict.
Rejections by the cull planes or the near plane stay a verdict.

**CLAIM HOLES: claims the engine did not register.** The check counted every withheld claim this epoch didn't draw, even
one the engine would not have registered this frame (an LOD child no longer selected, a reference unloaded). Under
`CS_DCLF_SET_PARITY`, `PassCapture::TakeWithheld` logs what the shadow registrations actually withheld, and only those
count as holes. The rest are "left the claims unregistered". What remains: one frame in a few minutes with a single
caster.

**LIGHT EXCLUSION: a fade that completes between the build and the cull.** A harvestable mushroom stump's shape
(`ReachTreeStump01_Mush:4`) was registered by a paraboloid light, unwithheld, under an excluded entry. The probe
(`LocalLightCull`'s lost-pass line: the exclusion's build frame, whether it was reused, and each candidate geometry of the
entry with its object, flags and claim) showed a fresh build from the previous frame, with the shape unclaimed and
`kObjectNoShadow` for `ShadowReject::Faded`: the stump was fading in. The exclusion is used a frame after it is built, and
the point lights' culls update the fade themselves (`BSFadeNode::OnVisible`), so a fade that completes in between is cast
by the engine while the entry is skipped. `BuildSunExclusion` now counts a faded object as an engine caster: an entry
stays in the culls while anything under it fades, for the sun's exclusion as well. The switch node was ruled out on the
way (the shape was the selected, current child; no index store was dropped off the render thread).

**MISSED: a controller write after the drain.** The candle's emissive multiplier is written by
`BSLightingShaderPropertyFloatController`, whose write is an event (`MaterialSources`). The animation job runs the
controllers alongside the render thread, so a write could land between `RefreshFrameConstants`' drain and the parity
compare right after it, with its event queued for the next frame. The compare now drains the queues again afterwards: a
changed slot those late events cover is resampled and counted as "changed after the events were taken (queued)", and only
the rest are missed.

**A crash in DXVK's allocator.** `DxvkResourceAllocationPool::alloc` read a corrupted free list during an engine
`CreateBuffer` on the loading thread (once in about ten runs). The pool is shared by every allocation and free under the
allocator's `m_mutex`, but `importBufferResource`, `importImageResource` and the sparse `createAllocation` popped it
unlocked (upstream DXVK, where imports are rare: 11on12 and the presenter). The ORG interop's
`dxvkCreateBufferFromVkBuffer` imports on the render thread (`RenderGraphRuntime::WrapBuffer`: DCLF's buffer growth, and
the parity diagnostics' readbacks every check) while the loader creates buffers, so two threads could pop the same node.
All three now take `m_mutex` (`extern/dxvk/src/dxvk/dxvk_memory.cpp`). Details, and how upstream DXVK can hit it:
[dxvk-allocation-pool-race.md](dxvk-allocation-pool-race.md).

Traversal run trav30 after the three: 750 OK, 4 ENGINE FADE (the engine's own race above), no crash.
