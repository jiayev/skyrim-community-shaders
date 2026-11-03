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
