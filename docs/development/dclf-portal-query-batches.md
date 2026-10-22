# Portal query batches: implementation status

## Implemented

The engine compound-frustum builder remains the source of the Boolean expression. Its finalized records are lowered into reachable,
forward-topological 16-byte instructions `{kind, set, trueTarget, falseTarget}`. Lowering rejects cycles, invalid sets, branches outside
the program, operand targets, and unthreaded structural tokens. Each directory entry's last word is its maximum executable path length.
The plane-set ABI and room directory indexing are unchanged. The GPU no longer accepts bounds on exhaustion of a 512-step limit.
`Encode` runs from `Build`'s exit guard (a destructor) and inside the light walk's swapped state, so a rejected stream must not throw:
that program is published as a single `ACCEPT` (`PortalBytecode::AcceptAll`; the view test still applies), as the previous interpreter
accepted a malformed stream, and a `[structural-gap]` line is logged at power-of-two occurrence counts.

The main camera's encoding is published whole at each frame's first depth commit, into an immutable pooled backing sized to it
(`PortalPrograms.cpp`, a uint structured buffer read as a `ByteAddressBuffer`); an unchanged encoding keeps the last backing. Every
execution retains it until GPU retirement. The cull binding `Programs` answers its SRV index for that frame only, and the depth latch
carries it (`BuildDrawsLatch::portalProgramsIndex`, byte 320, `kLatchPortalPrograms`). The member and fade query stages and both batches
read it there; no pass declares it. There is no capacity substitution: the only descriptor-0 frame is an allocation failure, logged,
where every reader takes the frustum alone (the "no programs" meaning).

Fade roots use three passes before `fades.state`:

1. `fades.portal-queries` merges each root's seed into a local copy and captures its placement bound. Preprocessed roots and the legitimate
   no-program case retain the existing frustum semantics. Every capacity slot receives a current-frame query, including dead slots.
2. `fades.portal-batch` counts queries per program, computes hierarchical exclusive scans of query and task counts, scatters query indices,
   and builds 64-query tasks. Scratch grows with published root and room requirements. No task mixes programs. Scan levels are recorded
   from publication capacity without CPU readback; explicit barriers order stages within this pass.
3. `fades.portal-evaluate` indirectly dispatches one workgroup per task and scatters `{frame, verdict}` to stable root slots. The state pass
   checks the frame before consuming a verdict, retaining the previous frame-ahead fade publication timing.

Main-camera members now use a separate batch before `core.z.build-draws`. `core.z.portal-queries` clears every published object slot,
then walks the existing GPU draw inputs with the latched dispatch. An atomic claim emits one bound query per object, shared by skin
partitions and other inputs for that object. `SetFadeRow` assigns the same root from the object-root column to those inputs. The producer
also covers decal, forward, and refraction stamp-only inputs, and preserves the zero-view-mask meaning. Unclaimed slots receive current
rejected verdicts. Cheap frustum rejection and legitimate no-program outcomes are resolved before bucketing.

`portals.member-batch` and `portals.member-evaluate` use the shared executor in `PortalBatchPass.cpp`; fade passes use the same executor
with their own storage. The resource prefix identifies each batch independently. Member scratch grows by the already witnessed object-slot
count, and participates in `SceneFit::objects`, so a newer payload cannot address a member slot absent from its adopted buffer set.
`BuildDrawsCS` checks the result's visibility stamp for both normal phase-one culling and stamp-only feedback. Its main entry no longer
interprets portal bytecode. Phase two still retests only occlusion, and the colour segment still reads the depth visibility verdicts.

The default evaluator follows each lane's branches independently. Both predicates share one fixed six-plane calculation, preserving
empty-set and boundary semantics. `CS_DCLF_PORTAL_UNIFORM_SCAN=1` selects a diagnostic uniform instruction scan. Its extra plane work
must be included in comparisons. Neither backend uses subgroup operations or assumes a subgroup width. Task dispatch supports two dimensions.

