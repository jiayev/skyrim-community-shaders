# Texture streaming

Game textures are the largest cost a save adds to the device heap. With streaming on, the engine no longer allocates most of them.
Each texture holds only the mips its surfaces need, and an object is drawn only once its base mips exist.

On by default, as the Texture Streaming feature (Utility) in the menu, which holds its settings. Turning it off takes effect after a restart, and `CS_DCLF_TEX_STREAM=0` or `=1` overrides it. It needs DCLF: with DCLF off or forced off, new loads stay the engine's.

## How it works

### Shells and windows

The engine needs its texture objects: it and CS keep the SRV pointer, read its description, and treat null as "not ready". A texture
is also loaded before anything knows which objects will use it, and a file can be shared by DCLF objects and native ones. So each
streamed file has two textures:

-   **Shell:** the texture and SRV the engine holds, with the file's real description and no storage. It uses DXVK's deferred storage.
-   **Window:** an ordinary texture holding mips `[top, last]`. It is created on a background thread with initial data, which is the
    call the engine's loader makes. So uploads, spill placement, defrag, attribution and DCLF's imports work as they do for any texture.

`D3D11ShaderResourceView::GetImageView` returns a shell's current window, so every native bind reads it. DCLF imports the window, keyed
by the shell. If something needs the shell's own storage (a `Map` or a copy), DXVK asks for the whole chain and uploads it. The census
found no such use, and the `needed their storage` count shows any that appear.

The hook is on the DDS loader (SE 75721, AE 77533), which vanilla and EngineFixes' replacement both call. It reads the header through a
stream of its own, because the engine's load needs its stream untouched. These stay the engine's:

-   anything but a block-compressed 2D texture with mips, larger than the base window
-   textures under `interface`, `sky`, `water`, `effects` and `cubemaps`
-   streams with no name to reopen

### Admission

A streamed view's `GpuTextures::Request` is answered once its base window exists, through two artifacts of DCLF's graph:

| Artifact | Requires | Runs on | Does |
| --- | --- | --- | --- |
| `TextureWindow` | Nothing | Streaming domain (2 threads, lowest priority) | Reads the file and makes the window. A newer revision replaces an unstarted one |
| `TextureGate` | `ReadyGateAtLeast(window, 1)` | Preparation | Queues the request for the import thread |

Until then the material stays unresolved, so `MainReady` holds its objects out (why = 1), as it did for a slow import.

Engine passes need the same rule, because an unadmitted object is normally the engine's to draw. `PassCapture` withholds every pass of a
lit object whose diffuse or normal map has no base, including depth and shadow passes. Withholding only colour leaves depth behind, which
shows as flat silhouettes of the empty G-buffer. So an object is a short hole until its base arrives. This replaces today's rule that a
texture import never makes a hole.

Anything else that binds a shell with no window (a direct CS bind, rarer texture slots) loads the base right there on its own thread.
That is the safety net, counted as `bound before their base`.

### Swaps

A new window supersedes the import as a DXVK relocation does: `NoteWindowChanged` → `RelocatedViews` → `FollowRelocatedTextures`. The old
window stays alive, held by the import's backing, until its last owner lets go. Objects never leave the set.

Holders that do not follow swaps (grass, tree LOD, landscape seams, frame textures, which all use `ResolveBinding`) pin their texture at
full resolution, and ask again until it is there.

### Demand

One worker thread, `THREAD_PRIORITY_LOWEST`, ticks at 4 Hz. All demand is a **footprint**: log2 of the UV one screen pixel covers. It
needs no texture's size, so one value serves every texture of a material: `top = log2(largest side) + footprint`.

| Source | Footprint | Toggle |
| --- | --- | --- |
| Distance (the floor) | Assumes one UV repeat across the object's diameter. Walks the published tables with bounds a `FrameValues` producer posts for the worker. Not limited to the frustum, so turning the camera streams nothing | Use Distance |
| Feedback (the correction) | One value per material per colour pixel shader, from `ddx`/`ddy` of the base UV, with `MipBias` and the anisotropy ratio | Use Shader Feedback |
| Native binds | A shell bound on 4 ticks in a row (about 1 s) asks for full resolution, so engine-drawn objects look as they did | Always on |

