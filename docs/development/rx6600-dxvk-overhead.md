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
-   **Fixed (2026-11-08): objects disappearing while the camera turns.** That parity check was with a still camera. In a camera
    sweep, set parity counted about 100k in-view rejections per 300 frames, all from the portal programs (0 with
    `CS_DCLF_CULL=none`), in both evaluators and on either queue. The cause is AMD's compiler: in `PortalTestAt`, past the
    `program == kPortalNoProgram || program >= count` return, it took the program to be `kPortalNoProgram`, so every lane read its
    directory entry at byte 128 + 0xFFFFFFFF * 32 = 96 (the mask and counts). That gave cull mode 18, the skip-view bit and the
    wrong operator bases. The SPIR-V was right (`BASICRHI_VK_DUMP_LOWERED`); the ISA (`BASICRHI_VK_DUMP_ISA`) loaded the entry
    from constant offsets 0x60, 0x64 and 0x78. `program >= count` alone covers both cases, and parity now shows 0 wrong culls in
    sweeps. With the bug restored, it flags 105k to 127k per 300 frames.
    -   Found with the decision trace (`CS_DCLF_PORTAL_TRACE` with `CS_DCLF_PORTAL_CAPTURE`): the inline batch records each
        query's deciding step and hashes of the planes and directory entry it loaded; the capture compares them with the CPU's.
        The GPU's directory hash matched the captured words from word 24 on, not word 32 where program 0's entry is.
    -   Set parity's frustum count also held occluder and portal culls. It now judges each in-view rejection on the CPU's program
        and flags the ones it has in view (`<- WRONG CULLS`). About 98% of the remaining in-view rejections in a sweep are culled by
        the program too (occluders), about 2% are decided on the preprocessed node flags, which the CPU reads later.
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

## The blurs through DXC

The reflection blur is the game's `ISBlurCS` (descriptors `0x6C` and `0x6D`, defines `GRIDSIZE_X=960` or `GRIDSIZE_Y=540`,
`TEXTAP=9`, `BLUR_SCALE=2`, `SIGMA=2`, `OUTPUT_CHANNEL_COUNT=4`). `package/Shaders/ISBlurCS.hlsl` transcribes it from the
dumped DXBC. Built with FXC, it matches vanilla instruction for instruction, apart from an added bounds test on the store, and costs
the same. Without `CS_SHADER_DXC`, Community Shaders now builds it and the volumetric blurs with DXC under DXVK (`CS_SHADER_DXC=^$`
turns that off). Native D3D11 keeps FXC.

GPU frame ms, run again in one session (vsync off, one sample per rung):

| Rung | Newest save (GPU-bound) | Whiterun (CPU-bound for native) |
|---|---|---|
| Native D3D11 | 14.43 | 15.60 (GPU idle 4.96) |
| DXVK, DCLF off, blurs through FXC | 18.16 | 16.82 |
| DXVK, DCLF off, blurs through DXC | 16.96 | 16.01 |
| DCLF GPU-driven, blurs through DXC | 20.55 | 15.07 |
| DCLF CPU-driven, blurs through DXC | 19.98 | 14.60 |

-   **DXC saves 1.2 ms at the GPU-bound save**, which cuts DXVK's gap to native from 3.7 ms to 2.5 ms (26% to 18%). The four
    dispatches account for 0.99 ms of it: reflection blur 0.73 to 0.06, volumetric blurs 0.39 to 0.07.
-   With DCLF at its defaults, the GPU-bound save went from 21.57 (GPU-driven) and 21.17 (CPU-driven) to 20.55 and 19.98.
-   The water cubemap and Terrain Blending are not helped by DXC. Both draw the game's Lighting shaders, and with every game shader
    through DXC they moved by +0.01 and +0.06 ms. Their extra cost on DXVK comes from somewhere else.

## Attributing what was left

### The blurs' real cause: zero-initialized shared memory

dxbc-spirv declared every groupshared array with a null initializer. AMD's Windows driver implements that as a fill from one
lane per wave before the shader starts (`s_ff1_i32_b64 exec`, then one `ds_write_b128` after another): 64 serial stores for a
1 KiB array, about a thousand for the reflection blur's 15.6 KiB. D3D11 leaves shared memory undefined, and the micro benchmark's
groupshared case runs the same on native D3D11 as on DXVK without the fill (1.47 and 1.46 ms; 3.43 ms with it). dxbc-spirv now has
an option for it, and DXVK leaves shared memory uninitialized on AMD's proprietary driver (`DXVK_ZERO_LDS=0|1` overrides). With it,
the blurs built with FXC cost the same as with DXC.