`PortalContext.h` defines owned contexts, room identity, queries, tasks, and results. Main-camera and strict-light walk contexts own their
encoded words. Strict lights are captured before restoring main-camera scratch; selected-light records retain their snapshots.
`PortalContextAtlas.h` packs those snapshots into an owned atlas on the selected-light frame. Its 64-byte directory entries preserve
64-bit context and room generations, the room walk's light sphere and equatorial plane (`walkSphere`/`walkPlane`: never the
accumulation process's, which each shadow-map descriptor's camera sets), explicit program byte offsets, and dense bucket ranges across contexts.
`PortalTestAt` reads the existing program format at an explicit byte offset; the main-camera wrapper still uses offset zero. The optional
context-atlas evaluator resolves a dense bucket with a workgroup-uniform directory search, handles empty contexts, and rejects mismatched
frame identities. It is compiled and available to future atlas-backed batches; production batches still bind the standalone camera program.
Point-light consumers still require hierarchy and full publication-identity binding. Tests cover
mixed camera/light contexts, identities, offsets, bucket ranges, malformed directories, and ownership after source snapshots change.

## Point-light publication and native contract

`LightContextResources` publishes the selected-light atlas at commits. The atlas has its own 64-bit selection identity in header words
2 and 3, independent of the 64-bit walk/context and room-map identities in its directories. Validation rejects mixed frames, malformed
context offsets, missing program data, incorrect bucket totals, and missing publication identities before allocating GPU storage.
A pool leases an immutable uint structured buffer (the camera programs' view kind) sized to the complete snapshot, using DCLF's existing backing-allocation and cleanup services.
Each execution retains the publication and its lease until GPU retirement. `CurrentLightContexts` rejects a previous frame or an earlier
selection in the same frame. This upload has no revision-capacity substitution and does not reuse old contents on allocation failure.
Commit hooks run unguarded on the render thread: a rejected atlas or failed allocation publishes nothing that frame (logged), and an
atlas the selection cannot encode is not published either.
It publishes walk snapshots; an accumulation consumer must still supply its own process/descriptor parameters.

The AE executable was audited directly in Ghidra (`SkyrimSE.exe.unpacked.exe`), against these functions:

| Function | Contract retained by the new point predicate |
| --- | --- |
| `BSParabolicCullingProcess::Process1`, `0x141519a90` | Modes 1/2 take precedence; preprocessing precedes always-draw even in mode 4; compound and sphere failure paths have distinct state effects. |
| Sphere helper, `0x14151a1e0` | Uses `(sqrt(dx*dx + dy*dy + dz*dz) - boundRadius) - lightRadius < 0`, followed by the native hemisphere codes and strict plane comparisons. |
| Sphere/OnVisible wrapper, `0x141519fe0` | Writes the hemisphere state even on sphere rejection, unlike the compound path with `skipViewFrustum == false`. |
| Camera setup, `0x141519d20` | Accumulation sphere and equatorial plane come from its descriptor's camera and the light radius. The room-walk camera is a separate input. |
| `BSMultiBoundNode::OnVisible`, `0x140e2d710` | Tests shape bounds freshly in mode 4; switches children into mode 1 for `kAllPass`, restoring the mode after the subtree. |
| `NiNode::OnVisible`, `0x140d1e2c0` | Walks children only for nonzero radius or always-draw, and does not save/restore the process hemisphere state. |

`ParabolicPrograms.hlsli` is a separate evaluator, reusing only the compact compound interpreter. It takes an effective hierarchy mode,
node flags, explicit process sphere/plane, the back-accumulator presence, and `ignorePreprocess`. Its result records visibility, the
native hemisphere code, whether that code overwrites process state, and invalid-input status. It does not apply the camera wrapper's
zero-radius or mode-4 preprocessing rules. `ParabolicQueriesCS` is a reference/replay kernel, compiled with both interpreter backends;
it is not a production leaf-only replacement for native strict traversal.

An additional hierarchy requirement follows from the audit: untested visits preserve the current process hemisphere state, which can
come from a previous sibling rather than an ancestor. A rejected sphere can write zero. Parent-before-child visibility stages alone do
not reproduce these effects. The integration must preserve native traversal order for hemisphere-state writes, for example by an
ordered prefix of the surviving write events after hierarchical visibility is resolved. Compound rejection, sphere rejection, and
untested visits must remain distinct. This also requires respecting node-specific child traversal and descriptor constraints.

`ParabolicStateCS` implements this prefix stage for replay and future graph integration. `Seed` records the last writer as its
native traversal ordinal plus one, so a mask of zero remains a write. `Scan` and `AddOffsets` compute an exclusive maximum in the
existing 256-wide hierarchical scratch layout; `Resolve` includes the current event and reads the last writer's mask. A separate
prefix component propagates invalid upstream state. No subgroup operations or fixed wave width are required. Each stream must cover
one descriptor traversal, start with its captured initial mask, and receive ancestry-resolved visited flags in native preorder.
Visibility rejection alone must not remove a visited state-write event. These kernels are not scheduled in production: the
strict gates do not need the state ("Strict point-light gates", the hemisphere code).
Both hierarchical scan shaders reject surplus groups in a two-dimensional dispatch before barriers or scratch writes.

### Strict point-light traversal (AE)

Mapped from the AE executable for `CS_DCLF_STRICT_LIGHT_PARITY` (`Features/Shadows/Local/Diagnostics/StrictLightParity.cpp`), which
models it on the CPU and compares every append with the engine's on the parity frames.

-   **The request** (`BSShadowParabolicLight::Accumulate`, `0x14151b960`): cull mode `portalStrict + 3` (4 for a strict light),
    `ignorePreprocess` set, `cameraRelatedUpdates` clear, no initial compound frustum, and the light's room process's portal graph
    entry (`+0x30190`). The process (`0x14335bfa0`) takes descriptor 0's camera for the sphere's centre and plane and the light's
    radius (`FUN_141519d20`), and descriptor 1's accumulator as the back hemisphere. `Process2` (`0x141519c90`) clears the active
    view planes and culls the scene graph's object root (`ShadowSceneNode` child 3) when the light has no `sceneAccumArray`.
-   **`Process1`** (`0x141519a90`): modes 1 and 2 first; a preprocessed node only when `ignorePreprocess` is clear; `kAlwaysDraw`
    untested; without a compound frustum with operators (or in mode 3) the sphere wrapper (`FUN_141519fe0`) writes
    `alphaGroupStopIndex` even on rejection; with one, `skipViewFrustum` decides whether the sphere or the frustum is tested first,
    and only the sphere-first path leaves the state unwritten on rejection.
-   **Registration** (`AppendVirtual`, `0x140e288e0`, then `AppendNonAccum`, `0x141519e60`): every geometry is appended with the
    current `alphaGroupStopIndex`: codes 1 and 3 join the front list, 2 and 3 register into the back accumulator at once, 0 registers
    nowhere. An untested geometry (mode 1, `kAlwaysDraw`) takes the last state written in traversal order, possibly by an earlier
    sibling's subtree, or a rejection's zero.
-   **Rooms** (`BSMultiBoundRoom::OnVisible`, `0x140e089f0`): the room's frustum is looked up in the entry's visibility map
    (`FUN_140e14460`). Outside modes 1 and 3 a room without one is skipped whole. A room whose own culling mode is neither 1 nor 3
    installs its frustum (or none); the mode is the room's only outside modes 3 and 4; both are restored after its children, the
    frustum only when the outer mode was neither 1 nor 3. `BSPortalSharedNode::OnVisible` (`0x140e24740`) installs its own lookup
    the same way; the light contexts do not carry shared nodes yet.
-   **Multibounds** (`BSMultiBoundNode::OnVisible`, `0x140e2d710`): the shape test takes the process's planes (`+0x3c`), which
    `Process2` leaves inactive, so a point light's multibound never rejects. `kAllPass` (outside mode 3) culls the children in mode 1.
-   **Node types**: `NiNode` (`0x140d1e2c0`) walks its children only with a nonzero radius or `kAlwaysDraw`; `BSFadeNode`
    (`0x141479f50`) and `BSLeafAnimNode` without camera updates skip a node whose fade is not settled at 1 while `currentFade <= 0`
    or `fadeAmount == 0`; `BSTreeNode` applies its height test (process `+0x121`); `NiSwitchNode` culls its active child; geometries
    append with a shader property (`BSSubIndexTriShape` with segments); `BSSegmentedTriShape` and `BSLODTriShape` append nothing.
    `BSOrderedNode`'s and `BSMultiStreamInstanceTriShape`'s variants are not modelled (counted as excluded subtrees).
    `NiBillboardNode` without camera updates is `NiNode`'s; `BGSDecalNode` culls its decals' objects (excluded: a point light
    never casts a decal). The switch index is read at `+0x12c` as the engine does: CommonLib's `NiSwitchNode::index` does not
    land there in the multi-runtime build.
-   **Shared portal geometry**: `Traverse` stores each crossed portal's shared node with a newly allocated, empty frustum
    (`0x1414a4804`), so its subtree takes the sphere alone; a shared node the walk did not cross is skipped. The light contexts
    carry the crossed shared nodes (`Context::sharedNodes`); a shared node reached from two rooms appends its geometry twice.
-   **Ordering**: the lights' `Accumulate` runs inside `CalculateActiveShadowCasterLights`, before DCLF's selection walks the
    lights, so the parity compares at `LightSelectionMade` from a snapshot of each traced cull.

**In-game parity** (AE 1.6.1170, `CS_DCLF_STRICT_LIGHT_PARITY=1`, Bleak Falls Barrow, Dragonsreach and the Bannered Mare, 3,900
frames, camera turning): every traced portal-strict light's appends agreed with the model, 30,136 engine appends with their
hemisphere codes and repeats, no room or shared node membership differing from the engine's own visibility map; 0 to 8 geometries
a window of 10 lights were left out under `BSOrderedNode` and `BGSDecalNode` subtrees. No `kAllPass` multibound and no untested
append occurred, and the per-object predicate (a geometry's own bound in its room's frustum, with the structural gates: hidden,
zero-radius nodes, inactive switch children, unsettled fades, the tree height test, unreached rooms and shared nodes, and no
ancestor bound test) agreed with the engine on every append. Dragonsreach's strict lights cull their `sceneAccumArray`; the
Bannered Mare's lights have no rooms to walk. Not yet run: SE, exteriors with strict lights, scenes with `kAllPass` multibounds.

### Strict point-light gates

The per-object predicate above is what DCLF applies in place of a portal-strict light's native cull (`Portals/StrictGates.h`). An
object's *chain* is its gating ancestors, top-down: `BSMultiBoundRoom` (its program, `1 + room index`, and culling mode),
`BSPortalSharedNode`, `kAllPass` `BSMultiBoundNode`, and a multibound shape the engine would test outside modes 1, 3 and 4 (not
modelled: open, counted). `Evaluate` runs the chain from mode 4 in a light's context: an unreached room or uncrossed shared node
closes it, `kAllPass` makes it untested, and the verdict names the room program whose compound frustum the object's own bound must
pass (none: the sphere alone, which every consumer already tests).

-   **GPU column** (`LightContextResources`, cull binding `ObjectGates`): a versioned `uint` per object slot, its gate (an interned
    chain id; gate 0 is the empty chain). An object takes its fade root's chain; an object without a listed root takes its own
    geometry's chain. The column is rebuilt only when the objects, the roots or the gating structure move (`PortalViews::NoteStructure`
    counts room, shared-node and multibound parents), and only differing words are uploaded to the adopted version. It is declared by
    the shadow views' and the colour segment's BuildDraws passes and reaches them as `BuildDrawsConstants::objectGatesIndex`.