Each texture keeps two demands, because they give up detail in different orders:

-   **On screen:** what feedback reported in the last 8 s, native binds, and distance for a material in view that has never reported. That last
    case covers what cannot report (alpha-tested Lighting, grass, Effect shaders), which would otherwise blur on screen when distance is pulled back.
-   **Distance only:** everything else, which is mostly surfaces out of view.

First person is drawn by the engine unless `CS_DCLF_FIRST_PERSON=1`, so the arms and weapon cannot report. Native binds are their only on-screen demand, which
is why that rule's wait is short. Their distance demand exists, but it is the third-person body's and proves nothing about what the view shows.

The finest of the two wins, after the budget's pull-back. The render thread's only part is a position, a direction and two ratios per frame, in a
commit hook.

**Feedback's cost.** The shader writes straight to host-visible memory through a device address in the material row, so there is no pass,
copy or fence. Only one pixel in 64 writes, on a pattern that moves each frame. Writes are not atomic: a value lost to a race comes back on a
later frame. It is compiled in only when streaming is on, so a run without streaming keeps its shaders and pipelines. Both toggles apply live.

Feedback runs on opaque Lighting only. It needs `[earlydepthstencil]`, or the store turns off early depth testing, and a draw that writes
depth in colour and alpha-tests would then write depth for a fragment it discards. Alpha-tested and other shaders stay on the distance floor.

### Residency rules

-   Upgrades wait one tick, or start at once when 3 or more mips short. The furthest short go first, capped at `CS_DCLF_TEX_STREAM_RATE_MB`.
-   Downgrades need 2 mips of slack for about 5 s. Feedback is kept 8 s, so a surface out of view lets go about 13 s after it was last seen.
-   For about 8 s after the player changes place, what is not wanted goes at once, with no wait or slack, and what shaders reported of the last place is
    dropped. A place is a worldspace, or any interior: the engine's cities may share Tamriel, so a long teleport within one worldspace is not a change, and
    going from one interior to another is not one either. The plain wait covers both.
-   What is on screen upgrades first. An upgrade that does not fit the allowance waits. Over the allowance, the wait for a downgrade is off, and the
    largest surplus goes first, down to what is wanted.
-   A window's top has sides of whole blocks. The base window's largest side is at most `CS_DCLF_TEX_STREAM_BASE` (128), and above 32: Lighting
    treats 32 or less as "no texture".

### The budget

The worker fits the windows to an **allowance**: 92% of the budget, less everything that is not a window. The budget is the VRAM Budget setting, or
the driver's when that is 0. It can be set above the driver's, and past it DXVK places what can spill in system memory.

Each tick, `Policy::StepPullback` sums the bytes the windows would take at the current pull-back, with no build or VRAM reading involved, so it does not
wind up while the windows catch up:

1.  Over the allowance, it adds up to a mip per tick to the distance demand.
2.  Once no distance demand is left to give, it does the same to what is on screen, up to the **On-Screen Detail Offset** setting and only as far as it needs.
    The default is 1. At 0, what is on screen loads what it asks for, under any budget, and past the budget it overflows.
3.  Under it, after 5 s, it steps back a quarter mip per tick while the step would fit 95% of the allowance.

So a load's spike, or a budget lowered in the menu, takes distance first, and turning the camera moves a surface from one demand to the other.

On-screen demand is not capped by the allowance because everything else in the game can fill the driver's budget on a small card. Walking on the RX 6600 took what is not a window to about
5,380 MiB of a 5,700 MiB budget, so the allowance was 0, and every surface stayed at its base however long it was looked at.

**Measuring what is not a window.** It counts DXVK's own memory (used, in both heaps), the windows subtracted, plus what is outside DXVK (the driver's use
less DXVK's allocations). The driver's figures alone do not work:

