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
    `vulkanMemoryModelDeviceScope` (DXC's atomics use Device scope, and DXVK enables
    `vulkanMemoryModel`), `deviceGeneratedCommands` (DCLF), and the descriptor models' features below.
    These are enabled on the `VkDevice` only; DXVK's own code keeps the feature set it would have chosen.
-   **One negotiated binding model.** BasicRHI records into DXVK's own command buffers, so the two must
    bind descriptors the same way (see "One descriptor model" below). `DxvkLoader` also registers
    `dxvkSetBindingModelCallback`: during device creation DXVK calls `NegotiateBindingModel`
    (`RenderGraphRuntime.cpp`) with the physical device and its own choice, and the callback answers
    with `rhi::vulkan::EvaluateBindingModels` (BasicRHI's limit checks, with DXVK's 2048-sampler pool and
    descriptor buffers counted in; every check is logged under `[ORG]`). DXVK keeps its choice if
    BasicRHI accepts it, else switches between heaps and descriptor buffers if the device supports the other,
    else keeps it; legacy descriptor sets (RDNA2, Pascal, most Intel) are never switched away from.
    `dxvk.enableDescriptorHeap` / `dxvk.enableDescriptorBuffer` keep precedence. DXVK then grants
    exactly its own model: `descriptorHeap` on heaps, `descriptorBuffer` + `mutableDescriptorType` on
    descriptor buffers (BasicRHI's descriptor-buffer backend, shaders lowered), nothing on legacy
    descriptor sets. `Initialize` logs the model, or disables the graph with the reason (RenderDoc's
    capture config forces legacy sets).
-   **Persistent graph.** A `org::PersistentGraphHost` drives the graph in persistent mode: it is compiled
    once and each frame runs `PrepareFrame` + record + submit. Resizes and pass-set changes rebuild it.
-   **Epoch.** `RenderGraphRuntime::ExecuteEpoch` runs the graph once, on the thread that drives the
    D3D11 immediate context (LLF calls it from `UpdateLights`). CPU data for a frame, such as the light
    list, is uploaded by the graph's own `BUFFER_UPLOAD` path inside the epoch.
-   **Frames in flight.** `PersistentGraphHost::Desc::framesInFlight` is game frames: whole frames of the epoch order
    (4, `kGameFramesInFlight`). The host's frame slots are that many frames of every epoch (`FrameSlots`, 24 with six
    segments), one ring that every execution takes the next slot of. A slot's next execution waits for its last one's GPU
    work: the "ticket wait" in `CS_ORG_EPOCH_STATS`, on the host's thread, which the render thread sees only when the ring
    is shorter than the CPU's lead over the GPU. The ring used to be a fixed 16 slots: the reflection faces' sixth epoch
    shortened it to 2.7 frames, and every few frames the render thread waited 2-7 ms on whichever epoch came round
    (p99 frame time 25 ms instead of 12). Sized by the epochs, a new segment needs no change here.

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

