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
    BasicRenderer ships (`../../BasicRenderer/ThirdParty/NVPerf`) in a fresh build tree. A tree
    configured before this default keeps its cached `NVPERF_SUPPORT=OFF`; reconfigure it with
    `-DNVPERF_SUPPORT=ON -DNVPERF_SDK_ROOT=<that path>`.
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
    from the capture's arming, which rebuilds the graph so that its kept recordings (a revision's
    epochs) are recorded again with their ranges. The first pass waits for that to have happened,
    not for a frame count: the host's build generation advanced and a selected range recorded
    (`RangesRecordedSinceArm`), then the host's frame slots + 1 frames, so every command buffer
    that executes in it was recorded while armed. A capture whose ranges never record fails after
    600 frames, naming whether the graph rebuilt.
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
    - On Vulkan, OpenRenderGraph's barrier batches around each graph pass are ranges too:
        `cs.org.barrier.<entry|before|after|external-entry|external-exit>.<pass>`. A pass's own range
        covers only its commands, so these are what an epoch costs between its passes.
    - Only the graphics queue is measured. A selected graph pass that runs on the compute or copy
        queue (DCLF's fades, portal batches, tree wind, cull counters under `CS_DCLF_ASYNC_COMPUTE`) is
        never in the results; the capture lists them (`CaptureResult::otherQueueRanges`) and logs a
        warning.
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
revision's recordings for as long as the revision stands, so arming (and disarming) rebuilds the graph, and the first pass waits
for the new recordings (see **Delay**).

## Traces

A trace runs a named **template** as a sequence of captures and merges them into one report
(`src/Features/RemoteControl/NvPerfTrace.h`). It handles what a single capture leaves to the caller:

-   **Metric groups.** Metrics that cannot share a capture are captured one group after another, each
    with the template's ranges and `gpu__time_duration`, and merged into one table.
-   **Repeats and hitches.** Every group is captured `repeats` times. A row's GPU time is the median
    over every sample (a sample that lacks the range counts 0), with its min and max. A sample whose
    frame total is beyond 1.5x its capture's median (a pipeline compile, a streaming spike) is dropped
    and counted.
-   **Rows by range.** One row per measured range, by its own name, not the filter that matched it;
    report groups sum their rows (a row belongs to the first group with a matching filter).
-   **Warnings.** Selected passes on another queue, filters that matched nothing, unavailable metrics,
    dropped samples and rows that vary by more than half their median.
-   **Retries.** A failed capture is retried twice before the trace moves on.
-   **Statistics.** Every row and group also carries the mean of its per-sample time with a confidence
    interval, median, p05/p95, effective sample count (`n_eff`) and whether it converged
    (`src/Features/RemoteControl/TraceStatistics.h`, BasicTelemetry's `StatisticalSampler`, as
    BasicRenderer's sampler: Newey-West long-run variance for autocorrelated frames, Student t, MAD
    flags counted). A series converges once its interval's half-width is within `relativeHalfWidth`
    of its mean, or within `absoluteHalfWidthUs`, with at least `minSamples` samples and effective
    samples.
-   **Convergence (NvPerf).** With `maxRepeats` above `repeats`, `repeats` is a minimum: after the
    planned captures, while a row's or group's `gpu__time_duration` has not converged, another round
    (every metric group once) is captured, up to `maxRepeats` rounds.
-   **Baselines.** Each report is written to `NvPerf/<label>.json` and `.txt` next to
    `CommunityShaders.log`, and logged. A trace compared with an earlier report shows each group's and
    row's difference, e.g. DCLF on against a `CS_DCLF=0` run, marked `*` when the two means differ by
    more than their confidence intervals combined (`sqrt(a^2 + b^2)` of the half-widths). The panel
    greys a difference that is not.

### Timestamp traces

A template with `"source": "timestamps"` reads OpenRenderGraph's pass timestamps instead of
capturing (`src/Features/RemoteControl/GpuTimestamps.h`): cheap and continuous, every frame is a
sample of every graph pass and barrier batch (`cs.org.barrier.<where>.<pass>`), on every queue,
including the async compute queue NvPerf cannot see. The NVIDIA SDK is not needed.

-   **Recordings.** DCLF's epochs replay recordings kept across frames, which carry no shared-pool
    queries. The trace turns on `GraphHost::SetRecordingTimestamps`, which rebuilds the
    graph: each kept list is recorded again with queries of its own (a pool and readback buffer per
    list), resolved by every submission and read when its frame slot completes
    (`extern/OpenRenderGraph/docs/persistent-epochs.md`, "Timestamps of their own"). The trace turns
    them off when it ends (another rebuild).
-   **Phases.** Headless, the trace starts collecting when a game loads and waits for present
    `startFrame` after it; it then waits for the rebuild, discards `settlingFrames` frames, and
    samples every frame. Every 30 frames it checks: it ends once every row and group has converged,
    after `maxSamples` frames, or `timeoutFrames` presents after sampling began.
-   **Rows.** A row is a range's time per frame: every occurrence (every epoch that ran it) summed,
    0 in a frame without it; its time is the mean. A range on the compute queue is a row of its own,
    `<name> [compute]`, left out of the groups and the total (it overlaps the graphics queue's
    work). Engine (D3D11) events are not timestamped: with DCLF off only the graph's own passes are
    measured, so a DCLF on/off comparison of groups is NvPerf's to make.
-   **Ranges overlap; spans don't.** A range begins (top of pipe) when the queue reaches it and ends
    (bottom of pipe) when its work is done, so a barrier batch's range holds the tail of the pass
    before it, and a sum of ranges counts that work twice. A template's `spans` (each `{ "name",
    "ranges" }`, matched against every range of the frame, not filtered by `ranges`) add a row
    `span:<name>` per frame: the graphics queue's time from the first matching range's beginning to
    the last one's end. Compare an epoch or a stretch of passes by its span; a span's rows stay out
    of the total and the groups.
-   **Hitches and gaps.** A frame whose selected total is beyond 1.5x the median's is dropped and
    counted, as is a frame whose epochs did not all come back within 64 frames.
-   **Warnings** add: rows on another queue, no range from a kept recording (DCLF's epochs not timed;
    expected with DCLF off), and the rows furthest from converging.

Built-in templates:

| Template | Measures |
| --- | --- |
| `frame-timing` | every timestamped range (passes and barrier batches, every queue) |
| `dclf-timing` | `dclf-passes`'s ranges and groups, from timestamps |
| `dclf-barrier-timing` | `dclf-timing` plus ORG's barrier batches (`cs.org.barrier.*`) |

Template keys (both sources): `source` (`nvperf` or `timestamps`), `confidence` (0.95),
`relativeHalfWidth` (0.02), `absoluteHalfWidthUs` (1.0), `minSamples` (30); NvPerf: `maxRepeats` (0:
off); timestamps: `maxSamples` (3000), `settlingFrames` (120), `timeoutFrames` (6000).
`metrics`, `samples` and `repeats` do not apply to timestamp traces. For example:

```json
{
	"name": "dclf-shadow-timing",
	"extends": "dclf-timing",
	"ranges": ["cs.dclf.shadow.*", "cs.org.barrier.*"],
	"relativeHalfWidth": 0.01,
	"minSamples": 120,
	"maxSamples": 6000
}
```

Built-in templates:

| Template | Measures |
| --- | --- |
| `frame` | every outermost range's GPU time |
| `frame-timing`, `dclf-timing`, `dclf-barrier-timing` | timestamp traces (above) |
| `dclf-passes` | DCLF's graph passes and the engine events holding the same work with DCLF off (`Depth`, `World`, `Directional Light Shadowmaps`, `First Person View`), grouped by epoch: GPU time, 3 repeats |
| `dclf-barriers` | `dclf-passes` plus ORG's barrier batches (`cs.org.*`) |
| `dclf-units` | `dclf-passes` with every unit's throughput |
| `dclf-shader` | `dclf-passes` with instructions, occupancy and warp stall reasons |
| `dclf-memory` | `dclf-passes` with DRAM bytes, register spills, LSU and texture instructions, early-Z samples and input primitives |
| `dclf-full` | the units, shader and memory groups in one trace |

User templates are `*.json` files in `Data/SKSE/Plugins/CommunityShaders/NvPerfTemplates`, one template or an array of them,
read at first use and by the panel's **Reload**; a file's template replaces a built-in one of the same name.
`package/SKSE/Plugins/CommunityShaders/NvPerfTemplates/examples.json` ships two. A template may `extend` one defined before it:

```json
{
	"name": "dclf-z-prepass",
	"extends": "dclf-barriers",
	"description": "The Z-prepass epoch alone.",
	"ranges": ["Depth", "cs.dclf.z.*", "cs.dclf.hzb", "cs.org.barrier.*"],
	"metricGroups": [
		{ "name": "time", "metrics": ["gpu__time_duration:counter", "sm__throughput:throughput"] },
		{ "name": "shader", "metrics": ["gpu__time_duration:counter", "sm__warps_active:active"] }
	],
	"samples": 4,
	"repeats": 3,
	"startFrame": 2000,
	"groups": [{ "name": "Z-prepass", "ranges": ["cs.dclf.z.*"] }]
}
```

`ranges`, `metricGroups` (or `metrics`, one group) and `groups` replace the base's; `addRanges` appends ranges and `addGroups`
prepends groups. `startFrame` (default 2000) is how many presents after a game loaded (SKSE's `kPostLoadGame`) a headless trace's
first capture starts: DCLF compiles its pipelines in the first ones, and on the main menu and loading screens the graph's owners
do not build, so a capture armed there waits for re-recorded lists that never come.

## In game

Open **Developer Tools → Nsight Perf**:

-   **Traces:** pick a template (its description and shape are shown), name the report, optionally
    pick a saved report to compare with, and press **Run trace**. The latest report shows its groups,
    rows (with their spread and difference) and warnings; **Copy report** copies its text.
-   **Single capture:**
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

A trace is requested the same way:

| Switch                  | Meaning                                                                       |
| ----------------------- | ----------------------------------------------------------------------------- |
| `CS_NVPERF_TEMPLATE`    | the template to run as a trace                                               |
| `CS_NVPERF_LABEL`       | the report's name (default: the template's)                                   |
| `CS_NVPERF_BASELINE`    | a report (label or `.json` path) to compare it with                           |
| `CS_NVPERF_START_FRAME`, `CS_NVPERF_SAMPLES`, `CS_NVPERF_REPEATS` | override the template's                |
| `CS_NVPERF_MAX_REPEATS`, `CS_NVPERF_MIN_SAMPLES`, `CS_NVPERF_MAX_SAMPLES` | override the template's convergence bounds |
| `CS_NVPERF_HALF_WIDTH`  | override the template's `relativeHalfWidth` (e.g. `0.01`)                    |

For example, DCLF on against off, one launch each:
`CS_NVPERF_TEMPLATE=dclf-passes CS_NVPERF_LABEL=dclf-off CS_DCLF=0`, then
`CS_NVPERF_TEMPLATE=dclf-passes CS_NVPERF_LABEL=dclf-on CS_NVPERF_BASELINE=dclf-off`.
A change to DCLF against the build before it, from timestamps:
`CS_NVPERF_TEMPLATE=dclf-timing CS_NVPERF_LABEL=before`, then
`CS_NVPERF_TEMPLATE=dclf-timing CS_NVPERF_LABEL=after CS_NVPERF_BASELINE=before`.

`CS_NVPERF_RANGES` and `CS_NVPERF_TEMPLATE` also turn Frame Annotations on at startup, so the engine's passes are hooked.
For example, DCLF's graph passes beside the engine's batches:
`CS_NVPERF_RANGES=cs.dclf.*,BSShaderAccumulator::RenderBatches*`.

## Code

- `src/Features/RemoteControl/NvPerf.h`: the API every caller uses.
- `src/Features/RemoteControl/NvPerf.cpp`: the front end. Requests and status (across threads,
    under a mutex), the backend's choice, the delay, the summary, log and CSV, the completion
    callback, the switches.
- `src/Features/RemoteControl/NvPerfHost.{h,cpp}`: what the front end and the D3D11 backend need from the plugin
    (device, frame counter, annotations, log directory, build hint).
- `src/Features/RemoteControl/NvPerfTrace.h` / `.cpp`: traces: the templates (built-in and
    `NvPerfTemplates/*.json`), the runner that queues a template's captures through `RequestCapture`
    (or samples timestamps) from `OnPresentEnd`, and the report (median, statistics, convergence,
    hitches, groups, warnings, baseline, JSON and text).
- `src/Features/RemoteControl/TraceStatistics.h` / `.cpp`: a series' mean, confidence interval,
    effective sample count and convergence (BasicTelemetry's `StatisticalSampler`).
- `src/Features/RemoteControl/GpuTimestamps.h` / `.cpp`: a timestamp trace's frames: recording
    timestamps on and off through the graph host, and each frame's ranges assembled from the epochs
    `RenderGraphRuntime` hands over (`MarkEpoch` at submission, `Aggregate` at completion, from
    `IStatisticsService::GetCompletedTimestamps`).
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