-   AMD reports the device-local heap's use as the budget at most. Measured on an RX 6600, 580 MiB of new windows moved Vulkan's use by 58 MiB and
    Windows' adapter counter by 498 MiB.
-   While the use reads at the budget, what is outside DXVK cannot be read, so it keeps its last value. Taking the peak instead is wrong: each chunk DXVK
    frees looks like growth outside it. That collapsed the windows to their base 15 s after they fitted.
-   A rise in what is not a window counts within a second or so, and a fall takes tens of seconds, because a replaced window stays held for a while.

**What a budget above the driver's gets.** The driver's budget is the heap less what other processes hold (2.1 GB of the RX 6600's 8 GB on a desktop with
DWM, an editor and a browser open), and Windows did not take that back from them. Dedicated memory stayed near 5.9 GB with 4 GB of windows wanted, so the
rest was in system memory whichever way it got there. An override that made DXVK treat the heap's size as its budget gave the same frame rate within
noise (16.5 against 16.0 fps) and hid the overflow from DXVK's figures, so it was removed.

## What carries over from BasicRenderer

Whole-image mip windows with no sparse binding, upgrade settling, deferred retirement and artifact-gated readiness. Not its 4-mip steps,
its one-way ratchet, its per-sample feedback, its image-table indirection or its upload service. BasicRenderer has no budget and no distance
mode.

## Results

**The numbers below are too low.** Until 2026-11-04 the hook read the header from the engine's stream, so every texture left to the engine
failed to load: uncompressed textures, small ones and the tree LOD atlas. That made terrain LOD black and removed tree LOD. These tables were
measured with that bug, so their savings and frame times need measuring again. `dclf:tex-stats` now reports these failures.

With the fix (Dev-Fast, AE 1.6.1170, a save overlooking distant land, one run each): 1,103 MiB of game textures, against 4,633 MiB with
streaming off. All 498 loads left to the engine succeeded, and terrain and tree LOD match streaming off by eye.

Dev-Fast, AE 1.6.1170, RTX 5080, 3840x2160, `C:\Modding\MO2`, one run each unless noted.

| Game textures in the VRAM dump | Whiterun | Dragonsreach after it |
| --- | --- | --- |
| Streaming off | 3,269 MiB | 3,525 MiB |
| Full chain (`CS_DCLF_TEX_STREAM_FULL=1`) | 3,155 MiB | 3,423 MiB |
| Distance only | 820 MiB | 956 MiB |
| Distance and feedback | 843 MiB | 1,070 MiB |
| Base windows only (no upgrades) | 121 MiB | 123 MiB |

The Dragonsreach column carries textures from the Whiterun stop that had not yet reached their downgrade.

-   **Forced full, then released:** resident 2,925 to 826 MiB in under 60 s, and device heap use 7,638 to 5,530 MiB.
-   **A 4,500 MiB budget:** 212 MiB resident, upgrades held.
-   **Load cost:** 1,364 base windows read in 1,559 ms in all (about 1.1 ms each), and 76 ms to create them. The file cache was warm.
-   **Failures:** none read or created, none needing shell storage, in any run.

The census (Whiterun, DCLF on) found 1,711 file textures, all `DEFAULT` usage and SRV only, none mapped or copied. 3,262 MiB of streamable
textures had no native bind in the last 15 s and would need 38 MiB as base windows. Native binds came to about 20 MiB. After a load the native
figure is much higher for a while (432 MiB), because objects are the engine's until admitted, which is why native demand waits at all.

**Frame time (Release, Whiterun, settled, 3,000 frames, one sample each):**

| Run | Frame time | Game textures | Device heap |
| --- | --- | --- | --- |
| Streaming off | 9.27 ms | 3,246 MiB | 7,936 MiB |
| Distance and feedback | 9.26 ms | 813 MiB | 5,310 MiB |
| Distance only | 9.27 ms | 796 MiB | 5,367 MiB |

This is a standing test: it does not show the cost while loading or turning.

**SE 1.5.97 (Release, RTX 5080, 3840x2160, one run each).** A small load order, so its numbers are not comparable with the AE tables. The save opens the Survival Mode prompt, which pauses the world, so these runs show no motion.

