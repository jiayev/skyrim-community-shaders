# Memory: this branch against dev

Where the extra memory on this branch comes from, compared with Community Shaders `dev` at the same settings, and how to repeat it. Measured
2026-11-03: RTX 5080, 3840x2160, Release builds, `C:\Modding\MO2`, frame generation off. `dev` was `e296f96d` with a small patch for tracking.

## Summary

-   **The reports matched a real bug.** DCLF could hold on to every texture and object the scene released, indefinitely, after going from an interior to an
    exterior. This is fixed.
-   **DXVK itself costs about 1.5 to 2.4 GB more than dev,** at the same stop. Most of that is chunk slack and resources D3D11's driver avoids allocating.
-   **DCLF now adds up to about 1.1 GB on top of DXVK,** and 0.2 to 0.5 GB at Solitude and Bleak Falls Barrow. Before, it added up to 5.4 GB. At Whiterun it reads lower than DXVK alone, because DCLF off keeps more of the previous interior's textures at that stop.
-   **Light counts do not change DCLF's allocation.** The growth that matters is shadow map layers multiplied by the scene's casters, which is now about a tenth of the original (about a quarter of what it was after sharing).

## VRAM

MiB by the kernel's count, the same counter in every build, at the end of each stop of the load chain (1,500 frames after the load).

| Stop | dev | DXVK, DCLF off | DCLF before | DCLF now (2 runs) |
| --- | --- | --- | --- | --- |
| Save (Whiterun exterior) | 8,517 | 10,200 | 11,042 | 10,823, 10,829 |
| Dragonsreach | 8,684 | 10,180 | 11,051 | 10,892, 10,898 |
| Whiterun | 8,866 | 10,649 | 10,617 to 11,477 | 10,049, 9,728 |
| Riverwood | 7,158 | 9,153 | 11,446 to 12,245 | 10,262, 10,005 |
| Solitude | 4,928 | 7,319 | 11,446 to 12,733 | 7,849, 7,592 |
| Bleak Falls Barrow | 5,419 | 7,575 | 9,995 to 10,171 | 7,787, 7,850 |
| Whiterun again | 6,902 | 9,013 | 9,913 to 10,251 | 9,834, 9,900 |

Peaks across the chain: dev 11,296, DXVK 10,777, DCLF now about 11,490. Flying across exteriors (`move`) settles at dev 6,344, DXVK 8,672, DCLF 8,794 and 9,054.

Private bytes follow the same shape, and the idle growth of about 2.5 GB in two minutes is gone. At Solitude: dev 10.8 GB, DXVK 13.9 GB, DCLF 17.0 GB (22.4 to 26.4 GB before).

## The leak

`Direct`'s newest CPU cull frame (`latest` in `Core/Render/Direct/Cull.cpp`) holds the frame's scene tables and its retirement node. Interiors use the CPU
pipeline. On leaving one for an exterior, the graph switches to the GPU pipeline and no new CPU frame ever replaces it.

That pins the publication, and a publication's retirement node holds every later node. So nothing the scene retires from then on is freed: engine references,
texture imports, slots. The log says so:

```
3 snapshots held past the pool (publication:holders) 3000:3 ... engine references 0 released (through publication 3000, ...)
```

It ends when the next interior publishes a CPU frame, which is why it depends on the path: Whiterun to Solitude directly holds nothing, while going through Dragonsreach held
6.2 GB of game textures at Solitude where dev has 1.3 GB, for as long as the player stayed outside. `pcb` did not release them.

The idle CPU stage now lets the frame go (`Direct::DropCullFrame`). At the same stop: 12,701 MiB falls to about 7,400, and 26.4 GB of private bytes to 16.3 GB. To check that it stays fixed, run `hold2` with `CS_DCLF_STATS=1`
and read `snapshots held past the pool`: the oldest should stay within a few publications of `published`.

## DXVK against dev

At Solitude, 7,319 to 7,575 against 4,928. In approximate terms:

| Item | MiB | Notes |
| --- | --- | --- |
| Free space in DXVK's chunks | 500 to 980 | Not in use. Chunks are up to 256 MiB, and it varies with fragmentation |
| dev's driver not committing resources | 450 to 550 | On dev, the kernel reads this much below what the resources' sizes add up to. Probably targets it has not allocated because they are unused. Inferred, not shown |
| Larger or extra resources | about 400 | Against dev's named and unnamed rows at the same stop |
| Driver's own | about 260 | Device-internal 143, pipelines 102, images and command pools 13 |
| ORG, without DCLF | 80 | LightLimitFix |

