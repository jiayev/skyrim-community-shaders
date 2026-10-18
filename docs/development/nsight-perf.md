# Nsight Perf GPU counters

Developer Tools can measure NVIDIA GPU hardware counters per render pass with the
[Nsight Perf SDK](https://developer.nvidia.com/nsight-perf-sdk) range profiler. A capture records
metrics such as `gpu__time_duration`, `sm__throughput` and the warp stall reasons over named
ranges: the frame's perf events (`State::BeginPerfEvent` / `EndPerfEvent`, the same names
RenderDoc and Tracy show) and, on DXVK, the render graph's passes.

There is one integration (`NvPerf`): one API, one panel, one devbench tool, one CSV and one set of
switches. Its backend follows the device in use:

| Backend    | When                                                                | Profiler                                                                   |
| ---------- | ------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| **D3D11**  | the system D3D11 runtime (no DXVK, or `CS_NATIVE_D3D11=1`)          | NVPW's D3D11 range profiler on the game's immediate context                |
| **Vulkan** | DXVK, once the render graph adopted its Vulkan device               | OpenRenderGraph's `Telemetry/NvPerfCapture` on DXVK's graphics queue       |

The backend is chosen once, the first time the profiler is probed. Under DXVK the D3D11 profiler's
driver cannot see the device, so the D3D11 backend is never chosen there; if the render graph has
not adopted DXVK's device (yet, or at all), the profiler reports itself unavailable. It needs an
NVIDIA GPU the SDK supports, and is a developer build option.

## Building

The SDK is a login-gated NVIDIA download, so the build never fetches it.

-   **In SARP:** `NVPERF_SUPPORT` defaults on, with `NVPERF_SDK_ROOT` pointing at the SDK
    BasicRenderer ships (`../../BasicRenderer/ThirdParty/NVPerf`). Nothing to configure. A build
    tree configured before this default (cached `NVPERF_SUPPORT=OFF` with an empty root) gets it
    applied once on its next configure.
-   **Elsewhere:** download and extract the Nsight Perf SDK, then configure with
    `NVPERF_SUPPORT=ON` and `NVPERF_SDK_ROOT=<the directory holding include/nvperf_host.h>` (in
    the SDK archive this is `NvPerf/`). The `ALL-WITH-NVPERF` preset in
    `CMakeUserPresets.json.template` does this on top of `ALL-WITH-AUTO-DEPLOYMENT`.

One option and one SDK drive both backends. With it on:

- the plugin links `nvperf_grfx_host.lib` (as the `NVPerf::Host` target) with
    `/DELAYLOAD:nvperf_grfx_host.dll` and defines `NVPERF_ENABLED` (the D3D11 backend);
- when the render graph is built (`CS_HAS_RENDER_GRAPH`), OpenRenderGraph is built with
    `ORG_ENABLE_NVPERF` against the same target (the Vulkan backend);
- the AIO folder and `cmake --install` (component `DXVK`) get
    `SKSE/Plugins/CommunityShaders/bin/nvperf_grfx_host.dll`, beside the DXVK DLLs.

Both backends load the DLL from that one path the first time the profiler is used. If the file is
missing, profiling reports itself unavailable and nothing else changes.

With the option off (CI, and wherever the SDK is absent), the profiler compiles to stubs: the panel
and the devbench tool report that the build has no Nsight Perf. Without the render graph there is
no Vulkan backend, and under DXVK the profiler then reports itself unavailable.

