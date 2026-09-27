# DCLF: a frame without the engine's culling jobs

Status: phases 1-4 done, 2026-09-24 (phase 4's step 4 is phase 5's), and the resident draws persist across frames;
phase 5 next. The reasoning behind it is in [dclf-status.md](./dclf-status.md), "What removing the culling
jobs takes". The current state is in [drawcall-limit-fix.md](./drawcall-limit-fix.md), "The primary's cull without
DCLF's objects".

## Goal

The engine's list jobs (`ListAccumulationJob`) and the per-object work they do go away:
-   for DCLF's objects, entirely;
-   for the residue, replaced by a flat visibility pass with no traversal.

Every view culls on the GPU. The per-object engine state the cull maintained moves to one of two places:
-   events, for what changes rarely: switch selection;
-   the GPU, for what changes per frame with the camera: fade, LOD, and the tree clock.

The per-object state tolerates a frame of latency. The residue's visibility does not.

## Phases

1.  **A stood-in object needs nothing from the CPU per frame for its main pass** (detailed below).
    -   The sun's bits move to the GPU.
    -   The synthetic pass becomes static, rebuilt on change only.
2.  **Visibility feedback, and the per-object state from it.**
    -   A per-entry, frustum-only visibility bitmap from the GPU cull, read back a frame late.
    -   The fade, LOD, leaf and tree updates and the tree clock's `kAccumulated` run on DCLF's worker from that
        bitmap, not in the list jobs; later as GPU state (fade and LOD in a persistent buffer, the cross-fade copy as a
        second dithered draw).
    -   Admission comes from readiness (the pipelines and records exist), not from a draw, so entries never seen in view
        leave the lists too.
3.  **Switch selection as events.**
    -   Hook the writers of `NiSwitchNode`'s index (reverse engineer them: harvest, destruction, activators), plus the
        catch-up update of a newly selected child, run in the event handler.
    -   "Which child is live" becomes table state; the per-frame switch test and the stale-child fallback go away.
4.  **DCLF's entries leave the lists outright.**
    -   With phases 1-3, an admitted entry needs nothing from its job. The list filter is built on the worker from an
        immutable set: the list edit of the first version, but with the decision precomputed.
    -   The `Process1` stand-in shrinks to the entries that still need the engine.
