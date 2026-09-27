# Crash catalog

Crashes met while developing the Drawcall Limit Fix, from the crash logger's logs in
`Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`. AE 1.6.1170 addresses, with the Address Library
ID in brackets. Newest first.

## Open

### `SkyrimSE+14F79AA`: a material virtual call during a model load

| | |
| --- | --- |
| Seen | 2026-09-22 23:32, 23:52, 23:58; 2026-09-23 00:12, 19:52. The variant below: 23:57 and 00:07. |
| Thread | An IO thread (`IOManager::DoOnPreRunTask` on the stack), loading a model: `BSResource::EntryDB<BSModelDB>`, `BSStream`, `BSResourceNiBinaryStream`, `CompressedArchiveStream`. |
| Fault | `call [rax+0x18]` in `FUN_1414f7850` (107719+0x21A) reads 0xFFFFFFFFFFFFFFFF: a virtual call through a vtable that is not one. |
| Objects | R14 a `BSLightingShaderMaterial`, and on the stack the `BSLightingShaderProperty` being loaded and its `NiAlphaProperty`. |
| Stack | `FUN_1414f7850` (107719) <- `FUN_141476cc0` (105544) <- `FUN_1414ac820` (106494) <- `SkyrimSE+0D21AE7` (70381, which the logger labels `NiPSysSphericalCollider::Func38`, a mislabel). |
| Variant | The same call jumps to address `0x6` ("tried to execute memory at 0x6"), returning into `SkyrimSE+14F79AD` (107719+0x21D) from `FUN_140d25d00` (70502). Same function, same vtable call, different garbage. |
| When | Loading cells: repeated `coc`, and the test harness's flights. Seen with `CS_DCLF_FADING=0` too, so it is not the fading work. |
| Models | The geometry on the stack when named: `Door:7` (23:58), `RoadCurve90R01:0` (19:52). |

**Attribution: unknown.** DCLF writes none of the engine's material or property memory. It keeps pointers
to materials as cache keys, rewrites only its own material records (`RefreshMaterialPatch`), and runs the
engine's material setup on the render thread (`ConstantEvaluator::EvaluateMaterial`). A material whose
vtable reads as garbage on the loading thread looks like a use after free, or an object being built while
something else reads it.

**What the faulting call is (decompiled, 2026-09-23).** The function starts at `1414f7790` (107719; the
logger names it by the block `1414f7850`). It is the engine's shared material cache: `BSShaderProperty::
SetMaterial` (`14147c033`, frame 1's `FUN_141476cc0` reaches it) calls it to swap a newly loaded material
for an identical cached one.

-   It takes the cache's spin lock (`1435ef070` owner thread, `1435ef074` count), hashes the incoming
    material (vfunc 4, `ComputeCRC32`), and walks the hash chain at `cache+0x30` (entries of `{hash,
    material, next}`, 0x18 bytes).
-   For each entry with an equal hash it calls the **cached** material's vfunc 3, `DoIsCopy(incoming)`, at
    `+0x18`. That is the faulting `call [rax+0x18]`.

So the object with the bad vtable is a material still linked into the cache, not the one being loaded. Its
vtable pointer is ordinary heap memory: `rax` is `0x1B67F66AAC0` at 19:52, and `[rax+0x18]` holds
0xFFFFFFFFFFFFFFFF or 6. A material freed while its cache entry survived, then its memory reused, fits
both. The dereference is the first touch of the stale entry, so the free happened earlier, on any thread.

**Pattern across the test runs.** Five of the seven crashes have a run log (`CS_DCLF_TEST_COMMANDS` runs;
00:07 and 00:12 have none):

-   Four came within about 1 s of the fourth `coc` of a four-`coc` run (23:32, 23:57, 23:58, 19:52).
-   One came 17 s after the second `coc` (23:52).

None of the 65 other logged runs with one or two `coc` crashed, and every four-`coc` run in the logs did.
Every crashing run had DCLF on, with static ownership and shadow views; `CS_DCLF_ASYNC` was set to probe,
set to on, or unset.

**Hooks near materials, for the record:**

-   DCLF hooks `CopyMembers`, `OnLoadTextureSet`, `ClearTextures` and `ReceiveValues` on 14 material
    vtables, and the two property controllers (`MaterialSources.cpp`). Each thunk calls the original and
    then only records the pointer (`NoteWritten`): no reference counting, no writes.
-   Several stack scans also hold `EffectShaderNoDecalsFix::BSTriShape_LinkObject::thunk`
    (`src/EngineFixes/EffectShaderNoDecalsFix.cpp:12`, `BSTriShape` vfunc 0x19). It sets `kNoDecals` on
    soft-effect geometry after the original. These are stack-scan hits ([S]), not probable frames, and
    likely stale return addresses from the same load.
-   The first `MaterialSources` commit is 2026-09-23 02:18 (`169489e2`), after the first crash (22 23:32).
    But the code was in the working tree before that, so this does not clear it.

**Next steps:**

-   Repeat a four-`coc` run (`CS_DCLF_TEST_COMMANDS=600:coc WhiterunDragonsreach;1200:coc Riverwood;1800:coc
    Whiterun;2400:coc Riverwood`) three times each with `CS_DCLF=0`, with DCLF on but ownership off, and
    with Community Shaders off. The crash reproduces reliably at four `coc`, so three clean runs are
    evidence.
-   If it follows DCLF: find who releases a material that the cache still holds. Hook the cache's removal
    and the material destructor (vfunc 0) under a switch, log pointer and thread, and match them against
    the faulting entry.

## Fixed

### `CommunityShaders.dll`: `NativeProbe::OnNativeLightingDraw` (2026-09-22 22:36)

An access violation reading `0x1FFDD`, inside `SetupGeometry`'s hook (`Hooks.cpp:230` -> `NativeProbe.cpp`).

-   **Cause:** the native probe read an `NiSwitchNode`'s `childRevID` array through CommonLib's members.
    CommonLib declares them after `NiNode`, whose declared size in a multi-runtime build is VR's, so the
    members read the wrong memory.
-   **Fix:** `SceneStore::ReadSwitch` reads them at the SE/AE offsets (engine notes, "Switch nodes").

## Earlier, not investigated here

From the logs of 2026-09-22 before the tree and actor work:

-   **01:06 and 01:10:** a C++ exception from `Util::` in `src\Utils\D3D.cpp:237`, called from
    `GrassOptimizations.cpp`.
-   **01:15:** an engine null read in `FUN_140e4c730` (77397), from `bhkConstraintChain::Func55` on the
    pathing and navmesh job.
-   **01:59:** a null read in `org::StatisticsManager::SetupQueryHeap`, from
    `org::runtime::CreateDefaultStatisticsService` during `RenderGraph::Update`.