-   **Verdicts** (cull binding `StrictGates`): per frame, a pooled publication of every gate's verdict per atlas context, under the
    atlas identity (`kStrictGateClosed`, `kPortalNoProgram`, or the program). `StrictGateRejects` (`PortalPrograms.hlsli`) reads the
    object's gate, its verdict, and runs the program in the light's atlas entry. A mismatched atlas, missing context, or object past
    the column gates nothing.
-   **Consumers:** a strict light's paraboloid views carry its atlas entry (`ShadowCameras::Camera::portalContext`,
    `BuildDrawsLatch::strictContext`) and reject casters by their gate. The colour segment's shadow volumes carry it too
    (`GpuShadowVolume::portalContext`, 240 bytes) and gate the receivers' shadow mask. The engine's own receivers take the light's
    bit from `NoteMainRegistration` through `Admit` on the geometry's own chain.
-   **The frame's lights:** `CalculateActiveShadowCasterLights` can run twice in a scene frame, and the first call's lights
    accumulate and capture their views with that call's atlas entries. The selection keeps every light any call of the frame kept
    (`Frame::frameLights`), and the atlas holds all their contexts with stable entries.
-   **Native accumulation removed:** portal-strict point lights take the empty culls (no engine cull or registration) like every
    other covered light; `CS_DCLF_STRICT_LIGHT_CULLS=0` keeps them native. A strict light in a cell without a portal graph has no
    context and nothing to gate (`PortalViews::kLightNoGraph`, reported apart from a light without a context).

