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

## DCLF's draws appear not to carry the TAA / upscaler jitter

**Probably explained by the empty samplers; to re-measure.** A sampler that neither filters nor selects
mips returns the same texel for sub-pixel movement, which is what this measured.

**Evidence (inferred).** With `CS_DCLF_TARGET_PROBE` at one road pixel, every G-buffer target DCLF writes
holds the same value frame after frame (normal, roughness, AO to four decimals), while the native draw's
values move every frame. The camera is still, so the native variation is the sub-pixel jitter; DCLF's lack
of it suggests its vertex stage runs without the jitter. The colour epoch replays the Z-prepass's
vertex-stage constants (`prepassVS`), which come from the native depth pass.

**Why it matters:** temporal AA and the upscalers accumulate over the jitter sequence, so unjittered objects
lose their anti-aliasing and resolve detail, and may shimmer against jittered native objects.

**Next step:** compare the projection that DCLF's epochs pack (`VS_PerFrame.ViewProj`) with the jittered
one the native main pass binds, on the same frame.

## The VSM soft shadow differs at the road

**To re-measure after the sampler fix:** the VSM lookup samples through `LinearSampler`.

**Evidence.** Pixel shader telemetry (`CS_DCLF_SHADOW_DEBUG_OUTPUT`, `ShadowSampling::GetLightingShadow` at
the road pixel): DCLF 0.0005, native 1.0, from the same permutation, the same t18 VSM and the same t98
cascade data (`CS_DCLF_SHADOWS=0`, so the shadow maps match native). It only feeds soft lighting and coat
terms, so it is not the brightness, and it is not explained yet.

**Next step:** the VSM lookup inputs per pixel: the light-space position, the cascade picked and the moments
read, native against DCLF.

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

### The kept shadow state misses some clamped-cascade inputs

**Evidence.** `CS_DCLF_PERSISTENT_PARITY`'s shadow state check: 100-260 of about 556,000 inputs differ per
interval, and the first is always `object N: mode 1: an input of the per-frame build only`. The kept shadow
state holds fewer inputs for render mode 1 (clamped) than a per-frame build of the same frame.

**Why it matters.** Those are casters that the kept build leaves out of a clamped shadow view, so they may be
missing shadows, unless the per-frame build is the one that is wrong.

**Next step.** Log the missing objects' names and which kept-state event should have added them.

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

**Next step.** Find the thread and the writer: which function sets `SHIELD`'s bit at that point in the frame (the
weapon draw and animation object handlers first). Then make it an event (the hooks of the scene tables plan's
step 5), and read the shield's chain from the event, not live.

### The scene placement probe: one item moves inside its window

**Evidence.** Under `CS_DCLF_ASYNC=probe` the scene placement join takes every item again and counts those that
moved since the job read them: one item a frame (about 300 an interval), always `ImperialSwordBloodAdd`. An engine
writer moves that geometry between the end of the scene tables and `BeforeShadowMaps`. The records take it at the
end of the scene tables, as the light path always has, so this is not a change the job made.

**Next step.** Identify the writer (the candidates in that window are `FUN_140742470`, which `Main::Draw` calls with
the player's position, and the first-person culling), and decide whether the record should take the later value.

### Capture parity: `EyePosition` differs on 600-900 draws

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
    pass ends, DCLF on or off.
-   `CS_DCLF_SHADOW_DEBUG_OUTPUT=1`: a global shader define (`DCLF_SHADOW_DEBUG`) that makes Lighting.hlsl
    write chosen intermediate values into the Diffuse target, native and DCLF alike, cached apart.
    Changing the deployed Lighting.hlsl makes Community Shaders recompile its whole shader cache on the next
    launch, and DCLF does not start until that finishes.
-   The colour epoch's report now names frame textures a pipeline reads that the commit could not resolve.
-   The scene report counts the objects whose pass descriptor carries the sun's shadow mask bits and the
    objects with derived descriptors.
