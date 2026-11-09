# The full ORG port: the whole renderer through ORG, DXVK optional

The plan for moving every GPU command in CS and in the engine onto OpenRenderGraph, so BasicRHI can run it on Vulkan or
D3D12 and DXVK is not needed. RE results live in [engine-d3d11-surface.md](./engine-d3d11-surface.md); what an external
mod API will need is collected in [org-extension-api-notes.md](./org-extension-api-notes.md). A task moves out of this
file when it is built.

## Context

CS on `org_integration` runs Skyrim's D3D11 under a forked DXVK (`extern/dxvk`), and OpenRenderGraph adopts DXVK's
`VkDevice` to run graph epochs inside DXVK's command stream (`docs/development/render-graph.md`). DCLF and LLF's
clustered culling already run on ORG. Everything else still goes through `globals::d3d::context`, and the engine
owns the device, swap chain, Present, every render target and texture, state binding, ImageSpace, Scaleform, Bink and
the menu 3D scenes. DCLF itself gets all of its inputs as D3D11 objects imported through about 50 private DXVK
exports.

Goal: every GPU command, engine and CS, is an ORG pass, so BasicRHI can run it on Vulkan or D3D12 and DXVK is not
loaded. Decisions (2026-11-09):

| Decision               | Choice                                                                                                                                                             |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Engine strategy        | **Native replacement only.** No D3D11-API translation layer. Each engine subsystem becomes ORG passes.                                                             |
| Backend order          | **Vulkan first.** D3D12 follows behind a parity gate.                                                                                                              |
| D3D11 paths            | **Kept until cutover** behind switches, then deleted.                                                                                                              |
| Early step             | **Ownership inversion:** BasicRHI creates the device and swap chain, ORG owns the frame and Present, DXVK becomes a guest.                                         |
| Engine handle fields   | **Tripwire handles:** IUnknown-shaped objects whose refcount works and whose every other method logs the caller and fails.                                         |
| Third-party D3D11 mods | **The DXVK build stays the default.** The full port is a development option. A larger ORG extension API comes later; record what it needs in a notes doc as we go. |
| Effects11              | **Write an ORG FX runtime** (techniques parsed, compiled with DXC) to replace FX11.                                                                                |
| Deliverable            | This plan, plus `docs/development/org-full-port.md` (roadmap style, like `dclf-roadmap.md`) after approval.                                                        |

### Where things stand (from the 2026-11-09 survey)

**Infrastructure:**

- BasicRHI has full D3D12 (`rhi_dx12.cpp`, about 11k lines) and Vulkan (`rhi_vulkan.cpp`, about 14k lines) backends, but CS forces D3D12 off (`cmake/RenderGraph.cmake:34-35`).
- ORG already supports a frame it owns end to end: `RenderGraph::SetSwapchainResource`/`SetPresentPass`, `PassBuilder::Present`. BasicRenderer in SARP runs it on D3D12, Vulkan and both (`SARP/BasicRenderer/.../DeviceManager.cpp`, `RendererGraphConstruction.cpp`).
- BasicRHI gaps against what the engine uses:
    - **Pipeline state:** no stencil (`rhi.h:1795` `// TODO: stencil`); no blend factor, `SrcAlphaSat`, dual-source blending, sample mask or depth clip; no dynamic viewport or scissor; no MSAA resolve or GenerateMips.
    - **Vulkan:** no `VK_EXT_memory_budget` (`rhi_vulkan.cpp:3963`); the CPU waits for the GPU on every present (`sc_present`, `:7397`); vsync is ignored.
    - **Present, both backends:** no HDR colour space or metadata, and no latency control.
    - **ORG:** `GraphicsPipelineDesc` has no input layout, and there is no dynamic-state API in `PassRecordContext`.

**CS:**

- About 20 features run their own passes: SSGI, SSS, DynamicCubemaps, ExpHeightFog, VolumetricShadows, Skylighting, TerrainShadows, IBL, GrassCollision, ScreenSpaceShadows, PostProcessing (13 sub-effects), HDRDisplay, Upscaling, Effects11, Screenshot, LLF's remainder, TerrainBlending, GrassOptimizations, CloudShadows and the deferred composite.
- About 15 only change the engine's shaders and bind PS slots: ExtendedMaterials, Skin, WetnessEffects and others.
- The framework still uses D3D11 in `Buffer.h`, `Util::CompileShader` (FXC), `GpuEventTimers`/`Profiler`, ImGui DX11, and the texture streaming and VRAM tooling (DXVK only).

**Engine (Ghidra, SE 1.5.97 and AE 1.6.1170 laid out the same):**

- The context global has 357 references in 110 functions. Almost all are in the `BSGraphics::Renderer` block (IDs 75445-75654), the shader `Setup*` functions (constant buffer Map/Unmap) and about 40 ImageSpace setup functions.
- `SetDirtyStates` (75580/77386) is the only state flush, `SetShader` (75550) and `SetPixelShader` (75555) the only shader binds, and `DispatchCSShader` (75532) the only dispatch.
- Scaleform (GFx 3.x) keeps private device and context pointers in its renderer object (vtable SE 0x1417dbf90).
- Bink reaches D3D11 only through the Renderer's texture lock and unlock (75514/75515).
- Present has three callers: the main frame (35556), the loading-screen loop on a job thread (38085), and Bink (87890).

**DXVK:**

- It can already run on a device someone else created (`DxvkDeviceImportInfo`, `DxvkAdapter::importDevice`, used by `D3D11On12CreateDevice` in `extern/dxvk/src/d3d11/d3d11_main.cpp:371`). That is what makes the inversion cheap.

## Target architecture