| Run | Game textures, Whiterun | Game textures, Dragonsreach | Device heap, Dragonsreach |
| --- | --- | --- | --- |
| Streaming off | 1,437 MiB | 1,765 MiB | 6,576 MiB |
| Distance and feedback | 500 MiB | 576 MiB | 5,356 MiB |

-   **Screenshots against streaming off:** at most 0.03% of pixels differ by more than 16, and the largest patch checked is a moved object's edge.
-   **Forced full, then released:** resident 319 to 1,091 to 385 MiB, and 370 downgrades.
-   **Failures:** none read or created, none needing shell storage. 128 to 165 binds came before their base, in line with AE.
-   **`CS_DCLF=0`:** no shells, 1,452 textures left to the engine, 1,750 MiB of game textures.

**RX 6600 8 GB (Dev-Fast, AE 1.6.1170, 3840x2160, a save looking at paving, one run each).** About 4.8 GB is not windows, against a driver's budget of
about 5.7 GB. Before the controller, this was 258 MiB resident of 839 MiB wanted, with 7 upgrades in the whole run, because use never fell below 90% of the
budget. It looked muddy everywhere.

| Budget | Allowance | Wanted | Of it on screen | Pull-back, distance | Pull-back, on screen | Frame rate |
| --- | --- | --- | --- | --- | --- | --- |
| Driver's (5,717 MiB) | 466 MiB | 426 MiB | 368 MiB | 3.75 | 0 | 22.5 fps |
| 5,900 MiB | 548 MiB | 511 MiB | 328 MiB | 2.25 | 0 | 22.5 fps |
| 6,500 MiB | 1,100 MiB | 839 MiB | 312 MiB | 0 | 0 | 21.4 fps |
| 12,000 MiB, full chain forced | 5,930 MiB | 4,098 MiB | 2,273 MiB | 0 | 0 | 16.0 fps, 3,721 MiB in system memory |

**Overflow has a cost that depends on the scene.** On the paving above it was about 5%. At a rock wall filling the view after a walk, with the driver's
budget and about 300 MiB of windows in system memory, it was 14.8 fps against about 24 fps with every window at its base, the same spot and the same run. Which
of placement and sampling costs that was not separated: a run with the background programs closed, so that nothing overflows, would show it.

