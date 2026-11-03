# Memory: this branch against dev

Where the extra memory on this branch comes from, compared with Community Shaders `dev` at the same settings, and how to repeat it. Measured
2026-11-03: RTX 5080, 3840x2160, Release builds, `C:\Modding\MO2`, frame generation off. `dev` was `e296f96d` with a small patch for tracking.

## Summary

-   **The reports matched a real bug.** DCLF could hold on to every texture and object the scene released, indefinitely, after going from an interior to an
    exterior. This is fixed.
-   **DXVK itself costs about 1.5 to 2.4 GB more than dev,** at the same stop. Most of that is chunk slack and resources D3D11's driver avoids allocating.
-   **DCLF now adds up to about 1.1 GB on top of DXVK,** and 0.2 to 0.5 GB at Solitude and Bleak Falls Barrow. Before, it added up to 5.4 GB. At Whiterun it reads lower than DXVK alone, because DCLF off keeps more of the previous interior's textures at that stop.
-   **Light counts do not change DCLF's allocation.** The growth that matters is shadow map layers multiplied by the scene's casters, which is now less than half of what it was.

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

## DCLF's own allocation

At a stop: ORG resources 420 to 530 MiB, ORG allocator slack about 200 MiB, and old storages it still holds about 0 to 160 MiB. The largest is the shadow sequences, then the index pool (56 MiB), main sequences (30 to 78 MiB), LightLimitFix's list (32 MiB, also present with DCLF off).

### Lights

Allocation did not move at all from 0 to 64 added shadow-casting lights (inside and out), or from 0 to 1,024 added plain lights: 511 to 565 MiB, every family flat. The only change was about 200 MiB of private bytes at 1,024 plain lights. Shadow views drawn per frame stayed at 7 to 10, and the engine limits them. Whether every added light reached the engine's lists was not checked directly, but the private bytes did grow.

### Shadow map layers and casters

This is what can reach gigabytes on a large list. Every shadow view needs a sequence buffer sized for the scene's whole casting bound, and the placements are a catalog, not what is in use: per `kSHADOWMAPS` layer, a full-slice view in each of two modes, and two paraboloid halves.

```
shadow sequences = buffers x casters x 1.25 (growth headroom) x 204 bytes
```

| | Buffers (8 layers) | Per 10,000 casters |
| --- | --- | --- |
| Before | 47 | 114 MiB |
| Now | 22 | 53 MiB |

Two changes: a layer holds one light a frame, so its full-slice views and first half share a buffer (`ShareSequences`; a pair of halves does draw together, so a layer has two buffers), and the 25% headroom on slots (9 full-size buffers no view used) is gone. A log error fires if views that share a buffer ever draw together, and it did not fire in any run. Measured: 395 to 186 MiB after the save, 267 to 135 in Dragonsreach. Each extra `kSHADOWMAPS` layer adds 2 buffers now, where it added 4.

| Casters in the scene | 8 layers before | 8 layers now | 4 layers now | 16 layers now |
| --- | --- | --- | --- | --- |
| 10,000 | 114 MiB | 53 MiB | 34 MiB | 92 MiB |
| 50,000 | 570 MiB | 267 MiB | 170 MiB | 460 MiB |
| 100,000 | 1.1 GB | 534 MiB | 340 MiB | 920 MiB |
| 200,000 | 2.2 GB | 1.0 GB | 680 MiB | 1.8 GB |

These are from the formula, not measured at those sizes. This test scene reaches about 30,000. A 2 GB report would need roughly 180,000 casters before these changes, so the leak is the more likely cause of most reports; a large list on the old build could have both.

**A further cut is possible:** only 7 to 10 of the 38 views draw in a frame here, so buffers could grow only once their view is first used. A view not ready yet is already left to the engine, so that path exists. It needs per-view sizing in the shape, so it is not done.

## What this does not show

-   **One save, one load order, one machine.** No large list was run. The field reports were not reproduced directly, only explained: the leak gives figures of the reported size.
-   **Private bytes on DCLF.** About 3 GB above DXVK alone at every stop, not broken down.
-   **The dev "not committing" row** is inferred from the kernel reading less than the resources add up to.
-   **Chunk slack is not addressed.** The one option tried (`dxvk.maxChunkSize`) is not a DXVK option and did nothing.

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

Three things that caught these runs out:

-   **`-Pad` matters for a cold V1 cache.** usvfs crashes the game when its file table grows while threads read it, and a cold V1 compile creates thousands of files (twice here, within minutes). `-Pad` makes MO2 grow the table first. Field users building a V1 cache under MO2 can hit this too.
-   **Displays must stay on.** With them off, native D3D11 Presents throttle to about 3 fps. The harness holds them on.
-   **Settings names differ.** Dev reads `frameGenerationMode`, default on, so dev runs with its D3D12 FG proxy (about 900 MiB more) unless it is 0. The branch reads `frameGeneration`. Both are set off for these numbers.