1. **BasicRHI owns the device:** `rhi::CreateVulkanDevice`, later `CreateD3D12Device`, with the Streamline interposer.
2. **ORG owns the frame:** one `org::Runtime` with persistent graphs per engine phase, ending in a present pass on a BasicRHI swap chain.
3. **The engine's CPU code still runs** (scene walk, ImageSpace effect list, menus). Its D3D11 choke points stop issuing work: they latch state into ORG passes, as DCLF's `PassCapture` withholds passes today.
4. **Engine resources are ORG resources.** The engine's `ID3D11*` fields hold tripwire handles that point at ORG records.
5. **One shader path:** HLSL compiled by DXC through ORGModuleServices to SPIR-V or DXIL, with bindless bindings (the existing `DCLF_BINDLESS`/`LLF_ORG_BINDLESS` ABI).
6. **Until cutover the DXVK guest exists** as a "legacy" stage inside ORG's frame. The DXVK-free build is the moment that stage is empty.

Switch names (suggested):

- `CS_ORG_OWNED_FRAME`: the inversion.
- `CS_ORG_NATIVE`: no DXVK, Vulkan.
- `CS_ORG_BACKEND=vulkan|d3d12`.

The default stays DXVK as host until the user says otherwise.

## Execution model

### Who does what

- **Subagents implement.** Each phase's independent tasks go to parallel subagents, each in its own git worktree (`isolation: "worktree"`) on its own branch cut from `org_integration`.
- **The main session builds and validates.** It runs `BuildDevFast.bat`, in-game runs, `tools/cs-ab` A/B and screenshot comparisons, then reviews and merges. Implementing agents do not launch the game or benchmarks while two or more of them are running. An agent working alone may run its own build.

### The brief each agent gets

- The task row from this plan.
- The RE section of `engine-d3d11-surface.md` it depends on.
- The reuse list for its area (LLF template, `ComputeProgram`, `DxvkImageImport`, DCLF `PassDef`).
- Its file boundary.
- What "done" means: it compiles, has a switch, and has a fallback.

It returns:

- a summary;
- the branch name;
- anything it found that belongs in `org-extension-api-notes.md`.

### Lanes per phase

These are the units that run in parallel:

| Phase | Lanes                                                                                                                                                                                                                                                                                                                                                             |
| ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R     | 3 RE agents: {R1, R2, R3, R11}, {R4, R5, R10}, {R6, R7, R8, R9}. R12 goes to whichever finishes first. Read only, so they can run at the same time as anything else.                                                                                                                                                                                              |
| P0    | BasicRHI pipeline state {P0.1-P0.3}; dynamic state and ORG pipeline desc {P0.4, P0.5}; present, HDR and latency {P0.8-P0.10}; small utilities {P0.6, P0.7}; diagnostics {P0.11}; caching and allocator {P0.12, P0.13}. Each lane changes different files in `rhi_vulkan.cpp`/`rhi_dx12.cpp`, so agents name the functions they touch and the main session merges. |
| P1    | DXVK fork export {P1.1, P1.2}; ORG present and swap chain {P1.3, P1.5}; ImGui backend {P1.6}; Streamline device {P1.4} after P1.1. The main session does the integration (P1.7).                                                                                                                                                                                  |
| P2    | {P2.1}, {P2.2, P2.4}, {P2.3}, {P2.5}, {P2.6, P2.7}, all parallel. P2.2 lands first, because P3 builds on its API.                                                                                                                                                                                                                                                 |
| P3    | One agent per feature within a wave (W1 is 7 agents, run 3-4 at a time). PostProcessing is split by sub-effect after the shared raster helper. W5 (FX runtime) is one long-running agent with its own design doc.                                                                                                                                                 |
| P4    | {P4.1} first, then {P4.2}, {P4.3, P4.4}, {P4.5}, {P4.6}, {P4.7} in parallel.                                                                                                                                                                                                                                                                                      |
| P5    | Independent families in parallel: sky, water, BloodSplatter, ImageSpace (split by effect group), menu 3D, map, Scaleform, Bink. P5.1, P5.7, P5.12 and P5.13 stay serial in the main session, because they touch the frame skeleton.                                                                                                                               |

## Keeping the work separate and clean

The aim is that each task can later become its own PR against `dev` without dragging other work with it.

### Separate code from existing features

- New ORG code goes in new files beside the D3D11 code, not interleaved with it. For a feature this is `src/Features/<Name>/ORG<Name>.{h,cpp}`, following LLF's `LightLimitFix/ORGLightCulling.cpp`.
- The existing feature file gains only one seam: a call that picks the ORG path or the D3D11 path, behind the feature's switch.
- Engine takeover code goes in its own tree, `src/RenderGraph/Engine/` (resources, handles, swap chain, frame anchors), and stays out of `Hooks.cpp` and `Deferred.cpp`. Those files get only the hook registrations.
- Changes to BasicRHI, ORG and DXVK stay in their submodules. A CS branch only bumps the pointer.

### One task, one branch

- Each task row is one branch with its own commit series and conventional-commit titles: `feat(org): ...`, `refactor(org): ...`, `fix(...)`.
- A branch does not mix submodule bumps, feature ports and framework changes, unless the task needs them together. If it does, the bump is a separate first commit.
- Shared prerequisites (P2.2's anchor API, P4.1's handles) land before their users, so later branches depend only on merged work.

### Clean as we go

- Reuse what already exists rather than adding parallel helpers:
    - `Util::SetResourceName` and the ORG resource naming;
    - `ComputeProgram`, `LatchBlock`, `DxvkImageImport`, `PrefixSum`;
    - DCLF's `PassDef` and `FrameBindings`.
- No duplicated wrappers, no TODO/FIXME placeholders, Doxygen comments on public methods, and comments in short plain sentences.
- Use graph primitives instead of hand-written bookkeeping.
- The main session reviews each branch for these points before merging.
- When a D3D11 path is finally deleted (P8.1), the deletion is a separate commit from the port.

---

## Phase R: upfront RE (Ghidra, read only, can start now)

**Server:** http://127.0.0.1:8089, `program=/skyrim/se/SkyrimSE.exe.unpacked.exe` and `/skyrim/ae/...`.

- From Git Bash, `export MSYS_NO_PATHCONV=1`.
- Working endpoints: `decompile_function`, `batch_decompile`, `disassemble_function`, `get_xrefs_to` (RAM addresses only), `get_function_xrefs?name=` (for imports), `get_function_callers`, `get_bulk_xrefs`, `search_functions_by_name?name_pattern=`, `search_instructions`, `read_memory`, `list_strings?filter=`.
- The registry is in `BethesdaGhidraScripts/ghidra-mcp/src/main/java/com/xebyte/core/EndpointRegistry.java`.
- Address libraries: `BethesdaGhidraScripts/addresslibrary/sse`.

