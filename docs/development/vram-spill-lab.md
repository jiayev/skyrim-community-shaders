# VRAM under pressure: what a spill costs, and the priorities

What it costs when a resource ends up in system memory, what the driver demotes first, and the tools to measure both. Measured
2026-11-03 on an RTX 5080 at 3840x2160, Release profile, in `C:\Modding\MO2`.

## Summary

-   **Frame generation's memory was the first thing lost.** DLSS, DLSS-G and FidelityFX allocate behind DXVK at the driver's default
    priority (0.5), below DXVK's protected pool and the graph (1.0).
-   **The fix raises them, not lowers us.** `VkDriverTrace` now sits on the driver's `vkAllocateMemory` and sets other clients' video
    memory to 1.0.
-   **Cost follows how much a resource is read and written each frame, not its size or usage flags.** 15 MiB of volumetric lighting
    history costs 190 ms. 4.9 GB of game textures costs 0 to 10 ms.
-   **No further priority bands are shipped.** With the fix, frame times stay flat at every pressure level tested, so there is nothing yet
    for more bands to improve. See [What this does not show](#what-this-does-not-show).

## Priorities under real pressure

The `pressure2` scenario in Whiterun, DLSS-G on, DCLF on. A 7,168 MiB ballast takes the process about 3.2 GB over budget (about 17.5 GB used
of 14.25 GB).

| Scheme | DXVK protected | ORG | FG and DLSS | Frame time p50 |
| --- | --- | --- | --- | --- |
| Before | 1.0 | 1.0 | 0.5 | 195 ms |
| Everything tied at default | 0.5 | 0.5 | 0.5 | 152 ms |
| Only ORG lowered (FG raised) | 1.0 | 0.5 | 1.0 | 37 ms |
| **Shipped** | 1.0 | 1.0 | 1.0 | **30 ms** |

With no ballast it is 14 ms, and 19 to 24 ms at 5,120 MiB. FSR-FG behaves the same: about 4 presents a second before (it did not finish its
window in 170 s), 32 ms with the fix. `CS_VK_CLIENT_PRIORITY=0` restores the old behaviour for comparison.

Ties do not protect anything, because the driver does not know which memory is hot. Any scheme that leaves FG's memory level with the rest
still collapses.

### Why a hook

The clients get their Vulkan entry points themselves, so nothing above the driver sees these allocations. The hook resolves the caller's
module from the stack. It leaves `dxvk_*` and `CommunityShaders.dll` alone, since they set their own priorities, and leaves host-visible
memory alone. It installs only when the driver exposes `vkSetDeviceMemoryPriorityEXT`, so a driver without
`VK_EXT_pageable_device_local_memory` is not hooked unless tracing is on. Only NVIDIA was tested.

## What a spill costs

One group or resource at a time, moved to system memory while the game runs. Whiterun with DCLF off unless noted. "Group" rows use
200-frame windows; "single" rows use 60-frame windows, whose baseline drifts between about 13 and 19 ms, so treat single results under
about 5 ms as zero. Costs do not add up exactly: a group can cost more or less than its members.

| Class | Resource | MiB | Added frame time |
| --- | --- | --- | --- |
| Catastrophic | `kIMAGESPACE_VOLUMETRIC_LIGHTING` | 47 | +208 ms |
| | `kIMAGESPACE_VOLUMETRIC_LIGHTING_PREVIOUS` | 15 | +190 ms |
| | All game render targets (group) | 1,069 | +353 ms; +258 ms in Dragonsreach |
| | ScreenSpaceGI (group) | 520 | +105 ms; +110 ms in Dragonsreach |
| | DLSS (all of it, from creation) | 646 | about 7 presents/s |
| | FidelityFX (all of it, from creation) | 646 | about 8 presents/s |
| | DLSS-G (all of it, from creation) | 531 | about 14 presents/s |
| Severe | `kSHADOW_MASK` | 31 | +70 ms |
| | `SSGI::WorkingDepth` | 21 | +54 ms |
| | `kSSR` | 50 | +40 ms |
| | `SSS::ShadowTexture` | 15 | +26 ms |
| | `kSHADOWMAPS`, `kSHADOWMAPS_ESRAM` | 320, 64 | +23 ms each (+4 ms for all in Dragonsreach) |
| | `Deferred::Masks` | 47 | +22 ms |
| | Deferred (group) | 302 | +57 ms; +23 ms in Dragonsreach |
| | HDRDisplay (group) | 391 | +18 ms; +20 ms in Dragonsreach |
| Moderate | `SSGI::Radiance`, `kLDR_DOWNSAMPLE0`, `kMAIN`, `kMAIN_COPY`, `HDR::kLDR_DOWNSAMPLE0` | 4 to 135 | +10 to +19 ms |
| | `sl.common` (Streamline's clones, from creation) | 231 | +9 ms |
| | Every ORG buffer and DCLF table (from creation) | about 780 | +6 ms |
| Sampled | Game textures | 4,889 | +10.5 ms outdoors, +0.1 ms in Dragonsreach |
| Near zero | Vertex 126, index 25, constant 1, structured 36 (groups) | | +0.1 to +0.3 ms |
| | `kBOOK_TEXT*`, `kGETHIT_*`, `kMENUBG`, `kSCREENSHOT`, `kSNOW_SWAP`, `kLOCAL_MAP*`, `kIMAGESPACE_TEMP_COPY*`, `Skylighting::ProbeArray` | about 1 GB | no effect above the noise |

### The rules

1.  **Cost scales with bytes crossing PCIe per frame.** Size alone predicts nothing.
2.  **Worst: small targets sampled many times per pixel,** by ray marches, multi-tap filters and depth chains. They can cost several
    ms per MiB.
3.  **Then full-screen targets read or written several times a frame:** the G-buffer, the shadow mask, depth, and shadow maps when many casters render.
4.  **Sampled textures are cheap per MiB,** about 0.002 ms, and cost nothing if the view does not sample them. That justifies the spill pool.
5.  **Geometry and constants are close to free here,** unlike what the usage class would suggest. The scene is not geometry-heavy, so this may
    not hold for all scenes.
6.  **A target not used this frame costs nothing,** whatever its flags. About 1 GB of the protected pool is in this state outside its feature's
    moments (book open, hit effect, menu, screenshot).
7.  **Upscaler and FG memory behaves like the worst class:** it is read and written whole, several times, every frame.

### Going further

If a future scene does exhaust the spill pool, the data suggests this order for more bands, lowest priority first: sampled textures and idle
targets, then buffers, then history targets, then everything else, with upscaler and FG memory and the hot targets on top. DXVK demotes
whole blocks, so a band is a pool. `dclf:demote` and `dxvkSetResourcesSpillable` move a named image into the spill pool, for testing that.

Marking the near-zero targets for the spill pool today did not help: 36 ms against 29.5 ms at the pressure levels tried, because the pool
already held enough to demote and the marked targets include some that are used. It is not enabled.

## What this does not show

-   **Past the spill pool.** Frame time stayed flat from 3 to 8 GB over budget (29.5 ms with FG, 27.5 ms without). The ballast is never used by
    the GPU, so once demoting textures is not enough, the driver may be demoting the ballast itself. So this does not show what happens
    when real, used memory exceeds the budget by more than the spill pool. Testing that needs a ballast the GPU touches every frame.
-   **Other hardware.** One RTX 5080. AMD and Intel drivers may rank memory differently, and a system with slower PCIe will pay more per byte.
-   **Pinned resources with DCLF on.** Many images DCLF imports cannot move (3,034 of 3,220 MiB of textures stayed), so texture rows from DCLF-on runs do
    not count. Render target groups moved and agree with DCLF off: +275 ms and +113 ms for the game targets and ScreenSpaceGI.
-   **Individual ORG buffers.** Only ORG memory as a whole was placed in system memory.
-   **Demotion against placement.** Spilled rows place resources in system memory directly rather than wait for the driver. The texture row agrees with
    what real pressure showed (+6 to +10 ms at 5,120 MiB), but only that row was cross-checked.

## The tools

All of it is inert until asked for. Commands go in `CS_DCLF_TEST_COMMANDS`; `tools/cs-ab/scenarios/spill-*.env` and `pressure*.env` are worked examples.

| Command or switch | What it does |
| --- | --- |
| `dclf:spill=<filter>` | Moves the D3D11 resources a filter picks to system memory, and holds them there, within one run |
| `dclf:spill-clear`, `dclf:spill-check` | Brings them back; logs how many moved and how many stayed |
| `dclf:frame-window=<label>` | Logs p50, p95 and max frame times since the last window (`-` starts one that is not logged) |
| `dclf:ballast=<MiB>[:<priority>]` | Holds device memory, for real pressure. Needs the hook, and frees with 0 |
| `dclf:demote=<filter>` | Moves images to the spill pool: still video memory, demoted first |
| `CS_VK_SPILL=<module>,...` | A module's device memory in system memory from creation, for FidelityFX, DLSS and DLSS-G. Needs `CS_VK_DRIVER_TRACE=1` |
| `CS_VK_PRIORITY=<module>=<priority>,...` | A module's priority. Needs `CS_VK_DRIVER_TRACE=1` |
| `CS_VK_CLIENT_PRIORITY=<priority>` | Other clients' priority; 0 leaves them alone |
| `DXVK_MEMORY_PRIORITIES=<spill>,<protected>` | DXVK's two pools' priorities |
| `DXVK_DEVICE_MEMORY_REPORT=1` | Logs the driver's own memory by `VkObjectType` every 10 s |

A filter is alternatives separated by `|`, each made of terms separated by `&`. A term is text in `owner/name`, or `class:sampled`, `rt`, `ds`,
`storage`, `buffer`, `vb`, `ib`, `ubo`, `ssbo` or `indirect`. Resources that cannot relocate are reported as staying.

Note that DXVK is a separate meson build: `ninja -C extern/dxvk/build` in a Visual Studio x64 prompt. The CS targets only copy its DLLs.
