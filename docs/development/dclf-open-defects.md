# Drawcall Limit Fix: open defects

Defects found while investigating the "road lit as if in direct sunlight" report, at the save on the road by
the large tree. That report's cause was that every DCLF sampler was an empty descriptor (fixed; see
"Every DCLF draw sampled through an empty sampler descriptor" in [drawcall-limit-fix.md](./drawcall-limit-fix.md)).
Each is reproducible with the probes named below.

## The far cascade's remaining differences

The cascades' caster sets were fixed (see "The far cascade: DCLF drew the wrong set of casters" in
[drawcall-limit-fix.md](./drawcall-limit-fix.md)); the far cascade matches native within the scene's drift
across a live toggle. What is left, at the road save:

-   **0-3 extra casters per far-cascade report**: fixed by the sun's entry rule (see "Shadow-only casters,
    and the sun's entry rule" in [drawcall-limit-fix.md](./drawcall-limit-fix.md)). What remains is 1-2
    carried items a frame, pruned by the engine at a node below their actor's entry, with no visible effect.

The near cascade's step and Volumetric Shadows' are fixed: see "Shadow views draw with the view's rasterizer
state" and "The volumetric lighting copy is the engine's alone" in [drawcall-limit-fix.md](./drawcall-limit-fix.md).

## BasicRHI's `frontCCW` under Vulkan, outside DCLF

BasicRHI's `frontCCW` now has D3D's meaning on both backends (see "Front faces" in
[drawcall-limit-fix.md](./drawcall-limit-fix.md)). SARP and BasicRenderer set it true for glTF content; their
D3D12 behaviour is unchanged, and they have not been run under Vulkan since the mapping changed back.

## Opaque pipelines write alpha that the engine's opaque draws leave alone

**Evidence.** The write masks of the blend state bound at native deferred Lighting draws (Whiterun road save,
one interval):

| Targets 0-7 write mask | Engine write mode | Draws |
| --- | --- | --- |
| `7 F F 7 7 7 7 7` | 1 (opaque) | 16805 |
| `F F F F F F F F` | 10 (opaque decals) | 1846 |
| `7b Fb Fb 7b 7b 7b 7b 7b` | 1, blend mode 1 | 1065 |