If a session fails with `NVPA_STATUS_INSUFFICIENT_PRIVILEGE`, enable GPU performance counters for
all users in the NVIDIA Control Panel ([ERR_NVGPUCTRPERM](https://developer.nvidia.com/ERR_NVGPUCTRPERM)).

## How a capture runs

- **Passes.** Each frame is one profiler pass, ended before the swap chain's Present (so the ImGui
    overlay is not measured) and begun after it.
    - D3D11: on the immediate context, around Present.
    - Vulkan: the frame boundary is a DXVK queue callback enqueued before Present. The profiler's
        queue calls run on DXVK's submission thread with the queue locked, between its submissions.
- **Samples.** The chip collects only so many counters per pass, so one sample takes as many
    frames as its metrics need passes. A capture collects `samples` of them one after another.
- **Delay.** `delayFrames` presents pass before the first pass. On Vulkan, ranges then record
    from the capture's arming, and the first pass begins the graph host's frame slots + 1 frames
    later, once every command buffer that can execute in it (the graph records its epochs ahead)
    was recorded while armed.
- **Ranges.**
    - The ranges are the perf events whose names match the capture's filters. A trailing `*`
        matches a prefix. No filters means every range, except `State::Draw`'s per-shader-change
        markers, which are measured only when a filter names them.
    - On Vulkan the render graph's passes are ranges too, by their pass names (e.g.
        `cs.dclf.main-opaque`), and so are the parts of Drawcall Limit Fix's colour pass
        (`cs.dclf.colour.*`, pushed with `NvPerf::PushCommandBufferRange`). Engine events and
        graph passes sit side by side on the same queue, so they are measured alike.
    - On Vulkan a range must open and close in one command buffer: at each DXVK flush the open
        engine ranges close and reopen in the next one, so an event that spans a flush (e.g. around
        a graph epoch) is measured in pieces, which the summary adds up.
    - With one nesting level only the outermost selected ranges are measured. On D3D11 each extra
        level also measures selected ranges nested one deeper, and multiplies the passes.
- **Frame Annotations.**
    - A capture turns Frame Annotations on for its duration, since most perf events are only emitted
        with it on.
    - Engine passes (shadow maps, `BSShaderAccumulator` batches, cubemaps) are hooked only when Frame
        Annotations is enabled at startup. `CS_NVPERF_RANGES` turns it on at startup.

Results:

- **Summary:** per filter (or per name, with no filters) and per frame. Counters are summed and
    rates are weighted by each range's `gpu__time_duration`. A range nested inside one that
    matches the same filter is already counted in it, so only the outer one is summarized. Each
    capture's summary is also logged (`[NvPerf]` in `CommunityShaders.log`).
- **CSV:** every range of every sample is appended to `CommunityShaders-NvPerf.csv` next to
    `CommunityShaders.log` (or `CS_NVPERF_CSV`).

Metrics are written `name[:kind]`, where the kind is one of:

| Kind                | Meaning                                                |
| ------------------- | ------------------------------------------------------ |
| `counter` (default) | sum                                                    |
| `ratio`             | the ratio submetric                                    |
| `throughput`        | average % of peak sustained, elapsed                   |
| `active`            | counter average as % of peak sustained, active cycles  |
| `elapsed`           | counter average as % of peak sustained, elapsed cycles |
| `percycle`          | counter average per active cycle                       |

A metric the chip lacks is reported as unavailable, and the rest of the capture still runs.

### Backend differences

