# Drawcall Limit Fix: open defects

This file lists Drawcall Limit Fix's open defects, failing checks, open crashes and accepted differences. Each entry says how
to reproduce it or which check shows it. How DCLF works is in [dclf-architecture.md](./dclf-architecture.md). The switches
and report lines named here are described in [dclf-validation.md](./dclf-validation.md).

## Fixes built but not confirmed in game

The fixes below are in the code. No recorded run has yet shown the check that confirms each one.

-   **Newly loaded objects drawn opaque, then vanishing, then fading in.** `DCLFAlphaFade` fell back to the root row's `Initial` (the
    mirror's fade at listing) until FadeStateCS's state reached t120. The seed (the node at the frame's start) was used only by
    BuildDraws. The pairing of listings with seeds counted 144 generations listed at 1 but seeded at 0 (opaque, then gone, then fading
    in), and 511 listed at 0 but seeded at 1 (a visible object missing for a frame). The fix: the seeds are a frame binding at t117
    (`kFadeSeedsRegister`, answered with the pool's descriptor on the render thread: `FrameBindingSelect::index`), and the pixel stage
    falls back to the seed as `SeedState` does. **Check:** walk into a forest with DCLF; new full trees should fade in like the
    engine's, with no opaque first frame.
-   **Stood-in animated references that never fade freeze.** `SyncRootOwnership` (`Core/Handoff/PrimaryCull.cpp`) clears
    kAccumulated (bit 26) on stood-in roots. The engine's animation gate (`FUN_1402cff60`, which `UpdateAnimationJob` and the
    managed-node update call on `ref->Get3D()`) then stops updating roots that never fade: windmill sails, water wheels, flags,
    Dwemer machinery. The fix: DCLF writes kAccumulated from its GPU visibility at `Main::Update`'s entry, and the gate skips the
    roots whose fade DCLF writes. **Check:** `CS_DCLF_ACCUMULATED_PARITY=1`. `<- ANIM RATE` should show 0 differ, and never-fading
    stood-in calls should be rejected only when written out of view. Run vp10 (tree-forest route) showed 0 differ, but no never-fading
    stood-in root there (0 calls), nor at Riverwood (run rw1, 0 never-fading calls): no stood-in root that never fades was found
    yet, so the never-fading half is unverified.
-   **Stood-in BSTreeNode trees on the tree manager's tree list.** The tree list's loop (`FUN_140437e50`, list `+0x20`) visits
    a tree by its first child's multibound cull result, which no cull refreshes for a stood-in root. A stale "in" animates the
    tree every frame, and a stale "out" freezes it (`skyrim-engine-notes.md`, "Tree manager list (T7b5)"). The fix:
    `TreeAnimation` writes each stood-in tree's multibound result from DCLF's verdict around the loop's call, then restores the
    engine's value. **Check:** in the `tree animation list` line, "loops given DCLF's verdicts" > 0 where stood-in BSTreeNodes
    exist. **Blocked:** no `BSTreeNode` was found at Riverwood, Falkreath, Riften, Raven Rock or Morthal in the test load
    order. `CS_DCLF_TREE_LIST_ALL` stays off until a load order with such trees shows the comparison clean.

## Visible lighting defects

-   **Statics have no room for the first report after a load (`light data parity MISMATCH`, `DCLF room -1`).** Every run's first
    window (up to frame 300) has 80-920 draws of statics with DCLF room -1 against the engine's room (`WRIntCastleFreePillar01:4`,
    `WRCastleRugBEndL:1`): Light Limit Fix's first room map is posted after those objects joined. With room -1 a portal-strict light
    is not gated for them, so it can light them through walls for those frames. **Next:** check whether the first map's post
    (`FrameView::PostAncestorMap`) can precede the first joins, or whether `RefreshAncestorValues` misses objects joined before it.

## Open crashes

-   **A freed child detached during cell streaming.** It happens intermittently, 70-100 s into motion runs (`CS_DCLF_TEST_MOVE`,
    cells streaming). It is an access violation on an engine job thread with this stack: `DCLF::DetachChild2At<0>::thunk+0x65`,
    then the engine's detach (`SkyrimSE+0xd1d32e`), whose `call qword ptr [rax+1B8h]` goes through the child's vtable. The vtable
    reads as float data (`3f8000003f800000`), so the child is already freed when the thunk's original call runs. Reruns of the same
    build do not crash. Two dumps are kept in the scratch folder (`refactor2/v3-motion.dmp`, `refactor2/fdt2-motion.dmp`). **Next:**
    check whether DCLF releases a reference to a child that the engine detaches later (engine references are released only through
    `EngineReleases` at Present), and whether the thunk reads the child after the original call. The thunk itself only notes the
    detach and passes the child through, so the caller held a dangling pointer. **Not reproduced** in 120 s motion parity runs on
    the tree-forest route (vp11-vp13, 2026-10-22): set aside until it recurs.