"Not accounted by DXVK or ORG" in the VRAM report (about 1,205 MiB) is mostly not overhead: 646 of it is DLSS, which dev has as well. It is about 260 of the driver's own, 96 of swap chain, and the rest is not explained.

### Texture delay, the PBR material leak, and tighter defragmentation

Three changes from 2026-11-03, measured with DCLF off (kernel VRAM in MiB; dev has frame generation off):

| Stop | Dev | Before | Delay | Delay + leak fix | Also forced tight (50%) |
| --- | --- | --- | --- | --- | --- |
| Save | 8517 | 10200 | 9179 | 9179 | 9147 |
| Dragonsreach | 8684 | 10180 | 9467 | 9467 | 9371 |
| Whiterun | 8866 | 10649 | 9920 | 9920 | 9396 |
| Riverwood | 7158 | 9153 | 8180 | 8196 | 7796 |
| Solitude | 4928 | 7319 | 6409 | 5919 | 5919 |
| Barrow | 5419 | 7575 | 6665 | 6175 | 6175 |
| Whiterun 2 | 6902 | 9013 | 8020 | 7871 | 7711 |

**Delay:** a texture with no initial data gets memory at its first GPU use, as the native driver does. "Allocate Textures on First Use" in VRAM Budget turns it off (default on).

**Leak fix:** TruePBR reports its material writes to DCLF, and each report holds a reference until DCLF drains it. With DCLF not installed, nothing drained them, so every PBR material written stayed alive with its textures. Full dumps at Solitude: 371 orphaned PBR materials before, 0 after and 0 on dev. Textures alive on this branch but not on dev fell from 383 MiB to 16 MiB. With DCLF on, the queue drains, and a dump showed no orphans.

**Tightening:** defragmentation tightens only once the heap passes the VRAM Budget threshold. `DXVK_DEFRAG_PRESSURE_PERCENT` pins it for testing. The table's last column pins it at 50%, so tightening was on for that whole run. A real over-budget run has not been done, and the cost of the moves in frame time has not been measured.

## Fast switching: DCLF on against dev

Measured 2026-11-03, same machine and settings, Release profile for the branch (Forward family on the CPU included), frame generation off, DLSS at 2560x1440. `switch.env` runs three rounds of six loads, a load every 750 frames, then holds the last stop for 3,000 frames more (1,000 in the scenario now). Means are over the 12 stops of rounds 2 and 3, by the kernel's count, in MiB.

| | dev | DCLF off | DCLF on |
| --- | --- | --- | --- |
| VRAM, mean | 6,474 | 7,389 | 8,315 |
| VRAM, peak | 10,252 | 8,596 | 9,698 |
| Non-local GPU memory, mean | 32 | 263 | 773 |

Dev peaks highest, because of its spike during a load. A second run of dev and DCLF on read 6,459 and 8,397. Single stops can differ by up to 195 (dev) and 610 (DCLF on) between runs, because a chunk's return is a matter of timing.

### DCLF off against dev: 915

| Item | MiB | How it is known |
| --- | --- | --- |
| Free space in DXVK's chunks | 664 (362 to 1,292) | Measured |
| Driver's own, swap chain and other memory outside DXVK and ORG, above dev's | about 280 | Inferred, see below |
| ORG, without DCLF | 80 | Measured |
| Dev's larger textures and geometry | about -100 | Measured |

-   **The upscaler is not the source.** With FSR on both builds the gap is 916, against 915 with DLSS.
-   **"Not accounted by DXVK or ORG" (1,205) is mostly not extra.** DLSS's own textures are about 650 of it. Dev has the same textures, 677 MiB from `sl.common.dll`, listed in its rows as `Other/RenderTarget`, so reading the bucket as DXVK's overhead counts DLSS twice. The rest: device-internal 143, pipelines 102, images and command pools 12, about 96 of swap chain, and about 190 no report covers. The `DXVK_DEVICE_MEMORY_REPORT` runs give the first three; the swap chain and DLSS shares come from the earlier report and are inferred. The bucket falls to 800 under FSR, which has its own unknown share, so 650 was not re-measured directly.
-   **Why "about 280" is inferred.** Dev's resources add up to 500 to 700 above its kernel total, since the native driver does not commit about 915 MiB of targets DXVK now leaves unallocated (see Texture delay). The figure assumes dev leaves exactly those uncommitted. It also grows from about 180 to about 390 between rounds, because dev's kernel total falls while its rows do not. That fall is not explained.