**Output:** `docs/development/engine-d3d11-surface.md` gets one section per task. Each section has SE and AE IDs, signatures, callers, and the state each function reads and writes. Rename functions in Ghidra as they are confirmed.

**Running it:** the tasks are independent. Run R1-R4 and R5-R8 as two or three parallel subagents. Each gets the endpoint notes above and writes only its own doc section.

| #   | Task                                                                    | What to produce                                                                                                                                                                                                                                                                          | Feeds                           |
| --- | ----------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------- |
| R1  | Name the draw functions 75477-75500 (SE) and their AE twins 77263-77290 | Each one's signature and geometry kind (TriShape, dynamic, particle, line, instanced); which shaders and passes reach each; the inline input layouts built by 75490/75491                                                                                                                | P5 residue draws, P4 buffers    |
| R2  | `SetDirtyStates` (75580/77386) and `RendererShadowState`                | The full dirty-bit table and every state it flushes: VS/PS/CS SRVs and samplers, UAVs, RTs, blend/depth/stencil/raster, viewports, topology, input layout, alpha-test CB. Note any GS/HS/DS use. Stencil ops per pass (which passes use stencil: water? decals? the ImageSpace masks?)   | P0 state coverage, P5           |
| R3  | Constant-buffer contract                                                | For each shader type: its constant groups (`constantBuffers[level]`, IDs 52475x-52476x); the about 65 Map/Unmap sites in `Setup*` and ImageSpace setups; which layouts DCLF's `ConstantMirror` already decodes                                                                           | P5 constant path                |
| R4  | ImageSpaceManager                                                       | The effect list (`SetupEffects` 0x141297340), render functions 99023-99025, the render-target ping-pong, every `BSImagespaceShader` with a CS HLSL file and every one without (the vanilla-only, DXBC-only list), and the order and conditions for interior, exterior, menus and the map | P5 ImageSpace, shader authoring |
| R5  | Scaleform renderer                                                      | The full vtable at SE 0x1417dbf90 (GFx 3.x `GRenderer`): textures, vertex/index caches, mask/stencil use, blend modes, render-to-texture (menus with 3D), the font cache, and its shaders (embedded DXBC in the exe: extract them)                                                       | P5 UI                           |
| R6  | Threads and lifetime                                                    | Every D3D11 user off the render thread: the loading screen (38085 under `Renderer::Lock`), loader threads creating textures and buffers, Bink. Buffer creation (75592/75593) and ownership of `BSGraphics::TriShape`/vertex buffers. The dynamic ring (75484, query fence)               | P4 resource takeover            |
| R7  | Swap chain and window                                                   | `InitD3D` (75595/77226), `D3D11CreateDeviceAndSwapChain` (75589), `CreateSwapChain` (75457), resize, `KillWindow`, the DXGI mode enumeration, fullscreen handling                                                                                                                        | P1 inversion                    |
| R8  | Engine work around DCLF epochs                                          | Shadow-target bind and clears (`BSShadowLight::RenderShadowmap`, AE 0x1414f0cf0), reflection-face bind (AE 0x1414ed920), the `Main::RenderDepth` copy (100421/107139), `BSShaderAccumulator::FinishAccumulating` copies                                                                  | P5 first engine passes          |
| R9  | Menu 3D and map                                                         | UI3DSceneManager (51855/52727), `Inventory3DManager::Render` (0x140887750), RaceMenu's 3D, the local map (`LocalMapCamera`, `BSImagespaceShaderLocalMap` 0x1412b6110), and whether DCLF claims their geometry                                                                            | P5 menus                        |
| R10 | Bink and textures                                                       | `BinkDoFrame` flow (87911), plane textures, YUV conversion shader; AE 90260/90275/90276                                                                                                                                                                                                  | P5 Bink                         |
| R11 | Device-global readers                                                   | The 32 users of the device global (SE 0x143025f08) and the remaining shader, layout, query and state-object creation functions (75547, 75551, 75556, 75583, 75590...) with AE twins                                                                                                      | P4 tripwire coverage            |
| R12 | AE 1.7.104 spot check                                                   | Spot-check the same IDs on 1.7.104 if an exe is available. Otherwise mark them unverified                                                                                                                                                                                                | all                             |

**Also from RE:** extract the vanilla shaders that have no CS source from the game's shader archives (the ImageSpace ones and Scaleform's), and disassemble them with `fxc /dumpbin`. This is what makes "write HLSL for every DXBC-only shader" a task list instead of a surprise.

---

## Phase 0: ORG/BasicRHI capability work (submodules `extern/BasicRHI`, `extern/OpenRenderGraph`)

These repos are separate, so changes land there first, then go into CS through a submodule bump. SARP/BasicRenderer must keep building, so run its tests.

