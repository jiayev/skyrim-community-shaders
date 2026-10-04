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
`RESIDENT_DRAW_PARITY`, `BUILD_PARITY`, `SKYLIGHT_PARITY` and `CS_DCLF_ASYNC=probe` together; and
`CS_DCLF_CAPTURE_PARITY=1` with `CS_DCLF_OWNERSHIP=off` on its own. 60 seconds each; counts are per report
interval (300 frames). Every check not listed reports 0.

### The async probes compare two different builds

**Evidence.** `CS_DCLF_ASYNC=probe` reports every shadow build as different (`constants: 227840 vs 154128
bytes`, 230-282 of 230-282), and 6-70 of 300 colour builds (with an empty difference, which `SamePayload`
leaves for a bone-row or drawn-change count).

**Cause.** The probe rebuilds on the render thread with `BuildShadowPayload(job.inputs, tables, lookups,
probePayload)` and `BuildMainPayload(job.inputs, tables, lookups, probePayload)`, without the kept stores
(object records, bones, geometries, the kept shadow state and the build cache). The worker's build uses them.
Since the kept stores became the only path, the probe compares a kept build against a from-scratch one, which
is `CS_DCLF_PERSISTENT_PARITY`'s job, and it says nothing about whether the worker and the render thread agree.

**Fix.** Compare like with like: either build the reference from the same kept stores (as a snapshot, since
the build advances them), or compare only the parts that do not depend on them.

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

### The scene placement probe: one item moves inside its window

**Evidence.** Under `CS_DCLF_ASYNC=probe` the scene placement join takes every item again and counts those that
moved since the job read them: one item a frame (about 300 an interval), always `ImperialSwordBloodAdd`. An engine
writer moves that geometry between the end of the scene tables and `BeforeShadowMaps`. The records take it at the
end of the scene tables, as the light path always has, so this is not a change the job made.

**Next step.** Identify the writer (the candidates in that window are `FUN_140742470`, which `Main::Draw` calls with
the player's position, and the first-person culling), and decide whether the record should take the later value.

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
and the condition under which it writes zero, and apply the same condition in the tables (`SceneStore`'s
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

-   `CS_DCLF_SHADOWMAP_PROBE=1`: the sun's cascade texture and the VSM copy, summarised per slice and mip.
-   `CS_DCLF_SHADOWMASK_PROBE=x,y`: the engine's shadow mask at a pixel.
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