**Coverage diagnostics** (`CS_DCLF_STRICT_LIGHT_PARITY=1`, the parity frames culling natively): for every geometry in a traced
light's sphere under the structural gates, rooms and shared nodes descended whatever their verdict, DCLF's gates are compared with
whether the engine appended it: by geometry (the receivers' bits) and by object column (the GPU's), each counting engine-only
(a caster or receiver DCLF drops) and gates-only (one it adds) geometries, with the first difference's path and chain. The point
lights' report line counts strict lights culled empty, their bits' gate tests and refusals, tests without a portal graph and
without a context (`<- STRICT CONTEXT`).

**In-game result** (the same route, 3,900 frames): strict lights culled empty on every non-parity frame; every window agreed
exactly by geometry and by object column (0 engine-only, 0 gates-only; 15-138 rootless objects a window gated by their own chain),
with 0 lights without a context: the Bannered Mare's strict lights have no portal graph. A first column keyed by fade root alone
admitted rootless objects under unreached rooms (1-9 a window), which the object column fixed.

**The hemisphere code is not needed** (the ordered prefix stage, `ParabolicStateCS`, stays a replay kernel). A tested append's
code is never 0 or back-only: `FUN_14151a1e0` returns 1 for a bound wholly in front of the plane and 3 for any other bound inside the
sphere. So the engine registers every tested append and draws it in both hemispheres unless it is wholly in front. DCLF's paraboloid
views cull by the same rule (`kCullParabolic`, BuildDrawsCS's `ParabolicRejected` over `ParabolicSphereMask`): the front hemisphere
draws every caster meeting the light's sphere, the back one every such caster not wholly in front of descriptor 0's plane, and the
receivers' bits take the light wherever the gate admits, which is the engine's registration. (They first culled each hemisphere by
its camera's view-projection, which bounds nothing a paraboloid draws: it dropped whole casters, such as Dragonsreach's pillars under
the chandeliers, from the maps.) The code differs
only for an untested append (mode 1 under `kAllPass`, `kAlwaysDraw`), which takes the last state written in traversal order: 1 or
3 from an earlier sibling, or 0 after a sibling's sphere rejection, so the engine leaves it out of one hemisphere or casts and
registers it nowhere, by sibling order alone. DCLF gives such an object the sphere test and its true hemispheres instead (outside
the sphere it can neither occlude nor be lit). Reproducing the stale state would only drop shadows the geometry should cast.

## Remaining integration

Untested appends (above) did not occur on the parity route. `BSOrderedNode` and `BGSDecalNode` subtrees are not modelled by the
parity. Not yet run: SE, exteriors with strict lights, scenes with `kAllPass` multibounds.

The batch integration currently serves main-camera fade roots and members. GPU context-atlas upload is implemented, and the point lights' strict gates
bind the atlas identity (above); hierarchy stages for the batches remain necessary.
The main camera's programs publication has migrated (above). The batches' bucket scratch still grows with the revision's inputs: until
growth is adopted, `Count` evaluates a query whose program lies past the adopted buckets itself, unbatched, with the same verdict (and
the same context-atlas addressing), so a growth lag costs only batching. The lag is logged once per change and its resolution too.

## Validation

`DclfPortalBytecodeTests` compares an independent transcription of the original interpreter against compact traversal and uniform scan
for 40,000 deterministic randomized queries. Additional cases cover tangent/zero-radius/NaN boundaries, zero masks, unreachable malformed
records, operand targets, invalid sets/branches, cycles, forward branch order, and a 1,025-step executable path. The host scan layout is
checked against serial prefix sums across partial blocks and up to 1,000,003 buckets; dispatch tests cover the 65,535-group row boundary
and address-range overflow. Atlas tests also reject mixed frames, zero publication identities, truncated program data,
and inconsistent ranges.

`DclfPortalProgramModelTests` emits random AND/OR trees of portal and occluder tests through the program model's builders (with
`ReopenAsOr`/`Merge` reached-again rooms and `CopyMasked` children), finalizes, encodes and evaluates them by `Visible` against the trees
evaluated directly (200,000 queries), and checks the node rules (zero radius, always-draw, preprocessed, cull modes, unreached rooms).