| #     | Task                                                                                                                                                                               | Notes                                                                                                                                                                                                                                                      | Size |
| ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- |
| P0.1  | Stencil in `DepthStencilState`: front/back ops, read/write masks; `SetStencilRef` in the record context                                                                            | D3D12 and Vulkan both. The engine uses stencil (R2 says where)                                                                                                                                                                                             | M    |
| P0.2  | Blend: `BlendFactor`/`InvBlendFactor` with `SetBlendFactor`, `SrcAlphaSat`, dual-source `Src1*`, sample mask, logic op (Vulkan only, gated)                                        |                                                                                                                                                                                                                                                            | S-M  |
| P0.3  | Rasterizer: depth clip enable (`depthClampEnable` / `DepthClipEnable`)                                                                                                             |                                                                                                                                                                                                                                                            | S    |
| P0.4  | Dynamic viewport and scissor in `PassRecordContext` (`SetViewports`, `SetScissors`, several viewports)                                                                             | Engine draws change the scissor per draw (Scaleform, ImageSpace)                                                                                                                                                                                           | M    |
| P0.5  | Input layout in ORG's `GraphicsPipelineDesc` (BasicRHI already has it). Pipeline selection at record time from a permutation cache (the "not covered" item in `docs/pipelines.md`) | Engine permutations are picked per draw                                                                                                                                                                                                                    | M-L  |
| P0.6  | GenerateMips (compute, ORG utility pass) and MSAA resolve                                                                                                                          | DynamicCubemaps uses `GenerateMips`                                                                                                                                                                                                                        | S    |
| P0.7  | Vulkan memory budget: `VK_EXT_memory_budget` in `d_queryVideoMemoryInfo`                                                                                                           | Replaces `dxvkGetMemoryHeapReports`. Keep the AMD clamp caveat                                                                                                                                                                                             | S    |
| P0.8  | Vulkan present                                                                                                                                                                     | Remove the host `vkWaitSemaphores` in `sc_present` (let `Queue::Submit` signal binary semaphores); honour vsync and present-mode switching; fix the swapchain-image UNDEFINED VUID; `swapchain_maintenance1`; `present_id`/`present_wait`                  | M    |
| P0.9  | HDR output                                                                                                                                                                         | Choose the colour space (HDR10 PQ, scRGB); `vkSetHdrMetadataEXT` / `SetColorSpace1` + `SetHDRMetaData`. NVIDIA's profile needs the instance's engine name to be `"DXVK_HDR"` (`extern/dxvk/FORK.md`): carry that trick into BasicRHI's `VkApplicationInfo` | M    |
| P0.10 | Frame latency                                                                                                                                                                      | D3D12: the waitable object and `SetMaximumFrameLatency`. Vulkan: present_wait. A host-owned queue-depth policy to replace `dxvkSetPresentQueueDepth`/`SyncPresent`/`TearingPreference`                                                                     | M    |
| P0.11 | Crash diagnostics on native Vulkan                                                                                                                                                 | Checkpoints or `VK_NV_device_diagnostic_checkpoints` / `VK_AMD_buffer_marker`, `VK_EXT_device_fault`, Aftermath markers. Replaces `DXVK_DEBUG=hang` (every RX 6600 run needs this)                                                                         | M    |
| P0.12 | Pipeline cache on disk for Vulkan and D3D12                                                                                                                                        | Cold SPIR-V compiles took up to 109 s per PSO in SARP; engine permutations number over 1,000                                                                                                                                                               | M    |
| P0.13 | VRAM attribution                                                                                                                                                                   | Owner labels, spill pool and defrag in BasicRHI's allocator (replaces `dxvkPushResourceOwner`, `SetResourcesSpillable`, `ForceResourcePlacement`, `SetDefragPressurePercent`)                                                                              | M-L  |

**Verify:** BasicRHI and ORG unit tests; SARP benchmark; a stencil/blend sample in ORG `samples/` on both backends with validation layers.

---

## Phase 1: ownership inversion (DXVK as guest)

| #    | Task                                                                | Detail                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ---- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P1.1 | A DXVK fork export to create a D3D11 device on a device it is given | Model it on `D3D11On12CreateDevice`'s `DxvkInstanceImportInfo`/`DxvkDeviceImportInfo` path. BasicRHI creates the instance and device with DXVK's required features and extensions added. `dxvkRequestDeviceFeatures` turns around: DXVK _reports_ what it needs. Queue assignment: DXVK gets the graphics queue plus its transfer queue; ORG keeps compute and copy. This replaces `AdoptVulkanDevice` in `RenderGraphRuntime::Initialize`. |
| P1.2 | Headless guest swap chain                                           | DXVK's DXGI swap chain for the game stops presenting. Its back buffer is an ORG-owned image (imported into DXVK, or DXVK images imported into ORG as now), and `Present` becomes "frame done" for the guest stream. Hook point: `IDXGISwapChain::Present` (`Hooks.cpp:487-600`). The loading-screen loop (38085) and Bink (87890) presents go through the same path.                                                                        |
| P1.3 | ORG present pass                                                    | A BasicRHI swap chain on the game's HWND (R7), `SetSwapchainResource` + `SetPresentPass`, created from `DxvkLoader` at `InitD3D`. Resize and fullscreen follow the engine's window events. Fake fullscreen stays on (the FSR-FG alt-tab deadlock in FORK.md).                                                                                                                                                                               |
| P1.4 | Streamline on BasicRHI's device                                     | `CreateVulkanDevice(..., enableStreamlineInterposer=true)`. Move the frame-generation present ownership (`FrameGenController.cpp`, `dxvkSetSyncPresent`, queue depth, recreate) onto P0.10's policy. Re-check SARP's "Streamline on Vulkan corrupts the heap" first, since that is the largest risk here. The DXVK interop command-buffer ring in `Upscaling/DXVKInterop.cpp` stays while the inputs are DXVK images.                       |
| P1.5 | HDRDisplay on the ORG swap chain                                    | `HDROutputCS` writes the ORG back buffer instead of `CopyResource` to DXVK's. HDR surface state comes from P0.9 instead of `dxvkGetPresenterSurfaceState`.                                                                                                                                                                                                                                                                                  |
| P1.6 | ImGui to ORG                                                        | An `imgui_impl_basicrhi` render pass in the present stage. Replace `imgui_impl_dx11` in `Menu.cpp`, `Menu/OverlayRenderer.cpp`, `Menu/ThemeManager.cpp`; move IconLoader, CursorLoader, `Utils/UI.cpp` and the CSEditor textures to ORG textures. Overlays then sit above the guest's output and survive the cutover.                                                                                                                       |
| P1.7 | Frame structure                                                     | `GraphHost`'s epochs keep running inside DXVK's stream through external commands, which works today. The ORG frame tail (HDR, ImGui, present) waits on the guest's last submission. Record which interop features become unnecessary as each later phase lands: segment splits, queue boundaries, home states.                                                                                                                              |

**Done when:** with `CS_ORG_OWNED_FRAME=1`, a tour in Whiterun and Riverwood on the RX 6600 matches the DXVK host build in screenshots (`dclf:shot`) and frame time (one sample per version). Also check HDR on and off, a loading screen, a Bink logo, menus, and alt-tab. DLSS-G or FSR-FG needs foreground, and DLSS-G needs an NVIDIA card. NVIDIA-only checks are gates for whenever an NVIDIA card is available.

