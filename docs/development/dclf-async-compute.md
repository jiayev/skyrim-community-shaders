# DCLF: potential savings through async compute

Status: investigation only, nothing implemented. Measured on Save28 (Tamriel exterior, RTX 3090-class GA102), 2026-10-19/20.

## The cost being hidden: the shadow views' per-view floor

DCLF's shadow epoch (`shadows.view`, `Features/Shadows/Engine/ShadowEpochPasses.cpp`) records every shadow view the engine
renders that frame: on this save **28 views** (sun cascades in slots 2-3, then 4096x4096 + two 4096x2048 views per local
shadow light, two 512x512). Each view is its own render pass with one `vkCmdDrawIndexedIndirectCount` per shadow pipeline
bucket: **23 per view, ~640 per frame**, most with a GPU count of zero.

NvPerf per-view ranges (`cs.dclf.shadow.view-N`, kept in that file):

| view | normal | draws skipped (experiment) |
|---|---|---|
| 0 (sun) | 267 us | 288 us |
| 1 (sun) | 666 us | 657 us |
| 2, 3, 6 (near-empty lights) | 10 us each | 1-5 us |
| 4, 5 | 58, 71 us | 50, 57 us (pass/target cost, not draws) |
| 7-27 together | 206 us | 46 us |

So a render pass alone is ~1 us, and **the 23 mostly-empty indirect-count draws cost ~8-9 us per view, ~0.2 ms per frame**.
Skipping every light view's draws (shadows lost; experiment only) moved the frame from ~90.1 to ~91.3 fps (~0.15 ms).

### Why it costs: not "chained ExecuteIndirect" as such

Each indirect-count draw is resolved by the GPU front end: it must read the count (then the arguments) from memory before it
can launch or skip anything, and consecutive ones are processed in order. With a zero count the shader units do nothing while
the front end waits on that read, so the cost is a latency bubble per command, not work. The engine's native path issues direct
draws whose counts the CPU knows, so it has no such bubble. SM throughput during the shadow pass is 12-15% in both modes
(geometry/front-end bound), which leaves room for concurrent work.

Two consequences:
- **It can be removed structurally** by issuing fewer indirect commands (below), which helps whatever else is in the frame.
- **It can be hidden** with async compute: the SMs are idle during the bubbles, so compute work on another queue that has no
  dependency on the light views can run in them. Hiding only works while such work is available at that moment, and it competes
  for SMs with the two heavy sun views.

## Structural options (no async needed)

1. **One indirect draw per bucket per depth target, not per view**: draw all of a target's views (slices / sub-rectangles) in one
   pass, the pulled vertex stage selecting `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex` from a per-draw view index in
   the sequence. ~640 commands become ~23 per target (in batches of 16 viewports). Needs a view's draws to share a bucket's range
   across views (today each view has its own slot buffer), and per-viewport depth ranges (already applied in the vertex stage).
   Largest saving (most of the ~0.2 ms), largest change.
2. **Fewer shadow pipelines**: the 23 buckets are 23 shadow pipelines. The pulled vertex stage already takes the vertex layout
   per draw; merging variants that differ only in vertex-stage defines (layout, skinning, tree animation) behind per-draw flags
   would cut buckets for every view. Moderate change, also shrinks the main and depth passes' bucket counts.
3. **Skip empty views**: a per-view "any draw" word from the shadow build-draws and conditional rendering
   (`VK_EXT_conditional_rendering`, D3D12 `SetPredication`) around the view's commands. One predicate read per view instead of
   23 count reads; only helps truly empty views. Needs the RHI to expose predication.

## What could fill the bubbles

Candidates seen in Nsight near the local-light shadow rendering. All are plain D3D11 dispatches on the engine's immediate
context (DXVK's queue), not ORG passes, so none can overlap DCLF's epochs today; moving one means making it an ORG pass on
DCLF's compute queue (`AsyncComputeQueue()`, `Core/Render/Resources/Helpers.h`) with its resources imported.

| work | cost (Nsight / NvPerf) | inputs | depends on local light maps? | overlap possible |
|---|---|---|---|---|
| VolumetricShadows `DownsampleMip0/1` + 4 blurs (`src/Features/VolumetricShadows.cpp:75-319`) | ~75-80 us each + ~30 us blurs | sun cascades (PS t4 at the shadow-mask draw), VL ESRAM shadow map | no | yes, after DCLF's sun views (0, 1) if the light views are drawn after them |
| ScreenSpaceShadows clear + `RayMarch` (`src/Features/ScreenSpaceShadows.cpp:231-245`, `106-229`) | 80-170 us (Nsight) | scene depth (post-Z-prepass copy), sun direction | no | only if the light views move after the Z-prepass (they are consumed by the main lighting pass only) |
| TerrainShadows `ShadowUpdate` (`src/Features/TerrainShadows.cpp:580`) | ~20 us | terrain heightmap | no | yes, anywhere |
| DCLF's own depth-epoch culling (z latched copies, texture transforms, build-draws, tree-LOD cull) | ~50 us | scene payload, frame constants | no | yes, already compute; needs only to be scheduled concurrently with the shadow epoch |
| LightLimitFix `CopyLocalShadowMaps` (`src/Features/LightLimitFix.cpp:1903-2086`) | — | local shadow maps | **yes** | no: must follow the light views |
| SubsurfaceScattering (`src/Features/SubsurfaceScattering.cpp:220-363`) | — | main colour, G-buffer | no | no: post-opaque |

The ScreenSpaceShadows "SSS::ShadowTexture" clear is a full-screen R8G8 UAV clear; if the ray march writes every pixel it
could be dropped rather than moved.

## Estimate

The bubble is ~0.2 ms; the independent work available near it is ~0.25-0.4 ms (Volumetric Shadows ~0.19 ms, screen-space
shadows 0.08-0.17 ms if reordered, terrain shadows and DCLF's culling ~0.07 ms). Since the bubbles are interleaved with the light
views' real draws and the fill competes with the sun views for SMs, a realistic saving from async compute alone is
**~0.1-0.2 ms**, and less once the structural fixes above shrink the bubble. Recommended order: structural option 2 or 1 first
(it helps unconditionally), then async compute for Volumetric Shadows and DCLF's depth culling, which need no frame reordering.

## Measurement notes

- Our NvPerf range capture keeps one nesting level and matches by name: the screen-space shadow trace and clear never appeared in
  it (only their 1-2 us `EarlyPrepass` wrappers), so it under-reported them; use Nsight for those. The CSV's row order is not GPU
  execution order.
- Range times include overlap; frame cost comes from the present counter and hand-back sweeps (`dclf-validation.md`).