`DclfParabolicCullingTests` compares 100,000 sphere inputs against an independent native-helper transcription and exhaustively checks
2,560 mode/flag/compound/mask combinations against native Process1 control flow. Boundaries include sphere tangency, zero radius, NaNs,
one versus two accumulators, preprocessing versus always-draw, and ordered hemisphere-state writes, including writes of zero. The hierarchical maximum-prefix model is compared with serial state propagation across partial blocks and multiple levels up to 1,000,003 events, including skipped visits, rejected writers, initial masks, and invalid upstream state.

```powershell
cmake -S tests/DrawcallLimitFixCPU -B build/dclf-cpu-tests
cmake --build build/dclf-cpu-tests --config Release
ctest --test-dir build/dclf-cpu-tests -C Release --output-on-failure
```

Compile all nine `PortalBatchCS.hlsl` entry points (with `Inline`), `FadeStateCS.hlsl`'s `PreparePortalQueries` and `main`, and `BuildDrawsCS.hlsl`'s `ClearPortalQueries`, `PreparePortalQueries`, and `main`
with the runtime DXC SPIR-V configuration. Also compile `ParabolicQueriesCS.hlsl` with independent and uniform traversal, and all four `ParabolicStateCS` entry points. Compile `Evaluate` with all four combinations of standalone/atlas addressing and independent/uniform traversal. Compile `Count` with both standalone and atlas addressing.