Write mode 1 writes only RGB to target 0 (`kMAIN`) and targets 3-7 (Albedo, Specular, Reflectance, Masks,
Masks2); Community Shaders' `Deferred::OverrideBlendStates` copies target 0's mask to targets 3-7. DCLF's
opaque pipelines use the default blend state, which writes all four channels, so they write the pixel
shader's alpha where the engine keeps the target's cleared value. At the road: target 0's alpha is 1.0
natively and 0.0 under DCLF (the road's vertex alpha is 0).

Nothing found reads those alphas today (`DeferredCompositeCS` and Subsurface Scattering read RGB only), so
the image does not change. It is still a parity gap.

**Fix:** build opaque pipelines from the engine's blend state for their write mode, as decal pipelines
already do (`DrawPipelines::ReadEngineState`).

## Stood-in animated references that never fade freeze (found by T7b0, 2026-10-11; fixed by T7b2, pending validation)

`SyncRootOwnership` (formerly `SyncFadeOwnership`; `Core/Handoff/PrimaryCull.cpp` ~842) clears kAccumulated (bit 26) on the roots DCLF stands in for, and nothing
sets it again while they are stood in. `FUN_1402cff60` (`UpdateAnimationJob`'s and the managed nodes' gate, on `ref->Get3D()`)
then takes its "not accumulated" path (`skyrim-engine-notes.md`, "T7b0"):

-   **Roots that never fade** (ignore-fade set at attach, zero radius, or bit 29): the gate returns false, `UpdateAnimation` and
    the managed-node update stop, and the object freezes in its last pose while it is admitted (non-actor, non-tree references in
    the cell's animated array: windmill sails, water wheels, flags, Dwemer machinery - examples, not confirmed against flags).
-   **Fading roots:** the engine runs `FUN_14147a160` on the node from job threads twice a frame; with `+0x13C` no longer
    refreshed the fade snaps to its end, written over DCLF's write-back (the `AnimatedFade` hook sees it).

Fix: T7b2 (kAccumulated written by DCLF from its GPU visibility, at Main::Update's entry, in place of the clear;
`drawcall-limit-fix.md`, "T7b2"). The clear is removed, and the tree clock stays off DCLF's trees through the tree list removal
(`TreeAnimation::SetOwned`). It is built but not run in game. To validate: `CS_DCLF_ACCUMULATED_PARITY=1`. `<- ANIM RATE` must
show 0 differ, never-fading stood-in calls rejected only when written out of view, and the never-fading against fading counts. A
fading root DCLF writes out of view ran `FUN_14147a160` from the gate over DCLF's write-back; since T7b4 the gate skips the roots DCLF
writes (`drawcall-limit-fix.md`, "T7b4/T7b6"; `CS_DCLF_FADE_PARITY`'s `<- FADE DOUBLE` counts any engine step left).

## Stood-in BSTreeNode trees stay on the tree manager's tree list (found by T7b5, 2026-10-11; fixed, pending validation)

`TreeAnimation::SetOwned` takes stood-in roots off the tree manager's animation list (`+0x50`, BSLeafAnimNode roots) only. A
stood-in BSTreeNode stays on the tree list (`+0x20`), whose loop (`FUN_140437e50`) visits a tree by its first child's multibound
cull result - which no cull refreshes for a stood-in root. A stale "in" keeps the tree visited every frame (its bones animate, in
the near list's 40); a stale "out" freezes them (`skyrim-engine-notes.md`, "Tree manager list (T7b5)"). Fix options: TreeAnimation
manages the tree list too, or DCLF writes the multibound result from its verdicts (T7b5's port decides which trees). Related: DCLF
gives tree wind slots to BSTreeNode roots only (`kKindTree`); the BSLeafAnimNode trees get none.

Fix (T7f preparation; `drawcall-limit-fix.md`, "T7f preparation"): the trees stay on the list (DCLF follows the loop's LOD switch
stores and the near list's bones), and around the loop's call `TreeAnimation` writes each stood-in tree's multibound result from
DCLF's verdict (the root's `kAccumulated` as T7b2 wrote it), putting the engine's value back right after. Built, not run. To
validate: the `tree animation list` line's "loops given DCLF's verdicts" > 0 where stood-in BSTreeNodes exist; under
`CS_DCLF_TREE_PARITY`, `<- TREE MANAGER`'s stood-in trees "given DCLF's verdict" equal to the stood-in count, few differences.

## The fade write-back reads its events only on frames that record a revision (found by T7b1, 2026-10-11; fixed by T7b6, pending validation)

`FadeEventReadbackPass` (now the generic `MakeEventReadbackPass`, `Core/Render/Passes/Readbacks.cpp`, made by Features/Fades' `cs.dclf.z.fade-events` stage, `Features/Fades/FadeStage.cpp`) read its buffer in `Record`. The depth epoch is revision-driven
(each ring slot recorded once per revision and replayed: `extern/OpenRenderGraph/docs/persistent-epochs.md`), so on replayed frames
no fade events are read back and the write-back sees only the recording frames' events (T7b1's visibility readback had the same
flaw: 59 of 300 frames in motion, none in interiors). Fix: T7b6, the fade events moved onto the visibility readback's scheme (a
fixed per-frame-slot copy, harvested when the slot is armed again; `drawcall-limit-fix.md`, "T7b4/T7b6"). Built, not yet run in game.
To validate: the `fade write-back (T7b4/T7b6)` line's "frames read" near the window's frame count (about 300) in motion and in an
interior, "arms missed" 0 (or only frames whose depth epoch did not run), and milestones written in interiors.

## The third-person body drawn in first person after a camera switch (user report, 2026-10-11; fixed, pending validation)

**Symptom.** Switched to third person and back to first person, the player's third-person body went on being drawn in first
person. Only in runs without the walk, persistent or capture parities (they run the scene pass inline at the frame's start).

**What the engine does.** `FirstPersonState::Begin` (`0x1408deba0`) calls `FUN_140736150(player, 1)` (also reached from the
BSTaskPool, `AutoVanityState::Begin`, the bleedout and dragon cameras, `RaceSexMenu` and the load paths). It hides the third-person
root (`0x140736237`, `mov [r14 + 0xF4], ecx`) and shows the first-person root (`0x140736271`); with 0 it does the reverse. Both
stores are in HiddenStores' site table, so the hide reaches DCLF as an event. No cull test is involved: the main camera's cull
skips the root because it is hidden (`skyrim-engine-notes.md`, "The first-person switch").

**Why DCLF kept drawing it.** The camera switches in `Main::Update`, outside the frame's render, where a hidden store wakes a
scene pass at once (`SceneWake::Hidden`). That pass:
1.  is an event walk (the same `sceneFrame` as the frame's own pass), which does not evaluate the per-frame entries, and an
    actor's light-path entry took its hidden event only in the pass that drained it (`hiddenEventFrame == PassStamp()`). The
    event was dropped before any walk evaluated the body;
2.  holds the frame before's capture (`FrameGlobals::cullHidden`). It lists the third-person root (for `TESWaterReflections`'
    scoped hides) and outranks the mirror's bit, so even an evaluation in that pass judged the root by the frame before's "shown".

The next frame's walk found no event and left the verdict alone. The body kept its records and its main claim, and DCLF drew it
while the engine drew nothing. Under the walk/persistent/capture parities the pass runs inline after the frame's capture, and a
parity pass checks every verdict, so those runs never showed it. Going from first to third person was right: a hidden actor entry
has no record and is written in full every walk, so the body joins one to three frames later and the engine draws it meanwhile.

**Fix** (`drawcall-limit-fix.md`, "The third-person body in first person"):
-   An actor entry's hidden event is owed until a walk evaluates the entry (`Tracked::hiddenEventOwed`, set by `DrainHiddenEvents`,
    cleared by `MergeEntry`), not only in the pass that drained it.
-   An event on a node the capture lists, numbered after the capture (`SceneCapture::NextSequence`, taken by
    `FrameGlobals::Capture` before its reads: `cullHiddenSequence`) and leaving a bit the capture does not hold, is announced again
    at the first pass whose capture is newer (`Coordinator::hiddenAhead`).

**Validate** (no parity switch but `CS_DCLF_SET_PARITY=1`, which does not run the pass inline):
`CS_DCLF_TEST_COMMANDS='600:dclf:pov=third;900:dclf:pov=first;1200:dclf:pov=third;1500:dclf:pov=first'` from a save in first
person. `dclf:pov=first|third` is a harness command (`PlayerCamera::ForceFirstPerson`/`ForceThirdPerson` on the main thread): no
console command switches the camera. To check:
-   the `player 3D against the claims (set parity)` line: `<- OK`, runs past the publication's latency 0;
-   the `actor frame verdicts per pass` line: "announced again at a newer frame capture" > 0 (in first person about one a frame
    anyway: `Main::Draw` shows the first-person root inside the frame, which `cullHidden` holds hidden; in third person the water
    reflection's scoped hides of the body), and, in a separate run with `CS_DCLF_WALK_PARITY=1` (which runs the pass inline, so it cannot show the defect itself),
    `changed with no event` 0;
-   screenshots after each switch: no body in first person, a whole body in third.

## Validation errors in DCLF's indirect execution sets (found 2026-10-13 by core validation at the inn; not fixed)

Core validation at `coc RiverwoodSleepingGiantInn` (FP on), every run (i12vk, i16sync, p4vk, p5vk, p6vk):

-   **Main indirect set mixes fragment output interfaces** (`VUID-vkUpdateIndirectExecutionSetPipelineEXT-initialPipeline-11147`):
    pipelines writing locations 0-2 (forward) are added to a set whose initial pipeline writes 0-7 (deferred).
-   `VUID-VkGeneratedCommandsInfoEXT-indirectCommandsLayout-11079`: a vertex-buffer token with a pipeline lacking
    `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`.

The sequences overrun and the invalid DGC address found with them are resolved (below: "Resolved: first-person and reflection
draws read past their sequences").

## Intermittent crash: a freed child detached during cell streaming (found 2026-10-15 in motion run v3; not investigated)

It happened about 70 s into a motion run (`CS_DCLF_TEST_MOVE`, cells streaming). The exception was 0xc0000005 with rip at a heap
address (`0x1d059189840`), on an engine job thread. The stack (cdb on `CommunityShaders.dmp`):
-   `DCLF::DetachChild2At<0>::thunk+0x65`
-   then the engine's detach (`SkyrimSE+0xd1d334`), whose virtual call lands in the heap.

So the child being detached had a freed or overwritten vtable: a use-after-free of a scene object. A rerun of the same build (v3b)
did not crash. Earlier motion runs in other sessions crashed now and then at other addresses (m100, w18, w36/w76, w143, w181);
they were not analysed and may or may not be the same thing. The change under test (the view contract, V3) does not touch detach
events.

Next steps:
-   Check whether DCLF releases or drops a reference to a child that the engine still detaches later. Engine references are
    meant to be released only via `EngineReleases` at Present.
-   Check whether the `DetachChild2At` thunk reads the child after the original call.
-   The dump is kept as scratch `refactor2/v3-motion.dmp`.

It happened again on 2026-10-16, in a motion timing run (fdt2) of the Fades F3 build, about 100 s into the run, with the same stack:
`DetachChild2At<0>::thunk+0x65`, then `SkyrimSE+0xd1d32e`, `call qword ptr [rax+1B8h]` through the child's vtable, read as float
data (`3f8000003f800000`). So the child is already freed when the engine's original detach runs inside the thunk. Its dump is kept as
scratch `refactor2/fdt2-motion.dmp`.

## Accumulated writes parity (A5): an intermittent entry write the incremental pass skips (found 2026-10-15; not investigated)

In one `CS_DCLF_PERSISTENT_PARITY` run (v2-pp), 2 of the 5 compared frames differed. Both were the same entry root (4466): the
reference wrote it visible (a wide set), and the pass wrote nothing (`<- WRITES DIFFER; first: root 4466 set wide (reference's
alone)`). Two reruns of the same build compared 5 frames each with 0 differences, and no earlier run shows it.

The suspect is `JudgeEntry`'s skip (`Core/Render/Readback/Visibility.cpp`). It skips when the entry's `entryWritten` already holds the
wanted value and `entryWrittenVersion` is unchanged, so a write made while the entry was in some other state can hide a later one. The
change under test (the view contract, stage V2) does not touch this path.

Under motion it reproduces every time (`verify.sh mpp`: `CS_DCLF_TEST_MOVE` with `CS_DCLF_PERSISTENT_PARITY`). On the B1 build it
appeared in 3 windows (root 4466 again), and with the gate parity build in 9 windows, where the first was root 3953 (`set
(reference's alone)`). Both builds leave this path unchanged. The motion run is the reproduction to use when it is investigated.
In the material integrations' run (mat-fullc, 2026-10-16) one difference persisted about 20 s: roots 2463 and 2464 set wide in the
reference alone, with six geometries cleared, in 23 of the windows from frame 881 to 2203, then gone; the run of the build before
(matbase-fullc) showed none. That step does not touch the visibility write-back.

## LOD gates: an intermittent flipped incoming geometry missing from its flip's publication (found 2026-10-15; not investigated)

In one of four `verify.sh msp` runs (`CS_DCLF_TEST_MOVE` with `CS_DCLF_SET_PARITY`; the B2 b0 build, whose changes are inert),
`TakeDeltas`' check reported `1 flipped incoming geometries not claimed by the flip's own publication, first gate 171: geometry ...
claimed 0x0, flipped 0x21 <- GATE HOLE (publication)`. If real, the incoming block is not drawn for the frames that install that
publication, while the outgoing one is already hidden. No earlier log shows it, but moving set-parity runs are new. The B2 gate
rewrite runs `msp` at every sub-step, which shows if the rate changes.

## A resolution or upscaler change leaves one colour pass undrawn under standing claims (found 2026-10-17; not fixed)

Since 3af80a51 the set is never withdrawn: after a graph build or a main-resource change the frame waits at its start for a snapshot
current for the graph as built (`IndirectDraws::AdoptCurrentSnapshot`). The main resources, though, are made at the colour epoch's
`Setup` (Core/Render/Epochs/MainEpochs.cpp): it compares RT0's size and formats, the frame buffers' shapes and the graph buffers from the
colour capture, so a change is only seen there, mid-frame.
- **What happens.** That frame's adopted revision and payload predate the new resources, and a snapshot made mid-frame would belong to
  another publication than the frame's claims. So that one colour epoch is not submitted (logged `main resources made at frame N's
  colour pass ...`) while the frame's claims stand: the engine withholds DCLF's objects and nothing draws them, for one frame. The next
  frame waits for a snapshot built for the new resources (the context posted at `Setup`, `Impl::PostResourcesAhead`) and is covered.
- **A resolution change can cost two waits.** The views rebuild their imports at `FrameStartBeforeCoverage`, which requests another
  build for the next build point.
- **Fix direction.** Detect the main-resource change at the frame's start, before the adoption: from the engine's render-target
  state the colour capture reads, or the upscaler's render size, if it is known by then. The frame would then build and wait like
  any other build. Alternatively, record the frame's colour epoch synchronously for the new resources.

## A target format change: no DCLF epoch until the rebuilt pipeline catalog is installed, under standing claims (pre-existing; found 2026-10-17)

After `SetTargetFormats`, `FrameIndirectState()` stays invalid until a publication carrying the new pipeline catalog is installed.
During those frames `RunEpoch` returns before `Setup` (`failed, no lighting, pipelines off or indirect state invalid`). Meanwhile the
claims stand, and the snapshots look current by generation, so the frame neither waits nor fails. DCLF's objects can be missing for
the frames in which the catalog is rebuilt. Waiting on the catalog at the frame's start, as for a build, could exceed the 5 s bound
while the new pipelines compile. A fix needs its own design: for example, the old catalog's pipelines kept drawing into compatible
formats until the new one is installed, or the format change made at a point where the catalog is already compiled.

## Extras rows: a projected-UV or land-blend row changed without a journal entry (found 2026-10-17; not investigated)

The persistent parity's payload backings check (`CheckExtras`, Diagnostics/Draws/KeptChecks.cpp: each extras backing's rows as its
catch-ups sent them, against the tables' rows) reports `<- DIFFER` in one or two report windows of a moving `verify.sh full` run
since the LOD gates' adoption guard was removed (the frames no longer withdraw after a forced release). For example:
`extras backing 4: row 1523 of 11732 (version 339): uploaded (0.5 0 0.02 0.49999997), tables (0.29341206 0 0.00066666666 0.41317594)`.
- **What it means.** The extras journal (`UpdateExtras`, Core/Render/Payload/KeptStores.cpp) never marked those rows: every backing
  caught up past the change still holds the row's old value. On the GPU that object draws with stale projected-UV or land-blend
  parameters until the row changes again.
- **The pattern.** The tables' value is the same in every run and on every row reported (`0.29341206 0 0.00066666666 0.41317594`).
  The uploaded values differ (`0.46/0.3/0.5, 0, 0.02, 0.2-0.5`). So one writer sets rows to that value without a `kChangeExtras`
  note, or on a block whose offset the journal does not read (`UpdateExtras` marks the object's current `extraOffset`, so a block
  freed or moved without a change on a live owner is never marked).
- **A/B (runs ng1, ng2, ab1, ab2).**
  - Guard removed: DIFFER in both runs.
  - Guard in place: clean in all 20 windows.
  - Guard kept as a publication cause, with only the withdrawal removed: DIFFER.

  So the withdrawals masked it; the forced full publication did not.
- **Where to look.** The writers are `WriteObjectExtras` after `Accumulate` (AccumulatePhase.cpp: allocated or kept block, noted only
  when the write changed a row), the shading resample (FrameConstants.cpp), and `FreeExtras`/`AllocateExtras` (Tables), with the
  table diff's `kChangeExtras` (Tables.cpp). A withdrawn frame's reinstall joins every claim again (`InstallClaims`, whole), which
  may be what covered it.

## Material transform moves: nothing drains the queue without the kept shadow build (found 2026-10-15; not investigated)

`TransformMoves` (Core/Shading/MaterialWrites.cpp, drained by `Extensions::DrainTransformMoves`) has one consumer, the kept shadow
build's `WatchTransformMoves`. In a configuration that never runs the kept build, nothing drains it, and the EventQueue spills when it
fills. This predates the B2/wave-3 moves, which only renamed the queue.

## Portal parity: one unbound-space occluder mismatch under motion (found 2026-10-15; not investigated)

In one `verify.sh mpp` run (the B2 gate parity build), one window reported `<- PORTAL VIEW; first: unbound space: 4 ops 1 sets, the
engine's 0 ops 0 sets`. In that window the engine listed one unbound-space occluder plane (state 2, flags `0x1000001`), and DCLF had
it in state 0. The same motion path on the B1 build, and the earlier motion runs, showed none. The gates change did not touch
portals. It showed again in a wave 3 run (w3a, the same text), and in the Fades F1 run (f1-fullf, 2 of 21 windows) as another variant:
`first: unbound space: set 0 mask 0x3d, the engine's 0x39` (and `0x35`), so DCLF keeps one more of the set's planes than the engine. F1
moved only the programs buffer's upload into Features/Portals; the walk and its comparison are CPU-side and unchanged.

## Stand-in walk parity: three snow geometries missed at frame 868 under motion (found 2026-10-15; not investigated)

About one `verify.sh` full run in three (b2c, w1, w3a, w3b, the Portals step) reports, in one window only, `3 geometries handed to
the registration from entries the stand-in would not walk <- STAND-IN WALK`, always `[frame 868]` and always the same three:
'objHD-LargeRef', 'objsnow' and 'objsnowHD-LargeRef' (resident, set phases 0x0, claimed 0x0, admitted 1). The next window is `<- OK`.
The fixed frame on the fixed `CS_DCLF_TEST_MOVE` path makes it a one-frame cell-crossing case, likely the walk decided before the
snow members joined. Parity runs walk everything, so it draws there; normal play would drop these for that frame. It predates the
modular-layout feature steps that showed it.

## Capture parity: LOD techniques and one pixel permutation bit mismatch (found 2026-10-16; not investigated)

`CS_DCLF_CAPTURE_PARITY` with `CS_DCLF_PARITY_BOTH` (the tables against the engine's own lighting draws) reports MISMATCH every window:
about 44k of 153k draws (29k material, 315 per-geometry, 7k technique), and every LODObjects, LODObjectHD and LODLandNoise draw
(`capture parity by technique`), plus the permutation parity's `pixel 800000` on about 7.5k draws. The build before the material
integrations (e686aa1f, run matbase-fullc) and the one after (mat-fullc) give the same counts, so it predates that step; the refactor's
validation runs did not include capture parity until then. TruePBR texture, skin, light data and face capture parity are OK.

## Light Limit Fix's local shadow cache is off while DCLF is loaded (upstream #2757, merged 2026-10-17; not integrated)

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

## Pooled payload: open items (2026-10-14)

The payload ring was replaced by pooled versions published once per snapshot (dclf-architecture.md, "The payload"). These
were left open:

-   **No readback check of the material row bytes.** BuildDraws parity checks the rows' addresses. Nothing reads back a
    backing's material rows and compares them with the kept rows. That would catch a part that the four-part journal
    (`kMaterialRowParts`) failed to send, or that was patched with the wrong backing's address. The extras have this check
    per backing; the rows do not.
-   **Replaced builds cost builder CPU.** The publication gate (V3) lets newer work replace a sealed build that is not yet
    published, so the builder abandons more revisions than before: sealed rose from 486 to 544 in a comparable window. Each
    abandoned revision is a make and a draws build that is never published. GPU and upload costs fall; builder CPU rises.
-   **The builder stalls if the coordinator's pump fails for good.** A payload is published only after the scene pass of the
    frame that took the last snapshot has ended (`IndirectDraws::NoteScenePassDone`). If the coordinator's pump stops for
    good (a thrown pass that never runs again), no pass ends. The builder then never publishes again, and every frame keeps
    the last installed snapshot. Nothing detects or reports this case.

## Polls and render-thread work left (2026-10-14)

Left open by the polls-to-events pass (dclf-async-publication.md, "Polls become events"):

-   **`CS.DCLF.ListJobsWait` remains.** The zone is the engine's own fork-join (`JobList::Finish` over the main camera's
    scene-list cull jobs, `ListJobsFinish` in `Core/Handoff/PrimaryCull.cpp`), not a DCLF lock: the render thread waits while
    engine job threads cull the residue left in the lists (about 193 entries per DCLF build in the motion run; 0.17 ms in
    median frames, 0.5 ms on average and 1.6 ms at most in the worst). The per-reason counters of entries kept in the lists
    say what to shrink next. The wait ends only when DCLF owns that residue (dclf-cull-job-elimination.md).
-   **DXVK's `context->Flush()` stalls.** The worst remaining frames are `CS.ORG.FlushNativeProducer` (35-57 ms flushes
    inside DXVK), then Upscaling and `GrassOptimizations::ApplyPending` spikes and engine time Tracy does not see. Outside
    DCLF; not investigated.
-   **The mirror's capture races other threads' attaches.** Other threads attach and detach in the world inside the engine-read
    window (73-277 per 600 frames, `SceneTracker::TakeWindowEdits`, reported by the C2 parity line). The mirror-only
    portal-root pushes and `CaptureMirrorRequests` therefore stay on the render thread at the frame's start, read and pushed
    at once: a pool job's pushes could not be ordered with those hooks' events. The race is the baseline's, unchanged.
-   **`CellRoots` runs every frame.** The visibility decode (`Core/Render/Readback/Visibility.cpp`) walks the object
    root's children each frame (about 26 nodes; one container's bit is set again every frame because the main camera's
    list cull clears it). An object-root edit counter in `PrimaryCull` (attach/detach under the object root, a hidden store
    on a child, a new object root) would let it walk only on an edit.
-   **`UpdateActorRate` stays on the render thread** (`Features/Actors/ActorRate.cpp`, about 19 actors' plane tests a frame).
    Its bound tests need the frame's last shadow view made and must finish before `EngineReadWindow::Close` at Present, and no
    hook sits between the two. A kick after the shadow epoch's commit (where `ParabolicExclusionSlot::Publish` runs) would let a
    leased task do them.
-   **BasicRHI's present still holds `timelinesMutex`.** `q_submit` no longer does, but `sc_present` holds it across the
    present (SARP and BasicRenderer only). CS's upload queue may not need DXVK's `LockSubmissionQueue` either.

## The scene lane and the snapshot builder: work left (2026-10-14)

Left open by the scene-lane pass (dclf-async-publication.md, "The scene lane as bookkeeping"; plan `hazy-percolating-canyon.md`).
The motion run after it: the average pass 559 us (target 0.4 ms), the worst 30 passes 6.1 ms (target 3 ms).

-   **The lane's cost in burst passes.** In the worst 30 passes (motion r2g): `Scene.Detaches` 1.02 ms, `Mirror.Apply` 0.62,
    `Capture.ShadowSets.Index` 0.46, `SunCandidates` 0.42 (and its `.Snapshot` 0.26), `EvaluateStream` 0.39, `PrefetchBuffers.Wait` 0.32. On an average pass
    `MaterialCaptures` is still about 60 us, the largest single step.
-   **`Scene.Joins` reads 0.22 ms a frame (0.09 at W0), but the joins did not grow.** The bindings now run once a pass, after the
    joins and inside the `Joins` part, so the zone holds `MaterialBindings` (0.145 ms a frame), which W0 counted outside it (twice a
    pass, before and after the joins). Net of it the joins are about 0.075 ms a frame; the `[DCLF] parallel-for waits` line shows
    their blocked time at about 0.001 ms a frame.
-   **Not split into prepare and apply:**
    -   `ResolveMaterialBinding` still resolves on the lane.
    -   The walk merge's `geometryIndex` lookup cannot move into the evaluation without an index frozen per round.
    -   The shadow build's pre-phase (records, material rows, mode evaluation) does not overlap the main build: it needs a
        two-stage `ShadowBuildStages` in `Core/Render/Shadow/ShadowBuild.h`, as `MainBuildStages`.
-   **The builder's O(whole) left.** The main build's last stage copies the primary segment's payload whole into the other
    segment's (`other = out`) before giving it its own inputs; `PackPipelines` still compares every pipeline (O(P)).
-   **Texture transforms before the Z-prepass epoch.** `cs.dclf.z.texture-transforms` runs first in the Z-prepass epoch, so an
    epoch that reads the main material rows before it (the reflection, Skylighting's occlusion) draws with the previous
    frame's `TexcoordOffset` (and effect falloff and base colour). A scrolling material is a frame behind there. No GPU readback checks the patched bytes either;
    the parity checks are the producer's re-read and the builder's packing position.
-   **Capture parity's `CompareMaterial` must leave out VS variable 11.** A record now keeps its capture-time `TexcoordOffset`
    (the GPU patches the rows), so under `CS_DCLF_CAPTURE_PARITY` every scrolling material mismatches there. Leave out the
    variable in that `CompareBlock`, or compare it with FrameValues' entry for the slot (`TextureTransformList`). The same holds
    for a port's declared frame variables (the Effect port's VS `FalloffData` and PS `BaseColor`: `MaterialSpace::declaredVSFloats`,
    `declaredPSFloats` name their floats). Comments
    still naming `RefreshMaterialTransforms` or the transform watch: `Core/Scene/Model/Tables.h`, `Core/Shading/MaterialPortFrame.h`.
-   **Mirror parity under lane mode flags late or missed node fields with movement.** Likely asynchronous timing rather than a
    missed writer; not attributed. The parity system is to be reworked for the asynchronous scene.
-   **A material row is keyed per material but packed per pair (latent).** `MainRows::materials[m]` is one row per material slot,
    packed against the pair's pipeline: its key holds the technique's `bindingVersion`, `projected` and `TablesHash(p)`. Every
    pipeline of one slot has the same technique row and pass descriptor by construction, but not the same constant tables: two
    shader permutations of one pass, or a `ModifyShaderLookup` that changed between them, could differ in their `PerMaterial`
    offsets. The row then keeps the last pair's packing, and the other pair draws it with an unchanged witness. `ResolvePair`'s
    `material.tables != blocks.tables` guard can never fire. No evidence it occurs. Guarded by a parity assertion
    (`Policy::CheckRowPacking`: `CS_DCLF_PERSISTENT_PARITY` on a schedule of region builds, `CS_DCLF_BUILD_PARITY` every build;
    `<- ROW PACKING CONFLICT` in the `[DCLF] main pairs` line). If it fires: key rows per (material, packing), extra packings in
    slots past `tables.materials.size()` from a free list, refcounted by their pairs.
-   **Worst frames are not the lane's.** The worst frames of the run were single stalls of 17-56 ms in
    `CS.ORG.FlushNativeProducer` (DXVK's flush, above), `RHI.Vulkan.QueueAccess` and Upscaling. Not investigated.

## What the engine still draws and does on the render thread (survey, 2026-10-14)

Measured with CS_DCLF_CENSUS (30 s moving run) and a 60 s motion run with SkyrimEngineTelemetry's zones merged into CS's capture.
SkyrimEngineTelemetry has its own Tracy client, so it listens on port 8087 beside CS's 8086. Capture both and align them on the
paired `Main_RenderDepth` hooks: the CS thunk wraps the engine's `Main::RenderDepth`, and the two clocks agree to about 3 us.
Times are inclusive ms a frame, median frame (9.4 ms) / worst 30 frames (30.7 ms).

**Draws the engine still makes, per frame (census):**
-   **The water reflection's cube faces: about 440 passes on one face and 340 on the other.**
    -   **DistantTree:** about 400 passes over 559 geometries under "LOD Trees". These are the distant-tree draws in
        `PerformanceOverlay`'s DistantTree count.
    -   **Lighting:** about 140 passes the reflection phase doesn't cover ("eligible, bound, not a member of the view's phase").
    -   **Sky:** about 30 passes.
-   **Main: about 67 passes.**
    -   Sky (16), water (20) and grass (12, plus 12 more in main depth).
    -   About 14 Lighting passes that aren't members of the main phase or have no record.
    -   Particle effects (3).
    -   DistantTree (about 2, plus about 2 each in two unnamed views).
-   **Sun cascades: about 12 passes, about 4.6 of them DCLF members the engine still draws.** The census flags these
    `<- DEFECT`, and point shadows the same way (0.7 a frame). **Cause found and fixed (round 3, phase 3; validated in census run d23):** the
    registrations were tested against the last frame's shadow coverage and renderer map, the census against this frame's (see
    "T7a census" below).
-   **Occlusion, Skylighting occlusion, precipitation occlusion and first person:** a few passes each.

**After round 3 (2026-10-16, census):** the engine keeps about 110 passes a frame, and none of them is a DCLF member's.
-   Main: about 75 (water, sky, grass, about 3 particle effects).
-   The reflection faces: only the sky, about 21. DCLF draws the faces on every frame.
-   Sun cascades: about 5, none a member.

**Engine work on the render thread:**
-   **The water reflection: 1.66 / 5.3 ms** (`TESWaterReflections::Update`, two `BSCubeMapCamera::RenderFace` a frame).
    -   The engine culls the scene for both faces: 0.75 / 4.1 ms of `BSCullingProcess`. Most of it is
        `BSFaceGenNiNode::OnVisible` re-culling actors' heads, about 1,100 `ProcessObject` calls a frame.
    -   Then it builds batches (`BuildJob`, about 425 a frame) and draws them (`RenderBatches` 0.6 / 0.76 ms).
    -   **Todo:** DCLF draws the faces whole: the LOD trees through DCLF's tree LOD, the uncovered Lighting passes through the
        reflection phase, and the engine's culling for the reflection camera filtered out, as the main camera's scene lists are.
-   **Engine job waits: about 0.5 / 2.6 ms.** `JobList::WaitForCompletion` under `NiCamera::CalculateAndDrawShadowCasterLights`
    and `Main::PreparePlayerView`, plus `CS.DCLF.ListJobsWait`.
-   **The engine's own depth work: about 0.3 / 1.6 ms.** `Main::RenderDepth` self and its `NiCamera::RenderScene`, and the
    face-gen culling under the terrain depth.
-   **Scaleform UI and decal node updates:** about 0.17 / 0.35 ms.

**CS work on the render thread (outside DCLF):**
-   `TerrainShadows::UpdateShadow`: 0.40 / 0.46 ms every frame. **Todo:** off the render thread, or event-driven.
-   Upscaling: 0.19 / 1.9 ms.
-   `GrassOptimizations::ApplyPending`: about 0.6 ms on the frames it runs.

**The worst frames' unzoned main-thread time (about 6.7 ms in `Main::Update`): synchronous file reads, not an SKSE plugin.**
-   **What the sampler shows.** SkyrimEngineTelemetry's sampler (`[Sampler] bEnable=1`) attributes `Main::Update`'s uncovered
    time to `skse64+0x108B0`. That is only SKSE's call-site hook in `Main::Update`. The samples under it run game code, as do
    the blocked ones.
-   **The blocked samples read a file on the main thread.** The chain:
    -   `Main::Update` → `35641` → `35642` → `35652`. The last of these ends the frame's world update, and its last call drains
        `BGSDynamicPersistenceManager`'s queue (`15338`).
    -   → `QueuedPromoteReferencesTask::Func2` (`15319`; the large-reference task `QueuedPromoteLargeReferencesTask` doesn't
        override it) → `15328` → `15360` → a `TESWorldSpace` function (`20456`) → `13972`/`13977` → `Archive`/`NiFile` →
        ucrt `fread` → `ReadFile`.
-   **What that means.** While cells attach, promoting references reads worldspace records from the plugin files
    synchronously on the main thread. No SKSE plugin's code is on these stacks.
-   **Still to do:** confirm, in the merged Tracy capture, that these are the worst frames' stalls, by zoning `15338`/`15319`.
    This session's attempt failed: that run's two captures started one anchor apart, and `mergert.py` paired the anchors off
    by one. Then decide whether the read can move to the loading thread, or the records be read ahead.
-   **Tools.**
    -   The plugin doesn't create its report folder (`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimEngineTelemetry`),
        so without it no report is written.
    -   The sampler counts a blocked thread once per wait, not for how long it waits, so its blocked counts understate long
        waits.

## Unattributed visual defects seen in first person (2026-10-11/12; not investigated)

-   **Flickering fog-coloured bands** (user report): full-width horizontal bands of broken geometry, flickering all over the
    screen, at the Riverwood river view in first person with billboards and first person on (runs bb7, bb10, bb11). Gone with first
    person off (bb9, bb12) and with billboards off (bb7off), but the user saw it disappear on other actions too (equipping a sword,
    going to third person), so billboards are not established as the cause. The billboard observer found 0 of ~11,000 leaves a
    window turned differently from the engine. Next time: a toggle-by-toggle bisection in first person, empty-handed (first person,
    billboards, particles, effects), and the bad draw's vertex inputs (`CS_DCLF_TARGET_PROBE` or a GPU capture).
-   **Dark-red smudges (T7f item 7, r2):** the smudges in the first-person runs are the engine's screen blood (`BloodSplatter`, under
    `ScreenSplatterRoot`, census view "other #18"), drawn while the player takes damage; 2 to 28 geometries over r2's windows. Not DCLF's.
    No fog-coloured band appeared in r2, r3, r6 or r8 (billboards on, first-person billboards drawn by DCLF).
-   **Every world particle system froze** in fp8 at about 22:58:57: 0 snapshots afterwards, all 226 systems "newly frozen".
    Dark-red static smudges appeared in the same run with first person off. Neither recurred in fp13 or bl5. The T7f producers'
    work (`kAccumulated` writes, the tree list) was being built in the same tree at the time; a system whose update the engine skips
    (its root judged not visible) takes no snapshot, so watch the particles line's "newly frozen" in runs with
    `CS_DCLF_ACCUMULATED_ALL=1`.

## Parity issues deferred while the scene pipeline's structure lands (T6b, 2026-10-11)

Found in validation runs and left for a cleanup pass once the structure is in place (plan `hazy-percolating-canyon.md`).

-   **Reflection residue under the LOD roots rose with the LOD gates (T6b5).** Baseline w150: mostly 0 a window (max 18);
    w154: 17-83 in every window, almost all "bound, not a reflection member" ('Land' under 'chunk', 'obj'/'objsnow'). Suspected:
    outgoing blocks the engine reuses (a held handle attached again as a node's current block: "23 reused" a window) stay
    `gateHeldOut` (now their Hold veto, `Api::kVetoHold`) and are drawn natively; the scene report's "held out now" stays at 6-22. The fix is written (2026-10-11, not built
    or run yet): a kept notice (`LodGates::Outcome::kept`) at a reattach before the flip and at a release that finds the handle
    reused, which `ApplyGateKept` turns into members again; forced and superseded gates audited (dclf-async-publication.md, T6b5).
    To confirm: "held out now" near 0 between swaps, the residue back to the w150 baseline. Blocks T6b6's gate (residue 0).
-   **T7a census: member passes drawn by the engine (`<- DEFECT`).** w159: 506-7,793 a window, all in the sun cascades
    (first: 'leaves', 'Block (1, 0)'). Async mode judges membership from the frame's set, so this may be the census reading the
    set's phases against the cascade (shadow members by phase); confirm before treating it as a withholding gap.
    **Attributed (T7f item 8, w202-w232):** it fires only in the windows with frames that had no published sun exclusion (no shadow epoch
    that frame, 2-16 a window during cell loads and the start), and the census's new `<- WHOLE ENTRY` fires on the same geometries: on
    those frames the engine culls and registers the cascades whole (whether DCLF's cascades drew the same members then is not
    established). Open: an exclusion for every
    frame (the last one translated to the frame's candidates, or the cascades drawn from the kept state) would remove the engine's
    cascade culls there too.
    **Cause (round 3, phase 3):** the shadow views' renderer map (`ShadowViews::Rebuild`) and their coverage
    (`IndirectDraws::DecideShadowCoverage`) were decided at `BeforeShadowMaps`, after every shadow light had registered in
    `CalculateAndDrawShadowCasterLights`. So the withholding and the sun's claims tested the last frame's map and coverage while the
    census tested this frame's: on a frame turning covered (startup, a load: no shadow recording or payload yet, then one) every member
    registered unwithheld (`MEMBER drawn by the engine`); on one turning uncovered the members were withheld from views DCLF did not draw
    (lost casters for a frame); and a point light kept this frame was in no map. `Rebuild` with no selection also left the last map
    published. **Fixed (validated, census run d23):** the coverage is decided before Main::Draw calls `CalculateAndDrawShadowCasterLights`,
    each kept light's views are covered and their renderers published at `FUN_1414a3fb0`, before its `Accumulate`, and the list is
    rebuilt from DCLF's selection when the call returns (`Features/Shadows/Engine/ShadowCasterPoints.cpp`; skyrim-engine-notes.md, "The
    shadow lights' selection"); `Rebuild` with no selection publishes an empty map. **Measured (d23):** 0 member passes drawn by the
    engine in every window, the first after the load included; full capture parity clean apart from the known defects.
-   **T7b1 `<- ACCUMULATED DIFFER`** (w159): on compared frames, roots the engine alone sets are effect and untracked roots
    ('FXAmbWaterfallSalmon01.nif') and geometry the engine alone sets is mostly stale under a root the engine failed; 'Emerald'
    is DCLF's alone every frame. (The readback's missing batches are structural and being fixed.)
-   **T7b3 room parity** compared no rooms or shared nodes in the interiors run (every call a non-room parent): needs an interior
    with portals (Dragonsreach was in w160; check the run reached it).
-   **LOD gates' known effects of a hold:** both terrain levels' LOD water registered during a hold; a held block's segments
    are not updated; the map's hide-all is handled by flushing. Not measured visually.
-   **Gate coverage:** "neither level shown" at release is the engine's own hide of LOD under the loaded cells
    (`FUN_140512160`), counted apart; any `<- GATE COVERAGE` left names the store that hid the block.
-   **`<- MIRROR`:** a missed collision-body writer ('SoulGemPetty01' body true -> false without an event).
-   **`<- UNPINNED FADE NODE`:** fires for requests in flight at the report; should fire only for one unanswered across reports.
-   **Long-standing flags:** `<- CUT`, extras `<- DIFFER`, the PS25 `<- MATERIAL PORT` race, `<- TEMPLATE` (PS16), `<- FADE`,
    `<- PORTAL VIEW`, NO CASCADES.
-   **T6b2's gate coverage runs** (snow, LOD, skin) not done.
-   **`<- FADE DOUBLE` (T7b4, w168):** 0-11 stood-in roots a window in motion (clutter: 'HandCart01', 'Hoe', 'Bucket01',
    'DeadSalmon01.nif'; none stored by DCLF that frame) stepped by an engine `OnVisible` - some engine cull still reaches stood-in
    roots (which one is not attributed); 0 in interiors.
-   **`<- FADE STATE` (w169, interiors):** 1 differ of 320 root updates (currentFade 0.982 against 0.990); new since T7b4's milestone
    key change (LOD level, transition state) - check the port's key.
-   **Census `<- DEFECT` in point shadows** (w168: 50 member passes in a window, 'MountainCliffSlope:0'), beside the sun cascades'.
    **Fixed with them (round 3, phase 3; validated in census run d23):** a point light newly kept this frame was in no renderer map at its
    registration; its views are now covered at its `FUN_1414a3fb0`, before its `Accumulate`.
-   **`<- TREE MANAGER` coverage:** the tree list compared only 0-1 BSTreeNode a frame at Riverwood (all skipped both ways); the
    animation list's clock compared 3 nodes in a window (most leaf roots are not DCLF's). Needs a cell with BSTreeNode trees (T7f: none
    found in five exteriors, below).
-   **`<- FADE DOUBLE` (T7b4, w168), attributed (w190-w193):** the clutter roots were placement snaps (`FUN_14147aa20`, a cell attach that
    writes `+0x13C`), an event DCLF already follows by seeding the root again. They are now counted aside. Under `CS_DCLF_FADE_ALL` the
    observer is at 0 everywhere.

-   **Intermittent vertex-stage page faults at load (2026-10-11):** two device losses a few seconds after a load, both Aftermath
    `vertex_01`, not reproduced in the next runs: w167 at the main menu ("Shader Instruction Error", DCLF idle), and still-fix-on
    5 s into gameplay (MMU fault reading VA 0, fingerprint 5766533646317074780). Unattributed (shader debug info not written).
    -   **Index-pool firsts: likely resolved, pending a longer motion soak.** One device loss of this signature was traced to the
        index pool: `cs.dclf.pool-firsts` grew to a new, empty version, and the update re-uploaded only the slots that changed, so
        draws took another geometry's first index from the unfilled rows and fetched past their indices. Today's rule is
        structural, for every growable buffer: a writer keeps the version it filled (`FilledVersion`) and writes whole into any
        version the set names that it did not fill; the pool lays its indices out again and sends every first index into such a
        version (`IndexPool::indicesFilled`, `firstsFilled`). No device loss in the runs since (60 s or less, motion and inn),
        which are too short to close it.
-   **Capture parity indexed the tables by a lookup from another snapshot** (`OnFaceLightingDraw`, a CPU crash under
    `CS_DCLF_CAPTURE_PARITY`): now skipped when the index is past the tables (face and native paths).

-   **Use-after-free in actor teardown on a save load (2026-10-11, unattributed):** loading a save after going through a door,
    a job thread in `Actor::Predestroy` -> `FUN_140720830` (the middle-high process data, `+0x278`) -> `FUN_14021ca60`/`FUN_14021d140`
    destroyed a pool entry's object (keyed by a `BSLightingShaderProperty`) that was already freed (its first qword a free-list
    link). DCLF holds property and material references, but nothing ties it yet: reproduce with `CS_DCLF_RELEASE_GUARD=1` and
    with DCLF off. The crash then hung the game in Community Shaders' in-process `MiniDumpWriteDump` (a suspended thread held
    the heap lock); the dump is now written out of process (`tools/crash-dump-helper`, `CS_TEST_CRASH` to test it).

-   **`<- ACCUMULATED ALL` (T7f dry run, w176/w177): attributed and changed, pending a run** (`drawcall-limit-fix.md`, "T7f: what
    kAccumulated means"). Roots agreed (fade roots 3538 both / 7 DCLF's alone / 3 the engine's alone). The engine set kAccumulated on
    2,000-5,800 geometries a window that DCLF's verdict left out, DCLF alone on about 400, and 300 "cell roots" a window had no bit.
    -   **Not the HZB.** The geometry verdict (BuildDrawsCS phase 1's `cs.dclf.frustum`) was already the frustum and portal test before
        the HZB.
    -   **The causes:**
        -   most is stale engine bits on geometry under a node or root the cull failed or whose `OnVisible` stopped (no reader takes those:
            the receivers test every level);
        -   decal, forward (Effect cloud cards) and refraction inputs were never stamped, so their geometry read out of view;
        -   faded roots (DCLF's clear is the decided meaning);
        -   the readback's lag;
        -   the "cell roots" without the bit were the object root's children 0 and 1, list entries the main cull clears when out of view.
    -   **Changed:**
        -   phase 1 stamps every main input;
        -   the object event carries the fade drop;
        -   FadeStateCS passes always-draw and preprocessed roots without a program as Process1 does;
        -   `CellRoots` counts the containers apart;
        -   the dry run attributes each difference by cause and compares geometry with the receivers' bit.
    -   **Resolved (w190-w196; `drawcall-limit-fix.md`, "T7f: producers 1-6 with the culls still on"):**
        -   The cloud cards came into view after the decoded batch's frame. Each compare list now carries the engine's bit from its own
            sample of that frame; with that sample clear, the root is "arrived" and not steady.
        -   The interior's steady actor roots (`Skeleton.nif`) came from FadeStateCS using the seed's stale kNotVisible. FadeStateCS now
            makes the actor pre-cull's frustum test itself.
        -   The engine's residue geometry (effects DCLF does not draw) is no longer cleared by the wide writes.
        -   `CS_DCLF_ACCUMULATED_ALL` is on by default. Steady classes: 0 in motion and in the forests; interiors 0-1 actor root in 2 of
            30 windows (an actor out of high process keeps bit 12 and the engine passes it untested).
    -   **Resolved (T7f item 7, r1):** the 50-1,450 geometries a window whose parent walk did not reach their listed root all had the
        engine's empty fade node as root (the `BSFadeNode` at `0x14332a2a0` that every `BSShaderProperty` starts with, which the engine
        reads as none). It is no root now (`Engine::EmptyFadeNode`, `CS_DCLF_EMPTY_FADE_NODE`); such geometry takes its plain entry root.
        The count is 0 in every window. The plain entry roots this brought out are attributed like roots (`drawcall-limit-fix.md`, "T7f item
        7").
-   **`<- FADE STATE` in interiors (w195):** 1-3 actor roots a log, one fade step apart (`currentFade` 0.9888 against the port's 1 or
    0.9968). It appeared with the actor pre-cull change (FadeStateCS's frustum test for preprocessed roots). w191 had the same switches
    without that change and showed none, and w196 showed none. Not attributed yet: check the log's port for the preprocessed roots' last
    visible frame.
-   **`<- FADE INPUT` (w193):** an actor root's row fadeAmount 0.9947 against the node's 1, steady over three windows. An `Actor::SetAlpha`
    write that the events missed, or a fadeAmount writer other than SetAlpha. It surfaced under `CS_DCLF_FADE_ALL`, where every root is
    owned, but the parity compares every listed root, so it is not caused by the switch. Effect: FadeStateCS steps amountFade toward 0.995.
-   **`<- TREE MANAGER` coverage (T7f item 2):** no `BSTreeNode` found at Riverwood, Falkreath, Riften, Raven Rock or Morthal in this load
    order, so `CS_DCLF_TREE_LIST_ALL` stays off until a load order with such trees shows the comparison at 0.
-   **Particles frozen in fx-firstperson's fp8 (22:58):** all 226 systems "newly frozen" for one window, right after a live
    `dclf:first person=1` toggle, then normal again. Not reproduced in w190-w197, which had `CS_DCLF_ACCUMULATED_ALL` and particles on:
    0 newly frozen, except 2 a run in the window of the interior's coc (the systems of the cell being left).
-   **`<- MATERIAL PORT` (w176):** 1 a window left on an effect's FalloffData (VS variable 7) after the retake - a controller
    write the retake missed.

## Crashes observed, probably unrelated to DCLF

Crashes seen in validation runs whose stacks have no DCLF or Community Shaders frames and that match known engine instability. They are
recorded so a repeat is recognised. They are not chased unless one recurs with a DCLF or CS frame on the stack, or with a pattern that
points at DCLF.
-   **tbbmalloc read in an NPC's behaviour graph update (2026-10-17, run lod1-full, uGridsToLoad 5).** `EXCEPTION_ACCESS_VIOLATION` in
    `rml::internal::MemoryPool::getFromLLOCache` (EngineFixes.dll's tbbmalloc, `cmp [rcx+0x48], r8`, rcx `0xAC1C37CF`) under
    `BSSpeedSamplerModifier` / `hkbBehaviorGraph` "0_Master.hkb" for 'Talsgar the Wanderer' (`Character` 0xFF001323), on a moving
    and turning run near Riverwood. A known engine bug. The parity report lines written before it carried only the standing flags
    (frame values' DIFFER, the extras rows' DIFFER, above).
-   **Havok null write in FUN_140b660c0 at uGridsToLoad 21 (2026-10-17, the Whiterun stress save under `CS_DCLF_TEST_MOVE`).** Radius 21
    is unstable on its own. Runs there are judged by the report lines written before the crash.

## Accepted differences

Differences from the engine that are known, measured and left as they are, with the reason. Each one names what would
remove it if it ever matters.

### A root seen again is drawn for one frame with its old fade

**What.** The engine updates a fade root only when its cull reaches it (`BSFadeNode::OnVisible`). A root seen again after
more than a frame unseen is re-evaluated on that same frame, before the draw. A root that should have faded out while it
was out of view snaps out, and is never drawn. `FadeStateCS` runs a frame ahead: frame N's update, from frame N's camera,
is what frame N+1's builds read (`FadeStatesReadIndex`). So on the frame a stood-in root comes back into view, DCLF draws
it with the state from when it was last seen, and it disappears on the next frame.

**Measured** (2026-10-04, teleport route with the camera turning, the engine's own fades, `CS_DCLF_PRIMARY_EXCLUDE=0`).
Over five 10-second reports, 2,126 roots were seen again with a fade above 0 (BSFadeNode 1,286, BSLeafAnimNode 840):
-   1,291 snapped out on that frame (BSFadeNode 780, BSLeafAnimNode 511), each a one-frame draw under DCLF;
-   20 faded out while in view, as the engine's fade update does;
-   the rest stayed in.

Settled, at one place, none snapped.

**Why it is accepted.** A root comes back into view when its bounding sphere first touches the frustum. The sphere
encloses the mesh with a margin, so in one frame the mesh itself has usually not yet reached the screen.

**What would remove it.** The depth build reading this frame's `FadeStateCS` output, ordered after the pass in the same
epoch (the pass on the build's critical path). Or `BuildDraws` applying the engine's re-entry snap to a root whose
published state is long unseen.

**Since T1a** (2026-10-08) the second is in place for the snap out by distance: the first phase applies the fade-out
distance test (`kObjectFadeTest`) to every input whose root's published state has no serviced `OnVisible`, owned roots
included (before, only to roots DCLF did not own). Not yet measured on the teleport route.

### Skylighting parity lags while roots start fading

**What.** The engine's reference render of an occlusion map (`CS_DCLF_SKYLIGHT_PARITY`) culls stood-in roots by their
nodes. Those carry the GPU's fade milestones only a few frames after the GPU state ("The fade write-back" in
[drawcall-limit-fix.md](./drawcall-limit-fix.md): the readback's frame slots, then the next frame's job). DCLF's map
reads the current state.

**Measured** (2026-10-04, two runs). Every map was within 15 texels by more than 1/256, except in the first 17 s after
the last teleport, while the turning camera brought newly loaded roots into view. There a few maps had 2,500-19,000
texels, DCLF always nearer.

**Why it is accepted.** The difference is the reference's latency, not DCLF's map. The engine's own readers of the nodes
(the tree LOD's crossfade) take the same few frames.

**What would remove it.** A parity check that skips roots whose milestone changed within the write-back's latency.

### Shading that changes during a frame is drawn from the next frame

**What.** An object's shading (MaterialData, EmitColor, SSRParams.w, Linear Lighting's emissive multiplier) is FrameValues'
(`BindlessShading`, PS t121), sampled at the frame's start for the slots the walk named by their shading events
(dclf-async-publication.md, "Step 3, second part"). The engine's own draw reads the property when it sets the draw up. A
value written after the frame's start, while the frame renders, therefore reaches DCLF's draws a frame later than the
engine's:
-   a controller on the animation job, which runs alongside the render thread: a candle's or a chandelier's flickering
    emissive colour and multiplier;
-   a cull's `GetRenderPasses`: the alpha it leaves on a non-member's property, and the LOD fades a held fade node keeps
    on the property (a member's LOD fades are the draw's own, LodFadeFrame).

Before, these were resampled at Prepass, the last point before the colour draw. The reflection faces already draw a frame
behind for membership (dclf-lod.md, "The one-frame lag, accepted"); this is the same kind of difference for values.

**Measured** (2026-10-06, x4, the Riverwood bridge, the camera turning). The persistent parity's shading line, at Prepass
against the frame's rows: about 25 of 40,000 slots a check changed after the frame's start, all of them named by an
event for the next frame; 0 queued, 0 missed. Wetness: 0 differ (Advanced Skin's value is computed once a frame, at the
frame's start, before any draw). Capture parity counts the draws it affects on a separate line ("shading changed since the
frame's start sampled it"), and compares those draws with the engine's value now, which checks the sample.

**Why it is accepted.** The user's decision (2026-10-06): a frame's delay on an animated emissive or fade is not
visible. It keeps the render thread out of the shading entirely; the sample runs on the pool, and the GPU waits for it.

**What would remove it.** Sampling the named slots again just before the colour epoch, on the pool, into the frame's
shading backing (a second upload against the frame's timeline). That puts a pool job back on the colour epoch's critical path.

### Tree LOD instances the engine leaves hidden after `allVisible`

**What.** The terrain manager's node update (`FUN_140510730`) sets a tree LOD block's `allVisible` (`+0x82`) when the block
no longer needs per-instance hiding (its flags bit 14 clear), then calls the block's update (`FUN_140503f70`). That update
refills only the groups whose `shaderPropertyUpToDate` (`+0x24`) is clear, and nothing cleared it. So the engine keeps
drawing the records it packed before: every instance that was hidden (scale 0) stays hidden, although `allVisible` says
every instance should show. DCLF draws by the intended rule: when the update finds `allVisible` set, DCLF repacks those
groups with every instance shown (dclf-lod.md, "Tree LOD: the mirror").

**Measured** (2026-10-04, teleport route, three runs). 10-52 shapes in the reports after each load; none once the
camera had stayed at one place.

**Why it is accepted.** The engine's records are stale, not intended: hiding is for instances whose full tree is loaded,
and `allVisible` is the block saying none are. If the engine's behaviour is visible, it shows as holes in the tree LOD where
full trees have since unloaded, which DCLF does not reproduce.

**What would remove it.** Nothing in DCLF; it is the engine's. Not yet confirmed visible on screen. To investigate:
-   **Starting point.** `FUN_140510730` at the `allVisible = 1` write (the branch on the node flags' bit 14), and its call of
    `FUN_140503f70` (`0x1405108ff`). Find what bits 13 and 14 of the node's flags (`+0x40`) mean: which grid the node is in,
    and when they change as the player moves away from a loaded area.
-   **Count it.** `CS_DCLF_PERSISTENT_PARITY`'s "tree LOD mirror" line, `shown by allVisible without a refill`, counts the
    shapes where DCLF and the engine differ. `CS_DCLF_TREE_LOD_AUDIT` (`OpenDefectProbes.cpp`) counts instances by their
    reference's state.
-   **See it.** With DCLF's tree LOD off, teleport away from a forest and look back at it: an engine bug shows as gaps in
    the LOD forest where trees were loaded, lasting until the block is refilled for another reason. Then compare with
    DCLF's tree LOD on.

## External Vulkan work right after a graph epoch: workaround in place, cause unproven, frame generation not covered (2026-10-13)

The canonical rule, the symptom signature and the tests are in `render-graph.md`, "Hazard: external Vulkan work directly after a graph epoch".

-   **Frame generation is not separated.** `Streamline::EvaluateFSRFrameGen` and the DLSS-G path do not call
    `Upscaling::SeparateFromGraphEpochs()`. Frame generation was off in every inn run. Next: call it before those evaluates, and run the inn
    repro with frame generation on.
-   **The cause is unproven.** The workaround (a small D3D11 typed UAV clear before the evaluate) stays until one of these resolves it:
    -   Streamline without `VK_EXT_descriptor_heap`, or with heap state separate from the graph's.
    -   The graph's last command buffer re-binding or resetting the descriptor-heap state.
    -   A capture with a tool that supports descriptor heaps.
    -   A standalone repro to report to NVIDIA.

## Resolved: the first-person weapon and hands were not drawn (found 2026-10-15; fixed 2026-10-15)

**Symptom.** With `CS_DCLF_FIRST_PERSON` on, a readied sword and the first-person hands were missing; with `=0` (the engine
draws them) they showed. The counters were clean throughout: epochs ran on every latched frame, pipelines were ready, and the
C3 bits parity and the set parity's player-3D check reported OK. Parity does not cover what the epochs draw.

**Cause.** The first-person epochs' `VS_PerFrame` used the first-person camera's `world.translate` as `CameraPosAdjust`
(b12 c40), but that translate is in the first-person root's space. The rows are world space (FrameValues samples them before
the engine's first-person shift), so the vertex stage subtracted the wrong eye and the geometry landed off screen. The world
eye (DrawWorld+0x00) was only used for the cull's face eye and the motion vectors' previous camera.

**Fix.** `Features/FirstPerson/FirstPersonEpochs.cpp` `CaptureCamera` sets `view.eye` to the world eye before `PerFrameBlock`.
Confirmed with screenshot runs (`run_game.ps1 -Shots 5 -ReadyWeaponAt 3`; sw-eye against sw-v2). The parity round is clean.

## Resolved: BuildDraws parity row-address MISMATCH (pre-existing; resolved 2026-10-14 with the pooled payload)

**Symptom.** `CS_DCLF_BUILD_PARITY` reported `BuildDraws parity MISMATCH` lines whose first difference was the rows'
addresses (for example `rows 0x26504a0000/0x26500a0000 vs 0x237aa3e100/0x26226f0000`). They were in the pre-refactor
baseline (motion-pre0c) and in every run after it (p3gb, p5gb).

**Cause.** Two causes, both in the check, not in the draws:
-   **The check was also armed on Z-prepass epochs** (every 300th main epoch, whichever kind). The Z-prepass's draws go to
    its pipeline slots' buckets, never to the plain range or the decal slots the check compares. When the check was armed on
    one, it read what the last colour epoch had left there: another frame's output, drawn from another payload.
-   **The two sides took the rows from different buffers.** The GPU's draws carried the payload ring's rows: the frame's own
    buffers, whose addresses the commit wrote over the build's. The CPU templates carried the addresses of the dead named
    row tables.

**Fix.**
-   The check is armed on colour epochs only.
-   The CPU templates carry row offsets relative to their table. The check adds the bases of the epoch's `PayloadVersions`,
    recorded when it is armed.
-   Sequences are matched by (object, index buffer, first index): the partitions of a skin can share one index buffer.

The ring and the named tables are both gone (dclf-async-publication.md, "The payload and frame values as pooled versions").
A row address outside the epoch's tables now names the recent payload that held it.

**Validated.** Growth and parity runs (`CS_DCLF_BUILD_PARITY`, `PERSISTENT_PARITY`, `TABLE_START=small`): 0 mismatches,
with no sequence lacking a template.

## Resolved: first-person and reflection draws read past their sequences (found 2026-10-13; fixed by the scene's set)

**Symptom.** Core validation and syncval at the inn, around the views' growth (i12vk, i16sync):
-   indirect draws past `cs.dclf.first-person.sequences` (`VUID-vkCmdDrawIndexedIndirectCount-maxDrawCount-03143`): stride 92 x
    (521 - 1) + 72 + 20 = 47,932 bytes from a 5,888-byte buffer (the initial 64 draws a face), the arguments read from the next
    buffer in the heap (syncval: a read-after-write against the view's latched copies);
-   DGC with an indirect address inside no buffer (`VUID-VkDeviceAddress-size-11364`): the first-person forward part's arguments
    (`(forwardFirst + forwardCapacity) * 92`) past the same buffer.

**Cause.** The view's plan and its buffer were sized by two independent sizings: the plan from the main sizing, the sequences
from the view's, each named by a revision when its own growth was ready. A revision could name the main sizing's growth before
the view's, so its shape planned 521 draws a face against the 64-draw version its recording bound.

**Fix (structural).** One requirement-driven set (`SceneBufferSet`; `dclf-architecture.md`, "Growable tables"). Each view is
planned from the one `MainLayout`, the plan's draws (its buckets and forward range) are a requirement on the very sequences entry
the view's recording resolves (`RequireView`), and the shape's range is that entry's named units. `Seal` refuses any set that does
not hold the requirement, so no shape can index past its buffer. `CheckShapeCapacities` (persistent parity) checks every shape's
ranges against the named versions.

**Validated.** p5vk and p6vk (inn, method 3, core validation): no 03143 and no 11364. p6inn: `cs.dclf.first-person.sequences: 64
grown to 8192` before the first first-person seal, every first-person epoch drawn (300 of 300). p6g (`CS_DCLF_TABLE_START=small`,
persistent and set parity): `shape capacities: 617 revisions, 3690 shapes checked against their sets' buffers: 0 findings`.

## Resolved: the Sleeping Giant Inn's black or garbage upscaler output (found 2026-10-12; fixed 2026-10-13, outside DCLF)

This entry is the investigation record. The rule that came out of it, and where it applies, is in `render-graph.md`, "Hazard: external
Vulkan work directly after a graph epoch".

**Symptom.** At `coc RiverwoodSleepingGiantInn` with DCLF on, frames were black, or white and magenta in the top third, or right,
changing frame to frame. Every DCLF-on run reproduced it (about 25 runs; nearly every screenshot black or garbage, and consecutive
dumped frames zeros, garbage, right), with DLSS and with FSR alike. DCLF off, and exteriors, were clean.

**Where it starts (readbacks).**

-   The target dump (`CS_DCLF_TARGET_DUMP`) now also stages the upscaler's output at every point, adds an "after evaluate" point
    (before the copy over kMAIN), and writes the output's raw texels at "before upscale" and "after evaluate". The output texture is not
    touched during the frame; the evaluate itself writes it: frame 699 zeros, 700 garbage (±65504, alpha intact: the upscaler's own
    values, not a stomp), 701 and 702 right, from inputs with the same statistics.
-   An in-command-buffer readback (`InCbReadback` in `Upscaling/Streamline.cpp`, on dump frames) copies the colour, motion vectors and
    depth inside the upscaler's own command buffer just before the evaluate, and the output just after: the inputs are what the dump
    sees (same min/max/mean), the colour input is unchanged after the evaluate, and the output is the corrupt one. So the upscaler
    computes garbage from correct inputs and constants.

**What decided it (16-second inn runs, 5 screenshots each).**

-   `CS_DCLF_FIRST_PERSON=0`: clean. Narrowed in code: not submitting DCLF's first-person colour epoch: clean; the epoch submitted with
    no draws (colour, forward and depth passes recorded empty), with no image declared, and without its semaphore waits: still bad.
    Replacing the epoch with a D3D11 flush or with an empty interop submission: clean.
-   Device idle before or after the evaluate: clean; waiting for the upscaler's own previous evaluate (1 or 2 in flight), waiting for the
    first-person epoch's completion, or idling the upload or compute queue: still bad. The corruption needs the upscaler's command buffer
    to run on the GPU right behind the graph epoch.
-   A D3D11 UAV clear of a small typed texture (which DXVK runs as a compute shader binding image descriptors) between the epoch and the
    evaluate: clean; a typed-buffer clear (a buffer fill) there: still bad.
-   Not it (all tested): freed ORG memory reused (logged buffer VAs, never freeing any ORG memory), DGC preprocess pages (kept, 4x
    padded), ORG's deletion depth (4x), imported D3D11 images (kept alive), the upscaler's image views (kept), frame-values
    wait-before-signal, the upscaler's D24S8 depth (an R32 depth instead), the compute queue (off), early flush.

**Cause (as far as a black box allows).** The upscalers run through Streamline on the descriptor-heap path: a Streamline plugin asks
for `VK_EXT_descriptor_heap` ("requested by a plugin(s) added" in the log), which the render graph has enabled on DXVK's device (DXVK
itself stays on descriptor buffers on NVIDIA). When the upscaler's command buffer runs directly behind a graph epoch's command buffer,
which binds the graph's descriptor heaps, the evaluate computes from the wrong descriptors on NVIDIA's driver; DXVK work that binds
image descriptors in between, or the GPU going idle, avoids it. DCLF's first-person colour epoch, at `RenderFirstPersonView`, is the
only epoch with no such DXVK work after it before the upscaler (the forward epoch is followed by the engine's textured draws).

**Fix** (`Upscaling::SeparateFromGraphEpochs`, `src/Features/Upscaling.cpp`): before each evaluate, while the render graph is active,
DXVK clears a 64x64 typed UAV (the tested separator). No lock, no wait; about one tiny dispatch a frame.

**Validated (2026-10-13):** six inn runs f1-f6, 30 of 30 screenshots right, DCLF drawing (798 sequences, first-person epochs 300 depth
and 300 colour a window), frame rate as before (126-130 fps against 129-131); motion run innfixm: no crash, no device loss, DCLF's
epochs all submitted.

## Resolved: the render thread read the coordinator's tracked map (found 2026-10-12 in refactor run s3m; fixed 2026-10-13)

-   **Crash:** an access violation in `SceneStore::IsTracked` (`tracked.contains`; after the refactor
    `Scene::Coordinator::IsTrackedFromRenderThread`), called from `PrimaryCull::RefreshWalk` <- `PrepareFrame` <- `AfterFullFrustum`
    on the render thread, in a normal-path motion run (s3m). `tracked` is the coordinator's live map, which the scene pass rewrites on
    the lane meanwhile: the render thread broke the rule that it reads only the adopted publication and published tables. Present
    since admission was simplified (bf9eed8b), so not a refactor regression.
-   **What PrimaryCull asked.** `RunAdmission` (every member of a pending entry) and `RefreshWalk` (DCLF members not in the set)
    asked "is this member still tracked?" only as a liveness guard: an untracked member may be freed, so admission waits rather than
    dereference it in `MemberShown`.
-   **Fix (`Core/Handoff/PrimaryCull.cpp`).** Both answer from the cut's candidate snapshot (`cut.candidates->geometries`, the
    frame's `FrameView::GetSunCandidates()`), which comes with the installed publication. The snapshot lists exactly what was tracked
    when it was published: a sun entry is a candidate only while every dependent geometry is tracked (`UpdateSunCandidates`), and an
    untrack or a new tracked dependent rewrites the entry (its signature mixes each dependent and its slot), so the next snapshot drops
    or adds it and replans the entry, which requeues it for admission. A geometry the snapshot lists is held alive while the
    installed publication is (its release, at the untrack after that publication, sits in a later retirement-chain node). Lookups
    are by pointer only. `Coordinator::IsTrackedFromRenderThread` is deleted; `IsTracked` stays for the coordinator thread and capture
    parity (an observer). No lock, no new state.
-   **Difference from the live read:** an engine-drawn member tracked under a different sun entry than its plan's root (nested
    entries) is not listed by this entry; it now waits for the entry's next rewrite instead of passing at once. Admission counts did
    not move (below).
-   **Validated (2026-10-13, P/refactor/R3 v2*):** motion normal (v2m): primary exclusion per window 216/192/201/215 entries stood in
    (baseline dtm 218/190/199/214), admitted 3.8-7.5 a frame (dtm 4.1-9.9), set members 4,8xx/6,0xx/4,4xx as dtm, 0 crash, 0 device
    loss; motion parity (v2mp, SET + PERSISTENT + dbg.env): commit parity 5 checks 0 differ `<- OK` every window, set parity claims
    `<- OK`, stand-in walk parity 18/18 `<- OK`, DIFFER 24 (frame values and extras only, dtmp 30); release guard (v2g, dedicated motion run): scene lane 0, executor/pool 0, 25 of 25 windows `<- OK`. The
    normal set before the fix (v1: lights, bridge, inn) was identical to the diagtrim baseline (798 / 847, 9350 / 9901).

## Resolved: shadowed terrain drawn black under Terrain Blending (T7c1; fixed 2026-10-11, validated: probe matches the engine path, screenshot lit)

**Symptom.** With DCLF drawing the terrain under Terrain Blending (`CS_DCLF_TB_TERRAIN`, default on), shadowed terrain
was completely black. With DCLF off, or `CS_DCLF_TB_TERRAIN=0` (the engine draws the terrain), it kept its ambient light.

**Measured** (`CS_DCLF_TARGET_PROBE` at a terrain pixel: the G-buffer after DCLF's colour epoch against the engine path
after Terrain Blending's replay; pixel permutation MULTI_TEXTURE LANDSCAPE DEFERRED TRUE_PBR, TERRAIN_BLENDING). The
normals (rt2.xy) matched. DCLF's draw wrote Albedo (rt3), Reflectance (rt5), Masks (rt6) and Diffuse (rt0) as 0, and
glossiness (rt2.z) 0.96 where native wrote 0.287.

**Cause.** TruePBR's landscape `SetupMaterial` binds each tile's displacement (t80-t85) and RMAOS (t86-t91) through its
own `extendedRendererState`, not the engine's sixteen slots. DCLF's material record (`MaterialRecord`) held only the
sixteen slots and Advanced Skin's t71/t74, so the material port (`MaterialPortFeature.cpp`, `Landscape`) wrote the
colour and normal tiles and dropped the other twelve. Those registers fell through to the frame record, which holds
whatever was bound when the frame was captured: nothing. A full-PBR tile (`PBRFlags` bit per tile) therefore sampled
RMAOS as 0. Every output in the probe follows from that, in `Lighting.hlsl`:
- roughness `clamp(0, MinRoughness, ...)`, so glossiness `1 - roughness` is about 0.96;
- AO 0: `PBR::GetIndirectLobeWeights` multiplies the diffuse lobe by `MultiBounceAO(baseColor, 0)` = 0 and the specular
  lobe by `SpecularOcclusion(..., 0)` = 0. The Albedo target is that diffuse lobe (`outputAlbedo`) and Reflectance is
  that specular lobe, so both are 0. Masks.z is the luminance of the ambient times `outputAlbedo`, so it is 0 too.
- In shadow, ambient is the only light and it is multiplied by the albedo, so Diffuse is 0: black only where shadowed.

The colour tiles were bound correctly (t0-t5, the same views as the engine's). The albedo was 0 only because of the AO.

**Fix.** TruePBR's twelve slots are now feature textures: `kFeatureMaterialRegisters` is t71, t74, then t80-t91
(`ConstantEvaluator.h`). The rest of the path was already driven by that table and follows it: the material bindings, the
material row's `features`, the layout's ranges, the pulled shaders and `DCLFMaterialTextureIndex` (`DCLFObjects.hlsli`,
which now maps t80-t91 to words 34-45). `Landscape` writes each tile's displacement (default black) and RMAOS (default
white) from the snapshot's views, as TruePBR's hook does. The snapshot had already captured them (`PBRTextureFields`).
`EvaluateFeatureHooks` now writes only Advanced Skin's entries. The stand-in (`RunStandIn`) saves TruePBR's pending
state (`TruePBR::Get/SetExtendedTextures`), clears it for the call, collects what the call set into the engine's record,
and restores it. The material port parity therefore compares the twelve views, and the evaluation no longer leaves them
pending for the next native draw. Capture parity has a TruePBR line (`TruePBR texture parity`: t80-t91 at the native
landscape draws against the record). The feature binding parity no longer counts these registers as per-frame bindings.

**What to check.** The probe at the same pixel should match native: glossiness about 0.29, and Albedo, Reflectance, Masks.z
and Diffuse non-zero in shadow. `TruePBR texture parity OK` and `material port` without feature-texture differences
under the parity switches.

## Resolved: DCLF's draws carry the TAA / upscaler jitter

The earlier evidence was that every G-buffer target DCLF wrote at one road pixel held the same value frame after
frame while the native draw's moved. That came from the empty sampler descriptors, which return the same texel for
sub-pixel movement.

**Measured (2026-09-28, standing, full featureset).** Under `CS_DCLF_STATS` the colour epoch compares the
`ViewProj` it packs (VS_PerFrame c8) with other matrices, over 300 epochs:

- the same buffer's unjittered matrix (c12): differs by 3.6e-4 on average;
- the previous epoch's: differs by 5.9e-4 on average, with the camera still, so it moves with the jitter;
- the b12 the native main pass binds at that moment: identical (0);
- Community Shaders' cached frame buffer: identical (0).

DCLF projects with the engine's jittered matrix, bit for bit. The comparison stays in the report ("colour epoch
ViewProj over 300 epochs").

## Resolved: DCLF's draws lost the sun where Terrain Shadows' penumbra collapsed

**Symptom (2026-10-02, Riverwood save).** With DCLF on, the whole forward view was in shade: no direct sun and no
specular on anything DCLF draws. Native objects (actors with `CS_DCLF_ACTORS=0`) stayed lit. A live toggle flipped it
both ways. The shadow maps, the shadow mask, the VSM copy and the sun's colour and direction (b13 and SharedData) all
matched native.

**Cause.** `TerrainShadows::GetTerrainShadow` returned `saturate((z - lower) / (upper - lower))`. Where nothing occludes
a texel, Terrain Shadows' update blends the penumbra's two heights together, so after a few seconds of gameplay the
divisor is a difference of two identical `lerp`s. Its sign then depends on how the compiler rounds the arithmetic, and
the compilers differ. In the engine's FXC shaders, translated by DXVK, the divisor came out as exactly zero, so the
result was +inf and saturated to 1 (lit). In DCLF's DXC pipelines it read 0 (shadowed) at the same pixel with the same
inputs. With extra debug code around the call, the same DXC function read 1 again. The factor multiplies the sun's
colour (`GetWorldShadow`), so every DCLF surface lost the sun, and only once the heights had converged: the first frames
after a load were lit.

**Measured** (`CS_DCLF_SHADOW_DEBUG_OUTPUT` with Lighting.hlsl writing each factor, `CS_DCLF_TARGET_PROBE=1170,215` on a
sunlit roof):

| | DCLF on | Native |
| --- | --- | --- |
| Sun colour read (b13), after the colour transform | 1.84 | 1.84 |
| After height fog, caustics and world shadow | 0.00 | 1.44 |
| Terrain shadow | 0.0 | 1.0 |
| Its inputs: texel, U, world z, both decoded heights | 0.4172 / 0.4172, 0.5298, -3202, -5396 / -5396 | the same |
| Specular target (rt4) | 0.0000 | 0.003-0.006 |

**Fix.** `GetTerrainShadow` returns the step at the lower height where the penumbra is 1e-3 units or less, and divides
only where there is a penumbra. Its other callers (volumetric fog, Effects11's volumetric rays) share the fix. After
it, DCLF's specular at the roof is 0.004-0.008 against native's 0.004-0.005, and the view is sunlit with DCLF on.

**Why no parity check caught it.** Every check compares DCLF's inputs with the engine's: capture parity (descriptors,
constants, textures, samplers), capture-point parity (the frame's bound buffers and views), and persistent parity (the
published frame lighting against fresh evaluations). Every input matched. The difference arose inside the shader, where
the two compilers handle a degenerate division differently, and no check compares what DCLF's draws write with what the
native draws write at the same pixels.

## Resolved: the VSM soft shadow matches native

The earlier reading (DCLF 0.0005, native 1.0 at a road pixel) was taken while every DCLF sampler was an empty
descriptor, and the VSM lookup samples through `LinearSampler`.

**Measured (2026-09-28).** `CS_DCLF_SHADOW_DEBUG_OUTPUT=1` now makes Lighting.hlsl write the sun's shadow terms into the
Diffuse target, native and DCLF alike: `dirSoftShadow`, `dirVSMDetailedShadow` and `dirDetailedShadow`.
`CS_DCLF_TARGET_PROBE` read a 64 x 64 block at a still view while `CS_DCLF_TEST_TOGGLE` switched DCLF off and on three
times. The soft shadow rose from 0.62 to 0.76 over the run, as the sun moved. DCLF's and native's samples interleave
on the same curve, for example DCLF 0.630, native 0.624 and 0.651, DCLF 0.717 to 0.760, native 0.747, with no offset
between them.

## Resolved: fixed draw limits (16,384 per view, 2,048 per decal group)

**What they did.** Every sequence buffer held a compile-time `kMaxDraws` (16,384) draws, and each indirect draw's max
count was clamped to it. The main pass skipped draws past it on the CPU (`Skip::Capacity`) and capped its resident
region's share; the decal groups clamped at 2,048; a shadow view's max count came from its inputs, not its draws (a
skin draws once per partition), and a view with more draws dropped the tail while still claiming the casters.

**Fix (2026-09-28).** No draw limit: every sequence buffer (the main pass's, and each shadow view slot's) is grown on
the render thread before its epoch to hold every draw the scene's tracked objects can produce (`SceneDrawBound`: one
per object, or one per partition of a skin), and the main buffer's ranges (phase 1, phase 2, each decal group) are passed
to `BuildDrawsCS` and `SortSequencesCS` in their constants instead of fixed in the shaders. A view's max count is its
mode's own draws (`ShadowPayload::modeDraws`). A bound over the device's `maxIndirectSequenceCount` (4,194,303 on the RTX
3090 Ti; BasicRHI now reports it as `IndirectCommandsFeatureInfo::maxSequenceCount`), or over the sort's rank field
(2^20, the key now takes 12 bits and the rank 20), is a hard failure (`stl::report_and_fail`) until the draws are split
over several calls. At the new save the bound is past 16,384: the buffers grew to 32,768 on the first frame.

**Validated.** `CS_DCLF_TABLE_START=small` (64 draws, 16 per decal group) grew to 32,768 and 256 on the first frame;
`BuildDraws` parity 18 of 18 and decal parity 158 slots, 0 differ; set parity no damage, nothing withheld and drawn by
nobody; persistent parity 0 differ; `SKYLIGHT_PARITY` in range.

## Resolved: the render graph's upload submission failed (intermittent), and the scene vanished after it

**Evidence.** Four runs between 2026-09-28 and 2026-09-29 disabled the graph with `Async epochs could not submit their
uploads (rhi::Result InvalidArgument)`, seconds to minutes in, with no device loss. In `p6-cb2` (2026-09-29, the camera
being turned by hand) the new BasicRHI message named the rejected command: `Vulkan buffer copy rejected: 1024 bytes ...
(+12288 of 2097152), the list not recording`. The copy was in range: the command list was not recording. After the
failure every object DCLF had claimed stayed withheld from the engine's batch renderer and was drawn by nobody (80,000 to
190,000 holes per 300 frames, `static ownership: ... 1205 claimed but not drawn`), and the frame rate fell.

**Cause 1: ORG's stale-ticket reprepare raced the host thread** (`PersistentGraphHost::SubmitTicket`). The render thread
fixed its frame slot from the ticket, and on a stale ticket posted `Discard` and then `PrepareNow(slot)`, two messages.
`Discard` marked the epoch as needing a ticket, so the host thread's proactive pass could prepare one between the two
messages, into a slot of its own choosing. The render thread took that ticket from `WaitTicket` but recorded its uploads
into the list of its original slot, which nobody had begun again; the late `PrepareNow` then recycled a list that was in
use. It needed a stale ticket (DCLF's shadow and Z-prepass epochs go stale on a few percent of frames, more while the
camera turns) and the host thread winning that window, hence the rarity.

**Fix (2026-09-29, both ORG copies).** A discard now carries the slot to reprepare into, and the host thread prepares the
replacement in the same step, so no other ticket for the epoch can appear in between; `PrepareNow` is gone. The render
thread fails hard (`std::logic_error`) if the ticket it gets back is not for the slot it records.

**Cause 2: claims outlived a dead graph** (`IndirectDraws::PublishClaims`). A claim is dropped a frame after its last
draw, and "not drawn any more" was only learnt from a colour build's changes. With no colour epoch nothing reported the
undraws, so the claims stood forever; the shadow claims likewise, republished only by a successful shadow epoch.

**Fix.** A frame with no colour commit undraws every drawn slot (so the claims lapse on the usual one-frame path) and asks
the next colour build for every slot; while the graph is not running the shadow claims are cleared every frame.

**Validated.** `p6-fix-parity` (camera turning, build, persistent and resident parity): every check OK, 0 holes, 0 claimed
but not drawn, no errors.

## Resolved: device loss in every DCLF draw pass after the split records (Phase 3)

**Symptom.** With Phase 3's per-draw push data (the pipeline row's and material row's addresses and the object word,
20 bytes), the Z-prepass, colour and shadow passes lost the device within a second: `DMA_PageFault`, no fault address,
usually no active shader, at a rate rising with the draws executed.

**Cause.** A generated commands push data token over 16 bytes, on NVIDIA (GA102, driver 616.56). Isolated on top of the
last committed state: a 12- or 16-byte token was healthy and a 20-byte one lost the device, whatever the shaders read;
the same 20 bytes as two tokens was healthy. Neither the extension nor the device's limits state a bound.

**Fix.** BasicRHI's Vulkan backend splits a `Constant` indirect argument into push data tokens of at most 16 bytes
(`kMaxPushDataTokenBytes`, `rhi_vulkan.cpp`; its README's backend conventions). DCLF's signature and stream layout are
unchanged. Validated with the full featureset (two runs) and with the build, persistent and set parity checks on (two
runs): no device loss, `BuildDraws` and decal parity OK, the persistent tables 0 differ, set parity without damage. (One
of the parity runs hit the intermittent upload failure above, which disables the graph without a device loss.)

## Parity checks that already fail on the baseline

Found while validating the code cleanup (Stages 0-4 of the cleanup plan), by running every parity check on
commit `908a0529`, before the cleanup, and on each stage after it, at the Whiterun road save with the full
featureset. Each count below is the same before and after the cleanup, so the cleanup did not cause any of them. A
later stage's parity runs are compared against these numbers, not against zero.

Runs: `CS_DCLF_PERSISTENT_PARITY`, `WALK_PARITY`, `CHANGE_LOG_PARITY`, `RESIDENT_PARITY`,
`RESIDENT_DRAW_PARITY`, `BUILD_PARITY` and `SKYLIGHT_PARITY` together; and
`CS_DCLF_CAPTURE_PARITY=1` with `CS_DCLF_OWNERSHIP=off` on its own. 60 seconds each; counts are per report
interval (300 frames). Every check not listed reports 0.

The BuildDraws parity row-address MISMATCH that was listed here is resolved: see "Resolved: BuildDraws parity row-address
MISMATCH" above.

### Capture parity and draw parity MISMATCH under CAPTURE + PARITY_BOTH (2026-10-13; pre-existing, not a refactor regression)

**Runs.** Motion (60 s, the worldspace flight) with `CS_DCLF_CAPTURE_PARITY`, `PARITY_BOTH`, `CAPTURE_POINT_PARITY` and
`SKYLIGHT_PARITY`: the pre-refactor build (b1069616, the R0 DLL with b1069616's DCLF shaders; run pre0c) against the current tree
(dtc, v2c). The counts match window for window, so the refactor changed none of them:

| window | b1069616 (pre0c) | current (v2c) |
| --- | --- | --- |
| 1 (load) | 552,626 of 970,297 (549,351 material) | 554,021 of 970,209 (549,852 material) |
| 2 | 84,359 of 163,173 (77,313 material, 74 per-geometry, 3,459 technique) | 84,192 of 159,934 (76,773, 169, 3,338) |
| draw parity, windows 1-5 | 3, 5, 12, 11, 7 differ | 3, 5, 12, 11, 7 differ |
| permutation parity, windows 2-5 | 2,367, 2,944, 3,334, 7,066 | 2,298, 3,045, 3,325, 6,808 |

Capture point parity is OK in every window. Earlier capture runs gave no baseline: the last ones (2026-10-11) checked 0 draws (no
`PARITY_BOTH`), and the "R0 DLL no longer starts" of the diagnostics trim was only the run script's deployed-DLL hash guard.

**What mismatches, by field** (window 2's variable histogram and samples; the other windows alike):
-   **Material (all of it): PS PerMaterial 29, IBLParams.y**, e.g. `WRFenceBaseSupport01:47` DCLF 0, native 0.745; LOD `obj`, every
    LODObjects / LODObjectHD draw, most treeAnim. The record keeps IBLParams from its first evaluation on purpose
    (`MaterialSources::KeepUnreadFloats`): it drifts with the time of day and no Lighting stage reads it (Lighting.hlsl declares it
    at PerMaterial c8 and never uses it; dclf-async-publication.md, m1). **The observer compares wrongly**: `CompareMaterial`'s
    mask (`kPSGroups[kPerMaterial]`) still includes variable 29.
-   **Technique: PS PerTechnique 19 (FogColor) and VS PerTechnique 13-15 (FogParam, FogNearColor, FogFarColor)**, small drifts
    (FogColor 0.13725 against 0.13787). The technique rows keep their first fog (`KeepTechniqueFog`); the draw reads the vertex fog
    from the frame block (`DCLFFrameFog`, VS b13, under `DCLF_BINDLESS`) and no Lighting stage reads FogColor. **The observer
    compares rows the draw does not read.** A few `HighDetailRange` (VS 12, LODLandNoise) differences at load.
-   **Loaded terrain blocks drawn with another technique** (`Block (x, 0)`): DCLF's pipeline is MTLandLODBlend (19: descriptors VS
    13000001 / PS 13010019) where the engine's pass is MTLand (8: 08000001 / 08010019). Permutation parity's `vertex 1B000000`
    (= 0x13 ^ 0x08) is this, about 900 a window; `pixel 800000` (about 1,700-1,900) and `feature 1C0` (about 300) were not
    attributed. **The normal path draws a different technique** than the engine chose that frame (the shading of the
    land-to-LOD blend). Not chased.
-   **Per-geometry** (74-175 a window, 0 in most): PS PerGeometry 7 on a facegen tint part (`MarksMaleHumanoid06LeftGash`, DCLF 1
    against 0.944) and VS PerGeometry 6 (TextureProj); the counts follow the NPCs in view.
-   **Character light t11** on NPC armour (`outfit_1`, `merchanttorso_1`): DCLF's frame view (mode 0) against the draw's (mode 3);
    a handful of samples. The engine alternates the view frame to frame (`FrameCharacterLightView`).
-   **Draw parity** 0-15 of 50k-980k draws a window (different arguments or bound buffers), unattributed.
-   **Face capture parity**: every reflection face draw differs from its object's main row (face lighting PS PerGeometry 3-5 and the
    IBL / fog fields above): the face draws are not expected to match the main rows.

**Next.** Leave variable 29 out of `CompareMaterial`, and the fog variables out of the technique comparison when the draw reads
the frame fog, so that the report shows the real differences (the terrain technique, the per-geometry and t11 samples, draw
parity). Then look at why the land block's pipeline keeps MTLandLODBlend when the engine's pass is MTLand.

### Fade visibility parity differed in single windows (the check was removed in the 2026-10-13 diagnostics trim)

The CPU inference below was the check's own weak point; the fade path is checked by the fade port and fade state parities
(`CS_DCLF_FADE_PARITY`). What it showed:

**Evidence.** `CS_DCLF_FADE_PARITY`'s "fade visibility parity" line, which compares the engine's cull of entries with
engine-drawn parts against FadeStateCS's test, is 0 in almost every 300-frame window. Occasionally one window differs:
-   2026-10-07, x7, at the Riverwood bridge with the camera turning: the last window had 4,815 of 34,040 checks differ. The
    first was `'FXAmbWaterFishBucket01A.nif' slot 0: the engine culled, the port visible (compound accepted at op 63)`.
    The other 16 windows of that run, and every window of x5 and x6 (the same build except the fade uploads' runs), had 0.
-   2026-10-07, y11 and y13 (step 6d, the published tables replayed), the same place: y11 2 windows (1,014 and 10,500 differ, first
    `FXAmbWaterFishBucket01B.nif`), y13 4 windows (116, then 6,000 a window, first `IronMace01`, a loose item, "the engine culled,
    the port visible (compound accepted at op 99)"); y12 (the same code as y11 but the extras block log) 0. The check reads no
    DCLF table, so the replay cannot change a verdict; which object differs varies by run.
-   2026-10-08, y59 and y73 (y73: P0 of the persistent-views rework, which removed only inactive probes), the same place: 3-4
    windows (851, then 5,700 a window), first `'GourdCluster01' slot 0: the engine culled, the port visible (compound
    accepted at op 93)`. y60-y72 had 0.
-   Earlier sessions show the same class, in single windows, at other places: 3-37 differ, with "the engine visible, the
    port culled (compound rejected at op 22-110)" (`AkaviriKatana`, `BearTrap01`, `SpitPotClosed01`; logs junk-landOwn*,
    junk-trav15).

**What it is not.** The check runs on the CPU, in the list jobs' stand-in calls (`PrimaryCull.cpp`, `VisibilityPort` on the
job's sampled block in `fadeVisibility`). It reads no uploaded data, so it says nothing about the GPU's copy of the blocks or
the fade uploads; the GPU-side fade parities (port, state, roots with engine-drawn parts) were 0 in the same windows.

**Suspected causes, not yet tested.**
-   The engine's verdict is inferred: the node's LOD metric (+0x144) or last-visible frame (+0x13C) changed across its
    `Process1`. A node that another process already updated this frame, or whose OnVisible returns early without writing
    either field, reads as "culled" even when the engine kept it. The run of 4,815 differences in one window, all one way,
    fits a node class that the inference misreads, rather than a test that is wrong for single nodes.
-   The block is sampled at the job's first stand-in call. If the compound frustum's operators or plane sets change after that
    point within the job (63 operators here, near the block's sampled count), the port tests a stale program.

**To investigate.** Log the first differing node's class, the frame's two process calls on it, and its +0x13C/+0x144 before
and after; and compare the block sampled at the first stand-in call with one sampled at the differing call.

### Resolved: the kept shadow state missed casters once their materials outnumbered the record slots

**Evidence (2026-09-28, the new save).** `CS_DCLF_PERSISTENT_PARITY` split the missing inputs by why the kept build
lacked them: every one was "waiting", with a record slot of 792-799 against a capacity of 512 (`'Symbol'`, `'shoes'`,
`'HairMaleImperial1'`). The count grew from 8 to 168 per interval as NPCs came into view.

**Cause.** The kept state gives every alpha-tested material of every tracked caster its own record slot: about 800 at
this save, against the 110-130 a per-frame build of one frame's casters needs. The records were copied into each of
the 16 view slots (only the view's `b0` and `b12` differed), so their capacity was a compile-time 512 per slot, and a
slot at 512 or above never became ready: its casters waited for ever and stayed the engine's.

**Fix.** The shadow draws' registers were split by what they depend on, so that nothing caps the materials:
- a material row per slot (`ShadowMaterialRow`, 256 bytes: the Utility vertex shader's `PerMaterial` block and the
  diffuse's descriptor index), in one table every view reads (`ShadowResources::materialRows`);
- the view's blocks and the frame record (every other texture and sampler) in push data, once per view
  (`DrawPipelines.h`, `kShadowPushWords`).

The table grows (`GrowableRows`, `Buffer::ResizeBytes`) on the render thread before the build when the last build
wanted more rows; a slot past the capacity waits one frame. Kept slots are taken lowest-first, so the table stays
dense.

**Validated.** Persistent parity 0 differ in every interval, with 797 rows held at this save (it grew once, 256 to 1,024
rows, and 540 materials waited that one frame). With `CS_DCLF_TABLE_START=small` (4 rows) it grew once to 1,024 and
stayed at 0 differ. `SKYLIGHT_PARITY` in its usual range (DCLF farther 0, nearer 935-1,398 texels), which checks the new
layout's diffuse and pushed blocks on the GPU. The broader move away from fixed capacities is planned in
`dclf-architecture.md` ("Growable tables").

### Walk parity: the cooking spit's sun entry moves without an event

**Evidence.** `CS_DCLF_WALK_PARITY`: 5 of about 113,900 objects differ per interval, every one the sun entry
on `FireSptiCookingBase:17` (entry node `FireSpitCooking`), with bounds that drift by about 0.06 units:
`kept (16225.32 -5390.81 -4241.06 r 167.36) now (16225.31 -5390.87 -4241.05 r 167.36)`.

**Cause (inferred).** Something animates the spit's node, and no event DCLF watches reports it, so the delta
walk keeps the old bound. The effect is negligible at this size, but the same gap on a larger animated
object would cull it wrongly.

**Next step.** Find which writer moves the node (a controller, or Havok) and add it to the events.

### Walk parity: an NPC's shield changes its hidden bit during the scene phase

**Evidence.** On the `CS_DCLF_TEST_MOVE` flight (`200:1100:15`), 0-5 of the 8 walk-parity intervals report 2-4
stale verdicts on the parts of one NPC's shield (`Shield:0`, `Symbol`). The delta walk evaluated them that frame and
read the chain one way; the reference walk, run right after it in the same `BuildScenePhase`, reads it the other
way. The report's chain shows the node that flips: `SHIELD` under `NPC L Hand`. Nothing DCLF runs between the
two walks writes node flags: the scene placement job's palette update (`FUN_140e4ff90`) only locks, allocates,
copies and multiplies, and a probe that re-read every actor verdict before and after the placements found none
changed. The bit is written by another thread while `Main::Draw` runs. The actor-side `kHidden` writers include the
weapon draw handlers and `AnimationObjectDrawHandler` (`dclf-event-driven-tables.md`, "Reverse-engineering
results"). Since the scene placement job, a parity frame runs the placements inline before the reference walk,
so the two reads are about 1 ms further apart, and the parity catches the flip more often. It is not a change in
what the delta walk reads.

**Why it matters.** The scene phase reads a bit the engine is writing, so for that frame the shield's record can
follow either state. This is the race `dclf-event-driven-tables.md` describes for the worker walk, still present
for a writer that runs during `Main::Draw`.

**Status (hidden events).** Every store that can change `kHidden` is now an event (`HiddenStores.cpp`), and an actor's
frame verdict is taken again only when a node on its chain had one. A write inside `Main::Draw` after the walk is an
event the next walk takes, so the record follows the new state one frame later instead of racing it. The first build
patched only the `mov` stores that had a load of the same flags nearby. On one parity frame its witness reported
`'Shield:0' eligible now hidden` with no event. With every `mov` store to `+0xF4` patched, no witness has missed one:
standing, the flight, an equip and `killall` scenario, and a capture run. The writer itself is still unnamed.

**Next step.** Name the writer: log the stub's return address for the first event on a `SHIELD` node.

### Capture parity: two tracked draws excluded while NPCs crowded the view

**Evidence.** One `CAPTURE_PARITY` run with `OWNERSHIP=off` reported "2 tracked but excluded" in its third interval,
while NPCs filled the view (native draws fell from 29,100 to 3,500). Every other capture run reports 10 in the first
interval only (one NPC's face parts at startup) and 0 after. The check counts a draw DCLF left out and logs it only
when it is hidden now, so a missed hidden-to-shown change would pass silently. It did not recur in the next run, and
the hidden witness reported 0 misses in both.

**Next step.** The report now breaks these draws down by their verdict now, naming the first of each ("tracked but
excluded, by the verdict now"). If it recurs, an `eligible` entry there is a verdict DCLF kept stale.

### Fixed: capture parity: `EyePosition` differs on a candle lantern after a load

**Fixed (2026-10-02).** `SetupGeometry` writes `EyePosition` for any pass with descriptor bit 9 or one of `0x21c00`, not
only for Envmap, Eye and technique 0x10. The tables kept such a pipeline's eye from its first evaluation; they now copy
the frame's eye into every pipeline that writes it (`WritesEyePosition`). Complete capture parity has no `EyePosition`
mismatches. The history follows.

**Now (2026-09-29, `val-p3-capture`).** 45 mismatches in the first interval, 1 in the second, 0 after: all
`EyePosition` on one `CandleLanternWithCandle` (`DCLF -25480.871, native 0`). The beards and hair lines below no longer
mismatch. What follows is the entry as first recorded.


**Evidence.** `CS_DCLF_CAPTURE_PARITY` with ownership off: 600-900 of about 9,600 checked draws mismatch per
interval, all in VS PerGeometry variable 2 (`EyePosition`, written only by the Envmap, Eye and 0x10
techniques). Materials, techniques, bones, lights, permutations and draw arguments all match. The main pass's
render flags are only 0x41 and 0x45. The report's samples give the two values: the tables hold the eye (for
example `DCLF -25480.893, native 0` on `HumanBeard28`, `HairLineMaleNord07` and a candle lantern) where the native
draw's constant buffer holds 0. So this is not a timing difference between two eye samples: on these draws the
native `SetupGeometry` leaves the component at zero, and the tables write it.

The first interval after a load also has a few `PS PerGeometry 8` mismatches (3-7, a candle lantern's emissive
multiplier, `DCLF 1.0403805, native 1.0114646`): an animated emissive sampled once per frame, like the flicker
that `RefreshFrameConstants`' shading resample exists for. It does not recur after the first interval.

**Next step.** Find the branch of `BSLightingShader::SetupGeometry` (AE `0x1414dd040`) that writes `EyePosition`
and the condition under which it writes zero, and apply the same condition in the tables (the coordinator's
PerGeometry evaluation), not a per-frame sample.

### Primary exclusion never applies at this save

**Evidence.** `primary exclusion: applied on 0 of 300 frames (0 stale, 300 preconditions)` in every
interval. `PrimaryCull` skips the frame when any of these hold: there are no list processes, there are more
than the cut holds, `SunAccumulation::ExclusionLive()` is false, or a local light cast shadows last frame.
The save has a cooking fire, so the last is the likely one.

**Next step.** Count each precondition separately in the report.

### Fixed: every validated material record was reported stale

`ValidateMaterialSlice` reported every record it checked as stale (`validated 8, stale 8 <- STALE MATERIAL`,
with every float printed as `nan->nan`). The live and the served records were identical: unwritten components
hold `kUnwrittenBits`, a NaN, and `MaterialRecord::operator==` compared floats with `==`, which is never true
for a NaN. The constant blocks are now compared bit for bit (`ConstantBlock::SameBits`).

## Probes added for this investigation

-   `CS_DCLF_TARGET_PROBE=x,y`: every bound G-buffer target, averaged over a 64 x 64 block, where the opaque
    pass ends, DCLF on or off. Whole pixels of the render resolution (2560 x 1440 under DLSS Quality at 4K), the
    block's top-left corner; fractions parse as 0.
-   `CS_DCLF_SHADOW_DEBUG_OUTPUT=1`: a global shader define (`DCLF_SHADOW_DEBUG`) that makes Lighting.hlsl
    write chosen intermediate values into the Diffuse target, native and DCLF alike, cached apart.
    Changing the deployed Lighting.hlsl makes Community Shaders recompile its whole shader cache on the next
    launch, and DCLF does not start until that finishes.
-   The colour epoch's report now names frame textures a pipeline reads that the commit could not resolve.
-   The scene report counts the objects whose pass descriptor carries the sun's shadow mask bits and the
    objects with derived descriptors.
-   The fetch guard (`CS_DCLF_FETCH_GUARD`, `Lighting.hlsl`'s `DCLF_FETCH_GUARD` and its push words) was removed on 2026-10-13. Both
    faults it localised are fixed: the faces drawing before the frame's commit, and the index pool's empty grown `pool-firsts`.