---

## Phase 2: the CS framework on ORG

| #    | Task                                                                   | Detail                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ---- | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P2.1 | Graph-backed resource wrappers                                         | Give `src/Buffer.h` (`Texture2D`, `Buffer`, `ConstantBuffer`, `StructuredBuffer`...) graph-backed twins with the same names (the naming convention carries over to ORG resource names). While a D3D11 reader remains, `RenderGraphRuntime::WrapBuffer` (`dxvkCreateBufferFromVkBuffer`) exposes them; add an image equivalent to the DXVK fork.                                                                                                                                                                                           |
| P2.2 | A general feature-pass API                                             | Today each new feature needs a `Segment` enum entry and an epoch-order edit (`RenderGraphRuntime.h:75-125`). Make the frame's stages into named anchors (ReflectionsPrepass, EarlyPrepass, Prepass, DeferredPasses, Tonemap, PostProcessing, Present) that features attach `ExternalPassDesc`s to. Base it on the LLF template (`LightLimitFix/ORGLightCulling.cpp`: `TypedRenderGraphPass`, `IRenderGraphExtension`, `LatchBlock`) and DCLF's `Api/Passes.h` `PassDef`. Write each new anchor's ordering rule into the notes doc (P8.4). |
| P2.3 | Shader compile                                                         | Move `Util::CompileShader` (FXC `*_5_0`, `Utils/D3D.cpp:206`) for CS's own passes to `ComputeProgram::Load` / the ORGModuleServices shader store (DXC, SPIR-V now, DXIL later). Feature HLSL moves to the bindless ABI (`OrgBindless.hlsli`).                                                                                                                                                                                                                                                                                             |
| P2.4 | Raster passes                                                          | A fullscreen VS/PS helper on ORG render passes, to replace `PostProcessing/RasterPass.{h,cpp}` and the PS passes in Upscaling and Effects11.                                                                                                                                                                                                                                                                                                                                                                                              |
| P2.5 | GPU timers                                                             | Move `GpuEventTimers.cpp` and `Profiler.cpp` (D3D11 timestamp queries) to ORG statistics and query pools; move the GPU-load CS. Replace `dxvkGetSubmissionCounter`/submission trace (`GpuIdleTrace.cpp`) with ORG's queue timelines.                                                                                                                                                                                                                                                                                                      |
| P2.6 | Shared constant buffers                                                | Make `PermutationCB`, `SharedDataCB` and `FeatureDataCB` (`State.cpp:1296`) graph uploads, readable by graph passes as bindless constants. The D3D11 binds at PS b4-6 (`VanillaBinding.cpp`) stay for legacy draws until P5.                                                                                                                                                                                                                                                                                                              |
| P2.7 | The deferred composite (`Deferred.cpp:309-428`, `DeferredCompositeCS`) | Becomes an ORG compute pass on imported GBuffer targets (DCLF already imports them, `Core/Render/Resources/Resources.cpp:494-528`). The GBuffer render-target table entries stay engine-created until P4.                                                                                                                                                                                                                                                                                                                                 |

**Verify:** `BuildDevFast.bat` builds. Composite output in screenshots is identical with `CS_ORG_COMPOSITE=0/1`. GPU timer totals match.

---

## Phase 3: CS feature ports

**Rules for every port:**

- The D3D11 path stays behind a per-feature switch until cutover; on shader failure the feature falls back to D3D11 or disables cleanly (`impl.reset()` pattern).
- Resources opt into aliasing where they are transient (`docs/development/org-aliasing-port-priorities.md`).
- DCLF's matching integration (`DrawcallLimitFix/Integrations/<Feature>`) switches from an imported SRV to the graph resource.
- If the feature also binds PS slots for engine draws, keep exporting it to D3D11 (`WrapBuffer`/image wrap) until P5 removes those draws.
- Bump the feature `.ini` only if settings change.

**Waves** (ordered by dependency, then by memory saved):