DXVK also lowered every float-to-int conversion to about seven instructions on this driver. Its own conversion gives D3D11's
results for NaN, infinities and out-of-range values, signed and unsigned (the micro benchmark's `ftoi-values`, the same as native),
so the lowering is off there now as on NVIDIA (`DXVK_LOWER_FTOI=0|1`). That makes the micro benchmark's `Load` and conversion cases
match native (2.2x to 1.06x), but changes nothing measurable in game.

| Rung (newest save, 1280x720 render, GPU ms) | Start of this round | Now |
|---|---|---|
| Native D3D11 | 14.43 | |
| DXVK, DCLF off | 16.96 | 16.55 |
| DCLF CPU-driven | 19.98 | 19.71 |
| DCLF GPU-driven | 20.55 | 20.27 |

### Native against DXVK on the same build

The native build above is upstream dev, so its features differ from this branch's. This branch runs on the system D3D11 with
`CS_NATIVE_D3D11=1`, which also turns Upscaling off, so both sides of this pair render the full 1920x1080 with Upscaling disabled at
boot. GPU event timers now work there too: native D3D11 reports no capture tool, so Community Shaders had skipped the perf events
the timers read. The gap was 2.40 ms before the shared memory change and is 1.80 ms after (16.50 against 18.30). It barely changes
with resolution (2.53 ms at the 1280x720 render), so it is not pixel shading. Where it is, by exclusive pass time:

-   **Present and submission boundaries, about 0.4 ms.** DXVK splits the frame into about 11 submissions, and at each boundary the
    GPU idles 0.04 to 0.14 ms although the next one was queued milliseconds earlier (`CS_GPU_IDLE_TRACE`: 0.42 ms a frame). The
    present blit is 0.08 ms. Fewer splits (`d3d11.reproducibleCommandStream`) moved the frame by 0.07 ms, within noise.
-   **Pass boundaries, about 0.4 ms** (self time of Player View and World). 109 render passes and 410 barrier commands a frame. The
    barriers' own work is 0.4 ms in total (`DXVK_BARRIER_TIMING`) with no decompression among them; the cost is the drain each
    one forces. In the micro benchmark a render target change costs 3.2 us on DXVK against 0.73 us native.
-   The water cubemap's persistent pass list +0.2, SSGI +0.18 and SSS ray march +0.14 ms, and Lighting batches about 0.1 ms each.
    One grass batch is 0.18 ms faster on DXVK.

Ruled out:

-   **Wave size.** AMD's Vulkan driver compiles the large deferred Lighting pixel shaders as wave32 and the D3D11 driver as wave64
    (`DXVK_PIPELINE_ISA`, RGA's DX11 mode). Forcing wave64 (SPIR-V 1.5 with a subgroup-size read, DXC) moved the frame by 0.02 ms. The
    driver ignores a required subgroup size for fragment shaders, as the spec allows.
-   **Barrier work itself and layout decompression:** see above.

### DCLF: interop images rested in GENERAL, which cost depth compression

**Fixed 2026-11-08: resting layouts.** The runtime opts in to `DXVK_ORG_INTEROP_RESTING_LAYOUTS` (`dxvkSetInteropOptions`), so
DXVK no longer pins imported images. `DxvkImageImport` imports each image exclusively at the layout DXVK reports and seeds it as
the texture's home layout (ORG's `ExternalTextureResource::SeedInitialLayout`), and BasicRHI samples depth in
DEPTH_STENCIL_READ_ONLY_OPTIMAL. ORG's barrier trace shows the depth entering from DEPTH_STENCIL_READ with no discard; no colour or
depth image is pinned. `CS_ORG_RESTING_LAYOUTS=0` restores the pinning. Frame times (Dev-Fast, vsync off):

| Scene | DCLF off | CPU-driven | GPU-driven |
| --- | --- | --- | --- |
| Newest save, pinned GENERAL | 16.55 ms | 19.71 ms | 20.27 ms |
| Newest save, resting layouts | 16.47 ms | 18.21 ms | 18.52 ms |
| Whiterun, resting layouts | 16.52 ms | 12.21 ms | 12.61 ms |

What it was: every image the render graph imports rested in GENERAL on this driver (`DxvkImage::markOrgGeneral`), for DXVK's passes as well as
the graph's. With DCLF on, that is the main depth, both shadow map arrays (4096x4096x8 and x2), the terrain depth, the volumetric
lighting and precipitation depth maps, the main colour, motion vectors and the shadow mask (`DXVK_LOG_ORG_GENERAL=1`). With DCLF
off, no render target or depth buffer is pinned.

GENERAL turns depth compression off on AMD's Windows driver: putting every DXVK image in GENERAL (the unified layout path) makes the
DCLF-off frame 1.3 ms slower, almost all of it in the shadow maps and the depth prepass. CPU-driven DCLF did not change, because its
images are in GENERAL already. That is likely most of DCLF's remaining cost here (shadow views +1.45 ms, Depth +0.93 ms over the
native passes).

Letting those images keep their resting layouts is not a DXVK-only change. Imported exclusively at DXVK's layout, they rendered
wrongly (terrain missing, corrupt depth tiles): the graph uses them in COMMON, and every execution enters them from UNDEFINED with
discard (ORG's barrier trace, `ORG_TRACE=barriers`), which was harmless while they were simultaneous-access images in GENERAL.

## Next

-   Pass boundaries: keep render targets in their attachment layout until something reads them, instead of returning each to
    its resting layout after every render pass.
-   The HZB on AMD (dclf-open-defects.md), which would also bring GPU-driven DCLF under the CPU-driven pipeline's cost.