`python tools/validate-dclf-portal-shaders.py --dxc <path-to-runtime-dxc.exe>` performs these checks, reading descriptor bindings from
BasicRHI's ABI header. Its default shader directory is the prepared `build/ALL/aio/Shaders` tree; run `prepare_shaders` first.
Add `--spirv-val <path-to-spirv-val.exe>` to validate every generated module against Vulkan 1.3 as well.
These Shader Model 6.6/SPIR-V shaders require DXC; the repository's FXC-based hlslkit validator does not target them.

All four CPU tests, compilation of the twenty-three prepared-shader entry/permutation combinations (`Count` also with atlas addressing), and Vulkan 1.3 SPIR-V validation
passed during implementation. Modified
C++ translation units were compiled separately initially. The original build failures were missing Tracy macro definitions in the
non-profiling configuration and a profiling-only unused parameter. The common precompiled header now loads Tracy after selecting
`TRACY_ENABLE`, so disabled builds get the intended no-op definitions; the profiling-only parameter is marked accordingly.
The missing Streamline packaging dependency was rebuilt with `tools/build-streamline.ps1 -Required`; all four fork plugins were produced.
The complete `BuildDevFast.bat` build subsequently passed. `prepare_shaders`, `COPY_SHADERS`, and `DEPLOY_ALL` passed for `ALL/Debug`.
All 23 SHA-256 comparisons matched: the Debug plugin, nine shaders checked from source to prepared output and from prepared output to
the deployed files, and four Streamline plugins. The deployment directory is `C:/Modding/MO2/mods/Community Shaders DXVK ORG`.
The machine-readable comparisons are in `build/portal-deployment-verification.json`. `COPY_SHADERS` alone does not deploy native dependencies; use `DEPLOY_ALL`
after rebuilding Streamline. Run preparation and deployment serially in a given build directory: `prepare_shaders` can build the DLL,
and concurrent MSBuild targets contend for the same tracking files. The final serial Debug deployment passed. After adding shader files, finish CMake regeneration and rerun preparation before validation; an earlier staging run omitted the new parabolic files. The final prepared and deployed trees include both parabolic files.
Dev-Fast intentionally has auto-deployment disabled in its preset.