| Wave                        | Features                                                                                                                                                                                                                                                                                                                                                                                                 | Notes                                                         |
| --------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| W1: small compute producers | IBL (2 CS), ScreenSpaceShadows, TerrainShadows (1 CS, persistent heightmap), Skylighting probes (`UpdateProbesCS`), GrassCollision, VolumetricShadows (6 CS; hooked at the RenderShadowmask draw in `State::Draw`), LLF's remainder (lights buffer, local shadow copy `LocalShadowCopyCS`, fallback culling)                                                                                             | Each is a few passes; outputs already go to DCLF integrations |
| W2: lighting                | ExponentialHeightFog (5-10 CS, history copies, LLF lists), SSGI (7 CS, history split for aliasing), SSS (2-3 CS), DynamicCubemaps (about 9 CS + GenerateMips + BC6H, 3-frame state machine), CloudShadows (4th MRT on the sky's cubemap draws: keep the engine draw until P5 sky, port the propagation copies now)                                                                                       | SSGI/SSS/composite order inside the DeferredPasses anchor     |
| W3: post and output         | PostProcessing (13 sub-effects, about 45 dispatch sites + raster passes; about 1.2 GB aliasable at 4K), HDRDisplay remainder, Screenshot (staging readback through ORG readback), Upscaling's own passes (`UpscaleDepth` 2 PS, `CopyHudlessColor`, the copy into kMAIN), with Streamline inputs as graph resources. When no DXVK images remain, `SeparateFromGraphEpochs()` and the DXVK command ring go | Largest single port; split by sub-effect                      |
| W4: engine-coupled          | TerrainBlending (re-issues terrain depth passes: becomes a DCLF terrain depth variant plus the `DepthBlend` CS), UnifiedWater flowmap (D3D11 deferred context, becomes a compute pass), TruePBR glints noise (setup CS), Effects11 volumetric/sun rays (raymarch PS, 2 blur CS, apply PS)                                                                                                                |                                                               |
| W5: the FX runtime          | Replace FX11 (`Effects11/Effects/Effect.cpp`, `D3DX11CreateEffectFromMemory`): an `.fx` parser (techniques, passes, annotations, UI variables, render-target and texture declarations), DXC compile per pass, execution as ORG passes in the Tonemap anchor. Test against the presets Effects11 ships with. Keep FX11 behind the switch                                                                  | L; its own design doc first                                   |

**Not ported:** GrassOptimizations. Its HiZ, culling and indirect draws drive the engine's grass, which DCLF's grass family already replaces. Its D3D11 path stays until cutover, then is deleted.

**Shader-injection features:** ExtendedMaterials, Skin, Wetness, WaterEffects, HairSpecular, RainRefraction, TerrainHelper, LinearLighting, LODBlending, TerrainVariation, HorizonFix, InteriorSun, SkySync, GrassLighting, ExtendedTranslucency, VolumetricLighting. They have no passes, so they move with the engine shader they change:

- Their texture binds become `FrameBindings` entries (as DCLF already does for its families).
- They are done when every draw family that uses them is ORG.
- VolumetricLighting's replacement compute shaders move with ImageSpace (P5.6).

**Verify each port:** screenshots on/off by switch at the standard cells, the feature's GPU time from ORG statistics, VRAM from `dclf:vram-json`, a hot-reload test, and a forced shader-failure test.

---

## Phase 4: engine resources become ORG resources

The engine creates everything through a few functions (R6, R11). Order matters: textures and targets first, because DCLF already imports them and gains the most.

| #    | Task                                  | Hooks (SE / AE)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ---- | ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P4.1 | Tripwire handles                      | A handle object per type (Texture2D/SRV/RTV/DSV/UAV/Buffer/shader/state/input layout/query). Its IUnknown refcount works; every other slot logs the caller's return address (resolved with the address library) and returns failure or crashes with a report. `QueryInterface` answers only the IIDs the engine asks for. In the guest phase, handles wrap a real DXVK object made from the ORG resource, so un-ported readers keep working; in native mode they wrap nothing. Release can come from any thread (loader threads create and drop resources without `Renderer::Lock`), so the GPU free is deferred until the frames that used the resource retire. |
| P4.2 | Render targets                        | `BSShaderRenderTargets::Create` 100458/107175 drives `CreateRenderTarget` 75467/77253, `CreateDepthStencil` 75469/77255, 3D 75470, cube 75471, `CreateRenderTexture` 75507/77299. ORG creates them; the CS format upgrades (`Hooks.cpp:766-1220`, `State::ModifyRenderTarget`, HDR LDR upgrades) move into those descriptions. CreationEngineRaytracing's hooks (`external_refs/CreationEngineRaytracing/src/Hooks.cpp:181-540`) give signatures and a stand-in pattern. A window resize releases and recreates every target (114 and 12 depth-stencils on SE, 116 on AE) through `WindowSizeChanged`, so ORG resizes them as a set.                             |
| P4.3 | Textures                              | DDS load 75721/77533, then 75724/77538 (CreateTexture2D + SRV); Tex2D/Tex1D 75511/75513; release 75527/77322. One texture path through the graph-job loader.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| P4.4 | Texture streaming on ORG              | Replace the `dxvkStream*` shells (`TextureStreaming/Streamer.cpp`) with ORG-native partial residency: windows as ORG textures, bound through bindless indices. Keep the budget controller (`Policy::StepPullback`) and the traps in .                                                                                                                                                                                                                                                                                                                                                                                                                            |
| P4.5 | Buffers                               | Vertex/index buffer creation 75592/75593 and `TriShape` ownership; the dynamic ring 75484/75485/75487 (Map + query fence) becomes an ORG upload ring. DCLF's geometry imports (`Core/Gpu`) then read ORG buffers directly.                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| P4.6 | Shaders, input layouts, state objects | VS 75547, PS 75551, CS 75556, InputLayout 75583, Query 75590, blend/depth/raster/sampler creation (`CreateSamplerState`'s anisotropy cap in `Hooks.cpp`). Each records a description (bytecode hash, layout, state) for pipeline building; the handle stands in for the engine. `ShaderCache.cpp` (1506-1640, 2744-2817) gives the description its DXC/SPIR-V binary and reflection.                                                                                                                                                                                                                                                                             |
| P4.7 | VRAM tooling                          | `Vram/*` and `MemoryWindow` read ORG/BasicRHI's allocator (P0.13) instead of DXVK snapshots.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |

**Verify:**

- The tripwire log is empty over a full tour: menus, map, inventory, race menu, loading screens, Bink, a worldspace change (`cow Blackreach 0 0`), and save/load.
- VRAM totals match the DXVK build.
- `dclf:tex-stats` shows no regression.
- SE 1.5.97 and AE 1.6.1170 both pass.

---

## Phase 5: the engine's remaining passes as ORG passes

Each item follows DCLF's pattern:

- The engine's CPU code runs.
- Its pass is withheld or its draw choke point latches state.
- An ORG pass draws it with the same shader built for ORG.

Each item carries its RE dependency. They are ordered so the frame's skeleton (targets, clears, copies) goes first and the long tail last.

