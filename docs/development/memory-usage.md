# Memory usage: main menu baseline

What occupies device memory before any save is loaded, for choosing which resources to port to OpenRenderGraph for its memory
aliasing. Which resources can alias, and why, is in [org-aliasing-port-priorities.md](./org-aliasing-port-priorities.md). This file
adds fresh sizes for the whole heap, including what the attribution list does not name.

## Summary

-   The main menu uses **4,147 MiB** of the device-local heap at 3840x2160, about half of a settled Whiterun (8,555 MiB).
-   DCLF changes nothing here: 4,147 and 4,149 MiB on, 4,149 and 4,149 MiB off.
-   Almost all of it is fixed-size targets. Full-4K textures alone are 2,506 MiB, 60% of the heap: 1,402 MiB in CS and 1,104 MiB in
    the game. Game textures are only 71 MiB.
-   Those targets stay the same size in Whiterun, so they are a floor under every scene. Aliasing them saves memory in every scene.
-   **No memory is aliased today.** ORG's alias pools are 0 MiB in the menu and in Whiterun, and ORG owns only 37 MiB of resources in
    the menu.

## How this was measured

-   2026-11-02, RTX 5080, 3840x2160, shadow map resolution 4096, Dev-Fast build at `11b507453`, `C:\Modding\MO2`.
-   Two runs with DCLF on and two with it off, 3,000 frames in the menu (about 12 s), using the last `dclf:vram-json` snapshot of each.
-   Whiterun figures come from one DCLF-on run after `coc Whiterun`, on the same code apart from this file and the defect note.
-   PostProcessing has no resources in any of these runs. It would add up to about 1.2 GB with its effects on (see
    [Candidates](#candidates-for-org-aliasing)).
-   The driver breakdown comes from one run with a temporary `VK_EXT_device_memory_report` patch in DXVK, since removed.

## The whole heap

These rows add up to the heap total. "Whiterun" is shown only for owners whose size matters to the comparison.

| Owner | Menu (MiB) | Whiterun (MiB) | Notes |
| --- | --- | --- | --- |
| Game render targets, with 3D targets and back buffer | 1,187 | 1,165 | The 3D volumes move to VolumetricLighting in Whiterun |
| Game depth and shadow | 531 | 531 | Shadow maps 320, book text 90, three 4K depth buffers 120 |
| ScreenSpaceGI | 520 | 520 | |
| Outside DXVK and ORG | 513 | 658 | See [below](#outside-dxvk-and-org) |
| HDRDisplay | 392 | 392 | |
| Deferred | 303 | 303 | |
| Skylighting | 113 | 113 | |
| Free space in DXVK chunks | 103 | 753 | Not in use. The Whiterun figure varies with fragmentation |
| TerrainBlending | 88 | 88 | |
| SubsurfaceScattering | 80 | 80 | 16 MiB of this is ScreenSpaceShadows, misattributed by its `SSS::` prefix |
| LightLimitFix | 72 | 72 | 37 MiB in ORG and 35 MiB in its D3D11 fallback set |
| Game textures | 71 | 3,083 | The main thing a save adds |
| ORG allocations not tracked | 58 | 4 | Device memory no tracked resource claims |
| DynamicCubemaps | 34 | 34 | |
| Upscaling | 32 | 32 | |
| Other game resources | 11 | 145 | Mostly geometry and unscoped resources in Whiterun |
| CloudShadows, IconLoader, Effects11, GrassCollision, others | 31 | 31 | |
| DXVK internal | 10 | 10 | |
| Only in Whiterun: DCLF | 0 | 355 | ORG resources |
| Only in Whiterun: TerrainShadows, VolumetricLighting, others | 0 | 80 | |

The Whiterun column is one run and is rounded, and its other small rows (retired storages, ORG slack) are left out, so it does not
add up as exactly as the menu column does.

### Outside DXVK and ORG

None of this can be aliased through ORG.

| Item | MiB | Notes |
| --- | --- | --- |
| Driver's device-internal memory | 143 | |
| Pipelines | 95 | 9,291 of them |
| Other device memory | 96 | Matches the 3 swapchain images at 4K. That link is inferred |
| Images and command pools | 11 | |
| Not reported by the driver | about 169 | Unknown |

## Resources by feature

Lifetime classes (transient, history, persistent, external) and the reasons for them come from
[org-aliasing-port-priorities.md](./org-aliasing-port-priorities.md), which read them from the code on 2026-10-29. They were not
checked again for this file. Sizes are measured.

### ScreenSpaceGI (520 MiB)

| Resource | MiB | Format | Class |
| --- | --- | --- | --- |
| `IlY[0/1]` | 63.8 x 2 | RGBA16F | History ping-pong |
| `GiSpecular[0/1]` | 63.8 x 2 | RGBA16F | History ping-pong |
| `Radiance` (5 mips) | 43.7 | R11G11B10F | Transient |
| `RadianceTemp` | 31.9 | R11G11B10F | Transient |
| `IlCoCg[0/1]` | 31.9 x 2 | RG16F | History ping-pong |
| `PrevGeo` | 31.9 | R11G11B10F | History |
| `WorkingDepth` (5 mips) | 21.8 | R16F | Transient |
| `Normal` (5 mips) | 21.8 | RG8 | Transient |
| `AO[0/1]` | 15.9 x 2 | R16 | History ping-pong |
| `AccumFrames[0/1]` | 8.0 x 2 | R8 | History ping-pong |
| `Noise` | 2.8 | RG8 | Persistent |

### HDRDisplay (392 MiB)

| Resource | MiB | Format | Class |
| --- | --- | --- | --- |
| `HdrTexture` | 63.8 | RGBA16F | Live from imagespace to Present |
| `kTEMPORAL_AA_UI_ACCUMULATION_1/_2` | 63.8 x 2 | RGBA16F | History |
| `kIMAGESPACE_TEMP_COPY/_COPY2` | 63.8 x 2 | RGBA16F | Probably per-frame, unverified |
| `OutputTexture` | 31.9 | RGB10A2 | Live from imagespace to Present |
| `UiTexture` | 31.9 | RGBA8 | Live from UI to Present |
| `kLDR_DOWNSAMPLE0`, `kLDR_BLURSWAP` | 4.7 x 2 | RGBA16F | Probably per-frame, unverified |

### Deferred (303 MiB)

| Resource | MiB | Format | Class |
| --- | --- | --- | --- |
| `Water1/2` | 63.8 x 2 | RGBA16F | History (the game's water TAA) |
| `Albedo`, `NormalRoughness` | 31.9 x 2 | RGB10A2 | External: per-frame, written by game draws |
| `Specular`, `Reflectance`, `Masks` | 31.9 x 3 | R11G11B10F | External: per-frame, written by game draws |
| `Masks2` | 15.9 | R16 | External: per-frame, written by game draws |

### Smaller features

| Feature | Resource | MiB | Class |
| --- | --- | --- | --- |
| SubsurfaceScattering | `DiffuseNoAlbedo`, `BlurHorizontalTemp` | 31.9 x 2 | Transient |
| ScreenSpaceShadows | `SSS::ShadowTexture` | 15.9 | External: bound for all world lighting draws |
| LightLimitFix | `cs.llf.light-index-list` (ORG) | 31.9 | Rewritten each frame, read by D3D11 draws |
| LightLimitFix | `LLF::LightIndexList`, `LLF::Clusters`, `LLF::LightGrid` (D3D11 fallback) | 31.9, 2.0, 1.0 | Unused while ORG culling is active |
| Skylighting | `ProbeArray`, `ShadowBitmask`, two others | 64, 32, 8, 8 | History or read by game draws |
| TerrainBlending | `TerrainDepth`, `BlendedDepth`, `BlendedDepth16` | 39.8, 31.9, 15.9 | External: per-frame, used by game draws |
| Upscaling | `UpscaledTexture` | 31.9 | Used by Vulkan submissions DXVK does not track: unsafe to alias |

### Game targets (1,718 MiB with depth, shadow and back buffer)

An imported D3D11 texture is never an aliasing candidate, so all of these stay out of reach unless ORG comes to own the texture and
hands it to the game. The largest:

| Target | MiB |
| --- | --- |
| `kSHADOWMAPS` (4096, 8 layers), `kSHADOWMAPS_ESRAM` (2 layers) | 256, 64 |
| `kBOOK_TEXT` depth, `kBOOK_TEXT_0..3` | 89.8, 17.9 x 4 |
| `kSAO_CAMERAZ` (12 mips) | 43.7 |
| `kMAIN`, `kMAIN_COPY`, `kPOST_ZPREPASS_COPY` depth | 39.8 x 3 |
| 26 colour targets at full 4K, such as `kMAIN`, `kSAO*`, `kGETHIT_*`, `kSCREENSHOT` | 31.9 each |
| `kIMAGESPACE_VOLUMETRIC_LIGHTING` (3 targets), `kSNOW_SPECALPHA`, `kTEMPORAL_AA_MASK` | 15.9 each |
| `kSSR`, `kSSR_RAW`, `kSSR_BLURRED0` | 16.9 each |

## Candidates for ORG aliasing

The difficulty ratings come from the priorities file. MiB is what becomes eligible, not what is saved. Resources share memory only
when their lifetimes do not overlap, so the saving is the eligible total minus the peak live at once. Nothing here has been run
through ORG's planner.

| Candidate | Eligible (MiB) | Difficulty | What it needs |
| --- | --- | --- | --- |
| PostProcessing | 0 here, about 1,205 with effects on | Low-med | The port. It matters only to users with effects on |
| ScreenSpaceGI transients | 119 | Med | The port of `DrawSSGI` |
| ScreenSpaceGI ping-pong split | 183 more | Med | One history plus one scratch texture per pair. The priorities file says 176 with `AO` at 8 MiB; it measures 15.9 |
| SubsurfaceScattering | 64 | Low | Port with ScreenSpaceGI, since it should fit in that chain's dead transients |
| Deferred G-buffer with HDRDisplay outputs | 128 to 175 saved | High | ORG owns game render targets and handles hazards across game draws |
| `kIMAGESPACE_TEMP_COPY*`, `kLDR_*` | 137 | Unknown | First confirm they are per-frame. Menus may keep them across frames |
| LightLimitFix | 2, then 33 | Low, then med | Drop `Materialize()`, then declare the last D3D11 use to the graph |

### Savings that do not need ORG

These could change the case for a port, so they are worth settling first.

-   **HDRDisplay's originals, about 132 MiB.** The game's 32 MiB versions of the four upgraded targets are still live alongside
    HDRDisplay's copies, and the `kLDR_*` originals add 5 MiB. `savedLDRTargets` keeps the originals. Whether they can be released
    while the upgrade is active was not checked.
-   **LightLimitFix's D3D11 fallback, 35 MiB.** Create it only when falling back.
-   **Early allocation.** The feature targets exist in the menu with no scene. Creating them on first use would lower the menu figure
    only, not in-game use.
-   **ScreenSpaceGI at half or quarter resolution.** Sizing textures to the internal resolution saves memory without any port.

## Open questions

-   Other resolutions were not measured. Screen-sized targets should scale with pixel count, but only 3840x2160 has data.
-   The settings used here decide which features allocate, so another preset gives different totals.
-   This measures size at rest. It shows nothing about lifetimes in a frame, which is what the planner's saving depends on.
-   The 169 MiB the driver does not report, and the 58 MiB of untracked ORG allocations, are unexplained.

## Reproducing

1.  Disable Start On Save for the run. Moving `mods\Start On Save\SKSE\Plugins\po3_StartOnSave.dll` aside works, and it has to go
    back afterwards. Its ini's `Character Name` filter does not stop the load.
2.  Put these in `CommunityShaders.env` beside the log:

    ```ini
    CS_VRAM_DEBUG=1
    DXVK_MEMORY_STATS_LOG=5
    CS_DCLF_TEST_COMMANDS=300:dclf:vram-json;1500:dclf:vram-json;2700:dclf:vram-json;2720:dclf:shot=menu
    CS_DCLF_TEST_EXIT=3000
    ```

3.  Check the screenshot. A compass HUD means a save loaded and the run is not valid.
4.  In the JSON, skip rows with the `kRowNotCounted` flag (`flags & 2`) when adding up owners. They are ORG device memory and ORG's
    D3D11 imports, which would count the same bytes twice.