### DCLF on against off: 926

| Item | MiB |
| --- | --- |
| ORG allocated (resources 263 over off's, slack 126 over, untracked 43) | 433 |
| More free space in DXVK's chunks | 316 |
| Driver's own, mostly device-internal (271 against 143) and pipelines (130 against 102) | 109 |
| Storages retired to ORG | 51 |
| Resources | 16 |

Only 433 of the 926 is DCLF's own allocation. Why DCLF on leaves 316 more free in DXVK's chunks was not investigated. Non-local memory adds 510 outside these counts.

### Chunk free space

This is the largest single source, and it depends on timing. At Solitude it reads 2,027 in the first round and 1,036 to 1,292 in the later ones, 690 frames after a load, against 804 at 1,500 frames in `chain`. The period is short enough to catch chunks not yet handed back, so this scenario shows a larger gap than `chain` does at low-memory stops.

`DXVK_DEFRAG_PRESSURE_PERCENT=50` recovers little: means of free space move from 664 to 608 (off) and 980 to 817 (on). At the hold it does gain one chunk: 256 to 192 MiB of VRAM. Chunks are up to 256 MiB and the kernel total moves in those steps, so most of what remains is not tolerance but partly filled chunks.

### Closing the gap: DXVK's chunk policy

Free chunk space has two parts, seen with `DXVK_MEMORY_STATS_LOG=2`. At rest it is about 200 MiB in the device pool and 150 to 550 in the spill pool, with every chunk part full, which the tolerance of `used/8` plus a chunk allows. Right after a load, the spill pool reads 1,400 to 2,700 free, and it drains by one chunk at a time.

The fix, in `extern/dxvk/src/dxvk/dxvk_memory.cpp`:

-   **Device pool chunks of 64 MiB,** not 256. A chunk only returns once it is empty, so smaller chunks empty far more often.
-   **Tolerance of `used/16`.** `used/32` gained nothing more, for about 30% more moves with DCLF on.
-   **Defragmentation every 125 ms,** not 500. It moves one chunk at a time, so without this, smaller chunks return memory four times slower after a load, and Solitude read worse than before. Budgets, cache cleanup, eviction and restore stay at 500 ms.

Eviction margins and the choice of dedicated allocations are still counted in full-size chunks (`fullChunkSize`), because a dedicated resource cannot spill. `DXVK_MAX_CHUNK_MIB`, `DXVK_DEFRAG_TOLERANCE` and `DXVK_MEMORY_TICK_MS` override these for tests: 256, 8 and 500 are the old policy.

Same binary, two runs of each, `switch`, mean over rounds 2 and 3, MiB. Dev is 6,474 and 6,459.

| | Old policy | New policy | Change | Above dev, old | Above dev, new |
| --- | --- | --- | --- | --- | --- |
| DCLF off, VRAM | 7,474, 7,430 | 7,088, 7,074 | -371 | 985 | 614 |
| DCLF on, VRAM | 8,286, 8,414 | 7,739, 7,763 | -599 | 1,883 | 1,284 |
| DCLF off, free in chunks | 740, 721 | 343, 354 | | | |
| DCLF on, free in chunks | 970, 987 | 372, 403 | | | |
| DCLF on, peak VRAM | 9,700, 9,766 | 9,460, 9,460 | -273 | | |
| DCLF on, private bytes | 17,222, 17,422 | 16,733, 16,746 | -583 | | |

The extra free space DCLF used to leave is nearly gone: 39 MiB against 250 to 316 before. Probably its locked and retired allocations pin less when a chunk is smaller; that was not measured apart.

**Cost.** Earlier runs of this policy made about 23,000 moves against 3,800 with DCLF on. Median frame rate and the median and 95th percentile of each second's worst frame stay within run-to-run spread, and stalls over 150 ms stay at one per load. Total load stall time overlaps: 46 to 53 s against 46 to 52 s with DCLF on, 42 to 48 s against 44 to 45 s off. These are frame times at one-second resolution, not a measurement of the copies themselves.

**Under pressure** (`pressure2`, one run of each): 16.4 ms against 21.4 at a 5,120 MiB ballast, and 17.3 against 25.6 at 7,168. Why it improved was not investigated, and one pair is not a settled result.

### What is left

With DCLF on, 1,284 above dev:

| Item | MiB | Notes |
| --- | --- | --- |
| ORG | 547 | Named resources about 300, slack about 170, untracked about 40 |
| Free in chunks | about 390 | |
| Driver's own, above dev's | about 390 | About 280 of it inferred, as above. Device-internal 271 against 143 and pipelines 130 against 102 are measured against DCLF off |
| Retired storages and small resource differences | about 55 | |
| Dev's larger textures and geometry | about -100 | |

ORG's block size (`preferredBlockSize` in `DeviceManager.cpp`) is 64 MiB. Tried with a local patch, one run each: 32 MiB reads about 30 to 110 below 64 at repeatable stops, and 16 MiB about 10 to 70. The steady frame rate did not move. Non-local memory is separate: 773 with DCLF on, 263 off, 32 on dev.

## DCLF's own allocation

At a stop: ORG resources 420 to 530 MiB, ORG allocator slack about 200 MiB, and old storages it still holds about 0 to 160 MiB. The largest are the index pool (56 MiB), main sequences (30 to 78 MiB), LightLimitFix's list (32 MiB, also present with DCLF off).

### Lights

Allocation did not move at all from 0 to 64 added shadow-casting lights (inside and out), or from 0 to 1,024 added plain lights: 511 to 565 MiB, every family flat. The only change was about 200 MiB of private bytes at 1,024 plain lights. Shadow views drawn per frame stayed at 7 to 10, and the engine limits them. Whether every added light reached the engine's lists was not checked directly, but the private bytes did grow.

### Shadow map layers and casters

Every shadow view needs room for the scene's whole casting bound, since culling writes a list of what it draws. That list held a full 136-byte `DrawSequence` per draw, but only the 20 bytes of draw arguments differ by view. The rest describes the object.

So the views now keep only their arguments (`ShadowResources::drawArgs`), and one shared table holds the rest (`casterRows`). A draw's first instance names its row, and a row has the `DrawSequence` layout, so the vertex stage is unchanged and reads it the way the CPU pipeline reads its resident sequences. Each frame `ShadowCasterRowsPass` hands out rows before culling.

The view-type mask cannot replace the list: it says which view types an object could join, not what passed this view's cull, and an indirect draw needs its arguments in memory. There is one row per (caster, partition, view family), because a skin's partitions have their own vertex buffers and an occlusion map's key names its own technique.

```
shadow draws = casters x 1.25 (growth headroom) x (buffers x 32 bytes + families x 136 bytes)
```

Families averaged about 2.85 here (every caster in the sun, spot and point modes shares one family; the two occlusion maps add theirs). A layer's full-slice views and first half also still share arguments (`ShareDrawArgs`). A log error fires if views that share them ever draw together.

| | Buffers (8 layers) | Per 10,000 casters |
| --- | --- | --- |
| Original | 47 | 114 MiB |
| Sharing, no headroom on slots | 22 | 53 MiB |
| Now | 22 | 13 MiB |

Measured over the load chain, MiB of shadow draw buffers at each stop:

| Stop | Before | Now |
| --- | --- | --- |
| Save | 121.3 | 38.5 |
| Dragonsreach | 135.3 | 31.6 |
| Whiterun | 62.5 | 14.2 |
| Riverwood | 62.5 | 14.2 |
| Solitude | 70.8 | 16.2 |
| Bleak Falls Barrow | 45.9 | 10.4 |
| Whiterun again | 76.7 | 17.8 |

Computed from the formula, not measured at these sizes (this test scene reaches about 30,000 casters):

| Casters in the scene | 8 layers before | 8 layers now | 4 layers now | 16 layers now |
| --- | --- | --- | --- | --- |
| 10,000 | 53 MiB | 13 MiB | 10 MiB | 19 MiB |
| 50,000 | 267 MiB | 65 MiB | 50 MiB | 96 MiB |
| 100,000 | 534 MiB | 130 MiB | 100 MiB | 191 MiB |
| 200,000 | 1.0 GB | 260 MiB | 199 MiB | 382 MiB |

A 2 GB report would have needed roughly 180,000 casters on the original build, so the leak is the more likely cause of most reports; a large list on the old build could have both.

**Further cuts are possible:** only 7 to 10 of the 38 views draw in a frame here, so arguments could grow only once their view is first used, and a row needs 56 of its 136 bytes.

To check this, `CS_DCLF_STATS=1` adds `lost for want of a caster row` to the shadow culling line, for the view it samples. It should stay 0. `shadows-ab` takes shadow screenshots at a fixed hour and weather.

## What this does not show

-   **One save, one load order, one machine.** No large list was run. The field reports were not reproduced directly, only explained: the leak gives figures of the reported size.
-   **Private bytes on DCLF.** About 3 GB above DXVK alone at every stop, not broken down.
-   **The dev "not committing" row** is inferred from the kernel reading less than the resources add up to.
-   **Chunk slack before the chunk policy.** Figures above that section are from the old policy. `dxvk.maxChunkSize` is not a DXVK option and did nothing.

## The harness

`tools/cs-ab/cs-ab.ps1` swaps a build's mod folder and shader cache in MO2 and runs scenarios unattended. Close MO2 first.

```powershell
tools/cs-ab/cs-ab.ps1 stage dev <dev checkout>/build/ALL/aio -Pad 4000
tools/cs-ab/cs-ab.ps1 stage dclf build/Release/aio -Pad 4000
tools/cs-ab/cs-ab.ps1 run dclf chain -Label on
tools/cs-ab/cs-ab.ps1 run dclf chain -Label off -Env CS_DCLF=0
tools/cs-ab/cs-ab.ps1 run dev chain
tools/cs-ab/cs-ab.ps1 restore
python tools/cs-ab/compare.py <run folder> <run folder> ...
python tools/cs-ab/compare.py --owners whiterun <run folder> ...
```

Scenarios are `tools/cs-ab/scenarios/*.env`. Results go to `C:\Modding\CS-AB\runs`, and stashed caches to `C:\Modding\CS-AB\variants`. Dev's patch is uncommitted on the local branch `ab/dev-memory` of `external_refs/skyrim-community-shaders`: `src/MemAB.*`, `src/MemProbe.h` and four one-line hooks. It reads the same `CS_DCLF_TEST_*` commands, with the same frame counting, and writes the same VRAM JSON.

To find what keeps a texture alive, compare full process dumps of two builds at the same stop. This needs [procdump](https://learn.microsoft.com/sysinternals/downloads/procdump), and each dump is 13-20 GB, so delete them afterwards:

```powershell
tools/cs-ab/cs-ab.ps1 run dev solitude-dump -DumpAtFrame 7171 -DumpPath dev.dmp
tools/cs-ab/cs-ab.ps1 run dclf solitude-dump -Label off -Env CS_DCLF=0 -DumpAtFrame 7171 -DumpPath dclf.dmp
python tools/cs-ab/dump/texture_chains.py dev.dmp dev.pkl
python tools/cs-ab/dump/texture_chains.py dclf.dmp dclf.pkl
python tools/cs-ab/dump/compare_chains.py dev.pkl dclf.pkl=<dclf run folder>
python tools/cs-ab/dump/climb_holders.py dclf.dmp dclf.pkl PBRLandscape
```

The scripts need numpy and read AE 1.6.1170's vtables.

Three things that caught these runs out:

-   **`-Pad` matters for a cold V1 cache.** usvfs crashes the game when its file table grows while threads read it, and a cold V1 compile creates thousands of files (twice here, within minutes). `-Pad` makes MO2 grow the table first. Field users building a V1 cache under MO2 can hit this too.
-   **Displays must stay on.** With them off, native D3D11 Presents throttle to about 3 fps. The harness holds them on.
-   **`stage` mirrors its source.** Only use a complete AIO folder such as `build/Release/aio`. `build/ALL/aio` can hold just the DLLs, and staging from it would delete the mod's shaders.
-   **The branch can undo a test's upscaler.** With `hardwareDefaultsApplied` false, the first load turns FSR into DLSS. Set it true for an FSR run, and check `method` on the log's `internal resolution` line.
-   **Settings names differ.** Dev reads `frameGenerationMode`, default on, so dev runs with its D3D12 FG proxy (about 900 MiB more) unless it is 0. The branch reads `frameGeneration`. Both are set off for these numbers.
