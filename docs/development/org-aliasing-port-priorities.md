# ORG aliasing: CS feature port priorities

Which CS features to port to OpenRenderGraph first, ranked by how much device memory ORG's automatic
aliasing could reclaim against the effort of the port.

Sources:

-   Measured figures come from the VRAM attribution dump (`dclf:vram-json`) at 3840x2160, taken at a
    Whiterun save on 2026-10-29. PostProcessing, ExponentialHeightFog's volumetric fog and the
    upscalers were off in that run.
-   Figures marked "est." are computed from the creation code at 4K.
-   Resource lifetimes come from reading each feature's passes and shaders.

## What ORG can alias

ORG's auto-aliasing planner (`RenderGraphAliasingAlgorithms.cpp`) places a resource in the shared pool
only when all of these hold:

-   **The graph owns it.** It is a `PixelBuffer` or a `BufferBase` the graph created. An imported D3D11
    texture (`ExternalTextureResource`, e.g. DCLF's game-target imports) is never a candidate.
-   **`allowAlias` is set.** `TextureDescription::allowAlias` defaults to `false`, so each ported
    resource has to opt in. The planner itself defaults to `autoAliasMode = 2` (Balanced).
-   **It is device-local.** Upload rings and readbacks are excluded.
-   **Its first use in the frame is a write.** Anything that reads last frame's data, or data written
    once at setup, is excluded.

Resources whose first-to-last-use intervals within the frame don't overlap then share memory. The
saving is therefore not "sum of transients" but "sum of transients minus the peak of what is live at
once". A resource still read by D3D11 or game draws after the graph epoch has to stay live until that
last read, which usually makes it span most of the frame.

Lifetime classes used below:

| Class | Meaning | Aliasable |
| --- | --- | --- |
| Transient | Fully written before any read, every frame. All reads happen in the same frame. | Yes |
| History | Read in a later frame: ping-pong pairs, temporal accumulation, prev-frame copies. | No |
| Persistent | Loaded from disk, computed once or on settings change, or updated a slice per frame. | No |
| External | Written or read by game draws (render-target table slots, PS slots bound for the frame). | Only if the graph models the game's use |

## Priority list

| # | Feature | Total | Aliasable | Difficulty |
| --- | --- | --- | --- | --- |
| 1 | PostProcessing | ~1,246 MB est. | ~1,205 MB | Low-med |
| 2 | ScreenSpaceGI | 504 MB | 120 MB as is, ~296 MB with history split | Med |
| 3 | SubsurfaceScattering | 64 MB | 64 MB | Low |
| 4 | ExponentialHeightFog (volumetric) | ~98 MB est. | ~49 MB | Low |
| 5 | Deferred GBuffer + HDRDisplay outputs | 176 + 128 MB | ~128-176 MB from aliasing the two sets | High |
| 6 | LightLimitFix (finish) | 72 MB | ~2 MB now, +33 MB | Low / med |
| 7 | Effects11 | 5.3 MB | 5.3 MB | Med |

### 1. PostProcessing (~1,246 MB est.)

**Why it's first.**

-   It is the largest single block of transient memory in CS.
-   `PostProcessing::SetupResources` (`PostProcessing.cpp:510-515`) calls `SetupResources()` on all 13
    sub-effects whether or not they are enabled. Turning the feature on costs about 1.2 GB at 4K even
    with every effect off. Only MotionBlur allocates lazily.
-   Under ORG, disabled effects would cost nothing, and enabled ones would share memory.

**Structure.** It is one self-contained chain called from a single hook (`PreProcess`, from
`Hooks.cpp:479`):

-   `BeginLinearProcessing`, then the sub-effects in order, then `CopyToRenderTarget`.
-   Compute plus fullscreen raster passes, with no game draws in between.
-   Imports needed: kMAIN, kMAIN_COPY, scene depth and kMOTION_VECTOR. Border also writes
    kMOTION_VECTOR.

Almost every large texture is a per-effect `texOutput` (63 MB each at RGBA16F) or a scratch chain:

| Sub-effect | Total | Transient | Notes |
| --- | --- | --- | --- |
| Top level | ~127 MB | `texCopyMain`, `texInput` | |
| DoF | ~280 MB | all | Only the 1x1 focus textures are history. |
| PhysicalGlare | 159 MB | 127 MB | PSF FFT and aperture are persistent (regenerated only when dirty). |
| LensFlare | ~107 MB | ~101 MB | The bokeh FFT cache is persistent. `texFFTResult` is never written: a dead allocation. |
| CODBloom | 84 MB | 84 MB | Mip chain; lives until Composite. |
| Vignette, Composite, ColorGrading, LUT, Camera, Border | 63 MB each | all | One `texOutput` each. |
| MotionBlur | 64 MB | all | Lazy. |
| LocalExposure | 37 MB | all | `texLogLuminance` has 10 mips; mips 6-9 are never used. |
| HistogramAutoExposure | tiny | — | `adaptationSB` is history. |

With everything enabled, the live peak after aliasing is estimated at about 330 MB, inside DoF.

**Things to handle.**

-   **Dynamic-resolution partial writes.** The passes write only the dynamic-resolution rect, but
    `CopyToRenderTarget` copies the whole texture and Composite samples at full resolution. With
    aliasing, the area outside the rect becomes garbage instead of stale data. Check with
    DRS/upscaling on.
-   **The chain varies per frame,** depending on which effects are enabled.
-   **Raster passes** need ORG's raster path, not just compute.

**Done without ORG:** sub-effects are now set up only while active, and a disabled one is replaced
by a fresh instance, which frees its textures (`PostProcessing::SyncPipelineResources`).

### 2. ScreenSpaceGI (504 MB measured)

Everything is created in `ScreenSpaceGI.cpp:402-605` at full resolution from the kMAIN desc.
`ResolutionMode` (half/quarter) only shrinks the dispatch, so at half or quarter res 75% or 94% of each
buffer is never touched. `GiSpecular` is allocated even when specular GI is off.

| Resource | Size | Class |
| --- | --- | --- |
| `Radiance` (5 mips) | 44 MB | Transient: PrefilterRadiance to GI |
| `RadianceTemp` | 32 MB | Transient. In AO-only mode it is read without being written. |
| `WorkingDepth` (5 mips) | 22 MB | Transient |
| `Normal` (5 mips) | 22 MB | Transient |
| `IlY[0/1]`, `GiSpecular[0/1]` | 64 MB x 4 | History ping-pong; the output is read by DeferredComposite |
| `IlCoCg[0/1]` | 32 MB x 2 | History ping-pong |
| `AO[0/1]`, `AccumFrames[0/1]` | 8 MB x 4 | History ping-pong |
| `PrevGeo` | 32 MB | History |
| `Noise` | 2.75 MB | Persistent |

The whole chain is `DrawSSGI`, called from one site in `DeferredPasses`, so it is a clean port.

-   **As written:** 120 MB of transients alias.
-   **Splitting each ping-pong pair** into one history texture plus one transient scratch texture makes
    another ~176 MB aliasable, ~296 MB in total. The pass-to-pass flips in `DrawSSGI` use
    function-static indices, so the remodel is the main work.
-   **Sizing the textures to the internal resolution** saves memory independently of ORG.

### 3. SubsurfaceScattering (64 MB measured)

| Resource | Size | Class |
| --- | --- | --- |
| `DiffuseNoAlbedo` | 32 MB | Transient: extraction prepass to blur/Burley |
| `BlurHorizontalTemp` | 32 MB | Transient: horizontal blur to vertical blur. Unused in Burley mode. |

It is a 2-3 dispatch compute chain from one call site (`Deferred.cpp:348-350`) and runs right after SSGI.
Its whole 64 MB fits inside SSGI's dead transients, so the pair should be ported together.

The dump showed SSS at 80 MB because ScreenSpaceShadows names its 16 MB texture `SSS::ShadowTexture`,
and the attribution maps the `SSS::` prefix to SubsurfaceScattering.

### 4. ExponentialHeightFog, volumetric fog only (~98 MB est.)

The volumetric resources exist only when `volumetricFogEnabled` is set (off by default). The grid
follows the dynamic render size (240x135x64 near, 60x34x32 far at defaults).

| Resource | Size | Class |
| --- | --- | --- |
| `VBufferA` (+ far) | 15.8 + 0.5 MB | Transient |
| `LightScattering` (+ far) | 31.6 + 1 MB | Transient; copied to history at the end of Prepass |
| `LightScatteringHistory` (+ far) | 31.6 + 1 MB | History |
| `IntegratedLightScattering` (+ far) | 15.8 + 0.5 MB | External: bound at PS t19/t22 for the frame, imported by DCLF |
| Conservative depth + history | ~0.25 MB | Transient + history |

It is a compact compute chain from `Prepass`, and about 49 MB is aliasable. The integrated volume
stays external. If the history copy becomes an ORG history resource instead of a `CopyResource`, the
scattering texture is still transient.

### 5. Deferred GBuffer + HDRDisplay outputs (high effort, large payoff)

Neither set can alias within itself, but the two sets are never live at the same time:

-   **Deferred GBuffer (176 MB):** `Albedo`, `Specular`, `Reflectance`, `NormalRoughness`, `Masks`
    (32 MB each) and `Masks2` (16 MB).
    -   Cleared at `StartDeferred` (`SRTM_CLEAR`), then written by the game's opaque MRT draws, DCLF's
        MainOpaque epoch, TerrainBlending and decals.
    -   Last read by DeferredComposite, after SSGI and SSS. Nothing reads them next frame.
-   **HDRDisplay's own textures (128 MB):** `HdrTexture` (64), `UiTexture` (32) and `OutputTexture`
    (32).
    -   Live from imagespace/UI to the Present composite only.

Aliasing one set onto the other saves up to 128 MB, and more if PostProcessing's transients join the
same pool.

**The cost:** these are game render-target table entries written by game draws, so ORG would have to
own the textures and hand them to the game as D3D11 render targets, with the alias hazards respected
across the game's draws.

**Not aliasable here:**

-   `Water1/Water2` (128 MB) are the game's water TAA history.
-   HDRDisplay's upgraded `kTEMPORAL_AA_UI_ACCUMULATION_1/_2` (128 MB) are TAA history.
-   `kIMAGESPACE_TEMP_COPY/_COPY2` (128 MB) and the `kLDR_*` targets (9 MB) are probably per-frame
    game temps, but that is unverified; menus may keep them across frames. If they are per-frame, this
    item gains another ~137 MB.

### 6. LightLimitFix (already on ORG: finish it)

-   **`cs.llf.clusters` and `cs.llf.light-index-counter`** are transient inside the culling epoch but
    are `Materialize()`d (`ORGLightCulling.cpp:344-351`). Dropping that would alias about 2 MB.
-   **`cs.llf.light-index-list` (32 MB) and `cs.llf.light-grid` (1 MB)** are rewritten every frame, but
    D3D11 draws (PS t36/t37) and DCLF read them through the forward and water passes. They could alias
    only if their last D3D11 use is declared to the graph, and even then only with post-forward
    resources.

### 7. Effects11 (5.3 MB measured)

`TextureManager::DownsampleTexture` is transient inside `ExecuteEffects`, but porting the FX11 framework
is out of proportion to 5 MB. Creating it lazily when a preset loads gets the same saving without
ORG.

## Not worth porting for aliasing

| Feature | Size | Reason |
| --- | --- | --- |
| Skylighting | 112 MB | Probe grid is temporal accumulation (history) or read by game draws. |
| TerrainBlending | 88 MB | All three depth targets are per-frame, but game prepass/terrain draws write them and game shaders read them through the opaque phase. Aliasable only if those game draws become graph passes. |
| Upscaling | 32-95 MB | `UpscaledTexture` and `HudlessTexture` are used by Streamline/frame-generation Vulkan submissions that DXVK doesn't track; aliasing is unsafe. |
| DynamicCubemaps | 34 MB | Temporal capture history and a 3-frame filter state machine; ~0.4 MB of BC6H scratch is transient. |
| TerrainShadows | ~49 MB (Tamriel) to ~162 MB (Solstheim) est. | Persistent heightmap plus a shadow map updated one stripe per frame. |
| VolumetricLighting | 22.5 MB | Game-owned 3D volumes and passes. Probably transient within the frame (unverified), but CS doesn't own them. |
| ScreenSpaceShadows | 16 MB | Bound at PS t45 for all world lighting draws. |
| CloudShadows | 12 MB | Injected into game sky draws; cross-frame state. |
| GrassOptimizations | ~3 MB + scene-dependent buffers | Only the 2.8 MB HiZ is transient; the instance buffers are persistent or read by game grass draws. |
| GrassCollision, IBL, VolumetricShadows, Skin, HairSpecular | < 3 MB each | History, persistent, or negligible. |
| UnifiedWater, WetnessEffects | 0 | No CS-owned per-frame GPU resources. |

## Savings that don't need ORG

-   **LightLimitFix legacy duplicate (~35 MB).**
    -   `SetupResources` always creates the D3D11 fallback set: `LLF::LightIndexList` (32 MB),
        `Clusters`, `LightGrid` and the counter. With ORG culling active, lighting doesn't use it.
    -   **Fixed:** ExponentialHeightFog bound the legacy `lightIndexList`/`lightGrid`
        unconditionally, so on frames where ORG did the culling the volumetric fog read light lists
        that were never updated. It now uses `LightLimitFix::GetLightIndexListSRV/GetLightGridSRV`,
        which return the source this frame's culling wrote.
    -   Remaining: create the legacy set only on fallback.
-   **PostProcessing (done):** only active sub-effects hold resources (up to ~1.2 GB saved with
    effects off).
-   **ScreenSpaceGI:** size the textures to the internal resolution in half/quarter mode.
-   **Dead allocations and code:**
    -   LensFlare `texFFTResult` (2 MB) is never written.
    -   LocalExposure `texLogLuminance` mips 6-9 are never used.
    -   HDRDisplay's `SnapshotCleanScene` has no caller.
    -   HDRDisplay keeps the original LDR game targets alive in `savedLDRTargets` while the upgraded
        ones are in use.
-   **Attribution fix:** rename ScreenSpaceShadows' `SSS::` resources to `ScreenSpaceShadows::` so the
    VRAM widget stops counting them under SubsurfaceScattering.
