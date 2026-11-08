# RX 6600: native D3D11 against DXVK, and DCLF GPU-driven

Measured 2026-11-08 on an AMD Radeon RX 6600 (driver 26.10.1, Vulkan driverVersion 2.0.353), 1080p (`iSize` 1920x1080, FSR
quality, so a 1280x720 render), vsync off, Dev-Fast builds of both (`/Od`: CPU numbers read slow, GPU numbers do not). One sample
per rung. Native is `external_refs/skyrim-community-shaders` at `ab/dev-memory` (upstream dev plus the A/B probes). Every DXVK run
has `DXVK_DEBUG=hang`. Harness: `tools/cs-ab` with the `rx6600-*` scenarios; runs in `C:\Modding\CS-AB\runs`.

DXVK's author warns that RDNA1/2 on Windows "can only use the slow legacy binding model, and suffers from severe performance
issues". This build does not use the legacy model: Community Shaders' render graph needs descriptor buffers, which DXVK then
uses too (fmask off: only MSAA would be slower, and Skyrim with CS renders none).

## The ladder

GPU frame time from the `[GpuEvents]` blocks over frames 3000-4000 (ms). At the newest save the frame is GPU-bound (wall equals
GPU). In Whiterun (`coc Whiterun`, a bridge outside the walls) native is CPU-bound: its GPU idles 5 ms a frame.

| Rung | Newest save (GPU-bound) | Whiterun (CPU-bound) |
|---|---|---|
| Native D3D11 | 14.45 | 15.80 |
| DXVK, no render graph (`CS_ORG=0`, legacy binding model) | 17.73 | 15.14 |
| DXVK, graph, DCLF off (descriptor buffers) | 17.81 | 16.43 |
| DCLF CPU-driven | 21.17 | 15.47 |
| DCLF GPU-driven (frustum culling, see below) | 21.57 | 16.11 |
| DXVK, graph, DCLF off, no hang markers | 17.57 | 16.86 |

-   **DXVK costs 3.3 ms (23%) at the GPU-bound save** with DCLF off. The RTX 5080 was about 0.6 ms. In the CPU-bound scene DXVK
    without the graph is faster than native (its own thread takes the submission work); the graph adds 1.3 ms there.
-   **DCLF costs another 3.4 ms of GPU at the GPU-bound save** (shadow views +1.45 over the native shadow passes, World +0.97,
    Depth +0.93), and is a little faster than DCLF off in Whiterun (CPU-bound).
-   Hang markers cost about 0.24 ms at the GPU-bound save; the Whiterun pair differs by noise (CPU-bound).

## Where DXVK's 3.3 ms goes (GPU-bound save, DCLF off)

Pass timers mislead when work moves or the GPU idles inside an event, so the costs below are from live removal
(`rx6600-skip.env`: each pass dropped for 180 frames against its neighbours, four rounds; ms saved by dropping it):

| Pass | Native | DXVK | DXVK more |
|---|---|---|---|
| `ISReflectionBlurHCS` + `VCS` (vanilla compute) | 0.11 | 0.69 | +0.58 |
| Water Effects (the water cubemap) | 0.28 | 0.67 | +0.39 |
| `ISVolumetricLightingBlurHCS` + `VCS` | 0.08 | 0.39 | +0.31 |
| Terrain Blending's passes | 0.58 | 0.85 | +0.27 |
| Shadowmasks | 0.60 | 0.73 | +0.13 |

The timers put another 1.7 ms in the main opaque Lighting batch, of which removal confirms only what is listed; Upscaling is 0.2
ms faster on DXVK.

-   **The work is the same.** D3D11 pipeline statistics (`CS_GPU_EVENT_STATS`) for World, Depth, the cubemap and the sun shadows
    match native to within 2% (vertices, primitives, pixel invocations; the cubemap's to 0.2%), so DXVK is slower per unit of work, not doing more.
-   **Not the binding model:** legacy sets (no graph) against descriptor buffers (graph, DCLF off): +0.08 ms.
-   **Not pipeline libraries:** `dxvk.enableGraphicsPipelineLibrary = False`: +0.01 ms after warm-up.
-   **Partly the dxbc-spirv route:** all game shaders through DXC (`CS_SHADER_DXC=.*`): -0.83 ms, most of it the volumetric blurs
    (their timers 0.39 to 0.07) and a little across Lighting and the shadows. The reflection blur is a vanilla shader CS does not replace, so
    DXC does not reach it.
-   **Register pressure in TRUE_PBR Lighting:** its four pixel variants use all 256 VGPRs and spill 192-272 bytes to scratch under
    DXVK (`DXVK_PIPELINE_STATS`). Native's register use is not visible without AMD's tools.
-   The driver reports no wave size (`subgroupSize` 0) in pipeline executable properties, so Wave32/64 choices could not be read.

## DCLF GPU-driven on this card

-   **It runs now.** The driver has device-generated commands but no pipeline binding stage, so the colour epoch draws the plain
    buckets: one counted indirect draw per pipeline with the pulled colour pipelines (dclf-architecture.md).
-   **Fixed: wrong frustum culling.** The portal interpreter's short-circuit loop gave wrong verdicts on AMD (about 900 visible
    objects a frame culled: trees, rocks, plants missing). AMD now takes the uniform scan (`CS_GPU_AMD`); set parity shows 0
    wrong rejections, and frustum-culled GPU-driven shots match CPU-driven ones.
-   **Open: the HZB reads back empty**, so occlusion culled nearly everything. AMD defaults to frustum culling until it is found
    (dclf-open-defects.md). That costs the Z-prepass about 0.6 ms against the CPU-driven pipeline's culling.
-   Pixel waves do not mix draws on this driver (a probe over 171 Lighting pipelines), so the pulled stages' per-draw descriptor
    indices need no `NonUniformResourceIndex`.

## Locations

`rx6600-loc.env` with `coc <location>`, one sample each (GPU frame ms; "idle": GPU time outside any event, high when CPU-bound).
DCLF at its defaults: GPU-driven with frustum culling outdoors, CPU-driven indoors.

| Location | Native | DXVK, DCLF off | DCLF | Bound by |
|---|---|---|---|---|
| Riverwood | 25.88 (idle 8.2) | 25.27 | 24.65 | CPU (Dev-Fast) |
| Solitude | 14.37 | 17.88 | 21.60 | GPU |
| Bleak Falls Barrow (interior) | 8.81 (idle 1.2) | crashed twice on load | 11.84 | mixed |
| Blackreach | 14.46 | 18.27 | 21.46 | GPU |

Where the GPU limits, DXVK costs 3.4-3.8 ms (24-26%) and DCLF another 3.2-3.7 ms, as at the newest save. Where the CPU limits,
DXVK and DCLF are faster than native.

Three loads crashed (one GPU-driven in Whiterun, two with DCLF off at Bleak Falls Barrow), each in an engine thread during the
`coc` load with SkyrimEngineTelemetry's zone hook on the stack and no DCLF frame (two executed heap memory, one read null);
the Whiterun retry loaded.

## Next

-   The reflection blur: dump the vanilla compute shader (`DXVK_SHADER_DUMP_PATH`), find what dxbc-spirv makes of it (typed UAV
    loads, groupshared layout) and compare its cost per dispatch in the micro benchmark (`C:\Modding\CS-AB\mb`).
-   TRUE_PBR Lighting's spills, then the water cubemap and Terrain Blending's passes, the next largest.
-   The HZB on AMD (dclf-open-defects.md), which would also bring GPU-driven DCLF under the CPU-driven pipeline's cost.