| #     | Item                           | Notes / dependency                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ----- | ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P5.1  | Frame skeleton around DCLF     | Shadow-target bind and clears, reflection-face bind and clear, the `RenderDepth` copy into `kPOST_ZPREPASS_COPY`, `FinishAccumulating` copies, `Renderer::Begin` per-frame CBs and `ClearState`. (R8)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.2  | Finish DCLF coverage           | Unclaimed and `UnstableBuffer` shapes, Lighting without LLF, residue shadow casters and the rasterizer-state shadow view (needs P0.3), the reflection faces' cell-root residue, first person (`CS_DCLF_FIRST_PERSON` default) and first-person `HeatRefraction`, effect variants not ported (roadmap 4.1), master particles (1.4), blood impact particles (4.3), precipitation geometry. Use `dclf-roadmap.md`'s items; this plan does not repeat them.                                                                                                                                                                                                                                                                                               |
| P5.3  | Sky                            | BSSkyShader draw family: main view and reflection faces; CloudShadows' 4th MRT becomes an output of the ORG sky pass.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.4  | Water                          | BSWaterShader family, reflection/refraction inputs, the water displacement simulation (an ImageSpace effect), WaterEffects/UnifiedWater bindings. Stencil if R2 finds it (needs P0.1).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| P5.5  | BloodSplatter (screen blood)   | Small family.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| P5.6  | ImageSpace chain               | From R4's effect list, in the engine's order: HDR/tonemap (`ISHDR`, Effects11 tonemap hook), SSAO composite, DOF, menu blur, water blend, VL composite + VolumetricLighting's replacement CS, refraction, local map, and the rest. Each `BSImagespaceShader` setup's constants (about 40 functions) are latched instead of mapped. HLSL is written for every effect R4 lists as DXBC-only. Upscaling already replaces TAA. The surface doc lists all 159 slots: 39 run CS HLSL today, 68 have a CS source whose descriptor entry is commented out (enable and verify those first), and 17 have none (FXAA, BSLensFlare, CopySubRegionCS, the Downsample CS pair, LightingComposite x3, PerlinNoise CS x2, the SAO compute and raw-AO set, SILRawInd). |
| P5.7  | Constant-buffer path           | Replace every remaining `Setup*` Map/Unmap site (R3) with a latch into graph uploads. DCLF's `ConstantMirror` already decodes many of them. Constant buffers are pooled by size and rewritten before each use, so the latch is per use, never keyed by buffer pointer. About 40 functions go through the engine's 4-buffer custom mapper (ImageSpace b2, grass b7/b8, bones b9/b10, Effect VS b11).                                                                                                                                                                                                                                                                                                                                                   |
| P5.8  | Menu 3D                        | UI3DSceneManager (51855/52727), `Inventory3DManager::Render`, race menu, magic/map previews: their geometry as DCLF families with their own view, their `SetDirtyStates` calls withheld. (R9) DCLF does not claim this geometry today: these accumulators reach `RegisterPass`, but DCLF withholds only main, view and shadow renderers, so each needs its own view. Menu 3D also uses stencil (bit 0, then an ImageSpace NOT_EQUAL).                                                                                                                                                                                                                                                                                                                 |
| P5.9  | Map menu                       | `LocalMapCamera`, `BSImagespaceShaderLocalMap`, world map LOD. (R9)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| P5.10 | Scaleform                      | A new `GRenderer` implementation (vtable SE 0x1417dbf90, constructed by 82790, `SetDependentVideoMode` 82918) that records ORG raster passes: batched triangles, masks (stencil, P0.1), blend modes, render-to-texture, the font cache texture, Scaleform's shaders rewritten in HLSL from R5's DXBC. Swap it in at `BSScaleformManager` init (80603). Largest new subsystem in this phase. (R5) The renderer bypasses BSGraphics (raw device and context from `GetScaleformRendererParameters`). Its 58-slot vtable, 55 SM 4.0 blobs and the embedded HLSL source of its shaders are mapped in the surface doc, so the shaders are transcribed, not rewritten. Masks use its own stencil with INCR/DECR.                                             |
| P5.11 | Bink                           | Bink has no YUV path: `BinkCopyToBufferRect` writes RGBA8 256x256 tiles on the CPU and a copy shader draws them. The port is an RGBA8 upload ring plus a blit, presented through the ORG present stage from the blocking play loop or the sequence thread. (R10)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| P5.12 | Loading screen                 | The loop is `JobListManager::ServingThread` mode 2 (SE 38085 / AE 39039) under `Renderer::Lock` (recursive critical section, Renderer+0x2790 SE / +0x27F0 AE), drawing Scaleform and the loading-screen 3D model through the full accumulator and ImageSpace path. ORG submission and present must be safe from that thread under the same lock. (R6)                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.13 | The flush and the shader binds | When the tripwire shows no draws left, `SetDirtyStates` 75580, `SetShader` 75550, `SetPixelShader` 75555 and `DispatchCSShader` 75532 become latch-only or no-ops, and their D3D11 sides are tripwires.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |

**Verify each item:**

- Screenshots against the DXVK build at the standard cells, plus the case each item adds (water at Riverwood, rain, a menu with a 3D item, the race menu, the local map, the Bethesda logo Bink, loading screens).
- Set parity for culling changes on the RX 6600.
- DXVK pass counters (`dclf:` census) fall toward 0.

---

## Phase 6: cutover, a DXVK-free Vulkan build as a development option