| | D3D11 | Vulkan |
| --- | --- | --- |
| Ranges | perf events | perf events, graph passes, DCLF's colour pass parts |
| `nestingLevels` | 1-8 | 1 only (OpenRenderGraph's profiler configures one level); more is refused |
| `RangeResult::path` / `enclosing` | the enclosing selected ranges | the range's own name / empty |
| Chip name | at probe | after the first capture (until then `gpuSupported` means the library initialized; the startup probe is logged) |
| Status | `Decoding` while the GPU finishes a sample | never `Decoding`: the profiler decodes on DXVK's submission thread at the last pass's boundary; the pass count is approximate until the first sample |
| Cancel | immediate | ranges stop at once; a profiler sample already armed still runs out (within a few frames) before the next capture can start |

On Vulkan, a graph pass's range is written into its command list when the pass is recorded. The persistent graph replays a
revision's recordings for as long as the revision stands, so a pass recorded before the capture armed has no range and is not
measured. In a stationary or turning run, DCLF's colour pass (`cs.dclf.main-opaque`) and its parts are not measured; the engine's
perf events are. The fix would be to have arming force the graph to re-record its revisions; until then, measure DCLF's passes
while the scene changes, e.g. under `CS_DCLF_TEST_MOVE`. First seen 2026-10-18: a turning run captured 56 `RenderBatches` ranges
and no `cs.dclf.*` pass.

## In game

Open **Developer Tools → Nsight Perf**:

1. Pick metrics and range filters, and set samples, nesting levels and delay. The device line shows
   the backend.
2. Press **Capture**. The status line shows the sample and pass while it runs.
3. The latest result appears as a table, with the CSV path below it.

## Through devbench

The `communityshaders.nvperf` tool (MCP and REST) has these actions:

- `status` (includes `backend`)
- `metrics`
- `capture`, which returns a `captureId` immediately
- `cancel`
- `result`, which returns `pending` until the capture ends; `detail: true` adds every range

The host also emits `communityshaders.nvperf.completed` (`{captureId, success, error}`) when a
capture ends. For example:

```json
{ "action": "capture", "ranges": ["Deferred*", "Directional Light Shadowmaps"], "samples": 3 }
{ "action": "result", "captureId": 1 }
```

## Headless runs (switches)

The `CS_NVPERF_*` environment switches are a second request source, for runs without the panel or
devbench. When `CS_NVPERF_RANGES` is set, NvPerf queues the capture itself as soon as the backend
is ready, as a `RequestCapture`, and its result is logged and written like any other:

| Switch                  | Meaning                                                                       |
| ----------------------- | ----------------------------------------------------------------------------- |
| `CS_NVPERF_RANGES`      | range filters, comma-separated (a trailing `*` matches a prefix)              |
| `CS_NVPERF_METRICS`     | metrics, `name[:kind]` comma-separated (default: the panel's default metrics) |
| `CS_NVPERF_START_FRAME` | the present the capture starts at (default 1200), as `delayFrames`            |
| `CS_NVPERF_SAMPLES`     | samples (default 3), in one capture                                           |
| `CS_NVPERF_CSV`         | the CSV path, for every capture (default `CommunityShaders-NvPerf.csv`)       |

`CS_NVPERF_RANGES` also turns Frame Annotations on at startup, so the engine's passes are hooked.
For example, DCLF's graph passes beside the engine's batches:
`CS_NVPERF_RANGES=cs.dclf.*,BSShaderAccumulator::RenderBatches*`.

## Code

- `src/Features/RemoteControl/NvPerf.h`: the API every caller uses.
- `src/Features/RemoteControl/NvPerf.cpp`: the front end. Requests and status (across threads,
    under a mutex), the backend's choice, the delay, the summary, log and CSV, the completion
    callback, the switches.
- `src/Features/RemoteControl/NvPerfHost.{h,cpp}`: what the front end and the D3D11 backend need from the plugin
    (device, frame counter, annotations, log directory, build hint).
- `src/Features/RemoteControl/NvPerfBackend.h`: the internal backend interface and the capture
    in flight.
- `src/Features/RemoteControl/NvPerfD3D11.cpp`: the D3D11 backend. It configures the metrics
    (metrics evaluator → raw counters → config image), runs the D3D11 session and passes, and
    decodes and evaluates the ranges, all on the render thread.
- `src/Features/RemoteControl/NvPerfVulkan.cpp`: the Vulkan backend, on OpenRenderGraph's
    `Telemetry/NvPerfCapture` under `QueueControl::Boundaries`: DXVK's queue and command buffer
    callbacks, the graph host's pass-range hooks, and the mapping of `NvPerf::CaptureConfiguration`
    / `basic_telemetry::gpu_capture::CaptureResult`.
- `src/RenderGraph/RenderGraphRuntime.cpp`: `NvPerf::RegisterVulkanDevice` where the render graph
    adopts DXVK's device.
- `src/Hooks.cpp` (`IDXGISwapChain_Present`): `NvPerf::OnPresentBegin` / `OnPresentEnd`.
- `src/State.cpp` (`BeginPerfEvent` / `EndPerfEvent`): `NvPerf::PushRange` / `PopRange`, behind
    one atomic check when no capture records ranges.
- `src/Features/DrawcallLimitFix/Features/MainView/MainViewPasses.cpp`:
    `NvPerf::CommandBufferRangesActive` / `PushCommandBufferRange` / `PopCommandBufferRange`
    around the colour pass's parts (no-ops on D3D11).
- `src/Features/RemoteControl.cpp`: the panel, shown as a tab of `src/Features/DeveloperTools.cpp`.
- `src/Features/RemoteControl/DevBenchBridge.cpp`: the tool.