After the programs publication migration, the fail-open lowering, the unbatched overflow evaluation and the guarded light-context
publication, `BuildDevFast.bat` and all four CPU tests passed, and the twenty-three combinations compiled and passed Vulkan 1.3
`spirv-val` (Vulkan SDK 1.4.357 DXC, against the source shader tree). That build was not deployed or run.

## Portal culling bench

`tests/DclfPortalBench` runs the portal evaluation outside the game: the production `PortalBatchCS` entry points, compiled at build
time with the plugin's DXC flags, on a BasicRHI Vulkan device of its own, over **corpora** (`Portals/PortalCorpus.h`, `.dclfportal`:
one frame's encoding and the queries a batch evaluated against it). Every variant's verdicts are read back and compared bit for bit
with the CPU oracle, `PortalModel::Visible` (`Portals/PortalProgramModel.h`, the engine-independent half of `PortalViews`: the token
builders, `Finalize`, the encoding and `PortalTestAt` on the CPU); a mismatch fails the run. Each corpus runs 20 warm-up and 200 timed
repetitions, a timestamp pair and full barriers around each.

```powershell
cmake -S tests/DclfPortalBench -B build/dclf-portal-bench -DCMAKE_PREFIX_PATH=<build/ALL/vcpkg_installed/x64-windows-static-md-release>
cmake --build build/dclf-portal-bench --config Release
build/dclf-portal-bench/Release/DclfPortalBench.exe --synthetic --corpus <captures> --csv bench.csv [--programs-only] [--variants V1,V3]
```

-   **Real corpora:** `CS_DCLF_PORTAL_CAPTURE=<directory>` (every `CS_DCLF_PORTAL_CAPTURE_EVERY` depth epochs, default 300) writes the
    fade-root and member batches' queries as the GPU prepared them, with the frame's encoding (`Portals/Diagnostics/PortalCapture.cpp`;
    batches register with `RegisterBatchCapture`).
-   **Synthetic corpora** (`Synthetic.h`): grid rooms with doors, doors to unbound space, occluder boxes and start-room occlusion
    planes, walked by a port of `PortalViews`' walk through the model's builders (so the encodings are real; the portal-quad tests are
    approximated by the quad's sphere). Families: corridor, hub, grid, occluders, deep (the 26-operator cap), exterior, open (wall-wide
    openings: up to 35 programs, 459 sets, 44-step paths) and large (10x10 rooms, 60,000 queries). `--programs-only` keeps only the
    queries that reach a program's records, repeated to 64k: the evaluators' own cost.
-   **Variants:** V0 floor (queries read, verdicts written, nothing evaluated); V1 the batch (count, scan, scatter, tasks, indirect
    evaluate); V2 the batch with the uniform scan; V3 inline (one thread per query, the short-circuit interpreter: `PortalBatchCS`'s
    `Inline`); V3u inline uniform scan; V4 the hand-written baseline, each program's accepting paths flattened on the CPU into terms of
    literals (exact: in a decision program exactly one path is taken); B1/B1b plane bits, inline and batched (each program's distinct
    active planes once into two bit masks, then the records on bits). V4 and B1 are `PortalEvaluators.hlsli`, under
    `DCLF_PORTAL_EXPERIMENTAL` only, with auxiliary tables the bench builds (`AuxTables.h`).