| #    | Task                                                                                                                                                                                                                                                                           |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| P6.1 | `CS_ORG_NATIVE=1`: `DxvkLoader` does not load DXVK. `InitD3D` (75595) is replaced: no `D3D11CreateDeviceAndSwapChain`; the RendererData device/context/swap chain fields (+0x38/+0x40/+0x60) and their globals (SE 0x143025f08, 0x143027ea0) hold tripwire handles.            |
| P6.2 | Everything that still needs DXVK becomes a startup error that names it, so it cannot silently fall back. The DXVK build stays the default.                                                                                                                                     |
| P6.3 | Third-party D3D11 users fail with a clear message from the tripwire (the plugin's module name from the caller address), and each one is recorded in the API notes doc (P8.4).                                                                                                  |
| P6.4 | Validation: a full tour on the RX 6600 with Vulkan validation layers (`VK_LOADER_LAYERS_DISABLE='*nsight*'`), hang markers through P0.11, SE and AE, HDR on and off, upscalers, frame generation where hardware allows, and Release-preset frame times against the DXVK build. |

---

## Phase 7: D3D12

| #    | Task                                                                                                                                                                                                                                                                                                                                             |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| P7.1 | Turn on `BASICRHI_ENABLE_D3D12` in `cmake/RenderGraph.cmake`; add `CS_ORG_BACKEND=d3d12` device creation (BasicRenderer's `DeviceManager` pattern).                                                                                                                                                                                              |
| P7.2 | DXIL targets in the shader store with the D3D12 binding ABI (SM6.6 `ResourceDescriptorHeap`, root constants instead of push constants); `ShaderCache`'s reflection reads DXIL.                                                                                                                                                                   |
| P7.3 | Streamline on D3D12: DLSS/DLSS-G/Reflex natively; D3D12 builds of CS's `sl.fsr`, `sl.fsr_g`, `sl.xess` plugins (`extern/Streamline/source/plugins`, which are Vulkan-only today: `fsrEntry.cpp:291`, `xessEntry.cpp:275`, `fsrGEntry.cpp:910`); FSR-FG with a DXGI swap chain. BasicRenderer's `UpscalingManager`/`FfxBackendDX12` show the way. |
| P7.4 | HDR (`SetColorSpace1`), latency (waitable object), DRED.                                                                                                                                                                                                                                                                                         |
| P7.5 | Parity gate: the P6.4 tour on D3D12 matches Vulkan in screenshots, and both match the DXVK build. Multi-RHI peer work stays deferred.                                                                                                                                                                                                            |

---

## Phase 8: cleanup and docs

- **P8.1:** Delete the D3D11 paths per feature, the DXVK-only tooling (`GpuIdleTrace`, the `dxvk*` exports in `DxvkOrgInterop.h`), and `CS_NATIVE_D3D11`. Delete ORG's embedding machinery once nothing uses it (external epochs, segment splits, queue boundaries, home states, `HostOwned` imports). This only happens when the user chooses to make the native build the default.
- **P8.2:** Fix the doc drift found in the survey:
    - `dclf-architecture.md` / `dclf-roadmap.md` 1.5 still say the engine draws grass.
    - Roadmap 4.6 and `OcclusionMapsFeature.h` disagree about the precipitation mask.
- **P8.3:** Update `docs/development/render-graph.md` for the owned frame.
- **P8.4, from the start and ongoing:** `docs/development/org-extension-api-notes.md`. Each phase adds what an external mod would need:
    - an overlay pass after the UI;
    - texture create and upload;
    - render-to-texture (RaceMenu/SKEE overlays);
    - reading named engine targets;
    - a frame anchor to attach passes to (P2.2);
    - Present-hook users (ImGui mods);
    - every third-party D3D11 user the tripwire catches.

---

## Order and parallelism

1. Phase R (subagents, read only) and Phase 0 start together. P0.1-P0.5 gate P5, P0.8-P0.10 gate P1.
2. P1 (inversion) runs after P0.8-P0.10. P2 can start alongside it, since P2.1-P2.4 do not need the inversion.
3. P3 waves W1-W4 start once P2.2-P2.4 exist. W5 (FX runtime) can run in parallel with its own design doc.
4. P4 after R6/R11, alongside late P3.
5. P5 items in the listed order, each after its RE task and P4.2-P4.5.
6. P6, then P7, then P8 when the user decides.

**Commits:** each task's branch (see "Keeping the work separate and clean") is merged into `org_integration` by the main session after it builds and validates. Submodule changes to BasicRHI, ORG and DXVK go to their own repos first; ask before pushing the shared DXVK remote.

## Risks

- **A missed engine or mod call site.** Mitigated by the tripwire handles (P4.1) and by keeping DXVK as the default.
- **Performance.** DXVK batches state and barriers for the engine's residue; a naive ORG pass per engine draw would be slower. Every P5 item is a draw family with batching, not a draw-by-draw translation.
- **Streamline on BasicRHI's Vulkan.** SARP saw heap corruption. Check early in P1.4; DLSS-G needs an NVIDIA card.
- **AMD compiler defects** in new compute shaders (equality-with-constant early-outs). Test every new pass on the RX 6600.
- **Vanilla DXBC-only shaders.** 17 ImageSpace shaders have no CS source and 68 more are unmapped; Bink's copy shaders too. Scaleform's HLSL is embedded in the exe. Sizes are in the surface doc.
- **The loading-screen thread** renders off the render thread under the engine's lock (P5.12).
- **The Scaleform and FX runtimes** are each a subsystem of their own; scope them with their own design docs before building.

## Verification summary

**Builds:** `BuildDevFast.bat` per C++ change; Release only for timing.

**Shaders:** `hlslkit-compile` for shader changes.

**Every phase:**

- `tools/cs-ab` scenarios A/B against the DXVK build: screenshots with `dclf:shot`, frame time one sample per version, VRAM.
- In-game runs in `C:\Modding\MO2`, under 60 s by default, `DXVK_DEBUG=hang` on the RX 6600 while DXVK exists.
- SE 1.5.97 and AE 1.6.1170.
- An empty tripwire log from P4 on.

## Files most touched

- `src/RenderGraph/RenderGraphRuntime.{h,cpp}`, `src/DxvkLoader.cpp`, `src/Hooks.cpp`, `src/Deferred.cpp`, `src/State.cpp`, `src/Buffer.h`, `src/ShaderCache.cpp`, `src/Utils/D3D.cpp`, `src/Menu*.cpp`, `src/GpuEventTimers.cpp`
- `src/Features/*.cpp` and their subdirectories
- `src/Features/DrawcallLimitFix/{Core,Features,Integrations}`, `src/TextureStreaming/*`
- `extern/BasicRHI/rhi.h`, `rhi_vulkan.cpp`, `rhi_dx12.cpp`
- `extern/OpenRenderGraph/include/Render/PipelineDesc.h`, `RenderGraph.h`
- `extern/dxvk/src/d3d11/d3d11_main.cpp`, `d3d11_org_interop.h`
- `extern/Streamline/source/plugins/sl.{fsr,fsr_g,xess}`
- Docs: `docs/development/org-full-port.md` (new), `engine-d3d11-surface.md` (new, Phase R), `org-extension-api-notes.md` (new), and updates to `render-graph.md`, `dclf-roadmap.md`, `dclf-architecture.md`

## Settled decisions

1. **GrassOptimizations is retired** at cutover, not ported. DCLF's grass is the only grass path in the ORG build.
2. **NVIDIA checks are deferred.** Development runs on the RX 6600 and the iGPU. The DLSS-G and NVIDIA Streamline checks in P1.4 and P7.3 are gates to run whenever an NVIDIA card is available. They do not block the other phases.
