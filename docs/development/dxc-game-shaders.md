# DXC for the game shaders

Community Shaders compiles its replacement game shaders with FXC, and DXVK turns that DXBC into SPIR-V. This work builds them with DXC
and hands DXVK the SPIR-V directly. DCLF's own programs already take this route.

## Why

The DXBC route costs GPU time. At the same pixels and warps, the grass colour stage takes these on an RTX 5080:

| Route | Instructions per warp | Time |
| --- | --- | --- |
| Native D3D11, FXC | 2,399 | 0.585 ms |
| DXVK, DCLF's DXC build | 2,460 | 0.598 ms |
| DXVK, FXC through dxbc-spirv | 3,206 | 0.772 ms |

So the source is fine and NVIDIA's Vulkan compiler is fine. The translation is the cost.

**What it buys so far.** GPU frame time at the same spot, in the last three 500-frame blocks of one run each (Dev-Fast DLL, so frame rates are not
comparable, but these GPU timers are):

| Build | DCLF off | DCLF on |
| --- | --- | --- |
| FXC | 10.63 to 10.79 ms | 10.23 to 10.28 ms |
| Only `RunGrass` through DXC | 10.46 to 10.52 ms | not measured |
| All game shaders through DXC | 10.31 to 10.51 ms | 10.12 to 10.60 ms |

With DCLF off, grass colour goes from 0.78 to 0.57 ms and accounts for most of the gain, about 2 to 3% of the frame. With DCLF on, where most of the
expensive draws already use DXC, there is no gain that one sample can tell from noise. Lighting does not get faster through DXC (the World pass is
3.52 ms with grass alone and 3.54 ms with everything). Stage 4's repeated Release A/B decides how far this goes.

**Compile time is not the reason.** The roadmap expected DXC to be at least 3x faster. On the slowest variants of each file it is not:

| File | DXC | FXC |
| --- | --- | --- |
| Lighting PS | 4.7 to 5.3 s | 5.7 to 7.5 s |
| Water PS | 0.9 s | 0.9 s |
| Effect PS | 0.45 s | 0.31 to 0.41 s |
| Sky PS | 0.15 s | 0.05 s |
| Utility | 0.11 s | 0.02 to 0.06 s |

DXC's times were taken with 16 builds running at once and FXC's one at a time, so this flatters FXC somewhat. The interface pass described below
adds about 29% to DXC.

**DCLF on against native, in Release** (whole GPU frame at the same spot, last three 500-frame blocks, one run each, Cloud Shadows on):

| Build | FXC | All through DXC |
| --- | --- | --- |
| Native D3D11 | 9.28 to 9.30 ms | |
| DXVK, no graph, DCLF off | 9.76 to 9.99 ms | 9.74 to 9.92 ms |
| DXVK, graph, DCLF off | 9.91 to 10.02 ms | |
| DCLF, CPU pipeline | 9.96 to 10.03 ms | 9.89 to 9.96 ms |
| DCLF, GPU-driven | 10.26 to 10.29 ms | 10.22 to 10.24 ms |

So about half of the gap to native is there with DCLF off, and DXC does not remove it. Trust whole-frame times here: pass timers move when work moves. Keeping DCLF's compute passes on the graphics queue shrinks Depth and Water and grows World and DLSS, and the frame stays the same.

**Removing passes under a fixed GPU load** (Release, DXC on DXVK against dev with frame generation off, no graph, DCLF off, one launch each,
reading the whole frame). A 6.5 ms compute load before Present keeps every row GPU-bound, because without it large drops stop at a floor of about
8 ms. Rows read within 0.03 ms. The temporary `CS_PASS_SKIP` and `CS_GPU_LOAD` switches and the `perf-pass-min` scenario do this.

| Dropped | Dev | DXVK | Gap |
| --- | --- | --- | --- |
| Nothing | 15.80 ms | 16.14 ms | +0.34 |
| Depth | 15.83 ms | 15.83 ms | 0.00 |
| Passes at parity | 13.69 ms | 14.13 ms | +0.44 |
| Passes at parity, Depth | 12.63 ms | 12.92 ms | +0.29 |
| Full-screen passes | 14.47 ms | 14.63 ms | +0.16 |
| Full-screen passes, Depth | 14.50 ms | 14.36 ms | -0.14 |
| Parity, full-screen | 12.44 ms | 12.74 ms | +0.31 |
| Parity, full-screen, Depth | 11.53 ms | 11.75 ms | +0.21 |
| All of Player View | 10.19 ms | 10.14 ms | -0.05 |

The gap is inside Player View: the load, DLSS and the rest of the frame match. Depth's share is 0.15 to 0.34 ms, because the rows do not add up.
Depth saves nothing on dev alone since colour then overdraws, so part of its share may be weaker early rejection on DXVK. That is not tested.

Removed one at a time, heavy geometry passes cost the same on both paths, while each small full-screen pass costs a similar fixed amount more on
DXVK, about 0.02 to 0.05 ms whatever its size. That looks like a cost at each pass boundary, from extra sync or less overlap, but each row is close
to noise and it has not been measured directly. The frame floor is not explained: Display Tweaks' 240 fps limit and the 144 fps Reflex limit are both
faster than it, and the main thread's stacks show no limiter.

## Survey

`tools/shader-compare/dxc_survey.py` builds with DXC every request the game made of FXC, and compares the results with the DXBC the game
stored. Its corpus comes from one run of `tools/cs-ab/scenarios/fxc-corpus.env`: 3,181 requests, 1,187 distinct builds, 86 shader files.

| Check | Result |
| --- | --- |
| Rejected by DXC | 0 |
| `spirv-val` failures | 0 |
| Constant buffer offsets that differ, of 18,954 compared | 0 |
| Constant buffer sizes that differ, in 16-byte units | 0 |
| `$Globals` | 0 |
| Resources FXC uses that DXC removed | 0 |
| Builds where DXC keeps a binding FXC removed (`t30`, `t79`) | 4 |
| FXC signature registers not predicted by the rule below | 0 |

The corpus is what a boot and a short look at one exterior request, so it is not every permutation. Stage 4 widens it.

## Design

The roadmap's container stands. Three decisions changed once the survey had real signatures to test against, and running in game added a fourth.

### Stages

| # | Stage | Pass when | State |
| --- | --- | --- | --- |
| 1 | Offline survey | No rejects, offsets equal, registers predicted | Done, above |
| 2 | DXVK ingestion | A DXC stage draws correctly in game | Done: all 1,187 builds at one exterior, DCLF on and off |
| 3 | CS: store, container, reflection, `CS_SHADER_DXC` | `RunGrass` in game: image match, grass cost at 0.60 ms or lower, hot reload works | Done except hot reload, not tried. Grass is 0.57 ms |
| 4 | Lighting, Utility, Effect, Water, Sky, DistantTree, Particle | Widened survey clean. Image match across `forward-tour.env` (animation frozen, or it hides real differences). Repeated Release frame-time A/B, DCLF on and off | Next |
| 5 | Image space and compute | Same | They ran in the all-DXC runs, not yet checked on their own |
| 6 | Default on, as a setting. Fall back on a failed `Create*Shader`. | Tester reports | |

Stages 2 and 3 were one vertical slice, tested in game together, rather than through the standalone sample the roadmap proposed: a boot takes under a
minute, and the survey had covered what the sample would have checked offline.

`tools/cs-ab/scenarios/dxc-slice.env` (DCLF off) and `dxc-slice-dclf.env` (DCLF on) are the gate: a shot and timers at the newest save, with `-Env CS_SHADER_DXC=<regex>`. Each
variant's store keeps its builds, so a second run of the same selection takes no compile time.
