# Portal query batches: implementation status

## Implemented

The engine compound-frustum builder remains the source of the Boolean expression. Its finalized records are lowered into reachable,
forward-topological 16-byte instructions `{kind, set, trueTarget, falseTarget}`. Lowering rejects cycles, invalid sets, branches outside
the program, operand targets, and unthreaded structural tokens. Each directory entry's last word is its maximum executable path length.
The plane-set ABI and room directory indexing are unchanged. The GPU no longer accepts bounds on exhaustion of a 512-step limit.

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
64-bit context and room generations, sphere/hemisphere metadata, explicit program byte offsets, and dense bucket ranges across contexts.
`PortalTestAt` reads the existing program format at an explicit byte offset; the main-camera wrapper still uses offset zero. The optional
context-atlas evaluator resolves a dense bucket with a workgroup-uniform directory search, handles empty contexts, and rejects mismatched
frame identities. It is compiled and available to future atlas-backed batches; production batches still bind the standalone camera program.
Point-light consumers still require hierarchy and full publication-identity binding. Tests cover
mixed camera/light contexts, identities, offsets, bucket ranges, malformed directories, and ownership after source snapshots change.

## Point-light publication and native contract

`LightContextResources` publishes the selected-light atlas at commits. The atlas has its own 64-bit selection identity in header words
2–3, independent of the 64-bit walk/context and room-map identities in its directories. Validation rejects mixed frames, malformed
context offsets, missing program data, incorrect bucket totals, and missing publication identities before allocating GPU storage.
A pool leases an immutable raw buffer sized to the complete snapshot, using DCLF's existing backing-allocation and cleanup services.
Each execution retains the publication and its lease until GPU retirement. `CurrentLightContexts` rejects a previous frame or an earlier
selection in the same frame. This upload has no revision-capacity substitution and does not reuse old contents on allocation failure.
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

## Remaining integration

This is not the complete point-light migration. Point-light caster lists,
receiver masks, ancestry/multibounds/`kAllPass`, registration side effects, and per-object ownership-gap diagnostics have not migrated.
Native strict-light accumulation remains. A captured context alone does not establish consumer coverage.

The batch integration currently serves main-camera fade roots and members. GPU context-atlas upload is implemented; hierarchy stages,
ordered hemisphere-state propagation, and binding the publication identity to point-light queries/results remain necessary. `PortalPrograms.cpp` also retains its previous empty-header substitution when a snapshot exceeds
publication capacity; the full publication migration must remove it. Batch bucket-capacity mismatches are separately logged as structural
gaps, emit an invalid verdict rather than an out-of-bounds write, and log resolution when capacity catches up.

## Validation

`DclfPortalBytecodeTests` compares an independent transcription of the original interpreter against compact traversal and uniform scan
for 40,000 deterministic randomized queries. Additional cases cover tangent/zero-radius/NaN boundaries, zero masks, unreachable malformed
records, operand targets, invalid sets/branches, cycles, forward branch order, and a 1,025-step executable path. The host scan layout is
checked against serial prefix sums across partial blocks and up to 1,000,003 buckets; dispatch tests cover the 65,535-group row boundary
and address-range overflow. Atlas tests also reject mixed frames, zero publication identities, truncated program data,
and inconsistent ranges.

`DclfParabolicCullingTests` compares 100,000 sphere inputs against an independent native-helper transcription and exhaustively checks
2,560 mode/flag/compound/mask combinations against native Process1 control flow. Boundaries include sphere tangency, zero radius, NaNs,
one versus two accumulators, preprocessing versus always-draw, and ordered hemisphere-state writes, including writes of zero.

```powershell
cmake -S tests/DrawcallLimitFixCPU -B build/dclf-cpu-tests
cmake --build build/dclf-cpu-tests --config Release
ctest --test-dir build/dclf-cpu-tests -C Release --output-on-failure
```

Compile all eight `PortalBatchCS.hlsl` entry points, `FadeStateCS.hlsl`'s `PreparePortalQueries` and `main`, and `BuildDrawsCS.hlsl`'s `ClearPortalQueries`, `PreparePortalQueries`, and `main`
with the runtime DXC SPIR-V configuration. Also compile `ParabolicQueriesCS.hlsl` with independent and uniform traversal. Compile `Evaluate` with all four combinations of standalone/atlas addressing and independent/uniform traversal.

`python tools/validate-dclf-portal-shaders.py --dxc <path-to-runtime-dxc.exe>` performs these checks, reading descriptor bindings from
BasicRHI's ABI header. Its default shader directory is the prepared `build/ALL/aio/Shaders` tree; run `prepare_shaders` first.
Add `--spirv-val <path-to-spirv-val.exe>` to validate every generated module against Vulkan 1.3 as well.
These Shader Model 6.6/SPIR-V shaders require DXC; the repository's FXC-based hlslkit validator does not target them.

All four CPU tests, compilation of the eighteen prepared-shader entry/permutation combinations, and Vulkan 1.3 SPIR-V validation
passed during implementation. Modified
C++ translation units were compiled separately initially. The original build failures were missing Tracy macro definitions in the
non-profiling configuration and a profiling-only unused parameter. The common precompiled header now loads Tracy after selecting
`TRACY_ENABLE`, so disabled builds get the intended no-op definitions; the profiling-only parameter is marked accordingly.
The missing Streamline packaging dependency was rebuilt with `tools/build-streamline.ps1 -Required`; all four fork plugins were produced.
The complete `BuildDevFast.bat` build subsequently passed. `prepare_shaders`, `COPY_SHADERS`, and `DEPLOY_ALL` passed for `ALL/Debug`.
All 17 SHA-256 comparisons matched: the Debug plugin, six shaders checked from source to prepared output and from prepared output to
the deployed files, and four Streamline plugins. The deployment directory is `C:/Modding/MO2/mods/Community Shaders DXVK ORG`.
The machine-readable comparisons are in `build/portal-deployment-verification.json`. `COPY_SHADERS` alone does not deploy native dependencies; use `DEPLOY_ALL`
after rebuilding Streamline. Run preparation and deployment serially in a given build directory: `prepare_shaders` can build the DLL,
and concurrent MSBuild targets contend for the same tracking files. The final serial Debug deployment passed.
Dev-Fast intentionally has auto-deployment disabled in its preset.

Neither SE nor AE runtime verification has been performed for this migration; the AE static audit is not runtime validation.
No Skyrim runtime parity, GPU batch execution/readback comparison, vendor ISA inspection, or NVIDIA/AMD/Intel performance measurements
have been completed. No measured speedup or shipping-backend selection is claimed. Compare original inline interpretation, grouped
traversal, and uniform scan on identical captures, including preprocessing, scan and barrier costs. Require exact verdict parity,
improved portal-heavy frame time, and no more than 5% median trivial-scene regression before accepting the optimization.
