# OpenRenderGraph on DXVK

Community Shaders can run GPU work through [OpenRenderGraph](https://github.com/panthuncia/OpenRenderGraph)
(ORG) on the same Vulkan device that DXVK uses to translate the game's D3D11. Light Limit Fix's clustered
light culling is the first feature that does this. Its D3D11 path stays in place as the fallback.

## Runtime shape

-   **One device.** `RenderGraphRuntime` (`src/RenderGraph/`) has BasicRHI *adopt* DXVK's `VkDevice`
    (`rhi::vulkan::AdoptVulkanDevice`). There is no second device, no shared handles and no cross-API
    fences.
-   **Device features.** Before the game creates its device, `DxvkLoader` calls
    `dxvkRequestDeviceFeatures` to ask for what BasicRHI needs beyond DXVK's own set:
    `VK_EXT_descriptor_heap` and `vulkanMemoryModelDeviceScope` (DXC's atomics use Device scope, and
    DXVK enables `vulkanMemoryModel`). These are enabled on the `VkDevice` only; DXVK's own code keeps
    the feature set it would have chosen, including its descriptor-buffer binding model.
-   **Persistent graph.** A `org::PersistentGraphHost` drives the graph in persistent mode: it is compiled
    once and each frame runs `PrepareFrame` + record + submit. Resizes and pass-set changes rebuild it.
-   **Epoch.** `RenderGraphRuntime::ExecuteEpoch` runs the graph once, on the thread that drives the
    D3D11 immediate context (LLF calls it from `UpdateLights`). CPU data for a frame, such as the light
    list, is uploaded by the graph's own `BUFFER_UPLOAD` path inside the epoch.

## Ordering against D3D11

The graph's batches go into DXVK's own command stream (`dxvkEnqueueInteropSubmission`):

1.  BasicRHI hands each queue submission to the host through `QueueSubmissionHooks::submit` instead of
    calling `vkQueueSubmit`.
2.  The runtime forwards it to DXVK. On the immediate context, DXVK ends its current command list and
    queues the graph's batch right behind it, as an external entry on its submission thread.
3.  So the batch executes after every D3D11 command issued before the epoch and before every one issued
    after it. The render thread waits for nothing: no `FlushRenderingCommands`, no CS-thread sync.
4.  Memory dependencies are covered by the graph's queue-boundary barriers
    (`RenderGraph::SetExternalQueueBoundary`): a full memory barrier at the start of the epoch's first
    command buffer and at the end of its last one.

Submissions from any other thread (none in steady state) first wait until everything already in the
stream has reached the queue, then submit directly under DXVK's queue lock, so the graph's own order
is preserved.

With a DXVK build that lacks the export (or `CS_ORG_SUBMIT=flush`), each epoch instead flushes and
waits for DXVK, then submits under the queue lock. This is undesirable, and will likely be downgraded 
to a failure in the future.

## Resources crossing the boundary

-   **Graph outputs** (LLF's `lightIndexList`, `lightGrid`) are graph-owned buffers. D3D11 reads them
    through wrappers from `dxvkCreateBufferFromVkBuffer`. DXVK never renames, relocates or maps a
    wrapped buffer, and only `D3D11_USAGE_DEFAULT` without CPU access is accepted.
-   **Game or DXVK resources** are not pinned or otherwise changed. Inputs the graph needs are either
    uploaded as copies (LLF's light list) or, for future passes, imported each epoch with the handle DXVK
    currently uses.

## GPU timing in the profiling window

The profiling window's D3D11 timers are timestamp query pairs, which DXVK writes at
`VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`: each one is written once everything submitted before it has finished.
Inside one DXVK command list, a pair therefore measures exactly the GPU work between them. A pair whose
two halves land in **different submissions** also counts the time the queue sat idle between them,
waiting for the CPU to submit the second one. How long that is depends on how far the CPU is behind the
GPU, not on the pass.

Measured on the auto-loaded save (FSR at 2560x1440 to 3840x2160). The old `Upscaling::Upscale` timer
bracketed the Streamline evaluation, which is submitted as its own command buffer after DXVK flushes:

| | D3D11 pair around the evaluation | Timestamps inside the evaluation buffer |
| --- | --- | --- |
| DCLF off | 4.77 ms | 1.87 ms |
| DCLF on | 2.09 ms | 1.91 ms |

With DCLF off the render thread is further behind the GPU, so the queue waits longer for the next command
list. `LightLimitFix::RenderGraphCull`, a D3D11 timer around the graph's epoch, had the same problem
(0.21 ms and 0.58 ms against 0.02 ms of graph work).

Where the timings come from now:

-   Work submitted outside DXVK's command lists is timed inside its own command buffers, never by a D3D11
    pair around it. Render-graph passes report through the graph's statistics
    (`<Feature>::<segment> / <pass>`). The Streamline interop ring writes a timestamp pair into each
    labelled buffer (`DXVKInterop::BeginFrameCommandBuffer(label)`), which feeds `Upscaling::Upscale`.
-   The DXVK fork exports `dxvkGetSubmissionCounter`, the immediate context's count of command lists it has
    submitted. The profiler reads it at each timer's begin and end, and flags a timer whose halves landed
    in different submissions. Implicit flushes cause this too: `Skylighting::OcclusionMask` renders the
    engine's precipitation mask, thousands of draws, and DXVK flushes partway through it in about 90% of
    frames. The window marks a timer with `*` when most of its recent samples were split (GPU mode only).
    `CS_PROFILER_LOG` prints `[split N%]`.
-   The reverse also happens. DXVK moves a copy or clear into the current command list's init buffer when
    none of its resources has been used in that list yet (`prepareOutOfOrderTransfer`), so it runs before
    every timestamp in the list. A D3D11 timer around such an operation reads about zero, and the
    operation's time appears in no timer. The copy of the upscaler's output into `kMAIN`, the first
    operation after the interop flush, is one (estimated from bandwidth at about 0.1 ms at 4K, not measured), so it has no timer.

## Switches

| Variable | Effect |
| --- | --- |
| `CS_ORG=0` | No feature request and no graph: every feature stays on D3D11. |
| `CS_ORG=features` | Request the device features but never create the graph (isolates device-configuration side effects). |
| `CS_ORG_SUBMIT=flush` | Use the flush-and-lock submission path even when DXVK supports the stream path. |
| `CS_ORG_LLF_PARITY=1` | Every 300 frames, also run LLF's D3D11 culling and compare each cluster's light set with the graph's (logs `LLF parity OK` / `MISMATCH`). |
| `CS_ORG_EPOCH_STATS=1` | Log render-thread CPU time per epoch (average and maximum every 600 epochs). |
| `CS_ORG_ASYNC_EPOCHS=0` | Run the epochs synchronously. By default each epoch is prepared and recorded ahead on the graph host's thread, and the render thread only submits ("Epochs that only submit" in `drawcall-limit-fix.md`). |
| `CS_GPU_IDLE_TRACE=<frames>` | Every that many frames, log one complete frame's GPU idle gaps and what the render thread was doing during each (`[GpuIdle]`). DXVK timestamps every submission on its queue (`dxvkSetSubmissionTrace`, two extra timestamp-only submissions per submission), the Streamline ring's buffers add their own spans, and the render thread's perf events are the CPU markers. Forces Frame Annotations on for the session (without saving it) but leaves out the per-draw geometry events, whose formatting would dominate the timeline. Diagnostics only: the trace's own overhead makes gaps somewhat longer than in a normal run. |
| `CS_PROFILER_LOG=<frames>` | Log the profiling window's rolling averages every that many collected frames, sorted by cost, with `[split N%]` on timers that straddle DXVK submissions. For comparing two configurations from their logs. |

Every `CS_*` switch can also be set in `CommunityShaders.env`, beside `CommunityShaders.log`
(`Documents\My Games\Skyrim Special Edition\SKSE`): one `NAME=value` per line, `#` for comments, read
when the plugin loads. This covers launches that do not pass a new environment to the game, such as an MO2
instance that was already running. A variable set in the real environment wins, and every value taken
from the file is logged (`[DevEnv]`).

## Building and tests

-   The libraries are submodules under `extern/` (`OpenRenderGraph`, `BasicRHI`, `BasicTelemetry`,
    `volk`); `git submodule update --init` fetches them. `cmake/RenderGraph.cmake` builds them from
    source (Vulkan only) and compiles the SPIR-V at build time with BasicRHI's DXC flags
    (`BasicRHIShaderFlags.cmake`). Set `CS_ORG_ROOT` to another directory with the same side-by-side
    layout to build against local changes to those libraries. If they are missing, the plugin builds
    without the render graph.
-   `tests/OrgDxvkInterop` runs the graph on DXVK's device without the game, in both submission modes
    (`OrgDxvkInteropTest.lock` / `.stream`). D3D11 writes a sentinel before the epoch and reads back
    after it, so either misordering fails the test.