**Results** (AE route Fort Greymoor, Bleak Falls Barrow, Dragonsreach, the Bannered Mare: 36 captures; RTX 3090 Ti, compute queue;
median microseconds; every variant exact on every corpus):

| Corpus | Queries (program) | V0 | V1 | V2 | V3 | V3u | V4 | B1 | B1b |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Captures, no program with records (24) | 4k-16k (0) | 3.6-4.1 | 28.4-30.0 | 28.5-30.2 | 4.9-5.2 | 4.9-5.1 | 4.9-5.1 | 4.9-5.1 | 28.6-30.0 |
| Dragonsreach members (3 programs, 5 sets) | 16,384 (736) | 3.8 | 29.8 | 29.8 | 6.4 | 6.5 | 7.8 | 7.8 | 33.1 |
| Synthetic occluders-1 | 10,800 (1,889) | 3.7 | 32.1 | 32.7 | 8.6 | 9.1 | 20.0 | 12.8 | 36.8 |
| Synthetic open-2 (35 programs, 459 sets) | 10,800 (7,818) | 3.7 | 35.6 | 44.8 | 12.5 | 21.1 | 31.2 | 14.6 | 37.3 |
| Synthetic large-1 | 60,000 (7,804) | 4.4 | 37.7 | 36.6 | 9.2 | 10.1 | 16.9 | 13.6 | 42.3 |
| open-2, programs only | 66,339 | 4.4 | 48.0 | 61.0 | 15.6 | 30.5 | 49.3 | 22.7 | 52.4 |
| hub-1, programs only (1 program) | 65,790 | 4.5 | 63.4 | 61.7 | 6.9 | 7.3 | 7.6 | 7.9 | 64.5 |

-   **The batch costs ~25 us a batch over inline evaluation, on every corpus**, and never pays it back: its count, scan, scatter and
    task dispatches, each behind a full barrier. It is worst with few programs: `Count`'s `InterlockedAdd` on one bucket counter
    serialises (hub, one-program corpora: ~64 us). Two batches run a frame (fade roots, members): about 47 us of GPU time a frame on the
    route.
-   **The short-circuit interpreter beats the flatter evaluators.** The uniform scan and plane bits test every set (plane bits halve
    the planes of the open graphs, 828 distinct of 1,787 active, but computing them all costs more than the short-circuit walk), and
    the hand-written flattened paths repeat the prefixes the decision program shares (2-3x the interpreter on large programs).
-   **Real programs are small**: at most 3 programs with records, 5 sets and a 4-step path on the route, and most queries are immediate
    (frustum-rejected by `PreparePortalQueries`: members 37-99.7%, median 97%; fade roots 46-93%, median 70%). Evaluation is 1-3 us above the 3.6-4 us dispatch floor.

**Decisions.** The batches evaluate inline: one `Inline` dispatch over the queries in the evaluate pass, no count/scan/scatter pass
(`CS_DCLF_PORTAL_EVALUATOR`, default `inline`; `batched` restores the batch). In game, inline with `CS_DCLF_SET_PARITY=1` and
`CS_DCLF_FADE_PARITY=1` over the route: 12 of 12 set parity windows OK (0 records disagreeing with the set) and 12 of 12 fade parity
windows OK (0 differ). The flattened and plane-bit evaluators stay bench-only. Not pursued: a strict-light gate prepass, a CPU collapse
of strict verdicts and three-state cluster culling: each adds at least a dispatch and a barrier (3-4 us alone) to save evaluation that
costs 1-3 us a frame in total.

ORG's pass timestamps read back nothing in this build (`0 epochs read back`, newest sample serial 0), so no in-game GPU timing was taken;
the bench's timings are the measurement. Not measured: SE, AMD and Intel GPUs, and overlap with the graphics queue in game.