BasicRHI's Vulkan `q_submit` serialises a submission only on its queue's `accessMutex`; it no longer holds the device-wide
`timelinesMutex` across the submit and the host hook. Before (2026-10-14), the render thread's epoch submission
(`ORG.Frame.SubmitPackets`) waited 20-40 ms behind a copy-queue upload (`CopyQueueUploadService`, which reaches the queue
through DXVK's `LockSubmissionQueue`); it is now at most 0.88 ms. `sc_present` still holds `timelinesMutex` across the
present (SARP and BasicRenderer only).

With a DXVK build that lacks the export (or `CS_ORG_SUBMIT=flush`), each epoch instead flushes and
waits for DXVK, then submits under the queue lock. This is undesirable, and will likely be downgraded 
to a failure in the future.

## Hazard: external Vulkan work directly after a graph epoch

**Rule.** No third-party Vulkan command buffer that goes through DXVK's interop may run on the GPU directly behind a graph
epoch's command buffer. That covers Streamline's DLSS, FSR, XeSS, DLSS-G and FSR-FG, and any future FidelityFX, NRD or vendor SDK.
DXVK work that binds image descriptors must sit between them. Today that is `Upscaling::SeparateFromGraphEpochs()`
(`src/Features/Upscaling.cpp`): call it, or a shared helper if it ever moves, immediately before such work. Found at the Sleeping
Giant Inn, 2026-10-12/13; the investigation record is in `dclf-open-defects.md` ("the Sleeping Giant Inn's black or garbage
upscaler output").

**Mechanism, as far as it is known.** This is inferred from black-box experiments, not proven.

-   A Streamline plugin requests `VK_EXT_descriptor_heap`; the log says "requested by a plugin(s) added".
-   With DXVK on descriptor heaps (NVIDIA's default until 2026-10-25; now opt-in), the render graph is granted that extension through `DxvkLoader`'s
    `dxvkRequestDeviceFeatures` (see Device features above). So Streamline's features run on the descriptor-heap path.
-   Graph epochs bind the graph's descriptor heaps.
-   Seen in that configuration (found when DXVK still ran on descriptor buffers beside the graph's heaps); not re-checked with
    the graph on descriptor buffers.
-   When a heap-path consumer's command buffer executes right behind an epoch's, NVIDIA's driver computes it from the wrong
    descriptors.
-   Unknown: whether this is a driver defect, or state or heap memory the graph leaves behind that the consumer relies on.

**Symptom signature** (to recognise it quickly):

-   The consumer's output alternates frame to frame: black (zeros), then garbage, then right. The garbage is values of ±65504 with
    alpha intact, which means the consumer's own arithmetic, not memory written by something else.
-   Its inputs and constants are correct. A readback inside the consumer's own command buffer proves this.
-   Identical with DLSS and FSR.
-   Goes away with a device idle immediately before or after the consumer, or with any DXVK descriptor-binding work in between.
-   Unaffected by queue choice, the consumer's in-flight depth, resource lifetimes and ORG's deletion depth.
-   Appears only when an epoch is the last GPU work before the consumer. In practice that means after adding an epoch, or after
    moving a pass late in the frame.

**What separates them and what does not** (all tested):

| Separates | Does not separate |
| --- | --- |
| A D3D11 UAV clear of a small typed texture (DXVK runs it as a compute shader that binds image descriptors) | A D3D11 flush |
| A device idle before or after the consumer | An empty interop submission |
| | A typed-buffer UAV clear (a buffer fill) |
| | Waiting for the epoch's completion |
| | Bounding the consumer's frames in flight (1 or 2) |
| | Idling only the upload or compute queue |

**Where it applies today.**

-   **The upscaler evaluate:** covered. `Upscaling::Upscale` calls `SeparateFromGraphEpochs()` before `EvaluateDLSS`/`EvaluateFSR`/XeSS.
-   **Frame generation:** *not covered*. That is `Streamline::EvaluateFSRFrameGen` (`Upscaling/Streamline.cpp`) and the DLSS-G path.
    Frame generation was off in every run of the investigation.
-   **Epochs that can end up last before the upscaler or Present.**
    -   DCLF's first-person colour epoch (`RenderFirstPersonView`) is the one that triggered this. It is the only epoch with no DXVK
        work after it before the upscaler.
    -   The forward epoch would be exposed too without the engine's textured draws that follow it.
    -   A new epoch, or one moved late in the frame, must be checked against this rule.

**How to check a change.**

-   The repro: `coc RiverwoodSleepingGiantInn` with DCLF and first person on, about 16 s and 5 screenshots. Without the separator,
    every run reproduced it.
-   `CS_DCLF_TARGET_DUMP` stages the upscaler output at every dump point, including "before upscale" and "after evaluate", and writes
    raw texels.
-   The in-command-buffer readback (`InCbReadback` in `Upscaling/Streamline.cpp`) copies the consumer's inputs and output inside its
    own command buffer. It runs on dump frames only: it allocates 512 MB host-visible and idles the device once.

**Open questions; the workaround stays until one of these resolves the cause:**

-   Run Streamline without `VK_EXT_descriptor_heap` (do not grant the plugin's request), or give it heap state separate from the
    graph's.
-   Have the graph's last command buffer re-bind or reset descriptor-heap state.
-   Check with a capture tool that supports `VK_EXT_descriptor_heap`. RenderDoc 1.46 does not, and CS's RenderDoc setup puts DXVK on
    legacy descriptor sets, so with RenderDoc loaded the graph turns itself off.
-   If it reproduces in isolation, report it to NVIDIA.

## External commands in DXVK's command buffer

`RenderGraphRuntime::EmitExternalCommands(record, user, waits, signals, bindsState, label, fullBarrier)` (render thread) queues
`record(user, VkCommandBuffer)` into DXVK's command stream (`dxvkEmitExternalCommands`): DXVK's CS thread runs it once, in D3D11
stream order, on its own command buffer, outside any render pass. DCLF's CPU-driven pipeline draws through it
(`Core/Render/Direct/Sites.cpp`; dclf-architecture.md, "The CPU-driven pipeline"). The contract:

-   **Entry.** DXVK has flushed its barriers (all earlier D3D11 work is visible) and every image is in its default layout
    (`DxvkOrgInteropImageInfo::layout`, GENERAL for every image DCLF imports), deferred clears done.
-   **Inside.** The callback begins and ends every rendering it opens. With `bindsState` DXVK forgets every binding it tracks
    afterwards (pipelines, descriptor buffers or heaps, push data, vertex and index buffers, dynamic state, render targets) and
    binds its own descriptors again, so the callback binds BasicRHI's heaps, layout and push data itself. BasicRHI wraps the
    buffer for recording (`rhi::vulkan::AdoptCommandBuffer`: no submission, no tracked layouts); secondaries recorded elsewhere
    (inside or outside a rendering, `SecondaryInheritance::insideRendering`) are executed with `ExecuteCommands`.
-   **Exit.** Every image is left in its default layout, and the callback's writes are released: its own barrier to all commands,
    or `fullBarrier` (DXVK records one global barrier). DXVK's barrier tracker is empty after the callback, so it orders nothing
    against what the callback wrote.
-   **Waits and signals** join the graphics submission of the command list the commands land in: a signal completes once that
    whole submission did (the callback's retirement point; values grow in stream order per semaphore). DCLF waits for the frame
    values' timeline and signals its CPU-frame timeline once a frame, behind the frame's callbacks.
-   **Lifetime.** DXVK tracks nothing the callback uses: every D3D11 object, view and buffer it reads or writes is held until its
    signal completes. ORG's staged uploads a callback depends on are recorded on the render thread into a secondary
    (`RenderGraph::RecordPendingUploads`) and executed at the callback's start, so DXVK's list is not ended for them. ORG's latch
    regions a callback's work reads are written at an external frame slot (`PersistentGraphHost::BeginExternalFrameSlot`), reused only
    after its last frame's signal.
-   Once the path has been used, DXVK no longer reorders transfers or layout transitions on interop images, or on buffers with a
    stable address, ahead of the command buffer.
-   **One descriptor model.** BasicRHI and DXVK always run on the same model, descriptor heaps or descriptor buffers, negotiated
    at device creation (Runtime shape, "One negotiated binding model"; `[ORG] Binding model:` in CS's log, `Binding model:` in
    DXVK's). With BasicRHI on heaps and DXVK on descriptor buffers, the two models in one command buffer made NVIDIA (RTX 5080,
    driver 621.01) fetch garbage descriptors in the callback's draws: MMU faults with no owning resource, depending only on where
    in DXVK's stream the callback landed, even with every command in secondaries. DXVK now grants the graph only its own model, so
    that pairing cannot occur. NVIDIA runs on descriptor buffers, as upstream (`dxvk_device_info.cpp`; heaps there: NGX's D24S8 descriptor, black DLSS
    output, level-to-3% slower frames); `dxvk.enableDescriptorHeap = True` opts into heaps, and BasicRHI follows.
-   **Heaps bound in DXVK's buffer too.** A secondary that binds its own heaps is not enough: on the same GPU, on heaps, a
    secondary's heap binding did not hold while the buffer executing it had DXVK's heap bound, and the draws fetched DXVK's
    descriptors at the graph's indices (MMU faults with no owning resource; a callback at the start of a command list, before DXVK
    bound a heap, ran clean, and a separate command buffer within the list did not help). The sites bind the graph's heaps
    (`SetDescriptorHeaps`: heaps, or descriptor buffers and their offsets) in DXVK's buffer before each execution of their
    secondaries (`Direct::SiteOut::BindHeaps`), on either model; DXVK binds its own heaps or descriptor buffers again after the
    callback, and re-sets its descriptor buffer offsets at its next pipeline bind.

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

## The schedule inspector

To see what the persistent graph executes (batches per queue, the barriers before each pass, and every cross-queue wait with its cause), use the menu's
"Render graph schedule" window or the `communityshaders.schedule` DevBench tool. See [render-graph-schedule.md](./render-graph-schedule.md).

## Switches

| Variable | Effect |
| --- | --- |
| `CS_ORG=0` | No feature request and no graph: every feature stays on D3D11. |
| `CS_ORG=features` | Request the device features but never create the graph (isolates device-configuration side effects). |
| `CS_ORG_SUBMIT=flush` | Use the flush-and-lock submission path even when DXVK supports the stream path. |
| `CS_ORG_LLF_PARITY=1` | Every 300 frames, also run LLF's D3D11 culling and compare each cluster's light set with the graph's (logs `LLF parity OK` / `MISMATCH`). |
| `CS_ORG_EPOCH_STATS=1` | Log render-thread CPU time per epoch (average and maximum every 600 epochs). |
| `CS_ORG_ASYNC_EPOCHS=0` | Run the epochs synchronously. By default each epoch is prepared and recorded ahead on the graph host's thread, and the render thread only submits ("Revisions and recordings" in `dclf-architecture.md`). |
| `CS_GPU_IDLE_TRACE=<frames>` | Every that many frames, log one complete frame's GPU idle gaps and what the render thread was doing during each (`[GpuIdle]`). DXVK timestamps every submission on its queue (`dxvkSetSubmissionTrace`, two extra timestamp-only submissions per submission), the Streamline ring's buffers add their own spans, and the render thread's perf events are the CPU markers. Forces Frame Annotations on for the session (without saving it) but leaves out the per-draw geometry events, whose formatting would dominate the timeline. Diagnostics only: the trace's own overhead makes gaps somewhat longer than in a normal run. |
| `CS_PROFILER_LOG=<frames>` | Log the profiling window's rolling averages every that many collected frames, sorted by cost, with `[split N%]` on timers that straddle DXVK submissions. For comparing two configurations from their logs. |

Every `CS_*` switch can also be set in `CommunityShaders.env`, beside `CommunityShaders.log`
(`Documents\My Games\Skyrim Special Edition\SKSE`): one `NAME=value` per line, `#` for comments, read
when the plugin loads. This covers launches that do not pass a new environment to the game, such as an MO2
instance that was already running. A variable set in the real environment wins, and every value taken
from the file is logged (`[DevEnv]`).

## DXVK fork: allocation-pool locking

`DxvkResourceAllocationPool` is an unsynchronized free list that `DxvkMemoryAllocator` may touch only under `m_mutex`
(`extern/dxvk/src/dxvk/dxvk_memory.cpp`). Upstream DXVK has three paths that call `m_allocationPool.create` without the lock:
-   `importBufferResource`: ORG's interop reaches it through `RenderGraphRuntime::WrapBuffer` → `dxvkCreateBufferFromVkBuffer`.
-   `importImageResource`: swapchain images and D3D11on12.
-   `createAllocation(DxvkSparsePageTable*, ...)`: every sparse or tiled resource.

**The crash.** When one of these runs while another thread allocates or releases under the lock (Skyrim's loading thread
creating buffers), both threads can take the same storage. The symptom is an access violation reading `0x1` in
`DxvkResourceAllocationPool::alloc`, inside an engine `CreateBuffer`.

**The fix.** The fork takes `m_mutex` around the pool access on those three paths only. None of their callers holds the lock,
which is not recursive.

**Status.** The patch is not yet listed in `extern/dxvk/FORK.md` or offered upstream. Keep it when rebasing the fork.

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
