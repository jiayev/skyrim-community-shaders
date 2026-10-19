# DCLF validation

For compact portal bytecode and fade-root/member query batching tests and current integration limits, see
[Portal query batches](./dclf-portal-query-batches.md).

How Drawcall Limit Fix (DCLF) is checked: its switches, the parity checks and what each report line means, the diagnostics
and probes, how a change is validated in game, crash triage, and the bugs parity found in the engine and in other features.

-   [dclf-architecture.md](./dclf-architecture.md) is how DCLF works. This document names its mechanisms only where a check
    needs them.
-   [dclf-open-defects.md](./dclf-open-defects.md) lists the checks that fail or flake today, the open crashes and the accepted
    differences. A flag listed there is not a regression.
-   [dclf-layout.md](./dclf-layout.md) is the code layout, including the diagnostics contract (`Diagnostics/`).
-   [dclf-roadmap.md](./dclf-roadmap.md) lists diagnostics not built yet.
-   [skyrim-engine-notes.md](./skyrim-engine-notes.md) holds the engine behaviour the engine-bug rules below rest on.

Every engine address here is Skyrim AE 1.6.1170's, the only runtime DCLF installs on.

## Contents

-   [Switches](#switches)
-   [Parity checks](#parity-checks)
-   [Diagnostics and probes](#diagnostics-and-probes)
-   [How to validate a change](#how-to-validate-a-change)
-   [Crash triage](#crash-triage)
-   [Bugs found by parity](#bugs-found-by-parity)

## Switches

### How switches are read

-   A switch is read from the environment first, then from `<Documents>\My Games\Skyrim Special Edition\SKSE\CommunityShaders-DCLF.ini`,
    a flat `NAME=value` file beside `CommunityShaders.log` (`Core/Settings/Switches.h`, `Utils/SwitchFile.h`). The file exists
    because Mod Organizer 2 hands the game its own environment block, so variables a launcher exports may never arrive.
-   Some switches are read from the environment only: the render graph's `CS_ORG_*`, `CS_GPU_*`, `CS_NVPERF_*`, and DXVK's and
    the Vulkan loader's variables. The switches file does not carry them.
-   Every switch is read once, on first use, so none changes under a running frame. Feature switches register at install
    (`RegisterSwitch`, `Api::FeatureSwitch`), Core's are the registry in `Core/Settings/Switches.cpp`.
-   The startup log prints `[DCLF] switches: ...`, grouped as features, parity, diagnostics, test and other. A `CS_DCLF_*` name
    that nothing registered is listed as `UNKNOWN (read by nothing)`, so a misspelt or removed switch does not silently do nothing.
-   It also prints `[DCLF] featureset: full ...`, or warns `[DCLF] featureset: REDUCED by ...` naming each feature switch set to
    a narrowing value. A validation or timing run must show `full`.
-   Most feature switches are the seeds of live toggles: the DCLF menu, or `dclf:<toggle name>=0|1` through
    `CS_DCLF_TEST_COMMANDS`, changes them in a running game.

Defaults: a feature marked "on" is on when unset. "Unless 1" means any value other than `1` turns it off; "unless 0" means
only `0` turns it off. Parity, diagnostic and test switches are off unless set.

### Feature toggles

| Switch | Default | Meaning |
| --- | --- | --- |
| `CS_DCLF` | on (unless 0) | `0`: DCLF does not install. |
| `CS_DCLF_PRECOMPILE` | on (unless 0) | `0`: DCLF's SPIR-V programs compile on first use instead of alongside the engine's shaders. |
| `CS_DCLF_CULL` | `occlusion` | `off`, `frustum` or `occlusion`: GPU culling. `off` and `frustum` count as a reduced featureset. |
| `CS_DCLF_SKINNED` | on (unless 1) | `0`: skinned objects stay native. |
| `CS_DCLF_SKIN_PARTITIONS` | on (unless 1) | `0`: skins of several partitions (LOD trees, actor bodies) stay native. |
| `CS_DCLF_ACTORS` | on (unless 1) | `0`: actors stay native. |
| `CS_DCLF_TREES` | on (unless 1) | `0`: trees stay native. |
| `CS_DCLF_DECALS` | on (unless 1) | `0`: decals stay native. |
| `CS_DCLF_LAYERS` | on (unless 1) | `0`: multi-index shapes' main passes stay native (cave walls with an ice or snow layer). |
| `CS_DCLF_MTLAND` | on (unless 1) | `0`: terrain stays native. |
| `CS_DCLF_TB_TERRAIN` | on (unless 1) | `0`: terrain under Terrain Blending, and the meshes that feature redraws after it, stay native. |
| `CS_DCLF_FORWARD` | on (unless 1) | `0`: blended Lighting geometry stays native (no forward epoch). |
| `CS_DCLF_EFFECTS` | on (unless 1) | `0`: blended `BSEffectShaderProperty` meshes stay native. Needs `CS_DCLF_FORWARD`. |
| `CS_DCLF_REFRACTION` | on (unless 1) | `0`: refraction (heat haze, invisibility) stays native. Needs `CS_DCLF_FORWARD`. |
| `CS_DCLF_PROJECTED_UV` | on (unless 1) | `0`: projected-UV objects stay native. |
| `CS_DCLF_SWITCH_NODES` | on (unless 1) | `0`: objects under switch nodes (trees, harvestables) stay native. |
| `CS_DCLF_LOD_OBJECTS` | on (unless 1) | `0`: object LOD stays native and untracked. |
| `CS_DCLF_LOD_TERRAIN` | on (unless 1) | `0`: terrain LOD stays native. |
| `CS_DCLF_LOD_TREES` | on (unless 1) | `0`: tree LOD stays native. |
| `CS_DCLF_LOD_GATES` | on (unless 0) | `0`: the engine's LOD swaps are ungated. By default a swap keeps the outgoing level shown, and the incoming hidden, until DCLF's claims cover the incoming. |
| `CS_DCLF_SHADOWS` | on (unless 1) | `0`: DCLF does not draw the shadow views. |
| `CS_DCLF_SUN_SKIP` | on (unless 1) | `0`: the engine keeps culling and registering the sun's casters. |
| `CS_DCLF_SUN_EXCLUDE` | on (unless 1) | `0`: DCLF's objects stay in the sun's culls. |
| `CS_DCLF_PRIMARY_EXCLUDE` | on (unless 1) | `0`: DCLF's objects stay in the main camera's cull. |
| `CS_DCLF_TREE_LIST_ALL` | **off** | `1`: the tree manager's list loop gets DCLF's verdict for every `BSTreeNode` DCLF lists; off: for the stood-in trees alone. |
| `CS_DCLF_SKYLIGHT` | on (unless 1) | `0`: Skylighting's occlusion map stays native. |
| `CS_DCLF_PRECIPITATION` | on (unless 1) | `0`: the precipitation occlusion mask stays native. |
| `CS_DCLF_REFLECTIONS` | on (unless 1) | `0`: the water reflection's faces stay native. |
| `CS_DCLF_FIRST_PERSON` | on (unless 1) | `0`: the first-person arms, weapons and magic hands stay native. |
| `CS_DCLF_FIRST_PERSON_DECALS` | on (unless 0) | `0`: decals on first-person entries (an effect's blood on the arms, a weapon's `BloodLighting`) stay the engine's. Needs `CS_DCLF_FIRST_PERSON` and `CS_DCLF_DECALS`. |
| `CS_DCLF_OPAQUE_EFFECTS` | on (unless 0) | `0`: opaque effect meshes (hint 0: forge coals, slag) stay the engine's. Needs decals. |
| `CS_DCLF_PARTICLES` | on (unless 1) | `0`: effect-shaded particle systems stay native. Needs `CS_DCLF_EFFECTS`. |
| `CS_DCLF_BILLBOARDS` | on (unless 1) | `0`: effect meshes under an `NiBillboardNode` (fire cards, spray jets) stay native. Needs `CS_DCLF_EFFECTS`. |
| `CS_DCLF_BILLBOARD_PARTICLES` | on (unless 0) | `0`: particle systems under billboard nodes (the magic hands' fire cores) stay the engine's. Needs billboards and particles. |
| `CS_DCLF_EMPTY_FADE_NODE` | on (unless 0) | `0`: the engine's empty fade node (the `BSFadeNode` at `0x14332a2a0` every `BSShaderProperty` starts with) is listed as one fade root of all its geometry, instead of no root. |

### Parity checks and observers

Each compares a live path with a reference built from scratch and flags every mismatch with a `<-` tag. The sections under
[Parity checks](#parity-checks) say what each reports.

| Switch | Meaning |
| --- | --- |
| `CS_DCLF_SET_PARITY` | `1`: the frame's set against what the GPU drew and what the engine registered. Also the feature observers and, with persistent parity, the view parities. |
| `CS_DCLF_PERSISTENT_PARITY` | `1`: every kept store (records, bindings, slots, shading, geometry, kept builds) against a rebuild, every 60 frames, and many structure checks. |
| `CS_DCLF_WALK_PARITY` | `1`: the delta walk against a dense walk, every 60 frames. |
| `CS_DCLF_MIRROR_PARITY` | `1`: the scene mirror against the live objects, a slice of the tracked set a frame. |
| `CS_DCLF_CHANGE_LOG_PARITY` | `1`: the change log against a diff of the tables. |
| `CS_DCLF_RESIDENT_PARITY` | `1`: resident records against their passes built again. |
| `CS_DCLF_BUILD_PARITY` | `1`: BuildDraws' GPU output against the CPU's templates, and the index pool. Turns the resident region off. |
| `CS_DCLF_CAPTURE_PARITY` | `1`: the tables against the engine's own Lighting draws. Run with `CS_DCLF_PARITY_BOTH=1`. |
| `CS_DCLF_PARITY_BOTH` | `1`: nothing is withheld from the engine, which draws everything while DCLF draws its set too (double draws). |
| `CS_DCLF_CAPTURE_POINT_PARITY` | `1`: the colour epoch's captured bindings against the frame's first native Lighting draw. |
| `CS_DCLF_ACCUMULATED_PARITY` | `1`: DCLF's decoded visibility against the engine's `kAccumulated`, and the animation gate (`FUN_1402cff60`) against DCLF's writes. Off, the readback and the writes still run. |
| `CS_DCLF_FADE_PARITY` | `1`: the fade port against the engine's functions, and `FadeStateCS` against the port. |
| `CS_DCLF_SKYLIGHT_PARITY` | `1`: DCLF's occlusion maps against the engine's, texel by texel. |
| `CS_DCLF_FOLIAGE_PARITY` | `1`: the alpha-tested colour draws' pixels against the frame before's, tree wind frozen; `wind`: wind running; `land`: wind frozen and terrain LOD coverage counted. |

### Probes and diagnostics

| Switch | Meaning |
| --- | --- |
| `CS_DCLF_STATS` | `1`: the periodic report, every 300 frames. |
| `CS_DCLF_PASS_STATS` | `1`: per-pass counts in the report. |
| `CS_DCLF_PROFILE` | `1`: scene-phase timings in the report. The profile perturbs what it measures. |
| `CS_DCLF_CENSUS` | `1`: the registration census, every report, with or without `CS_DCLF_STATS`. |
| `CS_DCLF_SHADER_DEBUG` | Default on: SPIR-V with debug info, and the HLSL sources copied beside the log. `0`: without. It inflates the depth pipeline groups. |
| `CS_DCLF_SHADER_SOURCE_DIR` | Where `CS_DCLF_SHADER_DEBUG` copies the HLSL sources. |
| `CS_DCLF_TARGET_PROBE` | `x,y`: the G-buffer targets at a pixel where the opaque pass ends, and the objects whose bound covers it. |
| `CS_DCLF_SHADOW_DEBUG_OUTPUT` | `1`: Lighting.hlsl writes the sun's shadow terms into the Diffuse target, native and DCLF alike. |
| `CS_DCLF_TARGET_DUMP` | `f1,f2,...`: at those Present counts, every target bound at each dump point, written as half-size PNGs with NaN and Inf marked, plus statistics, under `SKSE/dclf-dump/<count>`. |
| `CS_DCLF_SKYLIGHT_DUMP_DIR` | Where `CS_DCLF_SKYLIGHT_PARITY` writes the maps it compares. |
| `CS_DCLF_SCHEDULE_DUMP` | `1`: each epoch's passes as gathered, logged whenever a list changes. |
| `CS_DCLF_FRAME_TRACE` | `1`: every frame's adoption, epoch coverage, main epoch outcomes, colour culling counts and retirement point, and F10 as a marker. |
| `CS_DCLF_TREE_LOD_AUDIT` | `1`: tree LOD instances the engine leaves shown over a loaded, visible full tree (and hidden ones without one), every 60 frames. |
| `CS_DCLF_HIDDEN_WATCH` | `1`: hardware watchpoints name the store behind a hidden change no event announced (with walk parity). |
| `CS_DCLF_MIRROR_WATCH` | `1`: hardware watchpoints on a fade node's statics after its placement name the writers the mirror misses. |
| `CS_DCLF_RELEASE_GUARD` | `1`: the engine objects' last releases on DCLF's threads (the scene lane, the pool), counted and named by class (`NiRefObject::DeleteThis` patched). |
| `CS_DCLF_DRAW_TRACE` | `1`: the callers of the D3D11 draws. |
| `CS_DCLF_DEPTH_TRACE` | `1`: the D3D11 calls that touch the main depth buffer, for five frames. |
| `CS_DCLF_TRACE_TEXTURE_PATHS` | Set: texture identities are logged with their paths. |
| `CS_DCLF_TABLE_START` | `small`: the growable GPU tables start with a few rows, so growth runs early and often. |

### Test harness

`Diagnostics/TestHarness.cpp`, run every Present.

| Switch | Format | Effect |
| --- | --- | --- |
| `CS_DCLF_TEST_COMMANDS` | `<frame>:<command>;...` | A console command at a frame. Also `dclf:<toggle name>=0\|1` (a live toggle), `dclf:pov=first\|third`, `dclf:draw=1\|0` (weapons or magic hands drawn or sheathed) and `dclf:rebuild` (a graph rebuild). |
| `CS_DCLF_TEST_MOVE` | `<start>:<end>:<units per frame>;...` | Carries the player forward. |
| `CS_DCLF_TEST_TURN` | `<start>:<end>:<degrees per frame>;...` | Turns the player. |
| `CS_DCLF_TEST_TOGGLE` | `<off frame>:<on frame>...` | Switches DCLF off and on. |
| `CS_DCLF_TEST_EXIT` | `<frame>` | The game exits after that many frames. The count includes the main menu's frames, so below about 200 the game exits before DCLF draws. |

### Tuning

| Switch | Default | Meaning |
| --- | --- | --- |
| `CS_DCLF_WORKERS` | hardware threads - 2 | The preparation pool's threads (at least 1). |
| `CS_DCLF_FANOUT` | `2` | The scene pass's evaluation: `0` interleaved on the scene lane (the reference), `1` every entry evaluated then merged on the lane, `2` evaluated on the preparation pool from 128 entries, merged in order. |
| `CS_DCLF_ASYNC_PRIORITY` | unset | `normal`: the coordinator and scene threads keep normal priority instead of above normal; `noboost`: workers are not raised to a `ParallelFor` caller's priority. |
| `CS_DCLF_PAYLOAD_APPEND` | off | `1`: a payload part whose changes all lie past the sealed mark appends into the last backing instead of catching up a pooled one. |

### Other switches DCLF runs depend on

Read from the environment only.

| Switch | Where | Meaning |
| --- | --- | --- |
| `CS_ORG_EPOCHS` | registry, `RenderGraphRuntime.cpp` | `0`: the render graph runs without epochs, and DCLF does not install. |
| `CS_ORG_ASYNC_EPOCHS` | registry | `0`: epochs are recorded on the render thread. A reduced featureset. |
| `CS_ORG_CLOSED` | registry | `0`: epochs do not return their resources to their home states. |
| `CS_ORG_BATCH_SUBMIT` | registry | `0`: an epoch's submissions go to DXVK one at a time. |
| `CS_ORG_EARLY_FLUSH` | registry | `0`: DXVK's pending work is not flushed when an epoch starts. |
| `CS_ORG_COMPUTE_QUEUE` | `RenderGraphRuntime.cpp` | `0`: BasicRHI's compute queue aliases the graphics one. |
| `CS_ORG_UPLOAD_QUEUE` | `RenderGraphRuntime.cpp` | `0`: no spare transfer queue for uploads. |
| `CS_ORG_SUBMIT` | `RenderGraphRuntime.cpp` | `flush`: each epoch flushes and waits for DXVK's command stream instead of enqueuing its submission. |
| `CS_ORG_REUSE_RECORDINGS` | `RenderGraphRuntime.cpp` | `0`: epochs are prepared again instead of replaying kept recordings. Kept recordings carry no Tracy GPU zones, so turn it off to see the epochs' passes in Tracy. |
| `CS_ORG_EPOCH_STATS` | `RenderGraphRuntime.cpp` | `1`: render-thread CPU time per epoch, and the tickets prepared again after their inputs changed. |
| `CS_ORG_DETAILED_CHECKPOINTS` | `RenderGraphRuntime.cpp` | `1`: ORG records detailed command checkpoints for Aftermath. |
| `CS_ORG` | `RenderGraphRuntime.cpp` | `0`: no render graph; `features`: the interop device features are requested but no graph is made. |
| `CS_UPSCALE_SUBMIT` | registry | `direct`: upscaling's ring submissions wait for DXVK's command stream. |
| `CS_STREAMLINE_REFLEX` | registry | `1`: Streamline loads Reflex, and with it DLSS-G. |
| `CS_GPU_CRASH_ANALYSIS` | `Aftermath.cpp`, `DxvkLoader.cpp` | `1`: the device is created with DXVK's hang mode. See [Crash triage](#crash-triage). |
| `CS_GPU_EVENT_TIMERS` | `GpuEventTimers.cpp` | `<frames>`: GPU time per engine event, logged every that many frames. Works with DCLF off. |
| `CS_GPU_IDLE_TRACE` | `GpuIdleTrace.cpp` | `<frames>`: GPU idle attribution, one summary per that many analysed frames. |
| `CS_NVPERF_RANGES` | `Features/RemoteControl/NvPerf.cpp` | A headless Nsight Perf range capture (with `CS_NVPERF_START_FRAME`, `CS_NVPERF_SAMPLES`, `CS_NVPERF_METRICS`, `CS_NVPERF_CSV`). See [nsight-perf.md](./nsight-perf.md). |

`CS_ORG_LLF_PARITY` (`LightLimitFix.cpp`) is Light Limit Fix's own parity and is not DCLF's. The tree wind report's text names
a `CS_DCLF_TREE_LIST=0` mode, but no such switch exists: the animation list is always DCLF's.

## Parity checks

### How the checks work

-   **The normal path does no validation.** It trusts its producers. Every check is an observer the run installs at
    `Diagnostics::Install()`, before DCLF's threads start (`Diagnostics/Observation.h`, `Hooks.h`). The main path knows the
    observers only through `Diagnostics::Observe()` and the questions in `Diagnostics/Policy.h`, whose answers with every switch
    off are the normal path's.
-   **Some checks change what the main path does,** and `Diagnostics/Policy.cpp` names each change:
    -   persistent, walk and capture parity run the scene pass inline once a frame (`Policy::ScenePassInline`), so a parity run
        never exercises the scene pump, several passes a frame or the builder's pacing;
    -   mirror parity runs the fan-out serially (`Policy::SerialFanout`);
    -   persistent parity walks every stood-in entry (`Policy::WalkEveryEntry`), and on its frames leaves the scene lists and
        the point lights' culls whole (`FilterSceneLists`, `FilterLightCulls`) and builds the kept shadow exclusion in full
        (`ReuseShadowExclusion`);
    -   build parity turns the resident region off (`ReuseResidentRegion`);
    -   `CS_DCLF_PARITY_BOTH` withholds nothing (`WithholdFromEngine`);
    -   foliage parity records pixels and may freeze tree wind (`RecordFoliagePixels`, `FreezeTreeWind`).
-   **Schedule.** The rebuild checks run every 60 frames or scene passes, each at its own offset so their rebuilds do not share
    a frame (`Diagnostics::ParityDue`). Report lines come every 300 frames (`Reports::kInterval`). GPU readbacks are read a few
    frames after their epoch: set parity three frames later.
-   **Verdicts.** A line ends in ` <- OK`, or in a tag naming what differed, often followed by `first: ...` naming the first
    case. A line with nothing checked carries no verdict. `<- DIFFER` is the generic tag of many stores' checks
    (`Diagnostics/ParityCounter.h`). Some capture-parity lines use `OK` or `MISMATCH` in place of a tag.

Reading a log:
-   Read every report interval, not only the tail. A run that visits two cells reports two answers.
-   A check that suppresses a computation also suppresses everything downstream of it.
-   A flag listed in [dclf-open-defects.md](./dclf-open-defects.md), "Failing or flaky parity checks", is known. Anything else is
    a regression until shown otherwise.

### Set parity

`CS_DCLF_SET_PARITY=1`. Each colour epoch's visibility words (stamps and verdicts) are copied with the CPU's side that explains
them and read back three frames later (`Diagnostics/Draws/SetParity.cpp`).

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] set parity over N frames (...): depth without colour, colour without depth, in the set and drawn by nobody, in the set and GPU-culled, ...` | `<- SET PARITY` | A member drawn by nobody, an object drawn outside the set, the depth and colour segments disagreeing, or a record disagreeing with the set. |
| `[DCLF] set parity over N frames: members rejected with their live bound in view: ... by the frustum, ... by the fade test ...` | `<- FADE` | A member the fade test rejected while the engine's fade says it is shown. |
| `[DCLF] set parity over N frames: the registration hooks' main claims against the set: ...` | `<- CLAIMS` | A claim outside the set (drawn by nobody), or a member left unclaimed. |
| `[DCLF] set commit parity at frame N: ...` | `<- COMMIT` | The set's commit against a full evaluation (`Diagnostics/Scene/SceneCommitParity.cpp`). |
| `[DCLF] player 3D against the claims (set parity) ...` | `<- PLAYER 3D` | A hidden player skeleton's geometry claimed for the main view past the publication's latency (8 frames): DCLF draws it where the engine draws nothing (`Diagnostics/Feature/OpenDefectProbes.cpp`). |
| `[DCLF] terrain under Terrain Blending (sampled frame): ...` | `<- TB NEAR` | The GPU's near test for Terrain Blending's terrain against the CPU's over the same inputs. |
| `[DCLF] forward (T7d, sampled frame): ...` | `<- FORWARD` | Fewer forward inputs in the frustum than blended passes of members the engine registered: a member DCLF dropped. |
| refraction line | `<- REFRACTION` | The same for refraction passes. |
| `[DCLF] effects' Lighting variant (CS_DCLF_SET_PARITY): ...` | `<- EFFECT LIGHT` | An effect member's engine pass with a light 0 other than the sun. |
| `[DCLF] particles' engine passes (CS_DCLF_SET_PARITY): ...` | `<- PARTICLES` | A particle member's withheld pass without a recent snapshot. |
| `[DCLF] billboards' engine turns (CS_DCLF_SET_PARITY): ...` | `<- BILLBOARD` | DCLF's turn of a billboard leaf against the engine's for the culling camera. It sees only billboards the engine still culls. |
| `[DCLF] first person's registrations ...` | `<- FIRST PERSON` | A first-person member's registration the engine kept while DCLF drew the view. |
| first-person filing line | `<- FIRST-PERSON FILING` | A first-person slot filed under another category node. |
| paced publication line (`Core/Scene/Coordinator/ScenePump.cpp`) | `<- PACED PUBLICATION DIFFERS` | A paced publication's tables or claims against one made at once. |

Set parity also checks the main set's state classes: an indirect execution set's pipelines must agree in everything but their
shaders, so DCLF keeps one set and signature per state class (both variants' fixed-function state and fragment interface) and draws
each class's sub-range of every colour range with its own call. A draw in another class's sub-range would be drawn with that class's
fixed-function state.

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] set class parity (version of N pipelines, N classes): ...` (logged when found) | `<- SET CLASS` | A published set version's class failed a check (`CheckClasses`, `Core/Gpu/Pipelines/MainPipelines.cpp`, after each publish): a member's class word is not its (class, index); its state differs from its class's initial pipeline's (the differing fields named: what the original single-set defect looked like); the handle written at its index is not its own pipeline's; a pipeline is in no class or two; two classes have equal state. |
| `[DCLF] set classes (CPU, published versions so far): N checks, N failed` | `<- SET CLASS` | The count of the above over the run (`Reports/Frame.cpp`, with `CS_DCLF_STATS`). Cumulative: the failures were logged when found. It checks the keys the builds made (`PipelineDesc::State`); a field the key leaves out is invisible to it. |
| `[DCLF] set classes (GPU, colour epoch N): N classes, N sub-ranges, N draws checked (...), N without a class sub-range, N over capacity; N checks, N failed` | `<- SET CLASS; first: ...` | Every 300th colour epoch (`Diagnostics/Draws/SetClassParity.cpp`): the sequences, count words and `classCounts` copied after the epoch and read without waiting a few colour epochs later, against the shape's class plan and indirect state. Per (range, class) sub-range, every sequence below its count with an index count (decals: the group's count; forward: the staging half and the sorted half) names a pipeline its class's sets hold, at the index one of its object's inputs' set indices maps to in that class; no count is past its capacity, no class without a sub-range counted a draw, the unplanned and overflow words are 0, the plan matches the shape (classes, map, sizes), and with DrawSort each class's main sub-range is in pipeline order. It does not cover the first-person view's forward range. |
| `[DCLF] set classes (GPU, colour epoch N): the readback could not be mapped ...` | none | The copy was still in flight after 60 colour epochs, or a map failed: that window is not checked. |
| `[DCLF] state classes (sampled frame): N colour draws without a class sub-range, N past their sub-range's capacity` | `<- SET CLASS DROP` | Every run with `CS_DCLF_STATS`, from the culling counters: BuildDraws dropped a colour draw (each one an object not drawn) for want of a class map word or sub-range, or past its sub-range's capacity. |

Set parity also gates, together with persistent parity, the LOD gates' observers and the view parities below.

### Persistent parity

`CS_DCLF_PERSISTENT_PARITY=1`. The broadest check: every kept store against a rebuild, plus most structure checks. Its lines,
by area:

**Kept stores and publications**

| Tag | Line | Means |
| --- | --- | --- |
| `<- DIFFER` | object records, geometry draws, draw bounds, extras (`[DCLF] extras parity ...`), technique blocks (`[DCLF] pipeline constants: ...`), frame values' rows and palettes, texture transforms, decal order | The kept value against one computed again. |
| `<- MISSED` | `[DCLF] shading (sampled at the frame's start ...)`, actor frame verdicts | A slot whose shading or verdict changed without the event that should name it. |
| `<- KEPT SET DIFFERS` | `[DCLF] frame values' kept placement set (parity)` | The placement set FrameValues kept against one made again. |
| `<- VIEW MASK` | `[DCLF] view mask parity (U3)` | An input whose view mask is not its object's, or lies outside its list's views. |
| `<- NAMED ADDRESS`, `<- NAMED PARITY` | `[DCLF] snapshot draws ...`, `[DCLF] frame N: a commit staged bytes into ...` | A payload address the sealed set does not name, or a recording that resolved a buffer the adopted set does not name. |
| `<- SHAPE CAPACITY` | `[DCLF] shape capacities: ...` | A revision's shape indexing past its set's buffers (`CheckShapeCapacities`). |
| `<- CONSTANT TABLE` | `[DCLF] constant tables (...: DCLF's modules' against ShaderCache's)` | DCLF's reflected constant tables against Community Shaders' shader objects. |
| `<- REPLAY DIFFERS` | `[DCLF] tables replay (6d): ...` | The tables replayed from their logs against the live ones. |
| `<- CANDIDATES` | `[DCLF] sun/light candidates parity ...` | The sun or light candidates against a rebuild. |
| `<- REGION RESYNC` | `[DCLF] main region resync on the pool: ...` | The region's parallel resync against the serial path. |
| `<- PAIRS NARROWED` | `[DCLF] main pairs: ...` | The region's narrowed pair checks against every pair resolved again (`Policy::CheckBuilderPairs`). |
| `<- ROW PACKING CONFLICT` | main build line | Two pairs packed into one row (also checked on every build under build parity). |
| `<- PLACEMENT DELTAS DIFFER`, `<- ROOT SCHEDULE MISSED`, `<- GEOMETRY SCAN MISSED` | placements line | The placement deltas, the root schedule and the geometry scan against full ones. |

**Engine-facing checks** (`Diagnostics/Engine/CutObservers.cpp`, `Diagnostics/Reports/EnginePrimaryCull.cpp`)

| Tag | Line | Means |
| --- | --- | --- |
| `<- CUT` | `[DCLF] cut parity at frame N: ...` | Every 60th frame, the kept cut against one planned whole for the same snapshot. |
| `<- STAND-IN WALK` | `[DCLF] stand-in walk parity: N geometries handed to the registration from entries the stand-in would not walk` | Must be 0. |
| `<- LIGHT MASKS` | `[DCLF] light masks (owned members in view, which no main registration clears): ...` | An owned member in view whose `activeLightMask` is not 0 after the registration jobs. |
| `<- LIST FILTER` | scene lists line | On frames the engine builds its scene lists whole, a root the filter would have dropped wrongly, or the kept lists against a rebuild. |
| `<- LIGHTING SHADER` | `[DCLF] registration parity (the capture drain, an observer): ...` | A registered Lighting pass names a shader other than the engine's instance DCLF's programs are of. |
| `<- FLAG` | `[DCLF] frame-start captures by events (C2 parity): ...`, `[DCLF] first-person bits by events (C3 parity): ...` | A capture or bit that moved without the event that should announce it. |

**Scene mirror and structure** (`Diagnostics/Reports/SceneCoordinator.cpp`, `Core/Scene/Coordinator/Classification.cpp`). These
lines print in every `CS_DCLF_STATS` report. The comparisons behind `<- STALE`, `<- SLOT VIOLATION`, `<- DETACH`, `<- MIRROR APPLY`,
`<- MIRROR USES` and `<- CATEGORY INDEX` run only under persistent parity (the mirror's also under mirror parity); the other
tags are counters of the normal path.

| Tag | Line | Means |
| --- | --- | --- |
| `<- MIRROR APPLY`, `<- MIRROR USES`, `<- CATEGORY INDEX` | classification line | Mirror batches applied wrong, property or alpha records no geometry uses, and the category index against a rebuild. |
| `<- MIRROR EVICTED` | `[DCLF] scene mirror evictions (T6b1b): ...` | Records that left the world by no detach a hook saw. |
| `<- UNPINNED FADE NODE` | `[DCLF] fade node references (T6b3e): ...` | Fade nodes asked for and not answered. It also fires for requests still in flight at the report. |
| `<- UNPINNED PROPERTY`, `<- MASK UNHELD`, `<- SET PATCHED`, `<- STALE`, `<- SLOT VIOLATION`, `<- DETACH` | coordinator lines | A property refused for want of a pin, a shadow mask not held, a member patched after its commit, a cached verdict that differs from one computed again, a slot rule broken, and validation drops of detaches. |
| `<- CAPTURE` | `[DCLF] subtrees (T6b1c, ...)`, `[DCLF] frame capture (6e F2): ...` | New category nodes without their children captured, or unscoped captures. |
| `<- MATERIAL PORT` | material port lines | A material whose port output was not covered by its retake. |
| `<- APPEND BELOW MARK` | pooled payload line | A payload append below the sealed mark (with `CS_DCLF_PAYLOAD_APPEND=1`). |

**Features**

| Tag | Line | Means |
| --- | --- | --- |
| `<- TREE WIND` | `[DCLF] tree wind parity: ...` | Every 120th colour epoch, up to 64 tree members' TreeParams, fades and gusts against the engine's, read from the buffer the draws read. The readback also scans both wind buffers for non-finite values or amplitudes over 1000. |
| `<- TREE LIST` | `[DCLF] tree animation list: ...` | An owned tree still on the tree manager's animation list. |
| `<- TREE LOD MIRROR` | `[DCLF] tree LOD mirror: ...` | The tree LOD mirror against the engine's blocks. |
| `<- LOD SEGMENTS` | `[DCLF] object LOD segments: ...` | Object LOD segments against the shapes' drawn ranges. |
| `<- GATE PARITY` | `[DCLF] LOD gates` line | The LOD gates against a full sweep. |
| `<- LIGHT EXCLUSION`, `<- LIGHT FILTER`, `<- LIGHT BITS`, `<- EMPTY CULL BITS`, `<- SPHERES` | `[DCLF] point lights' ...` lines (`Features/Shadows/Local/Diagnostics/PointCullLines.cpp`) | On parity frames (the culls left whole): a paraboloid pass the skip would lose under an excluded entry, a category filter that missed a child, cut entries' point-light bits against the engine's, bits the empty culls would give against the engine's, and the lights' spheres. |
| `<- SUN EXCLUSION` | `[DCLF] sun entry exclusion: ...` | The sun casters watch: an unclaimed caster under an excluded entry on frames the cascades skip nothing. |
| `<- DIFFER` | `[DCLF] sun entry on the GPU: ...` | The GPU's sun entry test against the CPU's verdict. |

`<- MISSED` on the actor verdicts line is the hidden witness, which walk parity runs too.

### View parities

Under set parity or persistent parity. Each compares DCLF's view, made from the camera the engine set up, with the engine's.

| Tag | Line |
| --- | --- |
| `<- SUN VIEW` | `[DCLF] sun views (T2a: ...)` |
| `<- LIGHT VIEW` | `[DCLF] light views (T3a: ...)` |
| `<- LIGHT SELECTION` | `[DCLF] light selection (T3b, T3c: ...)`: DCLF's choice of shadow lights and focus hosts against `CalculateActiveShadowCasterLights`' |
| `<- LIGHT ROOMS` | the light rooms line: `PortalViews::WalkLight`'s rooms against the engine's walk |
| `<- LOCAL SHADOW VOLUME` | `[DCLF] local shadow volumes (T3b: ...)` |
| `<- FOCUS VIEW`, `<- FOCUS MEMBERS` | `[DCLF] focus views (T3c: ...)`, `[DCLF] focus members (T3c: ...)` |
| `<- SHADOW TARGET` | `[DCLF] shadow targets (T3d: ...)`: the predicted depth target and slice against the renderer's at the draw |
| `<- OCCLUSION VIEW` | `[DCLF] occlusion views (T4: ...)` |
| `<- PORTAL VIEW` | the portal line: DCLF's occluder states and sets against the engine's |
| `<- FACE CAMERA`, `<- REFLECTION RESIDUE` | the reflection lines: the cube faces' blocks and slices, and tree or LOD passes the engine still draws in the faces |
| `<- CAPTURE` | `[DCLF] shadow view capture: ...`: a view whose b12 eye is not the renderer's |
| `<- GATE HOLE`, `<- GATE COVERAGE` | `[DCLF] LOD gate releases (T6b5): ...`, and `[DCLF] LOD gate N: at its release ...` |

The LOD gates' two tags:
-   `<- GATE HOLE`: a flipped incoming geometry not claimed with the flip's phases, in the publication its commit made
    (`<- GATE HOLE (publication)`) or in the claims the releasing frame installed.
-   `<- GATE COVERAGE`: at a gate's release both levels are shown (a double draw) or neither is (a hole). A block the engine hid
    again itself is counted apart.

### Walk parity

`CS_DCLF_WALK_PARITY=1`: the delta walk against a dense walk from scratch, every 60 frames (`Diagnostics/Scene/SceneWalkParity.cpp`).
`[DCLF] walk parity: N checks, N objects compared, N differ, N missing, N extra, N stale verdicts, N stale traits ...`, then
`<- OK` or `; first: ...`, and `[DCLF] walk parity differences by what: ...`. On parity frames the movers the walk skipped are
witnesses: a changed one is a missed event. Clean: 0 differ, 0 missing, 0 extra.

### Mirror parity

`CS_DCLF_MIRROR_PARITY=1`: a slice of the tracked set read live at the frame start against the mirror
(`Diagnostics/Scene/SceneMirrorParity.cpp`, `SceneMirrorReads.cpp`). `[DCLF] mirror parity: ...` flags `<- MIRROR` for a record
absent from the mirror or a field the events missed; `[DCLF] mirror reads (T6b1b): ...` flags `<- MIRROR READ`. Fields are
classed evented, late or missed. `CS_DCLF_MIRROR_WATCH` names the writer behind a missed field.

### Change log parity

`CS_DCLF_CHANGE_LOG_PARITY=1`: `[DCLF] change log parity: N checks (N skipped), N slots compared, N changed, N changed with no
log entry`, flagged `<- MISSING`. A changed slot with no log entry means a kept store would never resend it.

### Resident parity

`CS_DCLF_RESIDENT_PARITY=1`: `[DCLF] resident parity: ...`, flagged `<- RESIDENT PARITY`. Records whose root is fading are not
compared and are counted apart.

### Build parity

`CS_DCLF_BUILD_PARITY=1` (`Diagnostics/Draws/BuildParity.cpp`). Every 300th colour epoch, BuildDraws' output (the sequences, the
decal slots, the local shadow lights) against the CPU's templates.
-   `[DCLF] BuildDraws parity OK: ...` or `[DCLF] BuildDraws parity MISMATCH: GPU wrote ...`, with
    `[DCLF] BuildDraws decal parity ...` and `[DCLF] BuildDraws local shadow lights ...`.
-   It covers the colour epoch only: the Z-prepass's draws go to buckets, so the depth segment has no template check. The CPU
    templates carry row offsets relative to the epoch's `PayloadVersions` bases.
-   `[DCLF] index pool parity: ...` flags `<- INDEX POOL` when a slot's index range changed with no log entry or a slot holds no
    range, on every 300th pool update.
-   It turns the resident region off, so it validates whole builds, not the region.

### Capture parity

`CS_DCLF_CAPTURE_PARITY=1` with `CS_DCLF_PARITY_BOTH=1` (`Diagnostics/Scene/CaptureParity.cpp`). The engine draws everything, and
every native main-pass Lighting draw is compared with the tables DCLF draws the same object with. Reported every 300 frames at
the end of the prepass.

| Line | Means |
| --- | --- |
| `[DCLF] location: cell ...` | Where the window was. |
| `[DCLF] capture parity OK\|MISMATCH: N native main-pass lighting draws, N checked against the tables, N mismatched (material, per-geometry, technique), N untracked eligible, N tracked but excluded, N native-only passes of DCLF objects, N whose pass differs only in the bits the draw decides ...` | The summary. |
| `[DCLF] capture parity by technique (mismatched/checked): ...` and `first <technique> mismatch: ...` | Per technique. |
| `[DCLF] parity mismatches by variable: ...` | The constants that differ, by stage, block and variable. |
| `[DCLF] mismatched materials: ...` | Materials that mismatched, with `written after its last mismatch` when a write event explains it. |
| `[DCLF] capture parity: tracked but excluded, by the verdict now: ...`, `untracked and ineligible, by the verdict: ...` | Draws DCLF does not model, by reason. An `eligible` entry is a verdict DCLF kept stale. |
| `[DCLF] capture parity: N draws' shading changed since the frame's start sampled it (accepted: ...)` | Shading written during the frame (see the accepted differences). |
| `[DCLF] draw parity OK\|MISMATCH: ...` | Draw arguments and bound buffers. |
| `[DCLF] permutation parity OK\|MISMATCH: ...` | The permutation buffer's bits Lighting reads. `IsBeastRace` is compared on face techniques only. |
| `[DCLF] feature binding parity OK\|MISMATCH: ...` | Feature bindings that differ within a frame's eligible draws. |
| `[DCLF] bone palette parity OK\|MISMATCH: ...` | Skinned draws' palettes. |
| `[DCLF] skin parity OK\|MISMATCH: ...` | Advanced Skin's t71/t74 and wetness. |
| `[DCLF] light data parity ...`, `[DCLF] TruePBR texture parity ...` | The integrations' checks (`Integrations/LightLimitFix/Diagnostics`, `Integrations/TruePBR/Diagnostics`). |
| `[DCLF] face capture parity: ...` | Reflection face Lighting draws against their objects' main rows (`CaptureParity::OnFaceLightingDraw`). |
| `[DCLF] constant block coverage: ...`, `[DCLF] variables the native shaders have that DCLF leaves unwritten: ...` | What was compared, and what DCLF leaves unwritten. |
| `[DCLF] render state ... (VARIES)` | Render states seen, flagged when they vary. |

Rules of the comparison:
-   Only components the engine writes are compared. Constant blocks compare bit for bit (`ConstantBlock::SameBits`); unwritten
    components hold `kUnwrittenBits`, a NaN.
-   Bits the draw decides (`PerDrawBits`: the sun bits on a cascade miss, LOD-faded features) are left to their own checks. Such
    a draw is counted as "differs only in the bits the draw decides" and compared on descriptors alone.
-   Hair's ProjectedUV bit is left out (`HairProjection`; see [the engine bugs](#in-the-engine)).

Clean is `OK` on every line. Today's known mismatches (IBLParams, `TexcoordOffset`, fog, terrain LOD techniques) are in
[dclf-open-defects.md](./dclf-open-defects.md), "Failing or flaky parity checks".

### Capture point parity

`CS_DCLF_CAPTURE_POINT_PARITY=1` (`Diagnostics/Draws/CapturePoint.cpp`): `[DCLF] capture point parity: N frames' captures against
the first lighting draw's bindings, N differ`, `<- OK` or `<- DIFFER:` with the bindings. It checks that the colour epoch
captured the frame's bound buffers and views as the engine's first deferred Lighting draw sees them.

### Accumulated parity

`CS_DCLF_ACCUMULATED_PARITY=1` (`Diagnostics/Engine/Accumulated.h`, `Core/Render/Readback/Visibility.cpp`).

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] accumulated (T7b1): ...` | `<- ACCUMULATED DIFFER`, `<- NOT COMPARED` | DCLF's decoded visibility against the engine's `kAccumulated`, sampled on the render thread at the list jobs' end and compared at the same frame. |
| `[DCLF] anim rate (T7b2): ...` | `<- ANIM RATE`, `<- NOT COMPARED` | `FUN_1402cff60`, the gate that decides whether an animated reference updates, detoured at its three calls, against DCLF's writes on stood-in roots. |
| `[DCLF] frame camera (...)` | `<- FRAME CAMERA` | The list processes' camera against the main camera `BeginSceneFrame` read. |
| `[DCLF] accumulated writes parity (A5): ...`, `[DCLF] accumulated writes decode (A5): ...` | `<- WRITES DIFFER`, `<- NOT COMPARED` | The incremental `kAccumulated` writes against a reference (under persistent parity). |

### Fade parity

`CS_DCLF_FADE_PARITY=1` (`Features/Fades/Diagnostics`).

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] fade port parity (the port against the engine's functions on node copies): ...` | `<- FADE PORT` | Every 30th frame, up to 64 fade roots: DCLF's port against the engine's own functions run on node copies. |
| `[DCLF] fade state parity (FadeStateCS against the port): ... N exact, N within rounding, N differ` | `<- FADE STATE` | The GPU pass against the port. Equal integers and floats within 1e-5 count as rounding, not as differences. |
| `[DCLF] fade inputs parity (the root rows' fadeAmount and +0x109 against the nodes): ...` | `<- FADE INPUT` | The root rows' inputs against the nodes. |

Set parity's `<- FADE` is the fade test's verdict against the engine's, at the draw.

### Skylight parity

`CS_DCLF_SKYLIGHT_PARITY=1` (`Features/OcclusionMaps/Diagnostics/SkylightParity.cpp`). Every 120th frame per map, the map is
rendered both ways and compared:
`[DCLF] <map> occlusion parity: WxH format F: N texels equal of M, DCLF farther F nearer N (K by more than 1/256), max diff, mean diff; clear texels ...`.
"DCLF farther" must be 0. "Nearer" is expected while roots start fading (see the accepted differences); otherwise maps agree
within a few texels by more than 1/256. `CS_DCLF_SKYLIGHT_DUMP_DIR` writes both maps as PGM files.

### Foliage parity

`CS_DCLF_FOLIAGE_PARITY=1`, `wind` or `land` (`Diagnostics/Draws/Foliage.cpp`, `FoliageParityCS.hlsl`, the `DCLF_OWNED_PARITY` and
`DCLF_FOLIAGE_PARITY` shader defines). It checks owned pixels the colour pass left unshaded or shaded with another object,
motion vectors more than a pixel off a static object's (wind frozen), near-white albedo or diffuse, and reprojected jumps against
the frame before. `[DCLF] foliage parity over 300 frames: ...`. Healthy: 0-1 unshaded pixels a frame, and about 1% reprojected
jumps at canopy edges while turning. `land` adds `[DCLF] terrain LOD coverage over 300 frames: ...`, flagged
`<- TERRAIN LOD COVERAGE` for frames that lost terrain LOD pixels.

### The registration census

`CS_DCLF_CENSUS=1` (`Diagnostics/Engine/RegistrationCensus.cpp`, `Diagnostics/Scene/SceneCensus.cpp`). It counts every pass a
registration hook hands on to a batch renderer (withheld passes are not counted), by view (batch renderer) and class, every
report, with or without `CS_DCLF_STATS`. So it shows what the engine still draws.

-   Lighting geometry that is not a member of its view's phase gets the coordinator's verdict: read directly when the scene work
    runs inline (under the parities), otherwise asked asynchronously (`PostCensusRequest`, `TakeCensusReply`, `CensusVerdictOf`).
    Until a reply arrives the line is marked `(live approximation)`.
-   `[DCLF] census (T7a) over N frames: N passes the engine kept, N geometries; P% attributed to a named class` with `<- OK` or
    `<- UNATTRIBUTED (gate 95%)`.
-   `N member passes ...` with `<- OK` or `<- DEFECT; first: ...`: a member of the view's phase that the engine drew. Must be 0.
-   `N passes under entries DCLF draws whole` with `<- OK` or `<- WHOLE ENTRY; first: ...` (`PrimaryCull::DrawsWhole`).
-   Expected in the main view: no `alpha-blend`, refraction `technique`, `effect-opaque`, `billboard` or `unsupported-parent`
    classes beyond the variants DCLF does not draw.

### Lines in every report

These need no parity switch. Most come with `CS_DCLF_STATS=1`.

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] main camera since the last report: N native passes drawn, N of them set members' ...` | `<- LEAK` | A set member the engine drew in the main camera. Must be 0. Printed in every run. |
| `[DCLF] persistent draws (last frame, colour): N kept inputs, N sequences, ...` | | Sequences must be thousands in a populated cell, never 0. |
| `[DCLF] indirect draws (last frame): N candidates ..., skipped: ...` | | Per reason: `pipeline`, `geometry`, `texture`, `sampler`, `constants`, `capacity`, `not-in-set`, `candidate-only`. `constants` and `texture` must be 0. |
| `[DCLF] N pairs read constant registers no frame slot supplies (...): their members are not drawn` | (warning) | Every pair of a pipeline whose constant register the commit does not supply is rejected: its members are withheld and drawn by nobody. |
| `[DCLF] frame textures (last colour epoch): N reads of registers the commit could not resolve, which read zero: ...` | (warning) | A frame texture a pipeline reads that the commit could not resolve. |
| `[DCLF] SPIR-V programs: ...`, `[DCLF] indirect pipelines: ...` | `<- FAILED` | Program and pipeline builds; a failed one is flagged. |
| `[DCLF] on-demand SPIR-V compile ...`, `[DCLF] on-demand pipeline build ...` | | Precompile misses. Expected only in the first frames. |
| `[DCLF] shadow views by the set: ...` | `<- SET SHADOW`, `<- HOLES` | Members a shadow build could not draw; views not ready that have withheld casters (a missing shadow). |
| `[DCLF] shadow capability: ...` | `<- PHASES` | A shadow view the capability did not foresee. |
| `[DCLF] colour sun test: N of 300 frames without the sun's cascades` | `<- NO CASCADES` | Expect 0 outdoors. |
| `[DCLF] sun on the GPU (sampled frame): ...` | `<- DIFFERS` | The GPU's cascade test against the CPU's over the same inputs. |
| producer and payload lines | `<- UNBUILT`, `<- UNREVISED`, `<- LATCH`, `<- POOL LEAK` | A covered epoch with no payload in the installed publication (not submitted), an epoch with no shape or recording in the selected revision, values past their latch, and a pool that kept growing over the windows. |
| `[DCLF] the <epoch> epoch of frame N has no payload ... not submitted <- UNBUILT` | (warning) | The same, per epoch. |
| first person, forward and reflection lines | `<- EPOCH FAILED`, `<- NO DEPTH EPOCH`, `<- NO FORWARD RECORDING`, `<- NO REFRACTION DRAW`, `<- LATE FACES`, `<- FIRST-PERSON PARTICLE SPACE` | A view epoch that failed or had no depth epoch, a forward epoch without a recording, refraction members with nothing drawn, reflection faces captured late, and first-person particles placed more than 2000 units away. |
| `[DCLF] effects ...` | `<- EFFECT PIPELINE FAILED` | An effect pipeline that failed to build. |
| `[DCLF] tree LOD draws: ...` | `<- TREE LOD HOLES` | Tree LOD commits that could not draw a frame whose tree LOD was withheld. |
| `[DCLF] sun whole skip ...`, `[DCLF] point lights' empty culls ...` | `<- STAMPS` | Geometry given DCLF's light bits without a stamp. |
| `[DCLF] sun entry exclusion: ...` | `<- VETO` (warning) | Excluded entries left in the full-frustum arrays after the veto. |
| `[DCLF] engine-read window: ...` | `<- LANE ENGINE ACCESS` | Engine memory read by the scene lane without a lease. Must be absent. |
| `[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): ...` | `<- LANE ENGINE RELEASE` | An engine object's last release on a DCLF thread. |
| `[DCLF] scene pump (T6b3d): ...`, `[DCLF] scene pump wakes by source ...`, `[DCLF] event to adoption ...` | | Passes a frame, passes that changed nothing, publications, and the delay from an event to the frame that installs it. |

### Accepted differences

These are known, measured and left as they are. [dclf-open-defects.md](./dclf-open-defects.md), "Accepted differences", has
the measurement and what would remove each; this list says how each shows in the checks.

| Difference | How it shows |
| --- | --- |
| A root seen again is drawn for one frame with its old fade (`FadeStateCS` runs a frame ahead). Fade verdicts lag one frame by design. | One-frame draws of roots that snap out; no flag. |
| The engine's occlusion map reference reads node milestones a few frames late. | Skylight parity "nearer" texels while roots start fading, mostly after a teleport. |
| Shading written during a frame (a controller on the animation job, a cull's `GetRenderPasses`) is drawn from the next frame. | Capture parity's "shading changed since the frame's start sampled it" line. |
| Tree LOD instances the engine leaves hidden after `allVisible`: DCLF draws by the intended rule. | The tree LOD mirror line's `shown by allVisible without a refill`. |
| Reflection faces render early in the frame (`Main::Draw`, before the world), so they light with another moment's sun. | Face capture parity differs on PS PerGeometry `DirLightDirection` in about the fifth digit; everything else is equal. DCLF reads the sun direction from the frame lighting block (PS b13), not from a row. |
| Tree clocks: the GPU advances every tree's clock, the engine only visible trees within its budget. | The tree wind clock rates line (`engine over GPU`); tree wind parity compares the timers within a drift. |
| `FadeStateCS` and the port may round differently. | Fade state parity's `within rounding` count. |
| Terrain Blending leaks write mode 1 into the blended decals after its hook; DCLF derives write mode 11. | An RT0 alpha difference the composite never reads. |
| `IsBeastRace` and Hair's ProjectedUV (see [Bugs found by parity](#bugs-found-by-parity)). | Not compared outside faces, and left out for hair. |
| Hint-10 passes DCLF does not model. | Capture parity's "native-only passes". |

## Diagnostics and probes

### The periodic report

`CS_DCLF_STATS=1` logs DCLF's report every 300 frames into `CommunityShaders.log`. Lines start `[DCLF]`; the render graph's start
`[ORG]`. The [lines in every report](#lines-in-every-report) say which ones show that DCLF draws at all. The report also covers
the scene pump, the set and its leaves by cause, the tables, the pooled payload, the frame values, the shadow views and each
feature's own lines (`Api::ReportPoint`, registered by `RegisterReportLines`). `CS_DCLF_PASS_STATS` and `CS_DCLF_PROFILE` add
per-pass counts and scene-phase timings.

### The target probe

`CS_DCLF_TARGET_PROBE=x,y` (`DrawcallLimitFix::ProbeOpaqueTarget`, `Diagnostics/Feature/OpenDefectProbes.cpp`;
`Diagnostics/Draws/ColourProbes.cpp`):
-   Every bound G-buffer target, plus the bound depth buffer and Terrain Blending's mask (t55), averaged over a 64 x 64 block,
    where the opaque pass ends: before and after DCLF's colour epoch, after Terrain Blending's passes, and after the deferred
    composite. It runs with DCLF on or off, so a live toggle compares the two.
-   `x,y` are whole pixels of the render resolution (2560 x 1440 under DLSS Quality at 4K), the block's top-left corner. Fractions
    parse as 0. The mean over a block is used because one pixel is too noisy under TAA jitter.
-   The colour epoch also logs `[DCLF] target probe: N objects cover the pixel ...`: the objects whose bound covers it.

### Shader debug output (`CS_DCLF_SHADOW_DEBUG_OUTPUT`)

`CS_DCLF_SHADOW_DEBUG_OUTPUT=1` adds the global shader define `DCLF_SHADOW_DEBUG` (`State::GetDefines`). With it, Lighting.hlsl's
deferred pixel stage writes chosen intermediate values into the Diffuse target in place of the colour, for the engine's draws and
DCLF's alike. As shipped it writes the sun's shadow terms: x `dirSoftShadow` (the VSM soft shadow), y `dirVSMDetailedShadow`, z
`dirDetailedShadow`. Read them with the target probe.

-   The define is part of the disk cache's names, so the debug shaders are cached apart. Changing the deployed Lighting.hlsl makes
    Community Shaders recompile its whole shader cache on the next launch, and DCLF does not start until that finishes: run a
    warm-up launch first.
-   To investigate another factor, edit the `DCLF_SHADOW_DEBUG` block to write it, and read it with the target probe across a
    `CS_DCLF_TEST_TOGGLE`, at a still view.

**Investigating a shader-side difference.** Every parity check compares DCLF's inputs with the engine's: descriptors, constants,
textures, samplers, bound buffers. None compares what the draws write. When the checks are clean and DCLF's draws still look
wrong, the difference is inside the shader:
1.  Bisect with sub-switches (`CS_DCLF_ACTORS=0` and the like) to tell a per-draw cause from a global one.
2.  Write the suspect factors with the debug block and probe a pixel with DCLF on and off.
3.  Look for divisions and comparisons on values that can be exactly equal. The engine's shaders are compiled by FXC and
    translated by DXVK, DCLF's by DXC, and the two round degenerate arithmetic differently. Terrain Shadows' penumbra division
    was such a case: a collapsed penumbra read +inf (lit) under FXC and 0 (shadowed) under DXC. HLSL that DCLF shares with the
    engine must not depend on a degenerate division (`GetTerrainShadow` divides only where the penumbra exceeds 1e-3).
4.  Compare against a curve, not a single reading, when the value drifts. The VSM soft shadow was checked this way: with the
    probe on a 64 x 64 block at a still view and DCLF toggled off and on three times, DCLF's and native's readings of
    `dirSoftShadow` interleave on the same curve as the sun moves (0.62 to 0.76 over the run), with no offset between them.

### The target dump

`CS_DCLF_TARGET_DUMP=f1,f2,...` (`Diagnostics/Feature/TargetDump.cpp`): at those Present counts, every target bound at each dump
point of the frame (and `kMAIN`'s depth) is read back asynchronously and written as half-size PNGs, NaN and Inf marked, with
their statistics in `stats.txt`, under `SKSE/dclf-dump/<count>/`. The log says `[DCLF] target dump: Present N, N points, N targets
written to ...`. Use it to compare a good frame with a corrupt one; Streamline also logs its constants on those frames.

### The schedule dump and `tools/dclf-schedule-diff.py`

`CS_DCLF_SCHEDULE_DUMP=1` (`Diagnostics/ScheduleDump.cpp`) logs each epoch's passes as gathered whenever a list changes:
`[DCLF schedule] epoch N: name/type/queue | ...` and `[DCLF schedule types] epoch N: ...`, beside ORG's `[ORG] epoch order` and
`[ORG] async epochs`.

`python tools/dclf-schedule-diff.py baseline.log current.log` compares two runs: each epoch's last pass list, the epoch order and
the async lists must match exactly. Differing C++ type names are only a warning (ORG names a recording's recipe by them, which
changes no GPU work). Exit status 0: the same schedule; 1: a difference; 2: a log without a dump. A refactor that should not
change the frame is verified by this diff.

### The layout check

`python tools/dclf-layout-check.py` checks the rules in [dclf-layout.md](./dclf-layout.md), "The rules" (include directions, the
dependency guard in one place, the closed top level, passes through the schedule's executor alone, features owning no part of the
renderer), and baselines for function complexity (lizard, when installed). It exits 1 on a broken rule or an exceeded baseline.
`--update` rewrites the baselines (`tools/dclf-layout-baseline.json`) after a step that shrank them.

### The frame trace and corrupt frames

`CS_DCLF_FRAME_TRACE=1` (`Diagnostics/Draws/FrameTrace.cpp`) logs, every frame, `[DCLF] frame trace: frame N ...` lines:
-   `adopts snapshot ...`: the snapshot it adopted, its set and payload;
-   `coverage ...`: the epochs covered;
-   `Z-prepass epoch: ...` / `colour epoch: ...`: each main epoch's outcome (`submitted` in the steady state, otherwise its
    reason: withdrawn, not ready, `FAILED`, `NOT COMMITTED`);
-   `colour exec stamp ...`: the colour epoch's culling counts;
-   `binds point P (clock C)`, flagged `<- BOUND AT A RETIRED POINT` when the clock had already passed it and `<- POINT WENT BACK`
    when it is below the last;
-   `===== USER MARK (F10) at frame N =====` when F10 is pressed.

To find single corrupt frames:
1.  Run with `CS_DCLF_FRAME_TRACE=1` and start `powershell -File tools/dclf-capture-burst.ps1 -Out DIR [-Seconds 70]` beside it.
    It waits for gameplay (`DLSS evaluate` in the log), then captures the screen as 960 x 540 JPEGs named `HH-mm-ss.fff`. The game
    window must stay in front.
2.  `python tools/dclf-corrupt-frames.py DIR CommunityShaders.log [--top N] [--threshold T] [--sheets DIR]` scores each capture
    against its neighbours, reports single-capture outliers, and prints the trace lines of the 250 ms before each, marking with
    `>>` those that are not the steady state.

In the steady state every epoch is covered and both main epochs are `submitted`.

### Watches

-   `CS_DCLF_HIDDEN_WATCH` (`Features/HiddenNodes/Diagnostics/HiddenWatch.cpp`): once walk parity sees a hidden change no event
    announced, hardware watchpoints on that chain's flags name the store behind it.
-   `CS_DCLF_MIRROR_WATCH` (`Diagnostics/Scene/MirrorWatch.cpp`): watchpoints on a fade node's statics after its placement name the
    writers the mirror misses.
-   `CS_DCLF_RELEASE_GUARD` (`Diagnostics/Engine/ReleaseGuard.cpp`): engine objects' last releases on DCLF's threads, by class. An
    engine destructor run off the main thread can run Havok teardown and crash.

### The tree LOD audit

`CS_DCLF_TREE_LOD_AUDIT=1` (`Features/TreeLod/Diagnostics/TreeLodAudit.cpp`): every 60 frames, `[DCLF] tree LOD audit: ...` counts
tree LOD instances by their reference's state: shown over a loaded, visible full tree, or hidden without one. It is the probe of
the accepted difference "Tree LOD instances the engine leaves hidden after `allVisible`" in
[dclf-open-defects.md](./dclf-open-defects.md).

### GPU timing

-   DCLF's epochs replay kept recordings, which record no ORG pass timestamps, so ORG's GPU timing report is empty for them.
    Compare whole frames (`[Perf] rendered N fps (N ms)` in the log) and Tracy's CPU zones; turn recordings off
    (`CS_ORG_REUSE_RECORDINGS=0`) to see the epochs' passes as Tracy GPU zones.
-   `CS_GPU_EVENT_TIMERS=<frames>` times the engine's events; select them by exact name. `CS_GPU_IDLE_TRACE=<frames>` attributes GPU
    idle time. `CS_ORG_EPOCH_STATS=1` gives the render thread's time per epoch.
-   Nsight Perf's range profiler drains around each range, so many small native ranges each pay a drain and DCLF's few large ones
    look cheaper than they are. Compare whole frames first ([nsight-perf.md](./nsight-perf.md)).
-   Tracy zone exports are large (gigabytes a run). Export them only when needed.

## How to validate a change

### The validation run

One run of about 60 seconds validates a step:
-   The camera moves and turns, so cells load and LOD swaps happen: `CS_DCLF_TEST_MOVE` and `CS_DCLF_TEST_TURN` (for example
    `300:1000000:40` and `300:1000000:0.2`). A still camera hides alpha-test holes and most culling cases; rotation alone reaches
    few cases.
-   `CS_DCLF_PERSISTENT_PARITY=1` and `CS_DCLF_SET_PARITY=1` together, with `CS_DCLF_STATS=1`.
-   The full featureset: the log shows `[DCLF] featureset: full`, and no `UNKNOWN` switch.
-   Clean: every flag `<- OK`, or listed in [dclf-open-defects.md](./dclf-open-defects.md); `<- LEAK` 0; no member drawn by the
    engine; no validation messages about generated commands when the layer is on.

Add `CS_DCLF_CENSUS=1` to see what the engine still draws, and the other checks when the change touches what they cover (walk
parity for events, build parity for BuildDraws and the index pool, capture parity for shading).

### Scheduling changes: a parity-free run

Persistent, walk and capture parity run the scene pass inline once a frame (`Policy::ScenePassInline`). A parity run therefore
never exercises the scene pump, several passes a frame, or the builder's pacing. A change to scheduling (the pump, passes, the
snapshot builder, wakes) needs one more motion run with no parity switch. Read passes per frame, set membership and sequences
against the parity run.

### Before trusting parity or performance: DCLF draws

A run in which DCLF draws nothing can look clean and fast: every store's parity can say "0 differ" while the members are
withheld and drawn by nobody. Before reading parity or timing, check in each run's log:
-   `[DCLF] persistent draws (last frame, colour)`: sequences in the thousands in a populated cell, not 0;
-   `[DCLF] indirect draws (last frame) ... skipped:`: no `constants` or `texture` skips, and no `pairs read constant registers no
    frame slot supplies` warning;
-   set parity's `in the set and drawn by nobody`: 0.

### Timing runs

-   **Measure from frame 3000 on.** DCLF's SPIR-V programs and pipelines compile in the first frames of gameplay (Community Shaders'
    own shaders compile before the load). No long warm-up is needed beyond that.
-   **The game window must have focus.** Community Shaders skips its upscaler while another window has the foreground
    (`Upscaling::IsWindowUnusable`), which makes a run 1.5-2 ms faster at 4K. Compare only runs whose log has
    `[Streamline] DLSS evaluate`.
-   **Nothing else heavy runs.** Builds or other heavy processes on the machine inflated every Tracy zone by about 1.6x. Background capture (Discord Clips at 30 fps) adds 1-1.5 ms to one frame every 33 ms.
-   **No validation layer.** Check that no Vulkan layer is forced on before any performance conclusion: the Vulkan Configurator
    can force `VK_LAYER_KHRONOS_validation` on for every application through
    `%LOCALAPPDATA%\LunarG\vulkan\vk_loader_settings.json`, and `VK_LOADER_LAYERS_DISABLE` does not override it. With it, DXVK's
    command-stream thread saturates and native looked 3x slower. Confirm that `VkLayer_khronos_validation.dll` is not loaded in
    the game process.
-   **No parity switches** in a timing run, and the full featureset.
-   Compare alternating runs, never single runs: two runs of one build differed by more than a step's gain. A live toggle in one
    run (`CS_DCLF_TEST_TOGGLE`) compares DCLF on and off under the same conditions.

### The Vulkan validation layer

Through Mod Organizer 2, `VK_INSTANCE_LAYERS` alone loads no layer; `VK_LOADER_LAYERS_ENABLE=*validation` does. The messages do
not reach `CommunityShaders.log`: write them to a file with `VK_LAYER_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG` and
`VK_LAYER_LOG_FILENAME=<path>`. Turn on extra validation with the `VK_LAYER_*` setting names (`VK_LAYER_VALIDATE_SYNC=1`,
`VK_LAYER_GPUAV_ENABLE=1`).

-   GPU-assisted validation disables itself on DCLF's indirect-bindable pipelines ("Unable to instrument shader using
    VkIndirectExecutionSetEXT"), and synchronization validation does not see descriptor-heap accesses. Neither checks DCLF's
    passes.
-   Messages that are not DCLF's and appear in every run: `VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328`,
    `VUID-vkCreatePrivateDataSlot-privateData-04564`, `VUID-VkImageMemoryBarrier-newLayout-01198` (Streamline's barrier to an
    undefined layout at startup), `VUID-vkQueueSubmit2-semaphore-03868` (swapchain semaphore reuse) and
    `VUID-vkAcquireNextImageKHR-semaphore-01779`.
-   DCLF's own open validation errors are in [dclf-open-defects.md](./dclf-open-defects.md).

### Other runs

-   **Capture parity:** `CS_DCLF_CAPTURE_PARITY=1 CS_DCLF_PARITY_BOTH=1`. It double-draws, so it is never a timing run.
-   **Cell-load stress:** a flight across cells (`CS_DCLF_TEST_MOVE=300:5000:5`) with build parity for the index pool, or console
    `coc` commands through `CS_DCLF_TEST_COMMANDS` (for example `600:coc WhiterunDragonsreach;1200:coc Riverwood`).
-   **Graph rebuild:** `CS_DCLF_TEST_COMMANDS=<frame>:dclf:rebuild`. Expect one frame-start wait line, no withdrawal, and sequences
    above 0 after it.
-   **Growth:** `CS_DCLF_TABLE_START=small` makes every growable table grow early and often.
-   **Fan-out:** compare walk and set parity against a `CS_DCLF_FANOUT=0` run. Walk and persistent parity run the pass inline (no
    fan-out), so only set parity on the lane checks the pool.
-   **Weather and feature paths:** `CS_DCLF_TEST_COMMANDS=100:fw 10a241` makes it rain (the precipitation mask);
    `player.damageav stamina 1000` exercises Advanced Skin's wetness; `dclf:pov=first` and `dclf:draw=1` exercise first person;
    `player.sendanimevent attackStart` with spawned enemies gives blood decals.
-   **Places** that cover the classes: Riverwood (waterfall foam, mist, the river, bridge spray), an inn with candles and a fire
    (refraction, particles, light cards), Whiterun and Dragonsreach (an interior with portals), a dungeon, save and load, and the
    live toggle.
-   **Shadows** are judged by readbacks and counters across a live toggle, within the drift between readings (the sun moves).
-   **An intermittent bisect** re-runs the control at the end as well as at the start: a GPU fault from an earlier crash can
    persist.

## Crash triage

### CPU crashes

-   **The crash logger's logs** are `Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`. They give addresses as
    `SkyrimSE+offset` with the Address Library ID; the code names engine functions by their AE 1.6.1170 address
    (`FUN_14xxxxxxx`) and `RELOCATION_ID`. Decompile the faulting function before attributing a crash: the logger's labels can
    be wrong (it labels by the nearest symbol), and a probable frame is not the same as a stack-scan hit.
-   **Community Shaders' minidump**, `CommunityShaders.dmp` beside the game executable, is written from outside the game by
    `tools/crash-dump-helper` (`CommunityShadersCrashDump.exe`). The plugin starts it at load (`XSEPlugin.cpp`,
    `StartCrashDumpHelper`); on a crash its vectored handler signals it and logs `[CRASH] exception ... - writing
    CommunityShaders.dmp`. `MiniDumpWriteDump` run inside the game deadlocked on the heap lock. Read the dump with cdb or
    WinDbg.
-   **Heap corruption** shows its fault later and elsewhere. Typical DCLF causes are an engine object read through a raw pointer
    nothing held, and a last release on the wrong thread. `CS_DCLF_RELEASE_GUARD`, `CS_DCLF_HIDDEN_WATCH` and
    `CS_DCLF_MIRROR_WATCH` name the thread or writer.

### GPU crashes

-   **Aftermath dumps** are armed whenever Community Shaders is built with `AFTERMATH=ON` (the default; it needs the Nsight
    Aftermath SDK). A GPU fault writes `gpu-crash-<stamp>.nv-gpudmp`, plus `gpu-crash-<stamp>-shader-<id>.nvdbg` shader debug
    info, into the SKSE log directory, and logs `[Aftermath] GPU crash dump written to ...`.
-   **Fault details need a diagnostic configuration.** `CS_GPU_CRASH_ANALYSIS=1` makes Community Shaders set `DXVK_DEBUG=hang`
    before the device is created (`DxvkLoader.cpp`; a `DXVK_DEBUG` already set is left alone): DXVK records checkpoints and
    debug-utils labels for its work and ORG's. It serialises command recording, so it is a diagnostic configuration, not a
    default. Without it a dump has no fault details or markers. The fault address, resources and warp PCs also need
    `DXVK_AFTERMATH_RESOURCE_TRACKING=1`, `DXVK_AFTERMATH_SHADER_DEBUG_INFO=1` and `DXVK_AFTERMATH_SHADER_ERROR_REPORTING=1`
    (`extern/dxvk/src/dxvk/dxvk_adapter.cpp`). `CS_ORG_DETAILED_CHECKPOINTS=1` adds ORG's per-command checkpoints.
-   **Decoding.** `tools/aftermath-decode` builds `aftermath-decode <dump.nv-gpudmp> [--debug-info <dir>] [--spirv <dir>]
    [--json <out.json>]`. It prints the device status, the page fault (address, access, client, resources), the active shaders and
    the last markers per context, and writes the SDK's full JSON (by default `<dump>.json`). `--spirv` names ORGModuleServices'
    shader cache (`Data/ShaderCache/ORG`, in MO2's overwrite folder), so the faulting warps map to SPIR-V, and to HLSL lines with
    `CS_DCLF_SHADER_DEBUG` on (the default). Without shader debug info it cannot name the shader.
-   **DXVK's own output.** Community Shaders points `DXVK_LOG_PATH` at the SKSE log directory, so `SkyrimSE_d3d11.log`,
    `SkyrimSE_dxgi.log` and, on a device loss with `VK_EXT_device_fault`, `SkyrimSE_device_fault.bin` land beside
    `CommunityShaders.log`.

### Reproducing and bisecting

-   **Startup loops** reproduce faults in the first DCLF frames: `CS_DCLF_TEST_EXIT` of at least about 200 (the count includes
    the main menu's frames), the game relaunched at once.
-   **Bisect by arms and rates.** Run each arm until its first loss and compare counts; an intermittent fault needs many runs per
    arm before an arm is cleared. Re-run the control at the end.
-   **Reduce the configuration** with the feature toggles, `CS_DCLF=0`, `CS_ORG_COMPUTE_QUEUE=0`, `CS_ORG_SUBMIT=flush` or
    `CS_ORG_ASYNC_EPOCHS=0` to tell DCLF's work from the render graph's submission paths.
-   Open crashes are in [dclf-open-defects.md](./dclf-open-defects.md), "Open crashes".

Not verified here: the crash logger's exact log format (it is a separate SKSE plugin), and that a release build of
`tools/aftermath-decode` sits at a fixed path (it builds with CMake from `tools/aftermath-decode/CMakeLists.txt`).

## Bugs found by parity

Bugs DCLF's parity checks found in other Community Shaders features, in the engine, and in the libraries DCLF runs on.
-   **Feature bugs** are each their own fix, in that feature. DCLF imitates none of them: it binds and computes what the feature
    intends, and the parity checks report the difference. An engine or feature shader that depends on the previous draw's
    bindings is a bug to fix there, not an order for DCLF to reproduce: DCLF registers and draws in its own order.
-   **Engine bugs** cannot be fixed at the source. Where the native result is undefined, DCLF picks a deterministic rule, stated in
    the entry, and the parity checks compare against that rule.

### In other Community Shaders features

**TruePBR: a loader thread writes MATO data into materials that are being drawn.** Still present.
-   Where: `TESBoundObject_Clone3D` in `src/TruePBR.cpp` (`TESObjectSTAT`'s vtable, slot 0x4A) applies a static's material object
    to every geometry of the freshly cloned model with `ApplyMaterialObjectData`.
-   The fork-before-write forks only when the material already has a different owner with a different MATO
    (`prevOwnerRefID != 0 && prevOwnerRefID != ref && materialObjectData != pbrData`). A material nobody has claimed has owner 0,
    so it is written in place, and every reference sharing it picks up this reference's values. The write runs on a loader thread
    in the middle of a frame.
-   Evidence: capture parity's `PS PerMaterial 22` (`ParallaxOccData`, filled from the projected material's roughness and specular
    level) differed on terrain cliffs that shared a material with a newly loaded reference: DCLF 1, native 0.7.
-   Proposed fix: fork whenever the material is not exclusively this reference's (its reference count, or
    `BSLightingShaderMaterialPBR::All`). A fresh copy is drawn by nothing until the clone is attached, so writing it from the
    loader thread is then safe.
-   DCLF's side: nothing to change. `ApplyMaterialObjectData` reports the write (`NoteWritten`) and DCLF corrects the record on the
    next frame. Capture parity reports a remaining case as `mismatched materials: ... written after its last mismatch`.

**Subsurface Scattering: `IsBeastRace` stays set from the previous face draw.** Still present.
-   Where: `SubsurfaceScattering::BSLightingShader_SetupSkin` in `src/Features/SubsurfaceScattering.cpp` sets or clears
    `ExtraShaderDescriptors::IsBeastRace` only for properties with `kFace` or `kFaceGenRGBTint`; every other draw inherits the
    last value. Lighting.hlsl writes it into `Masks.y` for the `SSS` + `SKIN` permutation, and `SeparableSSSCS.hlsl` picks the human
    or beast profile by it. So a body skin draw gets the last face's profile. The actor comes from `geometry->GetUserData()` and
    the bit defaults to beast when that is not an `Actor`.
-   Proposed fix: decide the bit for every Lighting draw in the deferred pass: skin by the owning actor's race (walking up to the
    reference), everything else cleared.
-   DCLF's rule: the bit is per object, decided as SSS decides it today, for faces only
    (`Integrations/SubsurfaceScattering`: `kObjectBeastRace`, `kRecordBeastRace`). Capture parity compares `IsBeastRace` on face
    techniques only (`CaptureParity::ComparePermutation`). Once SSS decides it per draw, DCLF needs the per-object bit for skin
    objects too.

**Advanced Skin: the RFAOS and wetness textures are never loaded.** Still present.
-   Where: `Skin::SetupExtraTexture` in `src/Features/Skin.cpp` derives `_rfaos.dds` and `_wet.dds` paths and writes them into the
    texture set, but sets `rfaosTexture` and `wetnessTexture` to the default white texture and never loads the files. Lighting.hlsl
    treats a texture of 32 texels or less as absent (`hasSkinExtra`, `hasSkinWetness`), so t71 and t74 are never used.
-   Proposed fix: load the two files when they exist, through the texture manager, and fall back to the black default.
-   DCLF's rule: bind what `Skin::MaterialTexturesOf` returns for the material (`Integrations/AdvancedSkin/SkinTextures.cpp`), so DCLF
    follows the fix. A FaceGen material with no hash key or no extra texture binds black to both. Capture parity's `skin parity`
    line compares t71 and t74.

**Terrain Blending leaks write mode 1 into the blended decals that follow its hook.** The difference is on RT0 alpha only, which
the composite never reads. DCLF's rule: derive write mode 11 for those decals; the difference is accepted.

**Community Shaders swaps the blend table** for its deferred variants between `StartDeferred` and `ResetBlendStates`. Not a bug,
but a trap: DCLF captures the engine's state objects inside the deferred pass (`CaptureEngineStates`), and opaque pipelines take
their write masks from the engine's state for their write mode (`kOpaqueWriteMode`), never a default blend state.

### In the engine

| Bug | DCLF's rule |
| --- | --- |
| Hair with ProjectedUV shades another object's snow: `BSLightingShader::SetupGeometry` (`0x1414dd040`) writes `TextureProj` (VS PerGeometry 6) and `ProjectedUVParams` 1-3 (PS PerGeometry 12-14) for every technique but Hair (6), while hair passes can carry bit 15. The draw reads the previous projected draw's constants. | Hair draws without the projection: `DrawnPassDescriptor` drops bit 15 from a Hair pass (`HairProjection`, `Core/Shading/LightingDescriptors.h`). Capture parity leaves that bit out for such passes. |
| The main registration reads `activeLightMask` and then clears it (`+0x160 == 0xFFFF`). | Write sun and point-light bits only until a geometry's first clearing registration of the frame (`FrameAddressSet`); keep owned members' masks 0. |
| `SetupMask` culls without `cameraRelatedUpdates`, so faded-out roots and new cells' roots are missing from the engine's occlusion maps. | Apply the same rule (`kCullFadeOnVisible`). |
| Utility's cull mode is per pass (0 for two-sided), so the state left at `FinishAccumulating` is the last pass's. | A shadow or occlusion view's raster state is its table entry with back-face culling; two-sided casters draw unculled by key. |
| The engine alternates a shadow viewport's max depth (1, 0.99997, 0.999985). | The depth range is a value in the view block (`DCLFDepthRange`), not part of the shape. |
| `RenderShadowmap` binds the target only for a descriptor that clears. | A point light's hemisphere 1 draws into hemisphere 0's slice (`PredictTargets`). |
| Indoors the sun does not draw (`0x14338c911`), so no full-frustum cull runs. | Clear the sun's frame state on such frames (`EndFullFrustumWindow`); do not require the sun's exclusion when the sun is off. |
| Main::Draw's light-mask clear (`FUN_1414cb640`) runs only while the player's third-person 3D is drawn. | Key the frame's lights on the selection, not on the clear. |
| The engine stamps actors' animation frame only from depth and shadow registrations. | DCLF stamps it from its own shadow views (`UpdateActorRate`). |
| `SetupGeometry` case 0xc advances a tree node's previous timer; model `+0xB0` is never read for trees outside the near list and can hold garbage (2.29e36). | Never evaluate tree setup per tree: the GPU's tree wind does the write's job. Animate, and scale the gust of, only a tree whose model has bones and that `FUN_14147d640` selects. |
| A tree's near-list test reads a cull result (`FUN_14147d640`); a fade root sampled during the frame is a step ahead (its cull updated the node). | Tree seeds (`TreeStaticOfNode`) and fade seeds are read on the render thread at `Kick`, before the culls. |
| The engine's fast square root shifts arithmetically, and some tree-animated ferns hold an uninitialised negative squared distance. | `FastSqrt` matches the engine's shift. |
| The engine's empty fade node (a static `BSFadeNode` at `0x14332a2a0`, read as null by engine readers). | Treat it as no node everywhere a fade node is read (`Engine::EmptyFadeNode`). |
| `BSMultiBoundNode::OnVisible` reuses a cached cull result for modes 0 and 2 while `lastAccumTime` matches. | Restore any value DCLF writes into a multibound for the tree loop right after the loop. |
| The actor pre-cull (`BSFadeNodeCuller`) sets bit 12 and, off screen, `kNotVisible` every frame; Process1 passes a preprocessed node unless `kNotVisible`, whatever its room. | `FadeStateCS` makes the pre-cull's test itself (frustum only, no room program). |
| The animation job runs fade updates and controllers beside the render thread; the engine writes inside the read window (`currentFade`, billboards, texture transforms, Havok transforms). | `FadeStateCS` models the animated fade updates (`AnimatedFade`); per-frame values are sampled by FrameValues, structural ones come from events. Parity drains the controller queues again after its compare and treats late events as queued, not missed. |
| 11 inline stores reset `currentFade` outside any hooked function (cell and sky placement). | All patched (`InstallFadeResetStores`): the mirror must see every write. |
| A property's fade node is set after its geometry's attach; clones point shared properties at the newest clone's fade node. | `PropertiesSetFadeNode` pushes a leaf update with pins; an unpinned fade node is acquired from a root owner or asked of the render thread (`AcquireFadeNode`). |
| Fade-node statics are reached through `AsFadeNode` (vtable); `netimmerse_cast<BSFadeNode*>` misses some. | Captures use `AsFadeNode`. |
| `TESWaterReflections::Update` hides the player's 3D and water objects around the cube faces; `Main::Draw` hides the first-person skeleton. | Capture those hidden bits with the frame (`FrameGlobals::cullHidden`); never publish them as scene changes. |
| The behaviour graph shows and hides an actor's parts during `Main::Draw`. | A hidden actor part, or a fading non-member, blocks its light and sun entry's exclusion (`Core/Scene/Model/Eligibility.h`). |
| The engine moves the first-person skeleton into root space mid-frame (`FUN_1414cb020`). | First-person rows and palettes are sampled at `Kick`, before the shift. |
| `Skin::GetWetness` advances an actor's fade on its first call each frame and is not thread-safe. | Wetness is captured on the render thread at the frame start, once per actor. |
| Detached roots are freed before the frame starts while a list filter made earlier names them. | Readers keyed by sun candidates may dereference them only because the candidates hold their entry nodes; the last owners release through `EngineReleases` at Present. |
| Engine destructors run Havok teardown and crash off the main thread. | No last release of an engine reference on a DCLF thread (`HandBack`, `EngineReleases`; observer `CS_DCLF_RELEASE_GUARD`). |
| A `BSTSmartPointer` release of an engine material deletes it without removing it from the material database. | Release through the engine's manager (`MaterialReference`, `kMaterialRelease`). |
| `FUN_14147a430` takes the LOD output as a float in XMM1. | Pass a float. |
| `BSLightingShader::SetupTechnique` writes `VPOSOffset` after unmapping PerTechnique (it works on DXVK). | DCLF reproduces the value; `ConstantMirror` re-reads mapped buffers from their last mapping. |
| `BSLightingShader::SetupMaterial` takes IBLParams from the shader object, not the material; IBLParams (PS 29) and FogColor drift with time of day, and no Lighting stage reads them. | Kept at first evaluation (`KeepUnreadFloats`, `KeepTechniqueFog`); not material state. |
| MaterialData.x (the envmap LOD fade) is `SetupGeometry`'s for techniques 1, 0xB and 0x10. | `SampleShading` writes it for all three. |
| SSE Engine Fixes writes PS 6 in `SetupGeometry` and NOPs `SetupMaterial`'s write. | Detected by instruction and ported in `GeometryPort`. |
| Weapon blood's AlphaTestRef comes from two globals `BSTempEffectWeaponBlood` writes each update. | Sample them as a frame value. |
| Some node classes implement child edits themselves (`BSParticleSystemManager`, `BGSDecalNode`, `BSFaceGenNiNode`); ShadowSceneNode's queued light removal detaches through no hooked function. | Each is detoured (`SceneEvents.cpp`, `Features/Decals/DecalHooks.cpp`); an out-of-world attach naming a held record evicts it (`SceneMirror::Evict`). |
| The engine reattaches a held outgoing LOD block from a cached handle; no detach erases it. | A reattach posts a kept notice that releases the hold-out (`ApplyGateKept`). |
| A decal with no z-write and bit 18 would cast into a paraboloid light by the code, but never does. | Its own reason, `ShadowReject::DecalPointLight`; parity reports any registration of one. |
| Box corners' grouping (`FUN_140e209e0`): corners 0 and 4 are `((X + C) + Y) +- Z` in the disassembly; the decompiler showed another order. | Port float arithmetic from the disassembly when the decompiler's order differs. |
| The address of a freed buffer is reused at once (DXVK). | A buffer address is never an identity (index pool ranges per slot; `GpuResources::Prefetch` holds a reference). |
| `FogFarColor.w` is copied from uninitialised stack memory; the permutation buffer's `IsSun` and `GrassSphereNormal` stay set from the last sky or grass draw. | Harmless: Lighting reads none of them. Capture parity's permutation check covers only the bits Lighting reads. |

Engine screen blood (`BloodSplatter`) and the first-person fire's changing size are engine behaviour, not DCLF defects.

### In CommonLibSSE-NG

| Bug | DCLF's rule |
| --- | --- |
| `NiSwitchNode`'s fields, and other members declared after a runtime-sized base, sit at VR's offsets in a multi-runtime build. | Read them at AE offsets (`Engine::ReadSwitch`). Never read members CommonLib declares after a runtime-sized base. |
| `NiBillboardNode::userFlags` is not at AE's `+0x128`. | Read the face mode at the AE offset (`PlacementRows::BillboardFaceMode`). |
| `NiTArray::size()` counts only non-null entries. | Iterate children to `free_idx()`. |
| `VertexDesc::GetSize` omits landscape data. | Take the stride as the description's low nibble times four. |
| `BSBatchRenderer::renderPass` is declared `BSTArray<PassGroup*>` but holds `PassGroup` structs inline. | DCLF reads it as inline structs. |

### In DXVK, BasicRHI and the render graph

Fixed in the forks; each fix states an invariant DCLF depends on.

| Bug | Invariant |
| --- | --- |
| DXVK's command-stream thread woke one waiter per synchronized chunk (`notify_one`); the ORG interop lets several threads wait for their own sequence numbers, so the right one could sleep forever (`extern/dxvk/src/dxvk/dxvk_cs.cpp`). | Interop waits are multi-waiter: wake all (`notify_all`). |
| DXVK's import paths (`importBufferResource`, `importImageResource`, the sparse `createAllocation`) popped the shared allocation pool unlocked, while loaders created buffers (`extern/dxvk/src/dxvk/dxvk_memory.cpp`). | Every import path takes the allocator's mutex. |
| A second `vkQueueSubmit2` concurrent with DXVK's submission thread lost the device (a page fault at 0 on the first DCLF frames). | Every submission DCLF or ORG makes to a DXVK queue goes under DXVK's submission lock (`RenderGraphRuntime.cpp`); the epoch's end waits on a semaphore of the runtime's own (`computeExit`), not on a graph timeline a rebuild destroys. |
| BasicRHI's copy service made command pools for the graphics family while submitting to a spare transfer queue. | Command allocators are made for their queue (`Device::CreateCommandAllocator(const Queue&)`). |
| BasicRHI's Vulkan push data: a 20-byte token lost the device on NVIDIA GA102. | Push data is split into tokens of at most 16 bytes (`kMaxPushDataTokenBytes`). |
| BasicRHI's Vulkan `frontCCW` was inverted relative to D3D12. | The D3D meaning on both backends; DCLF passes the engine's winding. |
| External Vulkan work (Streamline) right behind a graph epoch computed garbage. | It is separated by `Upscaling::SeparateFromGraphEpochs` ([render-graph.md](./render-graph.md)). |