-   **Vertex-stage faults at load.** Device losses a few seconds after a load (or at the main menu, DCLF idle), each Aftermath
    "Shader Instruction Error" at PC 0 of `vertex_01`, a 5 KB vertex shader with hash `0xfb2df49047e00dfa`, 84 warps active. The
    same hash appears in every dump since 2026-10-20 (02:58, 19:58, 20:00 and 21:50 on 10-21), across builds, so it is one
    shader. They are not tied to a switch: the three at 19:58-20:00 had `CS_DCLF_PERSISTENT_PARITY` with `CS_DCLF_TREE_LOD_AUDIT`
    (the audit is CPU-only), the 21:50 one persistent parity alone. The runner retries a launch that never reaches gameplay, which
    hid them: 7 of about 250 launches retried, not all with a dump. No shader debug info was written, so the shader is unnamed.
    One loss with this signature was the index pool's unfilled grown `pool-firsts`, now fixed. **Next:** a soak of launches with
    `DXVK_AFTERMATH_SHADER_DEBUG_INFO=1` (the decoder then maps the shader; `extern/dxvk`'s adapter warns that the heavier
    diagnostics configuration itself produced load-time faults on this driver, so add the flags one at a time).
-   **Use-after-free in actor teardown on a save load.** It happened when a save was loaded after going through a door. A job
    thread in `Actor::Predestroy` -> `FUN_140720830` (middle-high process data, `+0x278`) -> `FUN_14021ca60`/`FUN_14021d140`
    destroyed a pool entry's object (keyed by a `BSLightingShaderProperty`) that was already freed. DCLF holds property and
    material references, but nothing ties the crash to them yet. **Next:** reproduce with `CS_DCLF_RELEASE_GUARD=1`, and with DCLF
    off.
    **A second form, 2026-10-22 (run ex1, 75 s into a motion parity run out of Riverwood):** an actor's skin destroyed on the main
    thread under `Main::Update`: `BSDismemberSkinInstance::~NiRefObject` (`0x140d32470`) -> `NiSkinPartition::~NiRefObject`
    (`0x140d44640`) -> `FUN_140e46810` (its buffers' release) -> Engine Fixes' allocator, an atomic write through a null pointer
    (`xchg [rsi+10h]`, rsi 0). Run wr4's corrupt-heap crash (below, "probably unrelated") was also under an actor's geometry
    (a face morph blend). If DCLF keeps a skin partition's buffers or the partition past the engine's release (Features/Skins), this is
    where it would show. Dump: `ex1-skin-teardown.dmp` (session scratch folder).
-   **A hang 27 s into a motion parity run, after the shadow epoch's builds became one dispatch.** It happened once, on 2026-10-21,
    in the first run of the multi-view shadow build. That build is `ShadowViews` in `BuildDrawsCS.hlsl`: its view table is published
    by `org::PreparedTablePublisher`, `cs.dclf.shadow.build-views`. The log stops with ordinary report lines, about a second after
    the table was published at 4, then 8, then 28 rows. The game then stayed alive until the run's timeout. There was no device loss
    and no Aftermath dump, so it was a wait that never completed: a CPU deadlock, or a submission waiting on a signal that never came.
    The rerun did not hang: 87 s, with the 28-row table published 32 times. **Next:** if it recurs, the scratch folder's
    `hangwatch.sh` writes a full dump when the log stalls for 15 s. Read the threads' stacks with cdb. Suspects are the publisher's
    mutex during preparation, and a kept shadow recording whose table was republished.

-   **A null list in a geometry's render setup, `SkyrimSE+0x14fceb4`.** Once, on 2026-10-22, 85 s into a 120 s motion parity run
    (vp15, frame 10818), on the render thread: `Main::RenderPlayerView` -> `FUN_1414a1540` -> `BSGeometry::sub` (0x1414a2fc0), which
    passes `*(x + 0x70)` (null) to `FUN_1414fcea0`, which reads `[rcx+18h]`. A build's deploy into the MO2 mod folder overlapped the
    run (it finished 90 s after the crash), so the run is suspect. The dump is kept as `vp15.dmp` in the session scratch folder.
    **Next:** watch for a repeat in runs with no deploy during them.

### Crashes probably unrelated to DCLF

These crashes have no DCLF or Community Shaders frames on the stack and match known engine instability. They are recorded so
that a repeat is recognised. They are not chased unless one recurs with a DCLF or CS frame on the stack.

-   **tbbmalloc read in an NPC's behaviour graph update** (uGridsToLoad 5, moving near Riverwood): `EXCEPTION_ACCESS_VIOLATION` in
    `rml::internal::MemoryPool::getFromLLOCache` (EngineFixes' tbbmalloc) under `BSSpeedSamplerModifier` / `hkbBehaviorGraph`
    "0_Master.hkb". This is a known engine bug.
-   **A combat group's null target array, `SkyrimSE+0x804396`** (three times: 2026-10-21 twice, 2026-10-22 once in run vp19 at frame
    7240, all under `CS_DCLF_TEST_MOVE` on the tree-forest route, about one run in ten there). `GarbageCollector::Add` ->
    `Actor::StopCombat` -> `FUN_140558500` -> `FUN_140804330`, which removes the actor from a combat group's targets
    (`+0x20` data, `+0x30` count): the count is non-zero and the data null (`cmp [r8+rcx*4], ebx`, r8 = 0). An engine job thread,
    no Community Shaders or DCLF frame; the route's wildlife enters combat as the player walks into the forest. Dumps:
    `vp1-804396.dmp`, `vp19-804396.dmp` (session scratch folder). Not chased; a run with DCLF off (`CS_DCLF_TEST_TOGGLE`) would
    settle it if it ever matters.
-   **A corrupt heap in Engine Fixes' allocator under a face morph blend** (once, 2026-10-22, run wr4 at frame 12400, walking out
    of Riverwood under `CS_DCLF_TEST_MOVE`): a write through a bad pointer (`rcx = 0xb5410000`) inside EngineFixes' allocator, from
    `MemoryManager::Allocate` in `FUN_140e47ef0` <- `FUN_140e24ef0` (it blends two geometries' morph data, `+0x130`) <-
    `Actor::sub` (`0x1402e8510`), an engine thread. No Community Shaders or DCLF frame, but the heap was already corrupt, so
    whoever corrupted it is unknown. Dump: `wr4-enginefixes-heap.dmp` (session scratch folder). **Next:** if it recurs, a run with
    page heap or DCLF off would separate the two.
-   **Havok null write in `FUN_140b660c0` at uGridsToLoad 21** (the Whiterun stress save under `CS_DCLF_TEST_MOVE`). Radius 21 is
    unstable on its own, so runs there are judged by the report lines written before the crash.

## Frames with DCLF's objects missing under standing claims

-   **A resolution or upscaler change leaves one colour pass undrawn.** After a graph build or a main-resource change, the frame
    waits at its start for a current snapshot (`IndirectDraws::AdoptCurrentSnapshot`). The main resources, though, are made at the
    colour epoch's `Setup` (`Core/Render/Epochs/MainEpochs.cpp`), so a change is only seen mid-frame. That frame's colour epoch is
    not submitted (logged `main resources made at frame N's colour pass ...`) while its claims stand, so nothing draws DCLF's
    objects for one frame. A resolution change can cost two waits, because the views rebuild their imports at
    `FrameStartBeforeCoverage`. **Fix direction:** detect the change at the frame's start, before adoption (from the render-target
    state the colour capture reads, or the upscaler's render size), so that the frame builds and waits like any other build. The
    alternative is to record the frame's colour epoch synchronously for the new resources.
-   **A target format change: no DCLF epoch until the rebuilt pipeline catalog is installed.** After `SetTargetFormats`,
    `FrameIndirectState()` stays invalid until a publication carrying the new catalog is installed. Until then, `RunEpoch` returns
    before `Setup` (`indirect state invalid`) while the claims stand and the snapshots look current, so DCLF's objects can be
    missing for those frames. Waiting on the catalog at the frame's start could exceed the 5 s bound while the pipelines compile.
    This needs its own design: for example, the old catalog keeps drawing into compatible formats until the new one is installed.

## Payload and table defects

-   **An entry write the incremental visibility pass skips.** Under motion, `CS_DCLF_PERSISTENT_PARITY` reports
    `<- WRITES DIFFER; first: root N set wide (reference's alone)` in some windows. It reproduces every time with
    `CS_DCLF_TEST_MOVE` and `CS_DCLF_PERSISTENT_PARITY` set. The suspect is `JudgeEntry`'s skip (`Core/Render/Readback/Visibility.cpp`).
    It skips when `entryWritten` already holds the wanted value and `entryWrittenVersion` is unchanged, so a write made while the
    entry was in another state can hide a later one.
    **No longer verifiable:** 0 in ten moving full parity runs on the tree-forest route (vp1-vp10, 2026-10-21), with the check active.
-   **A material row is keyed per material but packed per pair (latent).** `MainRows::materials[m]` is one row per material slot,
    packed against the pair's pipeline. Two shader permutations of one pass, or a `ModifyShaderLookup` that changed between them,
    could differ in their `PerMaterial` offsets. The row would then keep the last pair's packing, and `ResolvePair`'s
    `material.tables != blocks.tables` guard cannot fire. There is no evidence that it occurs. `Policy::CheckRowPacking` guards it
    (`<- ROW PACKING CONFLICT` in the `[DCLF] main pairs` line, under `CS_DCLF_PERSISTENT_PARITY` or `CS_DCLF_BUILD_PARITY`). If it
    fires: key rows per (material, packing).
-   **No readback check of the material row bytes.** BuildDraws parity checks the rows' addresses only. Nothing reads back a
    backing's material rows and compares them with the kept rows, which would catch a part that the four-part journal
    (`kMaterialRowParts`) failed to send. The extras have this check per backing; the rows do not.
-   **The builder stalls if the coordinator's pump fails for good.** A payload is published only after the scene pass of the frame
    that took the last snapshot has ended (`IndirectDraws::NoteScenePassDone`). If the pump stops for good (a thrown pass that never
    runs again), the builder never publishes again, every frame keeps the last installed snapshot, and nothing reports it.
-   **Material transform moves: nothing drains the queue without the kept shadow build.** `TransformMoves`
    (`Core/Shading/MaterialWrites.cpp`, drained by `Extensions::DrainTransformMoves`) has one consumer, the kept shadow build's
    `WatchTransformMoves`. In a configuration that never runs the kept build, the EventQueue spills when it fills.
-   **Texture transforms reach epochs before the Z-prepass a frame late.** `cs.dclf.z.texture-transforms` runs first in the
    Z-prepass epoch. An epoch that reads the main material rows before it (the reflection, Skylighting's occlusion) draws with the
    previous frame's `TexcoordOffset`, effect falloff and base colour. No GPU readback checks the patched bytes. Comments in
    `Core/Scene/Model/Tables.h` and `Core/Shading/MaterialPortFrame.h` still name the removed `RefreshMaterialTransforms`.

## LOD gates

-   **A flipped incoming geometry missing from its flip's publication.** In one of four runs with `CS_DCLF_TEST_MOVE`
    and `CS_DCLF_SET_PARITY` set, `TakeDeltas`' check reported
    `1 flipped incoming geometries not claimed by the flip's own publication ... <- GATE HOLE (publication)`. If real, the incoming
    block is not drawn while the outgoing one is already hidden, for the frames that install that publication. Run vl1 (the validation
    layer on, so about 300 slow frames of gameplay after the load) reported 85 in its one window, right after the load: 96 releases
    there came before DCLF drew at all and 14 flips were deferred for an incoming record not yet published. The release-side check
    (the claims the releasing frame installed, the one that shows on screen) read 0 incoming not claimed of the 19 releases it
    checked. So the publication-side count includes flips whose incoming records a later publication claims before the release.
    **Next:** count publication holes apart for flips that were deferred, and judge by the release check.
-   **A hold's known effects.** During a hold, both terrain levels' LOD water registers, and a held block's segments are not
    updated. The map's hide-all is handled by flushing. These effects have not been measured visually.

## Failing or flaky parity checks

-   **Set parity: freed object slots stamped drawn by the GPU (`<- SET PARITY`).** In 3 of 8 moving full parity runs on the
    tree-forest route (vp3, vp6, vp7), always in two consecutive windows around frames 3360-3700 while cells stream in front of
    the player: 5-18 "depth without colour", 8-23 "colour without depth" and 14-62 "drawn outside the set" a window. Every sample
    is a slot with no geometry in the snapshot's tables (`'?'`, not bound, both builds `absent`), and the slots come in pairs
    (14988/14989, 15052/15053, 15116/15117). BuildDraws writes a visibility word only from an input's `objectIndex`, so the GPU's
    input list named slots the CPU's builds did not. The payload's tables are the snapshot's own (checked in vp7), so it is not a
    payload built from newer tables. If the GPU's inputs are stale (a region upload or journal gap), it draws freed objects'
    records for those frames. Since then (2026-10-22, 120 s runs vp11-vp18) it showed in 4 of 8 runs, now one slot a window at
    most, the same way. Established, each by a TEMP parity-only diagnostic still in place:
    -   no CPU list names the slot (`listed by 0x0`: neither segment's resident region nor input list; `SetParity.cpp`);
    -   the installed publication's inputs backing, copied after the colour epoch with the words, does not hold it anywhere in
        its 16,384 entries (vp17, slot 174, frame 4997: `name it at: none`);
    -   the commit pairs the payload with the publication made from its body (`CommitMainPayload`: 0 mismatches), the inputs
        backings' catch-ups match the whole list (`payload inputs backings`: 0 differ), and no segment commits a frame number
        twice (0 repeats in vp18).
    So the word was written with this frame's stamp from an input list that is not the installed publication's as copied.
    **Diagnostic in place (TEMP, CS_DCLF_SET_PARITY only):** each visibility word's writer is recorded per segment
    (`Resources::writers`, BuildDrawsCS's `RecordWriter`, bound through the main dispatches' unused `memberClaimsIndex`): the
    stamp, the scene list's SRV index, the input's position and the write site. An "outside the set" sample prints both segments'
    writers next to the installed inputs SRV. It did not reproduce in run wr4 (2026-10-22, about 12,400 frames walking out of
    Riverwood). **Next:** on a hit, a writer whose inputs SRV is not the installed one means a dispatch read another backing; the
    same SRV with no input naming the slot means the backing changed after the dispatch.
-   **Build parity's decal check reads only the classes' decal sub-ranges.** With the plain decal buckets on
    (`CS_DCLF_DECAL_PLAIN`) the decals go to the class plan's decal buckets, which `DrawnDecal` does not read, so the check reports
    every slot culled ("87 slots, 87 culled"). The colour and terrain draws' plain buckets were added to the comparison on 2026-10-22
    (it had compared nothing since the plain buckets came in). **Next:** read the decal buckets the same way.
-   **Accumulated parity on terrain and object LOD geometry (`<- ACCUMULATED DIFFER`).** In run vp10 (`CS_DCLF_ACCUMULATED_PARITY`
    with motion on the tree-forest route), every window: about 60-110 of 29,000 engine-culled-root geometries set by DCLF alone and
    80-180 by the engine alone, all `Land`, `obj`, `obj-LargeRef`, `objsnow` and `objsnowHD-LargeRef` (terrain blocks and object
    LOD), in runs of consecutive frames. Stood-in roots and the animation gate agree (`anim rate`: 0 differ). These geometries are
    not animated, so the bit only matters to whatever else reads `kAccumulated` on them. Not investigated.
-   **Capture parity mismatches under `CS_DCLF_CAPTURE_PARITY` with `CS_DCLF_PARITY_BOTH`**, in every window. Since the observer fix
    (run cp1, tree-forest route) 7-10k of about 340k draws a window (0 material, 3-4k per-geometry, 0 technique); before it about 44k
    of 153k (29k material, about 300 per-geometry, 7k technique), and every LODObjects, LODObjectHD and LODLandNoise draw. The cause, by field:
    -   **Observer fixed (run cp1: 0 material and 0 technique mismatches in every window, of about 340k draws checked):** `CompareMaterial` no longer compares PS PerMaterial 29 (IBLParams, kept from
        the first evaluation by `KeepUnreadFloats` and read by no Lighting stage) or VS 11 (`TexcoordOffset`, which the GPU patches
        into the rows), and the technique compare leaves out the fog (VS 13-15 and PS 19 FogColor, kept by `KeepTechniqueFog`).
        A port's declared frame variables (`MaterialSpace::declaredVSFloats`, `declaredPSFloats`) are not compared by
        `CompareMaterial` (Lighting's layout only), so nothing was changed for them. What remains is 7-10k mismatched draws a
        window: about 3.5k per-geometry and the rest below.
    -   **Loaded terrain blocks drawn with another technique.** `Block (x, 0)` draws with DCLF's MTLandLODBlend (19) pipeline where
        the engine's pass is MTLand (8). This is permutation parity's `vertex 1B000000`, about 900 a window. It is a real
        normal-path difference, not yet chased.
    -   **Not attributed:** permutation parity's `pixel 800000` (about 7.5k draws) and `feature 1C0`, PerGeometry differences on
        facegen tint parts (PS 7) and TextureProj (VS 6), character light t11 on NPC armour (DCLF's frame view against the draw's,
        which the engine alternates), and draw parity's 0-15 differing draws a window.
    -   **Expected:** face capture parity differs on every reflection face draw, because face draws do not match the main rows.
-   **Frame values' texture transforms (parity): frame variables drift (`<- DIFFER`).** Seen in vp5 and vp6 (not vp1-vp4), 1-62 of
    about 14,000 entries in a window, all `frame variable` entries (an Effect material's FalloffData or BaseColor), by small amounts:
    `sent (1, 0.4443106, 0.81752133, 0)`, `now (1, 0.44422826, 0.8172912, 0)`. The check reads the buffer again at the producer's end,
    so a controller that writes a declared frame variable inside the window shows here: the same race as `<- MATERIAL PORT`'s
    FalloffData below. A frame's lag on an animated effect; not visible.
-   **Frame values' moved-only sampling: rows differ (`<- DIFFER`).** In vp1 9 of 1,257 rows (first `row of slot 12678
    ('leftarm:0', not due): sunEntry, lodFadeNode`); in vp2 and vp6 one row a window, always a small loose clutter item not due
    (`'CoinBagMid:1'`, `'Basket04:0'`) with `sunEntry, lodFadeNode` differing: likely an item moved (Havok) without a move event, whose
    sun entry and fade node the sampling kept. The sun entry only selects its shadow cull, so a frame's stale entry is not visible.
-   **Stand-in walk parity: three snow geometries missed at frame 868.** About one full parity run in three reports, in one
    window only, `3 geometries handed to the registration from entries the stand-in would not walk <- STAND-IN WALK`, always at
    `[frame 868]` of the fixed `CS_DCLF_TEST_MOVE` path, and always 'objHD-LargeRef', 'objsnow' and 'objsnowHD-LargeRef'. It is
    likely a cell crossing where the walk decided before the snow members joined. Normal play would drop these for that frame.
    **No longer verifiable:** 0 in ten moving full parity runs on the tree-forest route (vp1-vp10, 2026-10-21), with the check active.
-   **Walk parity: the cooking spit's sun entry moves without an event.** `CS_DCLF_WALK_PARITY` reports 5 of about 113,900
    objects differing per interval, all of them the sun entry on `FireSptiCookingBase:17`, whose bounds drift by about 0.06
    units. A writer DCLF does not watch animates the node, so the delta walk keeps the old bound. The same gap on a larger animated
    object would cull it wrongly. **Next:** find the writer (a controller, or Havok) and add it to the events.
-   **Mirror parity under lane mode flags late or missed node fields with movement.** This is likely asynchronous timing rather than
    a missed writer, but it is not attributed. Separately, `<- MIRROR` reports a missed collision-body writer ('SoulGemPetty01',
    body true -> false without an event).
-   **`<- FADE STATE`:** 1-3 roots in some interior logs, one fade step apart (`currentFade` 0.982-0.989 against the port's
    0.990-1). It appeared with the actor pre-cull change (FadeStateCS's frustum test for preprocessed roots). **Next:** check the
    port's key and the preprocessed roots' last visible frame.
-   **`<- FADE INPUT`:** an actor root's row fadeAmount is 0.9947 against the node's 1, steady over three windows. Either an
    `Actor::SetAlpha` write was missed by the events, or another writer sets fadeAmount. As a result, FadeStateCS steps amountFade
    toward 0.995.
-   **`<- MATERIAL PORT`:** one difference a window remains on an effect's FalloffData (VS variable 7) after the retake, from a
    controller write the retake missed.
-   **`<- UNPINNED PROPERTY`:** once (run v1, 2026-10-23, the Whiterun save under `CS_DCLF_TEST_MOVE` with persistent and set
    parity), one references window: 100 of 22,720 tracked geometries' properties held anew with no pin, and 10 references refused
    (first: property `0x22f2828ac80` of geometry `0x22f2828b580`).
-   **`<- UNPINNED FADE NODE`** fires for requests still in flight at the report. It should fire only for a request left
    unanswered across reports.
-   **Light candidates parity: a dependent with no geometry row (`<- CANDIDATES`).** Seen once (run coc1, 2026-10-22, turn-only
    with `coc WhiterunBanneredMare` at frame 600 and `coc WhiterunDragonsreach` at 1500), in the first run after loads stopped
    rescanning the tracked set. About 30 s after the return to Dragonsreach, one root's light entry (`0x1f862fff100`) listed a
    dependent that the light table holds no geometry row for. It stayed so at every check until the run ended (1 then 5 of 10
    checks a window). The table rewrites an entry only when its verdict or signature changes (`UpdateLightCandidates`). Before
    the change, every load rebuilt the tables from nothing. The run before the change was not repeated. Nothing else failed in
    that run, and the death-reload run (die3) showed no such failure.
-   **Long-standing flags**, each seen in some runs: `<- CUT`, the PS25 `<- MATERIAL PORT` race and `<- FADE`. (NO CASCADES fired in
    every interior window, where the sun does not accumulate: it now counts exterior frames only. Not seen on the tree-forest route.)
-   **Seen once, watch for a repeat:** capture parity's "2 tracked but excluded" while NPCs crowded the view (the report now breaks
    these down by their verdict; an `eligible` entry there is a verdict DCLF kept stale). In another run, every world particle
    system froze for one window ("newly frozen" in the particles line) right after a live `dclf:first person=1` toggle.

## Unattributed visual defect: flickering fog-coloured bands in first person

The user reported full-width horizontal bands of broken geometry, flickering all over the screen, at the Riverwood river view in
first person with billboards and first person on. The bands went away with first person off and with billboards off, but also on
other actions (equipping a sword, going to third person), so billboards are not established as the cause. The billboard observer
found no leaf turned differently from the engine. It has not appeared since. **Next time:** bisect toggle by toggle in first
person, empty-handed (first person, billboards, particles, effects), and read the bad draw's vertex inputs
(`CS_DCLF_TARGET_PROBE` or a GPU capture).

## Light Limit Fix's local shadow cache is off while DCLF is loaded

Upstream's cache (`LightLimitFix::EnableLocalShadows`, default off) rotates which shadow-casting lights the engine renders each
frame. It vetoes lights through its `BSShadowParabolicLight` / `BSShadowFrustumLight` `UpdateCamera` hooks
(`FilterLocalShadowCaster`, armed by its `CalculateActiveShadowCasterLights` detour). It also copies the maps the engine just
rendered into its cache at `EarlyPrepass`.

DCLF's light selection (`Features/LocalLights/LightSelection.cpp`) is a port of `CalculateActiveShadowCasterLights`. Its
`Visible()` reimplements `UpdateCamera`'s test instead of calling it, and runs after the engine's call returns, by which time
the cache's veto is disarmed. With the cache on, DCLF would draw the shadow maps of the lights it selects and the cache would copy
and assign slices for the lights it chose, so the two would pick different lights.

Until one of these is done, `LightLimitFix::LocalShadowsBlockedByDCLF()` keeps the cache's hooks uninstalled whenever DCLF is
loaded, and the menu explains why:
- DCLF takes the engine's actual selection instead of re-deriving it;
- DCLF's port consults the cache's filter, through an Integrations extension;
- the cache gets a DCLF-aware view of which slices were drawn.

The cache's per-light `LocalShadow` flag, its `FirstPerson` / `WorldEyePosition` StrictLightData words and its
`LocalShadowData` buffer are untested under DCLF. DCLF's own StrictLightData supplies only `RoomIndex` and `ShadowBitMask`.

## Outside DCLF

-   **Frame generation is not separated from graph epochs.** `Streamline::EvaluateFSRFrameGen` and the DLSS-G path do not call
    `Upscaling::SeparateFromGraphEpochs()` (`render-graph.md`, "Hazard: external Vulkan work directly after a graph epoch"). Frame
    generation was off in every inn run. **Next:** call it before those evaluates, and run the inn reproduction
    (`coc RiverwoodSleepingGiantInn`, DCLF on) with frame generation on. The hazard's cause is also unproven. The workaround stays
    until one of these resolves it: Streamline without `VK_EXT_descriptor_heap`, the graph's last command buffer resetting the
    descriptor-heap state, a capture with a tool that supports descriptor heaps, or a standalone reproduction for NVIDIA.
-   **BasicRHI's `frontCCW` under Vulkan.** `frontCCW` has D3D's meaning on both backends. SARP and BasicRenderer set it true for
    glTF content. Their D3D12 behaviour is unchanged, but they have not been run under Vulkan since the mapping changed.

## Accepted differences

These differences from the engine are known, measured and left as they are. Each entry names what would remove it.

### A root seen again is drawn for one frame with its old fade

**What.** The engine updates a fade root only when its cull reaches it (`BSFadeNode::OnVisible`). A root seen again after
more than a frame unseen is re-evaluated on that same frame, before the draw, and a root that should have faded out meanwhile
snaps out and is never drawn. `FadeStateCS` runs a frame ahead: builds read the previous frame's published states
(`FadeScene::FadeStatesRead`). So on the frame a stood-in root comes back into view, DCLF draws it with the state from when it
was last seen, and it disappears on the next frame.

**Measured** (teleport route with the camera turning). Over five 10-second reports, 2,126 roots were seen again with a fade
above 0, and 1,291 of them snapped out on that frame, each a one-frame draw under DCLF. At a fixed position, none snapped.

**Why it is accepted.** A root comes back into view when its bounding sphere first touches the frustum. The sphere encloses
the mesh with a margin, so on that frame the mesh itself has usually not reached the screen yet.

**What would remove it.** The depth build reading this frame's `FadeStateCS` output, ordered after the pass in the same epoch.
The snap out by distance is already applied: BuildDraws' first phase applies the fade-out distance test (`kObjectFadeTest`)
to every input whose root has no serviced `OnVisible`. That part has not been measured on the teleport route.

### Skylighting parity lags while roots start fading

**What.** The engine's reference render of an occlusion map (`CS_DCLF_SKYLIGHT_PARITY`) culls stood-in roots by their
nodes. The nodes carry the GPU's fade milestones only a few frames after the GPU state (the write-back's readback latency),
while DCLF's map reads the current state.

**Measured.** Every map was within 15 texels by more than 1/256, except in the first 17 s after a teleport, while the turning
camera brought newly loaded roots into view. There, a few maps differed by 2,500-19,000 texels, with DCLF always nearer.

**Why it is accepted.** The difference is the reference's latency, not DCLF's map. The engine's own readers of the nodes
(the tree LOD's crossfade) take the same few frames.

**What would remove it.** A parity check that skips roots whose milestone changed within the write-back's latency.

### Shading that changes during a frame is drawn from the next frame

**What.** An object's shading (MaterialData, EmitColor, SSRParams.w, Linear Lighting's emissive multiplier) comes from
FrameValues (`BindlessShading`, PS t121), sampled at the frame's start for the slots the walk named by their shading events.
The engine's own draw reads the property when it sets the draw up. So a value written after the frame's start reaches DCLF's
draws a frame later than the engine's:
-   a controller on the animation job, which runs alongside the render thread (a candle's flickering emissive);
-   a cull's `GetRenderPasses`: the alpha it leaves on a non-member's property, and the LOD fades a held fade node keeps on the
    property. A member's LOD fades are the draw's own (LodFadeFrame).

**Measured** (the Riverwood bridge, the camera turning). About 25 of 40,000 slots per check changed after the frame's start,
and every one was named by an event for the next frame. Capture parity counts the affected draws on their own line
("shading changed since the frame's start sampled it").

**Why it is accepted.** It is the user's decision: a frame's delay on an animated emissive or fade is not visible, and it
keeps the render thread out of the shading.

**What would remove it.** Sampling the named slots again just before the colour epoch, on the pool, into the frame's shading
backing. That puts a pool job back on the colour epoch's critical path.

### Tree LOD instances the engine leaves hidden after `allVisible`

**What.** The terrain manager's node update (`FUN_140510730`) sets a tree LOD block's `allVisible` (`+0x82`) when the block
no longer needs per-instance hiding (its flags' bit 14 clear), then calls the block's update (`FUN_140503f70`). That update
refills only the groups whose `shaderPropertyUpToDate` (`+0x24`) is clear, and nothing cleared it. So the engine keeps
drawing the records it packed before: every instance that was hidden (scale 0) stays hidden, although `allVisible` says
every instance should show. DCLF draws by the intended rule: when the update finds `allVisible` set, DCLF repacks those
groups with every instance shown.

**Measured** (teleport route, three runs). 10-52 shapes differed in the reports after each load, and none once the camera
had stayed at one place.

**Why it is accepted.** The engine's records are stale, not intended: hiding is for instances whose full tree is loaded,
and `allVisible` is the block saying none are. If the engine's behaviour is visible, it shows as holes in the tree LOD where
full trees have since unloaded, which DCLF does not reproduce.

**What would remove it.** Nothing in DCLF, since the behaviour is the engine's. It has not been confirmed visible on screen. To investigate:
-   **Starting point.** `FUN_140510730` at the `allVisible = 1` write (the branch on the node flags' bit 14), and its call of
    `FUN_140503f70` (`0x1405108ff`). Find what bits 13 and 14 of the node's flags (`+0x40`) mean.
-   **Count it.** `CS_DCLF_PERSISTENT_PARITY`'s "tree LOD mirror" line, `shown by allVisible without a refill`, counts the
    shapes where DCLF and the engine differ. `CS_DCLF_TREE_LOD_AUDIT` counts instances by their reference's state.
-   **See it.** With DCLF's tree LOD off, teleport away from a forest and look back at it. An engine bug shows as gaps in the
    LOD forest where trees were loaded, lasting until the block is refilled for another reason. Then compare with DCLF's
    tree LOD on.