5.  **The residue without traversal.**
    -   Decals as synthetic passes: the order key rebuilt from the traversal position, and depth-writing decals drawn in
        DCLF's Z-prepass.
    -   `BSOrderedNode` subtrees and actors (one entry, `ObjectLODRoot`'s second child) brought in.
    -   What stays native (effects, blended, LOD, billboards) is found visible this frame by a flat frustum pass on a
        worker and handed to the engine's registration (`AppendVirtual`).
    -   Then the list jobs are no longer queued. The extra list, room and portal visibility, and the list processes'
        camera and planes stay.
6.  **The other views.**
    -   The sun's actors join DCLF's shadow set.
    -   Local shadow lights need the shadow-light selection rule (`FUN_1414fcf80`), which also makes the synthetic pass
        valid under local lights. That lifts the primary cut's precondition.
    -   Reflections and the precipitation mask as DCLF-native variants.

## Phase 1: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "The sun's bits on the GPU":
-   the static rule plus a per-draw cascade test in BuildDraws;
-   `kObjectSunMiss` on the object word;
-   `UnifySunBits` for engine-registered passes.

GPU = CPU exactly, 0 holes, and pipelines 88-90 -> 72-74. Caching the whole synthetic pass moved to phase 4.

## Phase 2: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "Visibility feedback":
-   the frustum stamps from depth phase 1;
-   an asynchronous readback ring (as BasicRenderer's CLod streaming) with DCLF's own timeline;
-   the decode on the worker, kicked after registration and joined at Present;
-   the roots held by the frame's tag.

Admission from readiness moved to phase 4, where the pass becomes table state.

## Phase 3: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "Switch selection by event":
-   the ten index stores patched (the tree manager, the local map, harvesting) and `NiSwitchNode`'s child edits detoured;
-   the catch-up (`CatchUpSwitch`) when the walk applies an event, and at attach;
-   the switch trait off the per-frame set (1,707 -> 1,057 entries at Riverwood);
-   `memberLive` in the stand-in, and the stale-child fallback gone.

## Phase 4 in detail: resident entries

**Why not a list edit.** The engine rebuilds the scene lists every frame (`DrawWorld_BuildSceneLists`, `0x14064bc20`,
appending each reference root round robin through `FUN_14021cf40`). Filtering there would also take the entries out of
the sun's full-frustum cull and the precipitation mask, which read the same lists. The jobs keep the lists; what goes is
the work an entry needs from its job.

**What an entry still takes from its job** (the stand-in, per frame):
-   the frustum test that picks which synthetic passes are built;
-   the root's settled check;
-   the hidden walk and switch selection per member;
-   the hand-over of the engine's members.

The synthetic pass itself also goes through the accumulate phase every frame, which restores what it patched at the next
walk. So nothing about an entry persists from one frame to the next except its admission.

**Resident entries.** An entry whose record needs nothing per frame is made resident:
-   **Its objects' accumulated half persists.** It is built once, through the accumulate phase's own patch, from its
    synthetic pass. It is never restored per frame, and it keeps `kObjectNativeVisible`, so BuildDraws draws it
    whenever the GPU's cull finds it (frustum, occlusion, the sun's cascade test). No CPU visibility is involved.
-   **Its list job returns at once**, from a lookup by root.
-   **Its fade, LOD and `kAccumulated`** come from the visibility feedback, as today for stood-in entries.
-   **The slots stay alive.** Each frame the accumulate phase keeps a resident's pipeline and material slots alive
    (`lastUsed`, and the lighting template when no other object used the pipeline). That is O(residents) writes, not
    patches.

**Joining** (render thread, when the cut applies, a bounded number a frame):
-   admitted, or on probation (step 2);
-   any eligible plan (trees since step 3);
-   every member the switches select DCLF's;
-   the root settled;
-   every selected DCLF member with a record that is written only by events: no face, actor or animated shading, and not
    written in full every frame (a kept skin qualifies since step 3).

**Leaving** (immediately, in the frame it happens):
-   **The walk rewrites or releases a resident's record** (`WriteObject`, `ReleaseObjectSlot`, the slot check): an
    event changed it, so the entry goes back to the stand-in and may join again.
-   **The patch fails** (a verdict of the frame, a material not ready, extras rows needed).
-   **The feedback decode finds the root no longer settled** (it started to fade or cross-fade).
-   **A frame where the cut does not apply** (a local shadow light, the sun's exclusion not live, the menu toggle):
    every resident leaves. A stale snapshot does not end residency, since residents do not use it.
-   **The frame globals the static sun bits read change**: every resident leaves.

**Entry 0 of a list** goes through the engine's `Process2`, so the engine registers it even when it is resident. The
accumulate phase leaves a resident's record alone in that case, and static ownership withholds the engine's pass.

**Checks:**
-   A resident parity (`CS_DCLF_RESIDENT_PARITY`, every 60 frames): each resident's synthetic pass is built again and
    compared with the one it was patched with, and its record with the patch.
-   Walk parity, and 0 holes.
-   Main-pass objects: the engine's kept ones plus the residents in view match a run without the cut.

**Steps** (all in [drawcall-limit-fix.md](./drawcall-limit-fix.md), "Resident entries"):
1.  Resident entries as above, with admission from a draw. **Done.** At Riverwood about 580 of the 827 stood-in entries
    became resident.
2.  Admission from readiness (probation) and the fade distance on the GPU. **Done.**
    -   An entry the engine found out of view joins on probation. The colour epoch's build admits it when it drew every
        member.
    -   BuildDraws' depth phase 1 drops a resident past its fade-out distance that was not in view last frame
        (`kObjectFadeTest`).
    -   The decode ends a resident whose LOD level or LOD metric state changed, in view or not.
3.  Trees. **Done.**
    -   The height test is on the GPU (`kObjectHeightTest`).
    -   The animation rows are taken from the tree manager's node state every frame. The feedback keeps that state
        advancing.
    -   Kept skins can be resident.
4.  Entries with engine members stay in the stand-in until phase 5.

Riverwood holds about 2,150 resident entries a frame (2,990 records). Resident parity, set parity and holes are 0.

**Found along the way: DCLF's build scales with drawable candidates.** Probation takes about 0.2 ms of cull off the list
jobs and 0.1 ms of waiting off the render thread. It adds about 0.7 ms of render-thread waiting on the epoch builds,
which build a draw for every resident each frame: about 4,230 candidates against 1,830. Most of them are
occluded, and the GPU's HZB rejects them. **Fixed** by the persistent resident draws
([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Persistent resident draws"): both epoch joins are now below their
cost with no residency at all.

## Open: an engine crash while loading (to investigate)

First seen 2026-09-24 while building the persistent resident draws. It has crashed 3 times, in 4 runs of the same
kind; the fourth run did not crash.

-   **Where.** `SkyrimSE.exe+0x783642`, in `FUN_140783590`: `cmp qword ptr [rcx+0x1F8], 0` with `rcx` null, an access
    violation reading `0x1F8`.
-   **Call stack.** `PlayerCharacter::sub_1406A4540` (`+0x6A45DC`) ← `FUN_1402eb3c0` ← `FUN_140657fc0` ←
    `FUN_140653a80` ← `TESObjectREFR::sub_140606640` ← ... ← `FUN_140645f20`, on the main thread. The objects nearby
    are a `MovementControllerNPC` (RBX) and the `Character` "Geirlund" (R15), which points to NPC movement or AI.
-   **When.** Right after `coc Riverwood` (test command at frame 1), about 13 s into the run, while frames run at
    400-460 ms each. The crash logs are `crash-2026-09-24-23-00-10.log`, `crash-2026-09-24-23-04-22.log` and
    `crash-2026-09-24-23-07-05.log` in `My Games\Skyrim Special Edition\SKSE`, with `CommunityShaders.dmp` from the
    last one.
-   **Environment.** Full featureset (`CS_DCLF_ASYNC=on`, `CS_ORG_ASYNC_EPOCHS=1`), with
    `CS_DCLF_RESIDENT_DRAW_PARITY=1` in every crashing run, and `CS_DCLF_RESIDENT_PARITY=1` plus
    `CS_DCLF_SET_PARITY=1` in two of them. No DCLF frame is on the probable stack; Community Shaders appears only
    in the stack scan (`TraverseScenegraphCollision`).
-   **What it follows.**
    -   **The `coc` at frame 1.** Every crash came from a run with the `coc Riverwood` test command at frame 1, straight
        after the save loads, while NPCs such as Geirlund are still being set up. Runs with `coc` at frame 300 have not
        crashed.
    -   **Not the parity checks, and not the resident draws being on.** It crashed with none of the parity checks on
        (rt4, rt5, 23:18) and with `CS_DCLF_RESIDENT_DRAWS=0` (rdE1, 23:11).
    -   **The builds since the change log.** With `coc` at frame 1, the builds before 23:00 ran 10 times without a
        crash: the Tracy A/B fa1-fa6, rd2-rd5, and rd1, which lost the device instead. The builds since then crashed 9
        times in 14 runs. The first of those builds (22:59) turned the resident change feed into an append-only log
        (`Tables::residentLog`, noted from `MoveObject`, `AppendKeptSkin`, `ResetObject` and the resident mark and
        drop). That code runs whether the region is on or off, and it only writes DCLF's tables.
-   **Not yet known:** whether DCLF (or another Community Shaders feature) writes something the engine then reads, or
    the engine races on its own once the load frames are slow enough. Earlier crash signatures today were different:
    `FUN_140ea5cc0` (the loader-thread tree crash) and `FUN_1414b2130`.