**The offset at one spot (RX 6600, Dev-Fast, a rock wall after a walk, driver's budget, one run, set live):**

| On-Screen Detail Offset | Windows | Frame rate |
| --- | --- | --- |
| 0 | 587 MiB | 17.6 fps |
| 1 (default) | 420 MiB | 20.7 fps |
| 2 | 299 MiB | 21.4 fps |
| 6 | 211 MiB | 24.4 fps |

A 7,400 MiB budget at the same spot let distance demand back in: 874 MiB of windows, 727 MiB in system memory and 13.7 fps. So on this desktop a higher budget
costs frame rate for detail mostly out of view.

**Device loss.** One run on the RX 6600 lost the device 14 s after arriving in Blackreach, standing still at 22 fps, with `VK_ERROR_DEVICE_LOST` from DXVK's
submission and no other sign before it. Four more trips with streaming on and two with it off did not, so this does not say whether streaming is involved. It came before the terrain seam fix (`LandscapeSeams.hlsli`), which removed the RX 6600's other device
loss. After that fix, three trips of about 65 s each, standing still, with streaming on and hang markers on, were clean. That is too few to say the two
were the same defect.

-   **Screenshots:** the paving and axe look sharp at every row by eye, with slightly finer axe detail from 6,500 MiB. The camera did not move far, and
    the axe's idle animation moves it between shots.
-   **Stability:** 5,900 MiB after 6,500 MiB settled on the same 2.25 mips.
-   **A budget enforced:** full chain forced at 7,400 MiB held to 2,019 MiB resident of 2,018 allowed. Forced windows still keep to the allowance; on-screen demand no longer does unless the offset is set.
-   **Failures:** none read or created, none failed in the engine, no device lost.

## Decisions

-   **No disk cache.** The plan expected to need a store of base windows. At 1.1 ms per base, it would not pay for itself, so it was not built.
    That figure is from a warm file cache on this machine. Measure a cold start on a slower disk before relying on it.
-   **Native textures use the path too,** beyond the request for DCLF objects only. Without it, a native draw of a shared file would need a
    second full allocation or a "materialize" state.

## Switches and commands

| Name | Purpose |
| --- | --- |
| `CS_DCLF_TEX_STREAM` | The master, read at startup: `0` turns it off. Shells exist only from the first load |
| `CS_DCLF_TEX_STREAM_FULL` | Every window the full chain, made as the texture loads: the baseline. No policy, no feedback |
| `CS_DCLF_TEX_DISTANCE`, `CS_DCLF_TEX_FEEDBACK` | Override the menu's setting; `0` turns one off |
| `CS_DCLF_TEX_STREAM_BASE`, `_BUDGET_MB`, `_RATE_MB`, `_DISTANCE_SCALE`, `_VISIBLE_OFFSET` | Override the menu's tuning. A set budget also overrides `dclf:tex-budget` |
| `CS_DCLF_TEX_STREAM_DELAY_MS` | A delay before each background build. Shows what late windows look like |
| `CS_TEX_CENSUS` | The survey, which changes nothing: `dclf:tex-census` writes `texture-census-<n>.json` |
| `dclf:tex-stats` | Totals, in the log |
| `dclf:tex-distance=<0\|1>`, `dclf:tex-feedback=<0\|1>` | The demand's two sources |
| `dclf:tex-force=<top>` | Every window at this top, `-1` for the policy's choice. Still held to the allowance |
| `dclf:tex-budget=<MiB>` | The budget, `0` for the driver's |
| `dclf:tex-offset=<mips>` | How far what is on screen may step down over the budget |
| `dclf:tex-dump=<name>` | Every texture's state to `texture-<name>.tsv`, and in the log how many material views draw their shell's current window |

Scenarios: `tools/cs-ab/scenarios/tex-*.env`. Pass switches with `-Env`, for example
`cs-ab.ps1 run dclf tex-stress -Env CS_DCLF_TEX_STREAM_DELAY_MS=100`. A streaming-off baseline needs `-Env CS_DCLF_TEX_STREAM=0`.

## Open questions

-   **Runtimes.** AE 1.6.1170 has run with the header fix. SE 1.5.97 ran before it and needs running again. AE 1.7.104 has not run, and DCLF
    supports it, so streaming is on there too. VR stays off.
-   **Other plugins.** Anything reading game textures directly lands on the storage fill, which is correct but stalls.
-   **Superseded holders.** `SharedBindings` keeps a superseded import, as it does after a relocation, so it holds that window's memory until it
    lets go. Most of its textures are held at base size.
-   **Swap cost.** Each swap costs a publication, and `FollowRelocatedTextures` scans the material slots. No problem seen so far. If one shows in
    profiles, add a view-to-slots index.
-   **Distance floor.** One UV repeat per diameter is a guess. A per-geometry UV density computed on import would be better, and would reduce how much
    feedback has to correct.
-   **Feedback coverage.** Alpha-tested materials, grass and Effect shaders have none, so what is in view stays on screen's demand by distance alone, and the
    view test ignores occlusion.
-   **Device loss on the RX 6600.** See the note under Results: seen once, before the terrain seam fix, and not in three trips since. With `VK_KHR_device_fault` unavailable on this driver, little else shows the cause.
-   **The budget in motion.** One walk was measured on the RX 6600, in one direction. Fights, fast travel and long play have not been.
-   **Chunk free space.** The controller counts used memory, not free space in DXVK's chunks (about 380 MiB in one run), which the 8% margin has to cover.
-   **VRAM Budget's threshold.** At 80% of a small card's driver's budget, the cell unloader runs out of cells to unload, as it did in these runs. It now counts
    a streaming budget above the driver's as room.
