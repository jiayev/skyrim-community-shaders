# Skyrim SE/AE engine notes (from the decompiled binary)

Findings from the Ghidra database of `SkyrimSE.exe` 1.6.1170 (AE), gathered while building Drawcall Limit
Fix. Addresses are AE runtime addresses with image base `0x140000000`. Address Library IDs are given as
`SE/AE` where known. Decompiled names such as `FUN_…` and `DAT_…` are Ghidra's; the meanings given are
inferred and should be re-checked before being relied on elsewhere.

## Lighting shader: from property flags to shader descriptors

### `BSLightingShaderProperty::GetRenderPasses` (AE `0x1414adfb0`)

Builds and caches the property's `renderPassList` (the passes stay on the property between frames). The
list is rebuilt only when `lastRenderPassState` changes. That state is `(renderMode << 8) | activeLightMask`,
or the list is forced when light lists change, when the shadow light count changes, or when render mode
`0x16` is used. Render mode `0x1b` takes a separate path.

The main lighting pass stores its technique ID in `BSRenderPass::passEnum` as
`descriptor + 0x4800002D`, where `descriptor` is built as follows (`flags` is the 64-bit
`BSShaderProperty::EShaderPropertyFlag` set):

| Descriptor bits | Meaning | Source |
| --- | --- | --- |
| 0 (`0x1`) | VC | `kVertexColors` (bit 37) |
| 1 (`0x2`) | Skinned | `kSkinned` (bit 1) |
| 2 (`0x4`) | ModelSpaceNormals | `kModelSpaceNormals` (bit 12) |
| 3–5 | point light count − 1 (clamped to 7) | light data |
| 6–8 | shadow light count (clamped to 4) | light data |
| 9 (`0x200`) | Specular | `kSpecular` (bit 0) or `kMultiIndexSnow` (bit 41) |
| 10 (`0x400`) | SoftLighting | `kSoftLighting` (bit 57) |
| 11 (`0x800`) | RimLighting | `kRimLighting` (bit 58) |
| 12 (`0x1000`) | BackLighting | `kBackLighting` (bit 59) |
| 13 (`0x2000`) | ShadowDir | directional shadow applies (light assignment, `FUN_1414fcf80`) |
| 14 (`0x4000`) | DefShadow | shadow mask applies (the accumulator's deferred-shadow flag at `+0x178`, distance and alpha conditions) |
| 15 (`0x8000`) | ProjectedUV | `kProjectedUV` (bit 23) with snow/material conditions |
| 16 (`0x10000`) | AnisoLighting | `kAnisotropicLighting` (bit 53) |
| 17 (`0x20000`) | AmbientSpecular | decal/fade conditions |
| 18 (`0x40000`) | WorldMap | LOD objects/land with `kMenuScreen` (bit 55) |
| 20 (`0x100000`) | DoAlphaTest | `NiAlphaProperty` alpha test (flag bit 9) and either the early-Z global or alpha blending/fading; or decals without `kMultiIndexSnow` (see below) |
| 21 (`0x200000`) | Snow | snow shader enabled and `kSnow` |
| 22 (`0x400000`) | CharacterLight | `kCharacterLighting` (bit 40) |
| 23 (`0x800000`) | AdditionalAlphaMask | screen-door fade (`kScreendoorAlphaFade` off, fade node fading) |
| 24–29 | technique | see below |

Technique selection (bits 24–29), later tests win:

| Technique | Condition |
| --- | --- |
| 1 Envmap | `kEnvMap` (bit 7) |
| 2 Glowmap | `kGlowMap` (bit 38) |
| 3 Parallax | `kParallax` (bit 11) without `kParallaxOcclusion` (bit 28) |
| 4 Facegen | `kFace` (bit 10) |
| 5 FacegenRGBTint | `kFaceGenRGBTint` (bit 21) |
| 6 Hair | `kHairTint` (bit 18) |
| 7 ParallaxOcc | `kParallaxOcclusion` (bit 28) |
| 8 MTLand | `kMultiTextureLandscape` (bit 14) |
| 19 MTLandLODBlend | `kNoLODLandBlend` (bit 46) |
| 18 LODLandNoise | `kLODLandscape` (bit 33) |
| 13 LODObjects | `kLODObjects` (bit 34) |
| 15 LODObjectHD | `kHDLODObjects` (bit 63) |
| 11 MultilayerParallax | `kMultiLayerParallax` (bit 56) |
| 12 TreeAnim | `kTreeAnim` (bit 61) |
| 14 MultiIndexSparkle | `kMultiIndexSnow` (bit 41) and `kProjectedUV` (bit 23) |
| 16 Eye | `kEyeReflect` (bit 17) |

Bits 6–8, 13, 14 and 20 depend on per-frame engine state rather than on the property:

-   ShadowDir, DefShadow and the shadow light count come from the light and shadow assignment.
-   DoAlphaTest for alpha-tested geometry is `alphaTest && (earlyZ || alphaBlended || fading)`. `earlyZ` is
    the byte at AE `0x14328cc79`: the `ToggleEarlyZ` console command flips it, and two offscreen renders
    set it and clear it around their work (`FUN_1406d2120`, and `FUN_140972590` for mode 1). Whether it
    is set in the main pass therefore depends on what rendered before: after a fresh load it was set in
    Whiterun, after `coc` into Dragonsreach it was clear.

Drawcall Limit Fix takes these bits from the pass the main-camera accumulator holds (see "Batch renderer").

`accumulationHint` (`BSRenderPass+0x1C`) records the batch group the pass goes to: 1 when alpha-blended
and fully faded in, 2/3 for decals, 9/10 for fading, 11 for TreeAnim, 15 for plain opaque with no shadow
work, and so on.

### `BSLightingShader::SetupTechnique` (vtable slot 2, AE `0x1414db810`)

Converts `passEnum` back to a descriptor with `d = passEnum - 0x4800002D`, then:

-   Technique 18 (LODLandNoise) becomes 9 (LODLand) unless a noise global is set; technique 7
    (ParallaxOcc) becomes 0 unless a parallax-occlusion global is set.
-   Vertex descriptor (`FUN_14151c730`): `(d & 0x48007) | ((d & 0x20a00) ? 0x200 : 0) | (d & 0x3f000000)`.
    Only VC, Skinned, ModelSpaceNormals, ProjectedUV, WorldMap, Specular (if Specular, AmbientSpecular
    or RimLighting) and the technique survive, so **light counts never reach the vertex shader**.
-   Pixel descriptor (`FUN_14151c760`): `(d & (0xfffffe05 | ((d >> 1) & 2))) | 1`. Bits 3–8 (light counts)
    are cleared, Skinned is kept only with ModelSpaceNormals, and **VC is always set**.
-   Stores `d` at `BSLightingShader+0x94` (read back by SetupGeometry), then sets per-technique sampler
    address modes and writes the PerTechnique constants.

Community Shaders then rewrites both descriptors in `State::ModifyShaderLookup` (strips shadow, snow,
ambient-specular and lighting-model bits and adds Deferred). The stripped bits still matter: Community
Shaders passes them to the shader in its permutation buffer, and SetupTechnique binds the shadow mask from
them (below).

What SetupTechnique writes (ported in `ConstantEvaluator.cpp` `EvaluateTechnique`):

-   **Sampler filter modes** (`PSTextureFilterMode`): slots 0 and 1 anisotropic (3) always; Envmap adds 4
    and 5, Glowmap 6, Parallax 3. SetupMaterial leaves filter modes alone for the vanilla material types
    (TruePBR's hook sets its own).
-   **VS PerTechnique** (`FUN_1414dfad0`): from the fog property of the current scene graph,
    `BSShaderManager::State` (AE `0x142033060`) → `shadowSceneNode[sceneGraph (+0xC0)]` →
    `ShadowSceneNode+0x220` (`BSFogProperty`):
    -   `FogParam` = `(near / (far − near), 1 / (far − near), power (+0x84), clamp (+0x88))`, or
        `(5000000, 0.1, 1, 0)` when both distances are 0;
    -   `FogNearColor` = `(nearColor (+0x38), BSShaderManager::State::invFrameBufferRange (+0x9C))`;
    -   `FogFarColor` = `farColor (+0x44)`; w is whatever was on the stack.
-   **PS PerTechnique**: `FogColor` = `FogNearColor`; `ColourOutputClamp` = `(fLightingOutputColourClampPostLit,
    PostEnv, PostSpec, 0)` (`[General]` settings at AE `0x142035498`, copied into globals at `0x14338ca0c`
    when the shader is constructed, `FUN_1414dad00`).
-   **Shadow mask** when `(ShadowDir || shadow lights) && DefShadow`: t14 = the shadow mask render
    target's SRV, address mode 0, filter mode `iShadowMaskQuarter:Display != 4 ? 1 : 0`; PS `VPOSOffset` =
    `(1 / width, 1 / height, 0, 0)` of the globals at AE `0x14328be9c` (the shadow mask size).
    **The VPOSOffset write happens after the buffer is unmapped.** On DXVK the dynamic buffer stays mapped,
    so the draw still sees it.
-   **Who writes a local shadow light's mask channel: its receivers.** Render modes `0x10`/`0x11`
    (`BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask`, `0x1414af030`) give a property with ReceiveShadows (flag bit 8)
    one Utility pass per shadow light in its `activeLightMask` (`0x11` also takes `firstPersonShadowMask`), kept in
    `shadowMapOrMaskPasses`: `0x400000` for a frustum light (vfunc 5), `0x1000000` for an omni light (vfunc 8), `0x800000` for a
    parabolic one (vfunc 7), `0x200000` for the directional light. Each draws the receiver's own geometry into the light's channel.
    Their constants (`BSUtilityShader::SetupGeometry` `0x1414fae40`, `FUN_1414fbab0` textures, `FUN_1414fbd30` matrices): the light
    is `pass->sceneLights[0]`; `ShadowMapProj[0]` its first descriptor's `lightTransform` with the eye folded into the translation;
    `AlphaTestRef.y` = `shadowBiasScale` (+0x540) × float `0x141ac1d48` × float `0x1433dcfb0`; `ShadowLightParam.x` the light's radius
    (paraboloids) or the frustum light's falloff; `ShadowLightParam.z` the fade distance squared, float `0x1418af8b0`, or `0x1433dcfac`
    for a light with `lodFade` (+0x63); `EndSplitDistances.x` (the slice) from the global `0x1433dcfb4`; `ShadowSampleParam.z` (PCF
    radius) = float `0x1420358f0` / uint `0x1433dcfc0` for filters 2 and 3. The shadow map is the light's descriptor's depth target.
-   **The shadow mask target is created** in `BSShaderRenderTargets::Create` (AE `0x1414cf210`): `lea edx, [rdi + 0x12]`,
    `lea rcx, [RenderTargetManager]`, then the create call at `+0x554` (the refraction normals' at `+0x502`, target `0xD`).

### `BSLightingShader::SetupGeometry` (vtable slot 6, AE `0x1414dd040`)

Per-technique constant writes (constant indices come from the shader's reflected `constantTable`):

-   Envmap/MultilayerParallax/Eye (1, 11, 16): a material float at material `+0x104`, and the eye position.
-   **The eye position** (VS `EyePosition`) is written for those three techniques and for any pass whose
    descriptor has bit 9 (`0x200`) or one of `0x21c00` set (the shader's `+0x94`), whatever the technique: hair,
    most opaque statics. The value is the current accumulator's eye (`FUN_141480b20()` returns `0x14332a3a8`;
    `+0x16c`) less `posAdjust`, the same for every pass that writes it once Community Shaders' patch keeps every
    pass in world space. Any other pass leaves the buffer's last value.
-   MTLand/MTLandLODBlend (8, 19): land blend parameters from the geometry's parent cell data.
-   LOD techniques (9, 13, 15, 18): two transform uploads (current and previous).
-   TreeAnim (12): wind parameters from the object's `BSTreeNode`-like parent (`vfunc 0x1f8`).
-   Everything not skinned: World and PreviousWorld from `geometry->world` (`+0x7c`) and the previous
    world (`+0xb0`, or the current one when render flag `0x10` is set).
-   Then the directional light: colour scaled by the image-space HDR multiplier, and direction
    transformed into model space unless the shader works in world space (Community Shaders patches it to
    always use world space).

## Scene graph layout

### Cell 3D category nodes

A loaded cell's 3D root (`TESObjectCELL` → `loadedData->cell3D`, returned by `FUN_1402b86e0`) has a fixed
set of child nodes, indexed by position in `cell3D->children`:

| Index | Node | Contents |
| --- | --- | --- |
| 0 | Actor | actor 3D (skinned bodies, equipped weapons and armor) |
| 1 | Marker | editor markers |
| 2 | Land | terrain (`BSLightingShader` MTLand) |
| 3 | Static | static references |
| 4 | Dynamic | movable references (Havok clutter, doors, activators) |
| 5 | MultiBound | multibound nodes |
| 6 | Water | water planes |

Source: the `ToggleCellNode` console command (`ConsoleFunc::handler::ToggleCellNode`, AE `0x14036ef60`,
help string "0-Actor, 1-Marker, 2-Land, 3-Static, 4-Dynamic, 5-Multibound, 6-Water"), which calls
`FUN_14019f8b0(TES, index, show)`. That helper reads `cell3D->children[index]` and toggles bit 0 of
`NiAVObject::flags` (`+0xF4`, `kHidden`/app-culled) for every loaded cell and the sky cell.

### Multibounds, rooms and portals: where references really hang

References start under the category nodes, but the engine moves many of them elsewhere, and those are
still drawn by the main pass. Everything below is under `TES::objRoot` (`TES+0x80`, named
`ObjectLODRoot`):

-   **Exteriors:** `objRoot` → `BSMultiBoundNode` (no user data) → `NiNode` → the reference's `BSFadeNode`.
    In Whiterun this held about 60% of the eligible static draws (walls, houses, rocks). It is not the
    cell's `loadedData->multiBoundNode`.
-   **Interiors with a portal graph:** the interior cell's `cell3D` is itself a `BSMultiBoundNode` child of
    `objRoot`. The rooms (`BSMultiBoundRoom`, also listed in `loadedData->portalGraph->rooms`) hang under a
    plain `NiNode` child of it, as does `NiNode 'Shared Portal Geometry'` → `BSPortalSharedNode 'Portal
    Shared Geometry'` (`portalGraph->portalSharedNode`). References are moved into their room, including
    actors.
-   Some interior geometry is drawn with **no parent at all** (about 30 draws per frame in Dragonsreach and
    Bleak Falls Barrow: `NorChamberRitual01`, `NorHallBg1wayStairs256` and other statics). Every such root is in
    the interior's portal graph's `alwaysRenderChildren` (`BSPortalGraph +0x58`; measured 2026-10-02: all of
    4,969 draws in a report, none in the lists at `+0x78`, `+0x90` or `+0xA8`). The graph is the cell's
    `loadedData->portalGraph`, and also `ShadowSceneNode`'s.

### `NiNode` child management overrides

`AttachChild` (vtable 0x35) is overridden by `BSFaceGenNiNode`, `NiSwitchNode`,
`BSParticleSystemManager` and `UI3DSceneManager`. The detach and `SetAt` slots are overridden by
`BGSDecalNode`, `NiSwitchNode` and `BSParticleSystemManager`. Every other node class (`BSFadeNode`,
`BSMultiBoundNode`, `BSLeafAnimNode`, …) uses the `NiNode` implementations, so detouring those catches
attachments for static scene content.

## LOD fades in `GetRenderPasses`

Before choosing the technique, `GetRenderPasses` fades some features out by distance. It compares the
property's fade node LOD metric, a float at `BSFadeNode+0x144` (the previous frame's at `+0x148`), with
`[LightingShader]` INI thresholds through `FUN_14147c470(metric, previous, start, end, &fade, &state)`:

-   If `end < metric`, the feature is off (the function returns false).
-   If `metric <= start`, `fade = 1`. Otherwise `fade = clamp((metric - end) / (start - end), 0, 1)`.

| Feature | Thresholds (default) | Effect when off | Fade stored at |
| --- | --- | --- | --- |
| Specular (`kSpecular` or `kMultiIndexSnow`) | `fSpecularLODFadeStart/End` (0.09 / 0.10) | clears `kSpecular`, so descriptor bit `0x200` is not set | property `+0x100` (`specularLODFade`) |
| Environment map (`kEnvMap`) | `fEnvmapLODFadeStart/End` (0.09 / 0.10) | clears `kEnvMap` unless `kSnow` is set, so technique Envmap becomes None | property `+0x104` (`envmapLODFade`) |
| Decals (`kDecal`/`kDynamicDecal`, not `kZBufferWrite`) | `fDecalLODFadeStart/End` (0.05 / 0.06) | multiplies the object fade | local |
| Refraction (`kRefraction`/`kTempRefraction`, not `kNoFade`) | `fRefractionLODFadeStart/End` (0.025 / 0.03) | no passes at all | property `+0x104` |

The settings are engine `Setting` objects (AE `0x142033370` onward, 0x18 bytes each: vtable, value, name).
`FUN_1414acf90` copies them into the working globals at AE `0x142033350` to `0x142033368`. Read them by
name with `RE::GetINISetting("fSpecularLODFadeStart:LightingShader")`.

Specular and envmap fades are therefore per-frame inputs to the shader descriptors: in Whiterun about 4%
of the eligible main-pass draws had specular faded out.

## External emittance

A Lighting property with `kExternalEmittance` (flag bit 29) does not own its emissive colour: its `emissiveColor`
pointer is aimed at a colour shared by everything with the same emittance source (AE 1.6.1170):

-   **The pointer.** `FUN_141480fc0(object, &colour)` walks the object's 3D. For a Lighting property with the flag, it
    frees the owned colour (when `kOwnEmit`), stores the pointer and clears `kOwnEmit`. For an effect property, it
    stores the pointer at `+0x88` and sets `kEffectLighting` when it is null. It is called (through `FUN_14147ff60`)
    by the cell's reference attach, `FUN_1402d2f60`, with:
    -   a light's colour, `TESObjectLIGH+0x118`, when the reference's emittance source (`ExtraEmittanceSource`, extra
        type 0x63: `FUN_1402fb9f0`) is a light;
    -   the region's `emittanceColor`, `TESRegion+0x40`, when the source is a region;
    -   the sky's colour at `Sky+0x9c` into its colour array, for a sky-lit movable static (`FUN_1402efca0`). The
        sky also lists the reference (`FUN_140411b90`).
-   **The colours.**
    -   The light's is constant. The cell's emittance update, `FUN_1402b4390`, rewrites it from the form's byte colour.
    -   The region's is rewritten by the same update, with `Sky::SetColor` from the region's current weather. The
        update runs from `Main::Update`, the cell animation job and `UpdateManagedNodesJob`.
    -   The sky's is rewritten by Sky's colour update (`FUN_14040ba70`), also with `Sky::SetColor`.
-   **`Sky::SetColor`** (`0x14040d970`, `(sky, NiColor* out, COLOR_BLEND*, flash)`): the four-way blend of the
    weather colours, plus the lightning flash clamped to the current weather's limits. It is the only writer of the
    region and sky colours, and it has these two callers.

So a change of weather changes the emissive colour of every object pointing at the colour, and nothing is written to
the objects themselves.

## Renderer shadow state and shader constant groups

`BSGraphics::RendererShadowState` (AE base `0x14202ab70`, CommonLib `RendererShadowState::GetRuntimeData`)
is the engine's CPU-side copy of the D3D11 state. The fields the Lighting shader's `Setup*` functions use:

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x04` | `PSResourceModifiedBits` | dirty bit per pixel texture slot |
| `+0x08` | `PSSamplerModifiedBits` | dirty bit per sampler slot |
| `+0xbc` | `PSTextureAddressMode[16]` | the material's `textureClampMode` (`+0x70`) for material textures |
| `+0xfc` | `PSTextureFilterMode[16]` | set per technique by SetupTechnique (and by TruePBR) |
| `+0x140` | `PSTexture[16]` | `NiSourceTexture->rendererTexture (+0x48)->srv (+0x10)` |
| `+0x348` / `+0x350` | `currentVertexShader` / `currentPixelShader` | `BSGraphics::VertexShader` / `PixelShader` |
| `+0x35c` / `+0x368` | `posAdjust` / `previousPosAdjust` | camera-relative origin, subtracted from world translations |

Texture binds (`FUN_1414e0460(slot, texture, material)`, and `FUN_1414e06b0` for the envmap cube in slot 4)
only write this state; `SetDirtyStates` later flushes it to D3D11.

Constants go through each shader's three constant groups (`PerTechnique`, `PerMaterial`, `PerGeometry`):
`constantBuffers[level] = {ID3D11Buffer*, void* data}` and a `constantTable` that gives each variable's
offset **in floats** (read as an unsigned byte). The Setup functions map the group's buffer with
`WRITE_DISCARD` (only if the buffer pointer is non-null), write `data + table[i] * 4`, unmap, and bind with
`VS/PSSetConstantBuffers(level, …)`. Variable indices are the engine's; Community Shaders reflects its own
shaders into the same index space (`ShaderCache.cpp` `GetVariableIndices`; a variable a permutation lacks
gets offset 0). The engine sometimes writes only part of a float4 (xy of `EnvmapData` and
`LightingEffectParams`, three floats of `EmitColor`); the rest of a discard-mapped buffer is undefined.

### Evaluating the Setup functions outside the render loop

Because the Setup functions reach D3D11 only through the constant-group pointers, they can be run for any
material or pass without drawing:

1.  Point `currentVertexShader`/`currentPixelShader` at stand-in shader objects whose constant buffers are
    null (so nothing is mapped) and whose `constantTable` sends variable *i* to a known float offset of a
    scratch block pre-filled with a sentinel.
2.  Set `BSLightingShader::currentRawTechnique` (`+0x94`) to the pass descriptor.
3.  Call `SetupMaterial` (vtable slot 4) or `SetupGeometry` (slot 6) through the vtable, so every hook on
    them runs too.
4.  Read the scratch blocks and `PSTexture`/`PSTextureAddressMode`, then restore the whole shadow state,
    `currentRawTechnique` and the D3D11 constant buffer bindings of that level (the functions end with
    `VS/PSSetConstantBuffers(level, 1, &nullBuffer)`).

`SetupGeometry` needs a pass whose `numLights > 0`: it dereferences `sceneLights[0]` (the sun) without a
check. `SetupTechnique` cannot be run this way, because it binds real shaders through `BeginTechnique`.

### `SetupGeometry`: what is per object

With Community Shaders' world-space patch (`Hooks.cpp`: `and r15d, 0` at AE `SetupGeometry+0x71`, which
zeroes the model-space flag), everything `SetupGeometry` writes is per frame except:

| Variable | Value |
| --- | --- |
| VS `World` | row-major 3×4 of `geometry->world`: rotation × scale, translation − `posAdjust` (`FUN_1414aaf10` then `FUN_1414df170`) |
| VS `PreviousWorld` | same from `previousWorld` (or `world` when render flag `0x10` is set), minus `previousPosAdjust` |
| PS `MaterialData` | x = property `envmapLODFade` (Envmap/MultilayerParallax/Eye only), y = `specularLODFade` (Specular only), z = property `alpha` (× fade node `+0x14c` for LOD fade passes) |
| PS `EmitColor` | `*property->emissiveColor × property->emissiveMult` |
| PS `SSRParams.w` | `specularLODFade` when Specular, times 0 if render flag `0x2` |
| PS light counts, point lights, shadow mask select | per-object light assignment; not read by the Light Limit Fix shaders |

The main (deferred) pass calls it with render flags `0x41` or `0x45`.

## Batch renderer: what the main pass will draw

The main-camera `BSShaderAccumulator` (`currentAccumulator`, batch renderer at `+0x130`) holds every pass
the main pass draws; walking it at the start of the main pass gives exactly the passes drawn this frame.

-   `BSBatchRenderer::renderPass` (`+0x08`) is an array of **`PassGroup` structs inline** (0x30 bytes:
    `passes[5]`, `validPassBits`), not the array of pointers CommonLib declares. The engine indexes it
    as `data + (list + group * 6) * 8` (`FUN_1414f39c0`).
-   Each `passes[list]` heads a list chained through `BSRenderPass::passGroupNext` (`+0x30`).
-   `renderPassMap` maps each group's technique ID to its index. The engine reads it raw: bucket count at
    `+0x2C`, buckets of `{ uint32 key, uint32 value, entry* next }` at `+0x48`, end sentinel at `+0x38`.
    An empty bucket has `next == nullptr`.
-   The group's technique ID is what `SetupTechnique` receives, and it can differ from the pass's own
    `passEnum`.
-   `RegisterPass` (AE `0x1414f2a20`, list chosen in `FUN_1414f4790`) puts a pass in list
    `(kTwoSided ? 2 : 0) | (NiAlphaProperty alpha-test bit ? 1 : 0)`, or list 4 for
    `kNoTransparencyMultiSample`. `RenderActivePassRange` draws lists 1, 3 and 4 with alpha testing.
-   A geometry group (`geometryGroups[i]`, 16 of them) has a batch renderer of its own (`+0x00`), which
    `RenderBatches` (AE `0x1414b4fe0`) uses when called with a group index.
-   `BSShaderProperty::renderPassList` holds whatever the **last** `GetRenderPasses` call built. That may
    be another camera's (reflections, cubemaps), so it is not a reliable view of the main pass.

A pass's DoAlphaTest bit is not a stable property of the object. `BSLightingShaderProperty::GetRenderPasses`
sets it for an alpha-tested property (`NiAlphaProperty` flag bit 9) when either:

-   the early-Z global (AE `0x14328cc79`) is set; or
-   the property is alpha-blended (flag bit 0); or
-   its `alpha * fade`, as computed for the camera the call is building passes for, is below 1.

The product is stored on the property. The pass list is rebuilt only when the property's state changes, and
it is shared by every camera that calls `GetRenderPasses`. The early-Z global is written only by the loading
and menu render loops (`FUN_1406d2120`, `FUN_140972590`) and the `ToggleEarlyZ` console command, so it is
stable in gameplay. Even so, the same object's registered technique gains and loses the bit from frame to
frame, most often while the camera moves, and near objects as well as distant ones. Which camera's
rebuild decides it in a given frame was not pinned down.

What is drawn does not depend on the bit:

-   `SetupAndDrawPass` (AE `0x1414f3dc0`) passes `alphaTest || earlyZ` as the draw's alpha-test flag, and
    lists 1, 3 and 4 are drawn with alpha testing.
-   The main pass tests depth EQUAL against an alpha-tested prepass.
-   In `Lighting.hlsl`, `DO_ALPHA_TEST` adds only the discard (and the screen-door path when
    `AdditionalAlphaMask` is set).

Drawcall Limit Fix therefore draws every pass in lists 1, 3 and 4 with the bit (`DrawnPassDescriptor`).

## Native main-pass render state for opaque lighting objects

Observed at `DrawIndexed` for every eligible draw in the main pass:

-   Draw: `DrawIndexed(triangleCount * 3, 0, 0)` with `rendererData->indexBuffer` (R16, offset 0) and
    `rendererData->vertexBuffer` in slot 0 (stride `vertexDesc.GetSize()`, offset 0).
-   Depth mode `kTestEqual` (the Z-prepass already wrote depth), stencil mode 0, blend mode 0, depth bias 0.
-   Cull mode 0 (none) for `kTwoSided`, 1 (back) otherwise.
-   Alpha test enabled exactly when the `NiAlphaProperty` tests, with reference `alphaThreshold / 255`.
    This is the batch list the pass is in (lists 1, 3 and 4), not the DoAlphaTest descriptor bit.

## Vertex input

When the shadow state's vertex description or vertex shader changes, the renderer looks up an input layout
by `key = currentVertexShader->vertexDesc & shadowState.vertexDesc` (AE `FUN_140e4b5b0`, shadow state
+0x340 and +0x348) in a hash map and builds a missing one with `FUN_140e4bf20`. The key uses the
`VertexDesc` encoding: the stride / 4 in bits 0-3, an offset nibble (in dwords) per attribute `a` at bits
`4a+4`, and presence flags at bits `44+a` (stream 0) and `54+a` (stream 1, dynamic geometry). The shader's
mask sets both flags and the offset nibble of every attribute it consumes (Community Shaders computes it by
reflection in `ShaderCache`). Elements, per attribute present:

| Attribute | Elements | Format |
| --- | --- | --- |
| Position (always emitted, offset 0) | POSITION0 | R32G32B32A32_FLOAT |
| Texcoord0 | TEXCOORD0 | R16G16_FLOAT |
| Texcoord1 | TEXCOORD1 | R16G16B16A16_FLOAT |
| Normal, Binormal, Color | NORMAL0, BINORMAL0, COLOR0 | R8G8B8A8_UNORM |
| Skinning | BLENDWEIGHT0 (+0), BLENDINDICES0 (+8) | R16G16B16A16_FLOAT, R8G8B8A8_UNORM |
| Land data | TEXCOORD2 (+0), TEXCOORD3 (+4) | R8G8B8A8_UNORM |
| Eye data | TEXCOORD2 | R32_FLOAT |
| Instance data | TEXCOORD4-7 (+0, +8, +16, +24), per instance | R16G16B16A16_FLOAT |

Bit 54 doubles as the geometry flag CommonLib calls `VF_FULLPREC`; the builder never emits a half-precision
position. DCLF (`VertexInput.cpp`) ports this table.

## Samplers

The state-apply function (AE `FUN_140e4b5b0`) binds each dirty pixel shader sampler slot `s` with
`PSSetSamplers(s, 1, &samplers[PSTextureAddressMode[s] * 5 + PSTextureFilterMode[s]])`, where `samplers` is a
table of `ID3D11SamplerState*` at AE `0x143288210` (4 address modes x 5 filter modes), just before the
`Renderer` singleton. DCLF copies these states' descriptions (`GpuTextures::Sampler`); the table's Address
Library ID is not known yet.

## Main-pass viewport

The main (deferred) pass draws with the viewport at the origin, the dynamic-resolution size (2560 x 1440 of a
3840 x 2160 target at 1.5x upscaling), and depth range [0, 0.999998]. Anything that has to reproduce its depth
values must use the same range.

The world-depth capture must flush the engine's pending state too, even though it runs **after** the world's
native batches. With DCLF ownership, those batches can contain no actual native draws. In AE 1.6.1170,
`Main::RenderDepth` sets the main depth target with `140e4fe00` and unbinds colour/cube targets with
`140e4fd40` / `140e4fe50`. These update `RendererShadowState` and mark state dirty; the latter calls also
invoke `Renderer::UpdateViewPort` (`140e441c0`), which calculates the dynamic-resolution viewport and marks
`DIRTY_VIEWPORT`. They do not bind it to D3D11. `SetCameraData` then establishes the world camera, but without
a native draw the context can still have the previous shadow-map viewport.

This caused a reproducible device loss when turning the camera: the Z-prepass's `CaptureBindings` read
4096 x 4096 from `RSGetViewports`, while `CaptureDepthPass` explicitly selected the 3840 x 2160 main depth
texture. Live validation stopped in `MainOpaquePass::Record` with `zPrepass=true` and this mismatched frozen
frame; Aftermath reported `Error_DMA_PageFault`. `CaptureDepthPass` now calls `SetDirtyStates(false)` before
capturing, just like `BeforeOpaquePass`. Keep the AE world-drawn hook at `Main::RenderDepth +0x1AA`: moving it
past the first-person draws loses the world camera (see below). Do not substitute full texture dimensions
for the viewport, since that discards dynamic-resolution semantics.

## Refraction: a normals pass and an image-space warp

Decompiled from AE 1.6.1170 for T7d (drawcall-limit-fix.md, "Refraction (T7d)", which has the detail).

-   `BSLightingShaderProperty::GetRenderPasses` (`0x1414adfb0`) gives a `kRefraction` / `kTempRefraction` property (flags `0x8004`) no
    Lighting pass. After its refraction LOD fade (unless `kNoFade`: `FUN_14147c470` against the Setting values at `0x142033408` /
    `0x142033420`, `fRefractionLODFadeStart/End`, result at `+0x104`; past the end, no passes), it builds one Utility pass with hint 4.
    The technique is `0x200 | (kModelSpaceNormals ? 0x2 : 0x1A) | Vc (bit 37) | Skinned (bit 1) | 0x400 with kRefractionFalloff
    (bit 16) | 0x800 with kNonProjectiveShadows (bit 13)`, plus 0x2B for the pass enum.
-   `FUN_1414b2330` puts hint 4 into geometry group 5 (hint 5 into group 6). The shadow modes' registration (`FUN_1414b2a60`, vfunc
    `0x158`) gives refraction nothing.
-   `Main::Draw` at `0x140644d20`: `FUN_1414b4840` (the main accumulator's group 5 count), then `FUN_1414ce1d0` ->
    `FUN_1414b4860(0x14338c830, world camera)`, else group 6 and 5 cleared (`FUN_1414b4ae0`). `FUN_1414b4860` sets depth mode 1 and
    write mode 2. It draws group 6 into render target 13 (`kREFRACTION_NORMALS`) only if the normals are begun (`0x1435ef130`). Then
    it binds target 13, cleared to (0.5, 0.5, 0, 0) (`0x141ad285c`) unless begun, marks begun and drawn (`0x1435ef130`,
    `0x1435ef131`), and draws group 5. `FUN_1414cb6d0` (the player's third-person block) runs it again on `0x14338c840`.
-   `BSUtilityShader::SetupMaterial` (`0x1414faa60`) binds the material's diffuse texture as t1 (`TexNormalSampler`) and writes
    `RefractionPower` from the material's `+0x84`. `SetupGeometry` (`0x1414fae40`) writes `EyePos.w` from the property's `+0x104`,
    and with `RenderNormalFalloff` `EyePos.xyz` is the camera in model space.
-   `ImageSpaceEffectRefraction` (render `0x1414f9390`, IsActive `0x1414f95a0` = `0x1435ef131`) warps the frame by the normals
    (`ISRefraction.hlsl`) and clears both flags.

## Decals: where the main pass draws them, and with what state

Decompiled from AE 1.6.1170 and then measured with `CS_DCLF_DECAL_PROBE=1`, which records the
`RendererShadowState` fields at every native Lighting draw of a decal property inside the deferred pass
and reads the D3D11 descriptions of the state objects they index.

`BSShaderAccumulator::FinishAccumulatingPreResolveDepth` for render mode 0 is `FUN_1414b2d90`: the whole
opaque G-buffer pass. In order: the Lighting range (`1..0x5c00002f`), geometry group 9, grass
(`0x5c000030..0x5c00005c`), groups 8, 1 and 0, sky, group 13, then with `alphaBlendWriteMode = 10`
**`FUN_1414b3bb0`** and with `alphaBlendWriteMode = 11` **`FUN_1414b3d50`**, then the water stencils.
Community Shaders' `Main_RenderWorld_BlendedDecals` hook wraps the second of those and calls `EndDeferred`
after it, so both decal groups draw **inside the deferred pass, into the G-buffer**.

`BSLightingShaderProperty::GetRenderPasses` gives a Lighting pass of a `kDecal | kDynamicDecal` property
accumulation hint `2 + (alpha < 1 || NiAlphaProperty blending)`. **A hint `h` lands in
`geometryGroups[h + 1]`** (measured: hint 2 draws with the first function's state, hint 3 with the
second's). Hint 4 passes are Utility-shader passes, not Lighting.

| hint | function | groups | depth mode | `rasterStateDepthBiasMode` | write mode | render flags |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | `FUN_1414b3bb0` | 3 then 2 | 3 (test and write) | `6 + b` while the `ToggleDepthBias` byte at `0x142032ff6` is set, else 0 (group 3); `8 + b` (group 2) | 10 | 0x41 |
| 3 | `FUN_1414b3d50` | 4 | 1 (test only) | `10 + b`, unconditionally | 11 | 0x45 |

`b` is `DrawWorld::disableSunShadows` (`+0x51`), so interiors use modes 7 and 11. The rasterizer states:
modes 6 and 10 hold `DepthBias -1, DepthBiasClamp -100, SlopeScaledDepthBias -0.65`; modes 7 and 11 hold
no bias at all.

Render flag `0x4` is what applies the geometry's `NiAlphaProperty` (`FUN_1414f5cf0` calls
`FUN_14150bc80(shader, alphaProperty, shaderProperty, alphaTest)` before `SetupGeometry`), so it applies
to hint 3 and not to hint 2. `FUN_14150bc80` sets `alphaBlendMode` from the blend functions when the
property blends: 1 for `SrcAlpha / InvSrcAlpha`, 2 for `SrcAlpha / One`, `One / One` and
`SrcAlpha / InvDestAlpha`, 3 for `DestColor / InvSrcAlpha`, 4 for `Zero / SrcColor` and
`DestColor / Zero`; any other pair leaves the mode as it was. With no blending and `shaderProperty->alpha`
below 1 it picks mode 1 (a fading object). It sets `alphaTestEnabled` from the property's test flag.

`BSLightingShader::SetupGeometry` (`Func6`) treats a decal pass specially in one place: for hint 3 with
`kZBufferWrite` it saves the write mode to `0x142035488` and sets **1**; `RestoreGeometry`
(`1414de3d0`, unnamed in Ghidra) puts the saved value back unless the slot holds its sentinel 13. For a
non-decal pass SetupGeometry is what drops the depth mode to 1 without `kZBufferWrite` and to 0 without
`kZBufferTest`; decal passes keep the group function's depth mode.

The blend states (indices `[alphaBlendMode][alphaToCoverage][writeMode][extra]`, the table at
`RelocationID(524749, 411364)`), as Community Shaders' deferred override leaves them: `[0][0][10][0]` is
blending off on every target with mask F; `[1][0][1][0]` is `SrcAlpha / InvSrcAlpha` on every target with
mask 7 on RT0 and RT3-7 and F on RT1-2; `[1][0][11][0]` is the same blend with mask F everywhere. The
only difference between write modes 1 and 11 is therefore RT0's alpha channel, which the deferred
composite never reads (it writes 1.0 into it).

Measured, Whiterun exterior, per frame: 137.5 hint-2 draws alpha-tested without `kZBufferWrite` plus a
few with; 40.7 hint-3 draws blended and alpha-tested with `kZBufferWrite`, 11.8 blended only without it,
5.9 blended only with it. Dragonsreach: 17 hint 2, 13 hint 3. About 0.5% of hint-3 draws without
`kZBufferWrite` were issued with write mode 1 rather than 11: Community Shaders' Terrain Blending sets
`alphaBlendWriteMode = 1` for its own passes from the same hook, just before the blended decals, and the
first decals after it inherit that until a `kZBufferWrite` decal's save and restore resets it.

**Which decals write depth, and where.** Only the opaque group (hint 2), in the main pass: its function draws with
depth mode 3 whatever the property's `kZBufferWrite` says, and with its bias mode. The blended group (hint 3) tests
only, `kZBufferWrite` or not (the flag changes the blend write mode, not the depth mode). The depth pass writes no decal
depth at all: its registration (`FUN_1414b2a60`, render mode `0xC`, from the table at `0x14332b020`; the draw function
`FUN_1414b44f0` is in the one at `0x14332b120`) drops decal properties except in two special cases, and the few hint-3
`kZBufferWrite` passes that do reach it (Lighting technique `0x220CA`, 4 to 8 a frame at Riverwood) are offered to
`RenderPassImmediately` but never reach `BSLightingShader::SetupGeometry`: a census of the native lighting draws (since
removed) counted no Lighting draw in the depth pass at all, with DCLF on or off.

**Effect-shader decals.** `BSEffectShaderProperty::GetRenderPasses` (`0x1414bd890`) gives a `kDecal | kDynamicDecal` property's pass hint
2 without a blending alpha property and 3 with one (nothing else counts; `kTempRefraction` with `kDynamicDecal` makes no pass), and
`FUN_1414b2330` puts them in groups 3 and 4 like any decal pass, so the same two functions draw them, in the deferred pass, with Community
Shaders' `Deferred` effect permutation. Within a group the buckets go by pass enum, so effect decals (`0x4000002C +` descriptor) precede
Lighting ones (`0x4800002D +`). `BSEffectShader::SetupGeometry` (`0x1414eab10`) keeps the group's depth mode for a decal pass and sets
`alphaBlendWriteMode` 11 unless the technique has MotionVectorsNormals (bit 26); it calls `FUN_14150bc80` itself only for the sky object
(bit 45). Render flag 4 (group 4 only) applies the alpha property through `FUN_1414f5cf0`, which skips it for pass enums
`0x5C000058..0x5C00005B` (`FUN_1414b4fc0`). In `FUN_14150bc80` a property alpha below one without blending picks mode 1; its last
argument (the alpha-test list) sets `alphaTestEnabled` (`+0xB4`) from the alpha property's test flag.

## Skinning: the palette the engine keeps, and the buffers a skinned draw uses

Decompiled from AE 1.6.1170 for Drawcall Limit Fix's skinned coverage, and measured with
`CS_DCLF_SKIN_PROBE=1`.

**The draw.** `SetupAndDrawPass` (`FUN_1414f3dc0`) takes a different branch when `geometry->skinInstance`
(`RUNTIME_DATA+0x10`) is set: after the shader's `SetupGeometry` it calls the skin instance's vtable slot
`0x25` (`NiSkinInstance::Func37`, `140d451f0`), which walks `skinPartition->numPartitions` and calls
`NiSkinPartition::Unk_25(args, i)` (`140d43a10`) for each. `BSDismemberSkinInstance::Unk_25` (`140d31f40`)
does the same but skips the partitions whose dismember flag is clear.

**Per partition** (`140d43a10`): a partition whose LOD byte (`Partition+0x42`, CommonLib's `pad42`) is not
enabled in the table at `0x14202a030` for the pass's LOD mode is skipped. Otherwise it calls the shader's
bone setter `NiBoneMatrixSetterI::Func1(skinInstance, &partition, &geometry->world)` and draws
**`partition->buffData`**, the partition's own `BSGraphics::TriShape`, for `partition->triangles`
triangles (renderer vtable `+0x38`, or the instanced `+0x30`). `geometry->rendererData` is a different
TriShape and is not what a skinned shape draws (measured: never equal).

**The setter** (`14150be30`): once per (skin instance, thread) it runs the palette update below, then
copies `skinData->bones * 3` float4 rows from `NiSkinInstance::boneMatrices` (`+0x48`) into a dynamic
constant buffer bound at **VS b10** (`Bones`) and `prevBoneMatrices` (`+0x50`) into **VS b9**
(`PreviousBones`). It is the whole skin's palette, whichever partition is being drawn.

**The update** (`FUN_140e4ff90(skinInstance, const NiTransform* world)`): under the instance's critical
section (`+0x60`), only when `frameID` (`+0x38`) differs from `gFrameCounter`: copies the current palette
to the previous one, (re)allocates `bones * 48` bytes when needed, sets `numMatrices = bones` and
`numRegisters = 3`, builds skin-to-world from `rootParent->world` (`+0x20`), `skinData->rootParentToSkin`
and the world passed in, and writes for every bone with a `boneWorldTransforms[i]`:
`boneWorld * skinToBone`, as **three float4 rows** - row-major 3x4, absolute world space, scale folded in,
translation in each row's `.w`. Idempotent within a frame, so whoever calls it first does the work; a
draw that is withheld from the batch renderer means nobody does, which is why DCLF calls it itself.

**The shader** (`Common/Skinned.hlsli`, `Lighting.hlsl` SKINNED): `actualIndices = 765.01 * BoneIndices`
(the UNORM byte times 255 times 3 is the row index), `GetBoneTransformMatrix` sums four `float3x4` rows
minus a pivot, and the pivot is `VS_PerFrame` c40 (`BonesPivot`) / c41 (`PreviousBonesPivot`) - measured
equal to `posAdjust` / `previousPosAdjust` on every one of 62,000 draws. The skinned position never reads
`World`; `SetupGeometry` does not write `World` / `PreviousWorld` for a skinned pass, so the buffer holds
whatever the previous draw left, and `GetBoneRSMatrix` builds the normal basis from the same rows.

**What the main pass draws, per frame.** Whiterun exterior: ~200 skinned Lighting draws, all tracked;
the largest class is one-partition `NiSkinInstance` shapes with 4 bones (47, technique 0), then
three-partition shapes with LOD bytes 1/2/0 and technique 12 (29, hint 11: tree-animated, one partition
per LOD level), then actor parts (`BSDismemberSkinInstance`, one or two draws each). Dragonsreach: ~130,
dominated by one-partition books (`Book01a`, 3 bones, hint 15) and banners (2 bones). 191 of 201 palettes
in the exterior and 288 of 328 in Dragonsreach changed between consecutive frames, so a static-pose skip
would buy little.

### Skin partitions: which ones a draw draws

Decompiled from AE 1.6.1170 for Drawcall Limit Fix's tree and actor coverage.

-   **The test.** `NiSkinPartition::Unk_25` (`140d43a10`) draws partition *i* when the byte at `0x14202a030`
    indexed by `((LODMode.index + LODMode.singleLevel * 4) * 3 + Partition+0x42)` is non-zero.
    -   The table is 24 constant bytes (`00 00 00 01 00 00 01 01 00 01 01 01 01 00 00 00 01 00 00 00 01 00 00 00`),
        read only by this function and `FUN_140d43b00`.
    -   Cumulative level *n* draws the LOD bytes below *n*. Single-level *n* draws LOD byte *n* alone.
-   **Dismember skins.** `BSDismemberSkinInstance::Unk_25` (`140d31f40`) first skips partition *i* when byte
    0 of its 4-byte `Data` entry (`editorVisible`) is clear. With no `Data` array it falls back to the
    plain loop.
-   **The level.** `GetRenderPasses` (`1414adfb0`) and `GetRenderPasses_ShadowMapOrMask` (`1414af030`) give a
    pass `LODMode` 3 (cumulative), or for geometry with `kMeshLOD` (`NiAVObject` flag bit 27) the fade node's
    `+0x152 & 0xF`. `GetRenderPasses` skips this for an accumulator with `+0xB9` set.
-   **The cross-fade.** While a `kMeshLOD` geometry's fade node has `(+0x153 & 0x70) != 0x20`,
    `GetRenderPasses` appends a copy of each lighting pass with accumulation hint 10 and
    `LODMode = (+0x152 & 0xF) | singleLevel`.
-   **Buffers.** A tree's partitions share one vertex buffer and have separate index buffers (measured).
-   **How the cross-fade copy is drawn.** It is not a plain second draw. `BSLightingShader::SetupGeometry`
    (`1414dd040`, ID 107300) makes three differences:
    -   **Material alpha.** For a pass with `LODMode & 0x80` (the single-level copy), the PS constant at
        `MaterialData.z` is `property.alpha * fadeNode[+0x14C]`, not `property.alpha`.
    -   **Stencil.** For accumulation hint 10 it sets `depthStencilStencilMode = 0xB` and
        `stencilRef = int(f * 31.0)` (`0x1419de7e0`). `f` is `fadeNode[+0x14C]` for the copy and
        `fadeNode[+0x130]` for a hint-10 pass without the single-level bit (the other hint-10 case in
        `GetRenderPasses`, a fade).
    -   **Order.** Hint 10 lands in the batch renderer's geometry group 10, which `1414b3390` draws just
        before the main opaque range.
    -   So the cross-fade is a stencil dither with 32 levels, against whatever the stencil buffer holds
        there.
    -   **The base pass is unchanged.** Hints 0, 11 and 15, cumulative `LODMode` of the new level, get
        neither change: it draws exactly like a settled object.
    -   **State layout.** The renderer state is at `0x14202ab70`: `+0x88` depth mode, `+0x90` stencil mode,
        `+0x94` stencil ref, `+0xA8` blend mode, `+0xB0` write mode (CommonLib's `RendererShadowState`).

### Switch nodes

`NiSwitchNode::OnVisible` (`140d29700`) culls `children[index]` alone, and nothing when `index < 0`. When
`childRevID[index] != revID` it first runs the child's `UpdateDownwardPass`, from the cull. `UpdateDownwardPass`
(`140d29240`) with flag bit 0 set bumps `revID` and updates only the selected child; with it clear it
updates every child like an `NiNode`.

Offsets on SE and AE:

| Offset | Field |
| --- | --- |
| `+0x128` | flags (u16) |
| `+0x12C` | index (i32) |
| `+0x130` | savedTime |
| `+0x134` | revID |
| `+0x138` | childRevID, an `NiTPrimitiveArray`: data at `+0x140`, capacity at `+0x148` |

`OnVisible`'s catch-up: `childRevID.SetAt(index, &revID)` (`FUN_140d29990`: array, index, pointer to the value), then
the child's `UpdateDownwardPass` (vtable slot `0x2C`) with an `NiUpdateData` of `{ savedTime, (flags >> 1) & 1 }` and
0.

**Who writes the index** (every `mov [reg+0x12C]` in `.text`, filtered by function):

| Writer | What | After it |
| --- | --- | --- |
| `FUN_140437e50` (`BSTreeManager`'s update, from `FUN_140437d40` in `Main::Update`), five stores; `FUN_140438840`, two | a tree's LOD switch (`BSTreeNode` `+0x180`): `(size - 1 != 0 && !*0x14200d640) ? 1 : 0`, then `size - 1` when `*0x142032dfc` is set and the tree is beyond the manager's distance (`+0x80`), so two stores in one pass | nothing: the cull's catch-up |
| `FUN_140438580(manager, level)`, from the local map (`FUN_140242e00`) | every tree's LOD switch to a level | nothing |
| `FUN_1401e8ef0` (harvest: `TESObjectTREE::Activate` and the flora's), `FUN_1401e9450` (a harvestable's 3D setup, from `TESObjectREFR::LoadGame`, `Revert`, `Explosion::Load3D` and vtables) | the root's first child when it is an `NiSwitchNode` with two children: the reference's harvested flag (form flag bit 13) | the switch's `UpdateDownwardPass` (vtable `+0x160`), then `FUN_1414a28b0` |
| `NiSwitchNode` constructor (`FUN_140d28c30`, -1), `CreateClone`, `LoadBinary`'s copy (`FUN_140d28950`) | before the node is in a scene | |
| `DetachChild1`, `DetachChildAt1` | -1 when the selected slot is left empty | |

`AttachChild` (`140d28ce0`), `DetachChild*` and `SetAt*` also reset `revID` to 1 and zero the edited slot's
`childRevID`, so the selected child can be out of date with no store to the index. Every other `+0x12C` store in the
scan belongs to another class (pathing requests, Havok shapes, `BSValueNode`, `BSFadeNode`'s clone, and so on).

CommonLib-NG declares these fields after `NiNode`, whose declared size in a multi-runtime build is VR's.
Reading them as members therefore reads the wrong memory.

### BSLightingShader::SetupMaterial: where every material constant comes from

Vanilla `BSLightingShader::SetupMaterial` (vfunc 4, `1414dc310`, AE). `this` is the shader, the argument is
the `BSLightingShaderMaterialBase`, and the technique is `this+0x94`. It writes the PerMaterial groups: PS
variable *i* through the register table at `0x14202aec0 + 0x40 + i`, VS variable *i* through
`0x14202aeb8 + 0x50 + i`.

CS replaces the function for TruePBR materials (`TruePBR::BSLightingShader_SetupMaterial`). Advanced Skin
and TerrainHelper hook it too, but only bind extra textures outside the engine's slots (t71-t74).

Sources:

-   **M:** the material's own fields.
-   **S:** the shader object, per frame.
-   **G:** engine globals, per frame.
-   **R:** a render target, per frame.

| Output | Written when | Source |
| --- | --- | --- |
| t0 diffuse | always (not for MTLand / LODLand) | M `+0x48`; or R `renderTargets[M +0x50]` when `+0x50 != -1` |
| t1 normal | always (same) | M `+0x58` |
| t2 specular | flag `0x4` with Specular | M `+0x68` |
| t9 | flag `0x1000` | M `+0x68` |
| t12 | flag `0x400` or `0x800` | M `+0x60` |
| t11 character light | flag `0x400000` (CharacterLight), and only while the target index `0x142033db0` is not negative (it is -1 while a cell loads) | R `renderTargets[FUN_1414e8b30(0x142033da8)]` |
| per-technique textures | technique 1-4, 7, 9/0x12, 0xb, 0x10 | M `+0xa0`, `+0xa8`, `+0xb0`, ... |
| texture address modes | with each texture | M `+0x70` |
| VS 11 TexcoordOffset | always | M `+0xc/+0x10` offset and `+0x1c/+0x20` scale, pair `[G 0x142033180]` (the texture-transform buffer the frame reads) |
| VS 9/10 Left/RightEyeCenter | technique 0x10 (eye) | M `+0xb4..+0xc8` |
| PS 6 | flag `0x20000` | G `0x14203315c..` |
| PS 21 EnvmapData | technique 1, 0xb | M `+0xb0` / `+0xc8`, and whether the mask texture exists |
| PS 22 ParallaxOccData | technique 7 only | M `+0xa8`, `+0xac` |
| PS 23 TintColor | technique 5, 6 | M `+0xa0..+0xa8` |
| PS 24 LODTexParams | technique 8/0x13, 9/0x12 | M `+0x148..+0x150` or `+0xb8..+0xc0`; `.z` from G `0x142032fda` |
| PS 25 SpecularColor | flag `0x200` | M `+0x38..+0x40` times `+0x8c`; `.w` M `+0x88` |
| PS 26 SparkleParams | technique 0xe | M `+0xa0..+0xac` |
| PS 27 MultiLayerParallaxData | technique 0xb | M `+0xb8..+0xc4` |
| PS 28 LightingEffectParams | flag `0x400` or `0x800` | M `+0x90`, `+0x94` |
| PS 29 IBLParams | always | S `+0xcc`, then `+0xd0..` or `+0xe0..` by S `+0xf0` |
| PS 30-33 landscape snow and spec-power | MTLand, flag `0x200000` | M `+0x118..+0x144`; `.z` of 31 from G `0x142035548`, `.w` 1/G `0x1420355f0` |
| PS 34 SnowRimLightParameters | flag `0x200000` | G `0x142035590`, `0x1420355a8`, `0x1420355c0`, `0x1420355d8` |
| PS 35 CharacterLightParams | flag `0x400000` | G `0x14203316c..`, zero unless full-bright mode |

-   **Unlisted variables:** anything not in the table is not written by `SetupMaterial`. The buffer keeps
    whatever the previous draw left there.
-   **Where DCLF saw staleness:** every value DCLF found stale in its material records (TexcoordOffset, t11,
    IBLParams) is an **M** field written after the record was taken, or an **S/G/R** source that changes
    per frame. No other kind of source exists in this function.
-   **SSE Engine Fixes moves PS 6.** Its `BSLightingAmbientSpecular` fix (EngineFixesSkyrim64,
    `src/fixes/bslightingambientspecular.h`; `bBSLightingAmbientSpecular` in its toml, on by default):
    -   NOPs 0x20 bytes of `SetupMaterial` at ID 107298 `+0x8CF`, its AmbientSpecular write;
    -   puts a `jmp5` in `SetupGeometry` (ID 107300, `0x1414dd040`) over its Specular test,
        `test dword [r13+0x94], 0x200`, into code that, for an AmbientSpecular pass, copies the 16 bytes at
        `0x14203315c` into PerGeometry PS 6. In 1.6.1170 that test is at `+0x1271` (`0x1414de2b1`); the
        current source says `+0x1293`, and the installed build patched neither there nor the source's
        guess, so the offset varies by build: DCLF finds the patch by the instruction (the jmp5 followed by
        the test's last six bytes).
    -   With True PBR's SetupGeometry hook (which adds AmbientSpecular to every TruePbr pass), every
        PBR pass's PerGeometry block then carries PS 6 (exteriors measured `(0, 0, 0, 1)`).
    -   DCLF's material port detects the NOPs at ID 107298 `+0x8CF` (measured present in 1.6.1170).
    -   `Lighting.hlsl` declares `AmbientSpecularTintAndFresnelPower` without reading it.
### Material writers, and the texture-transform buffers

-   **Two texture-transform buffers.** A material keeps two sets of UV offset and scale
    (`+0x0c/+0x10` and `+0x1c/+0x20`, 8 bytes apart per buffer).
    -   Every `SetupMaterial` reads the set at `0x142033180` (`BSShaderManager::State::
        textureTransformCurrentBuffer`): Lighting, Effect and Utility alike.
    -   `Main::Update` flips it once a frame (`1406460c7`: `xor [0x142033180], 1`); two menu functions
        flip it too.
-   **`BSLightingShaderPropertyFloatController::Update`** (`14150dde0`, vtable `0x141ac3be8` slot 0x27)
    returns early when its value is unchanged. Otherwise it writes, by its type (`+0x50`):
    -   type 0xb: the property's `+0xf8` (the emissive multiplier);
    -   types above 0x13: the texture-transform buffer `current ^ [0x142033184]`. That offset is 1 in play,
        so the write lands in the buffer the next frame reads; `Inventory3DManager::Render` and `StatsMenu`
        set it to 0;
    -   anything else: the material (`property+0x78`), at an offset from a table filled at startup
        (`0x1435ef210`).
-   **`BSLightingShaderPropertyColorController::Update`** (`14150ea50`, slot 0x27): type 1 writes the
    property's emissive colour (`+0xf0`); any other type a material colour (table `0x1435ef298`).
-   **`BSLightingShaderPropertyUShortController::Update`** (`14150e580`) is `ret 0`.
-   **In-place rewrites.** `BSShaderMaterial::CopyMembers` (02), and `BSLightingShaderMaterialBase::
    OnLoadTextureSet` (08), `ClearTextures` (09) and `ReceiveValuesFromRootMaterial` (0A). Per class,
    vtables `0x141ab6eb8` (Base) to `0x141ab7458` (MultiLayerParallax), and `0x14185dc08`
    (BSLightingShaderMaterial).


## The depth pass and first person

`Main::RenderDepth` (AE `1414ccb30`, ID 107139; SE ID 100421) is called from `Main::Draw` (AE `+0x395`)
as `RenderDepth(firstPerson, …)`. The flag is set when the player's first-person 3D is shown. In order:

1.  **The world.** `FUN_1414a9190(worldCamera, accumulator, 0x21)` renders the scene: it calls
    `BSGraphics::State::SetCameraData` for the world camera and runs the accumulator. `FUN_1414b47e0` and
    `FUN_1414b5120` (called at `+0x1AA`) then clear the accumulator's lists.
2.  **Rooms.** With portals in play, a loop draws each room again with stencil reference `room + 1`.
3.  **The first-person model**, when the flag is set:
    -   it reads the viewport depth range, sets it to `[0, 0.1]` (or another bound for the second argument)
        and later restores it;
    -   the model is drawn with the first-person camera (`0x14338c820`), and the eye (`posAdjust`) moves to
        the player's head;
    -   **it does not restore the camera.** `Main::Draw` sets the world camera again only after `RenderDepth`
        returns (`SetCameraData(p0, 1)`, a few calls later). Terrain Blending sets it before calling
        `RenderDepth` for the same reason.
4.  **The copy.** `CopyResource(kPOST_ZPREPASS_COPY, kMAIN depth)` on the immediate context (vtable slot 47).

At the call site's return in first person, then, the eye is the first-person camera's (DCLF measured
`(0, 0, 120.5)` against the world's `(17120, -47226, 9.7)`). The VS_PerFrame projection is also its own, with
a near plane of 5 against 15, and the viewport depth range is still `[0, 0.1]`.

## The first-person switch

Reverse engineered 2026-10-11 (AE 1.6.1170) for DCLF's third-person body drawn in first person.

-   **`FUN_140736150(player, firstPerson)`**: the switch between the two skeletons. Through the task queue when
    `TaskQueueInterface::ShouldUseTaskQueue` (then `BSTaskPool_HandleTask` calls it again). Unless the requested skeleton is
    already the one shown (or `vfunc 0x4C8` refuses first person), it **stores `kHidden` on both roots**: the third-person root
    (`Get3D1(false)`) at `0x140736237` (`mov [r14 + 0xF4], ecx`), the first-person root (`Get3D1(true)`) at `0x140736271`
    (`mov [r15 + 0xF4], ecx`), one hidden and the other shown. It also sets bit 0 of `player +0xBE3`, copies the player's position
    and rotation into both roots, moves the audio listener, resets the magic casters' nodes, and refreshes the 3D (`FUN_1402df9a0`
    on the root leaving, `FUN_1402df8a0` on the one shown).
-   **Callers:** `FirstPersonState::Begin` (`0x1408deba0`, with 1), `AutoVanityState::Begin`, `BleedoutCameraState::Update`,
    `DragonCameraState::Update`, `RaceSexMenu::ProcessMessage`, `PlayerCharacter::LoadGame`/`FinishLoadGame`/`Revert`, the
    furniture camera (`FUN_1408e38b0`, with 0), calls at `0x1408e9d19` and `0x1408e1a29` (no function defined in the database)
    and `FUN_140648cd0`. `ThirdPersonState::Begin` does not call it. Which caller shows the third-person skeleton on the switch to
    third person was not traced: for DCLF it does not matter, since every caller's stores are the same two sites.
-   **Nothing else hides the third-person body in first person.** The main camera's cull skips it because its root is hidden. No
    cull or scene list build tests "first person". `Main::Draw` touches only the first-person root: hidden before the cull jobs
    (`0x1406445a1`), shown for `FUN_1414a2170` (the receivers), hidden again, and in first person shown at the end to draw it.
-   **`TESWaterReflections::Update`** hides `Get3D()` (vfunc `0x380`) around the cube map's faces and shows it again only when
    `vfunc 0x388` (the third person shown) is true. The water shapes are restored to their saved state.

## The first-person view

`Main::Draw` (`0x1406444b0`) draws first person when `cVar23` holds: the first-person root's `kHidden` clear at entry, the third
argument false, and `PlayerCamera +0x28` (the current state) not `+0xD0` (the free camera, `tfc`).

-   **The cull:** `Main::unk_FPCulling` (`0x140645d30`) after `CalculateAndDrawShadowCasterLights`. It shows the first-person root,
    culls it alone (cull context `FUN_1414bf320(ctx, 0, 1)`, process `0x14338c640`, camera `0x14338c820`) into the first-person
    accumulator `0x14338c840`, and hides it again.
-   **The view's inputs**, a struct at `0x14338c8c0` (`FUN_1414cafd0`) filled by `FUN_1406452c0` (Main::Draw's first call):
    `+0x00` the camera position (`PlayerCamera::GetFirstPersonNodePosition`), `+0x0C` `PlayerCamera::firstPersonFOV` (`+0x140`),
    `+0x10` `worldFOV`, `+0x18` the first-person root, `+0x20` the third-person root, `+0x28` the world root.
-   **The camera** (`FUN_1414cb020`, before the depth and colour draws). `FUN_140d2cda0(camera, fov, near, far, width, height, ...)`:
    -   the FOV is `firstPersonFOV + 0x14338c628`, where `FirstPersonState::Update` writes the skeleton's `firstPersonFOVControl`
        node's local z (an animated offset);
    -   the near plane is the float at `0x14202a928` (5 measured);
    -   the far plane is the world scene graph's vfunc `0x1F0` (unverified: the world camera's far);
    -   the aspect is the screen's (`0x14328cc44`/`48`);
    -   the rotation is the world camera's world rotation.
-   **The shifted space.** The first-person draws are made around the first-person root. `FUN_1414ce9d0` adds minus the root's
    world translation to the world (and previous world) translation of every node under it. The camera is placed at the camera
    position minus the root's position (the `(0, 0, 120.5)` DCLF measured), and `BSShaderManager::State +0x2E8/+0x2F4` (posAdjust,
    the eye) follow. `FUN_1414cb270` undoes it after the colour draw. In world terms the view is the world camera's rotation at
    `GetFirstPersonNodePosition`, with its own FOV and near plane.
-   **Depth:** at the end of `RenderDepth` (`FUN_1414ccb30`; "The depth pass and first person" above), with the viewport depth range
    `[0, 0.1]`.
-   **Colour:** `FUN_1414cb6d0` (`Main::RenderFirstPersonView`, called at `Main::Draw +0x971`, after the main pass and its
    composite). It runs `FUN_1414cb640` first, which shows the root and runs the process with the accumulator (`FUN_140e28af0`;
    the "mask clear" DCLF's light code names). Then it sets a depth range (`0x14202b2d0`, or `0x14202b2e8` with the second
    argument), binds the targets (slot 2 cleared, depth target 7), and renders the accumulator with the first-person camera
    (`FUN_1414a90f0(camera, accumulator, firstPerson << 4 | 0x43)`), then its blended passes (`FUN_1414b4860`). Community
    Shaders' Deferred hooks the call to set `InWorld`: these are forward Lighting draws after the deferred composite.

Reverse engineered further 2026-10-11 for T7e-fp0 (drawcall-limit-fix.md, "First person as a view bit (T7e)"):

-   **Main::Draw's third argument** is `UI::IsMenuOpen(InterfaceStrings +0xA8)`, the main menu (its only caller, `FUN_140644360`).
    `cVar23` is taken at Main::Draw's entry; DCLF's BeginSceneFrame (the hook on `+0xD3`, `FUN_1406452c0`) runs before `0x1406445a1`
    hides the root, so it can take the same inputs.
-   **The far plane**: `(*gWorldSceneGraph)->vfunc 0x1F0` is `SceneGraph::GetFarDistance` (`0x1F8` `GetNearDistance`). The near plane
    `0x14202a928` is `fNear1stPersonDistance:Display`. `FUN_140d2cda0` builds the frustum from the FOV in degrees (times
    `0x1417a4b30`), the near and far planes and the screen size, and updates the camera.
-   **The first-person root has no parent.** `PlayerCharacter::Load3D` (`0x1407349b0`) demands the 1st-person model into
    `firstPerson3D` (`+0x8F8`, `Get3D1(true)`), hands it to `PlayerCamera::SetFirstPersonSkeleton` and the biped, and attaches it
    nowhere. Its equipment attaches under it through the ordinary NiNode child functions.
-   **The shift** (`FUN_1414ce9d0(node, delta, previousDelta)`): adds `delta` to the world translation (`+0xA0`) and `previousDelta` to
    the previous world translation (`+0xD4`) of the node and every node under it, and on a geometry sets `kAlwaysDraw` (`0x800`) and its
    fade (`+0xF0`) to 1. `FUN_1414cb020` (Main::Draw `+0x26A`) shifts by minus the root's world translation (and the previous frame's
    delta), places the camera at the camera position plus that delta, and sets `posAdjust` (`State +0x2E8` the delta, `+0x2F4` the
    camera); `FUN_1414cb270` (`+0x9C2`, after FUN_1414cb6d0) undoes it. `FUN_140742470(player, -position)` around them moves the player's
    magic nodes and light likewise.
-   **RenderDepth's first-person block** undoes the shift, builds a culling process on the stack with the first-person camera in world
    space and culls the root (`FUN_1414bf320`) into the **Z-prepass's** accumulator `0x14338c828`, shifts again and draws that
    accumulator with the first-person camera (`FUN_1414a90f0(camera, 0x14338c828, 3)` at `0x1414cd47e`), viewport depth `[0, range]`
    (`FUN_140e57cb0`), then hides the root and restores the depth range before `kPOST_ZPREPASS_COPY`. `0x14338c840` is the first-person
    colour accumulator (unk_FPCulling's); `0x14338c820` its camera.
-   **FUN_1414cb6d0's targets**: Main::Draw binds slot 0 = `kMAIN` (1), slot 1 = `kMOTION_VECTOR` (7), slot 2 = target 4, and the
    function unbinds slot 2, binds depth target 7 (`kPOST_ZPREPASS_COPY`), sets write mode 12 (`State +0xB0`) and the depth range
    `[0, 0x14202b2d0]` (`f1stPersonFarDepthRange`; `0x14202b2e8`, `f1stPersonFarDepthRangeControlDriven`, with its second argument: the
    player's movement controller's vfunc `0x78`, which RenderDepth gets too), and renders its accumulator at `0x1414cb7d1`.
-   **The skin palette update** (`FUN_140e4ff90(skin, geometryWorld)`) runs once a frame per skin (`+0x38` against `gFrameCounter`, under
    the skin's critical section): the first caller of a frame fixes the palette (bone world times skin-to-bone, through the root parent's
    inverse) for every later draw of that frame, and the first-person skeleton's bones are shifted between `FUN_1414cb020` and
    `FUN_1414cb270`.

## ProjectedUV and MTLand: the per-object constants and where they come from

Decompiled from AE 1.6.1170 for Drawcall Limit Fix's coverage of the two techniques; both are
`BSLightingShader::SetupGeometry` (`Func6`, `1414dd040`) work, and one helper.

**MTLand and MTLandLODBlend (techniques 8 and 19): `LandBlendParams`** (VS PerGeometry variable 3).
`xy` are `BSLightingShaderMaterialLandscape::landBlendParams.rg` (the property's material, `+0x108`).
`zw` are a position blended between two `BSShaderManager::State` points minus the geometry's world
translation:

    t  = clamp((State+0x20 - State+0xb8) * (float at 0x141ad2840 / State+0x98), 0, 1)
    z  = lerp(State+0xa8, State+0xb0, t) - world.translate.x
    w  = lerp(State+0xac, State+0xb4, t) - world.translate.y

with `State` at `0x142033060`. When `t` reaches 1 the function also clears the byte at `0x14332a394`.
Nothing else in the case is per object; the six landscape texture sets come from `SetupMaterial`.

**ProjectedUV (descriptor bit 15, any technique but Hair): `TextureProj`** (VS PerGeometry variable 6,
three rows). The projection is a `NiTransform` with a fixed rotation about Z
(`[[0,1,0],[-1,0,0],[0,0,1]]`, from the constant at `0x141abe6a0`), translation `posAdjust`, scale 1,
converted by `FUN_1414aaf10` (NiTransform to row-major 4x4, translation minus `posAdjust`, so it comes
out at zero). For technique 1 (Envmap) the matrix is that projection alone; for everything else the
geometry's world transform is converted the same way, `posAdjust` is added back to its translation (so it
is the absolute world matrix, with one rounding trip), and `D3DXMatrixMultiply(world, projection)`
(`14153d3c8`) gives `M`. `TextureProj` row `r` is **column** `r` of `M`: `(M[0][r], M[1][r], M[2][r],
M[3][r])`.

The same block binds four pixel textures with address mode 3 and filter mode 1: slot 11 from
`BSShaderManager::State+0x48 -> +0x10` and slots 3, 8 and 10 from the three `NiTexture` globals at
`0x14328cc28/30/38` (through `rendererTexture` `+0x48` then `+0x10`), the ones the
`ReloadProjectedUVTextures` console command replaces. DCLF captures the four views from a native
projected draw rather than dereferencing the globals.

**The pixel parameters** (PS PerGeometry variables 12-14) are written by `FUN_1414e00c0(shader,
dynamicData, property, projectedNormals)` at the end of that block, for a static shape from the
**property's** fields:

| variable | value |
| --- | --- |
| 12 `ProjectedUVParams` | `x = (1 - p.a) * p.r`, `z = p.b`, `w = (1 - p.a) * p.g + p.a` from `projectedUVParams` (`+0x10C`); **y is never written** |
| 13 `ProjectedUVParams2` | `projectedUVColor` (`+0x11C`), all four |
| 14 `ProjectedUVParams3` | `(float at 0x142035560, float at 0x142035578, 0, projectedNormals ? 1 : 0)` |

`projectedNormals` is the byte at `0x142035518`, unless render flag `0x8` is set together with the byte
at `0x142035530` (the main pass runs with `0x41` / `0x45`, so it is the first byte alone).

**The tree amplitude's square root, on garbage.** `SetupGeometry`'s TreeAnim case takes the node's
squared distance (`+0x158`) through `0x5f3759df - (bits >> 1)` with an **arithmetic** shift of the bit
pattern. A fern-type node can hold an uninitialised negative value there; the engine's estimate then
overflows in the direction that clamps the amplitude to its maximum, and a logical shift does not. DCLF
reproduces the signed shift.

## Shadow maps: views, descriptors and the Utility passes

Measured with a shadow probe DCLF has since removed (`CS_DCLF_SHADOW_PROBE`) on AE 1.6.1170, Whiterun
exterior by day and the Bannered Mare at hour 22, on top of the Ghidra reading of the functions below.

**Frame order.** `Main::Draw` (`0x1406444b0`) builds the main scene lists and finishes them; then
`NiCamera::CalculateAndDrawShadowCasterLights` (`0x1414cbb90`) queues each shadow light's list
accumulation and, inside `CalculateActiveShadowCasterLights` (`0x1414cc570`), walks
`ShadowSceneNode::GetShadowCasterLightArrayEntry` (`0x1414a4010`, an index into `shadowLightsAccum` at
`+0x230`) calling `UpdateCamera` (vfunc 0x10) and `Accumulate` (vfunc 0x9, one `FUN_1414f0920` cull per
descriptor). `FUN_1414cbff0`, the callee of Community Shaders' `Main_RenderShadowMaps` thunk
(`RelocationID(35560,36559)+0x2EC/0x30A`), then queues the **main camera's pass registration jobs** and,
while they run, calls every light's `Render` (vfunc 0xA), and only then `JobList__Finish`es. So at
shadow-draw time the scene graph and the main cull are final, the shadow views' registrations are
complete, and the main accumulator's passes are not - they are complete when the thunk returns.

**Per light.** `BSShadowDirectionalLight::Render` (`0x141511d60`), `BSShadowFrustumLight::Render`
(`0x14151aac0`) and `BSShadowParabolicLight::Render` (`0x14151bd10`) loop their `ShadowmapDescriptor`s
(0xF0 bytes, `RE::BSShadowLight::ShadowmapDescriptor`) into `BSShadowLight::RenderShadowmap`
(`0x1414f0cf0`, mislabelled `BSShadowParabolicLight::sub`): a free array slice from the bitmask at
`0x142035797` when `renderTarget == -1`, `SetDepthStencilTarget(descriptor.renderTarget, slice)`, a clear
when `clearRenderTarget`, then `FUN_1414a90f0(camera, accumulator, flags | 0x400)`:
`State::SetCameraData` (the shadow camera into `cameraData` and `posAdjust`), `UpdateViewPort`, the
accumulator's vfunc 0x25, **`FinishAccumulatingPreResolveDepth` (vfunc 0x2A)** and `PostResolveDepth`
(0x2B); afterwards it writes `descriptor.lightTransform` (what CS uploads to t98). Parabolic lights set
`0x142035df8` (the light radius) and `0x142035dfc` (+1 / -1) before each hemisphere.

**The views, as drawn.**

| Light | Descriptors | Render mode | Depth target / slice | Viewport |
|---|---|---|---|---|
| Directional (sun) | 2 cascades | `0xE` ShadowMapClamped | 2 (`kSHADOWMAPS_ESRAM`), slices 0 and 1 | `(0,0) 4096x4096`, full slice |
| Directional, focus shadow | descriptor 0 of `focusShadowmapDescriptors` when `drawFocusShadows` | `0xE` | 4, slice 4 | `(0,3547) 549x549` - **an origin** |
| Parabolic (point) | 2 hemispheres | `0xF` ShadowMapPb | 4, one slice per light | `(0,0) 4096x2048` and `(0,2048) 4096x2048` - **an origin** |
| Frustum (spot) | (none in the cells measured) | `0xE` by the decompile | | |

`ShadowMapPlain` (`0xD`, technique bits `0x4000`) was never used. The `shadowLightsAccum` list holds a
second directional light whose descriptors are never drawn, and the focus descriptors 1..3 have a zero
port and no clear. A view's `port` (`NiRect`: left, right, top, bottom, y up) matches the viewport's
size; the hemispheres and the focus shadow are sub-rectangles of the slice, so any render pass that
draws a view needs a viewport origin, not just a size. Render flags at the hook are `0x400`. The
depth-stencil set mode is 4 (no clear at bind; the clear was done before). Point lights: `parabola =
(radius, ±1)`, e.g. 579 for a hearth. The main pass's `RenderDepth` accumulations (mode `0xC`) come
through the same vfunc twice per frame.

**The passes.** `BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask` (`0x1414af030`, from
`GetRenderPasses` vfunc 0x2A for modes 0xC..0xF): `technique = DetermineUtilityShaderDecl()` (vfunc
0x3D) `| 0x80` when the alpha property alpha-tests `| 0x2000` (0xC) / `0x4000` (0xD) / `0xC000` (0xE) /
`0x14000` (0xF) `| 0x10000` when LOD-dissolving (0xC only) `| 0x8000000` when flags bit 34 or 63
`| 0x20080` when flags bit 26 or 27; `passEnum = technique + 0x2B`, cached per property in the
double-buffered `arrayQueue` across views of a frame. **For Utility passes the `techniqueID` handed to
`BSBatchRenderer::RegisterPass` is the `passEnum` itself** (unlike Lighting, where it is the descriptor).
The probe derived the technique from the property for ~1M registrations with **0 differing**.
Accumulation hints seen: 0 (most), 3, 7, 11. Exterior day: ~2,680 Utility registrations per frame into
the views (216 cascade 0, ~2,450 cascade 1, 9 focus) and ~2,450 `0x2000`-bit (RenderDepth) ones into a
renderer that is neither a view's nor one of the main pass's batch renderers.

**Who casts.** For the shadow modes the function returns no pass when: the property is not a lighting
one; hair-tint flag (bit 18) with a decal flag (26/27) unless `kZBufferWrite` (32) and alpha-blended;
`fadeNode->currentFade * material->materialAlpha < 1`; flags `0x8004` (`kTempRefraction`,
`kRefraction`); alpha-blended (non-decal); `kCastShadows` (bit 9) clear while the byte at `0x142033498`
is 1 (it is 1 here; 2 selects volumetric copies for 0xE with the accumulator's `+0x12E` flag);
`DetermineUtilityShaderDecl() == 0`. The rule reproduced in the probe rejected **none** of the engine's
registered casters. Of the main pass's kept objects without a registration in a cascade (~475 per
frame in the exterior), 40 were alpha-blended and the rest eligible - outside the cascades' cull.

**The constants** (`BSUtilityShader::SetupGeometry` `0x1414fae40`, `SetupMaterial` `0x1414faa60`,
`SetupTechnique` `0x1414fa160`; `package/Shaders/Utility.hlsl`). VS `PerGeometry` (b2): `ShadowFadeParam`
c0, `World` c1..c4 (row-major, translation in column 3 - the Lighting layout - and **eye-relative to
the shadow camera's `posAdjust`**: `FUN_1414aaf10` subtracts the current `posAdjust`, which the
directional camera puts up to 15,000 units from the main eye), `EyePos` c5, `TreeParams` c7. VS
`PerMaterial` (b1): `TexcoordOffset` c0 (`texCoordOffset`, `texCoordScale`), bound only for alpha-tested
draws. VS `PerTechnique` (b0): `HighDetailRange` c0, `ParabolaParam` c1 = `(1 / radius, ±1)`. The pixel
shader is bound only for alpha-tested draws, and its `PerGeometry` group is **not** - the alpha
reference reaches it as `AlphaTestRefRS` (b11) from `RendererShadowState.alphaTestRef`. `VS_PerFrame`
(b12) `c8..c11` holds the **transpose** of `cameraData.viewProjMat` at the 0x2A hook, and `posAdjust`
at the draw equals the one at the hook. Rasterizer state per draw: cull modes 0 and 1 (two-sided and
back), fill 0; the cascade depth-bias mode is read off the draws in S2.

**Cost.** Exterior day: 2.3 ms of render-thread CPU per frame inside the shadow-map call (max 3.6),
all in the directional light: cascade 1 with 2,569 draws takes 2.2 ms (0.86 µs per draw), cascade 0
with 236 draws 0.23 ms, the focus view 0.014 ms. Bannered Mare at night: two point lights, 4
hemispheres, ~1,500 draws, 0.5 ms.

**The volumetric copy (measured, S2).** Each cascade's accumulator is finished twice per frame: once
with `depthStencil = kSHADOWMAPS_ESRAM` (target 2, 4096², two slices) and once with
`kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` (target 3, 512², two slices, `D16_UNORM` like the others) for
the volumetric lighting. `FinishAccumulatingPreResolveDepth` (vfunc 0x2A) fires for each, with the same
view (accumulator) and render mode 0xE. They do **not** draw the same passes:

-   `Render` draws the copies first, with flag `0x100`, and only while the byte at `0x142033498` is 2.
    It is 2 in the current setup; the probe above ran with it at 1.
-   The mode's draw function (`FUN_1414b44f0`, mode 0xE in the table at `0x14332b120`) draws only batch
    group 15 when the flag is set, and never draws group 15 without it.
-   Group 15 holds the passes registered with accumulation hint 8: the volumetric-only casters, meaning
    Lighting objects without `kCastShadows` while the byte is 2.
-   Measured (`CS_DCLF_VOLUMETRIC_PROBE`, road, Riverwood, Ivarstead, Winterhold): 71-93 geometries a
    frame, all static `BSTriShape` terrain pieces (mountains, cliffs, road), none also drawn into a
    cascade.

**Registration by hint.** The shadow modes' registration function (`FUN_1414b2a60`, modes 0xC-0x11 of the
table at `0x14332b020`, filled by `FUN_14147e2c0`) sends a pass with hint 11, 7, 3 or 8 straight into
batch group 9, 1, 4 or 15 with `FUN_1414f5090(batch, pass, group)`, a direct call. Only the other hints
go through `BSBatchRenderer::RegisterPass` (vfunc 2). The main mode (`FUN_1414b2330`) does the same for
most hints. Confirmed for hint 8: `VolumetricProbe` saw none arrive at the `RegisterPass` hook, yet about 80
a frame were drawn. **Unresolved for 3, 7 and 11:** the S1 probe above recorded hints 3, 7 and 11 at that
hook. Those may be registrations into a geometry group's own batch renderer, which is a different path.
Before relying on either reading for those hints, check it with a counter on the `FUN_1414f5090` call
sites.

**The sun's focus shadow is drawn every frame.** In the third-person exterior runs, the directional
light draws one focus view a frame: target 4 (`kSHADOWMAPS`), slice 4, 546x546, flags `0x400`, about 9 NPC
draws. It is accumulated by `BSShadowDirectionalLight::sub` (`0x1414f0480`, called from
`CalculateAndDrawShadowCasterLights` when the setting byte `0x142032fd0` is set and `DAT_14332a498`
counts focus descriptors). It is drawn by `Render`'s third loop (gated on `unk558`, view type 4).

### The shadow lights' selection: where each light's views are decided (AE 1.6.1170, Ghidra, 2026-10-16)

Read for DCLF's round 3, phase 3: where the shadow views' coverage and the renderer map PassCapture withholds by must be decided so that
every registration into a view tests this frame's. **There is no single point after the selection and before the first registration:**
the engine selects and accumulates each light in turn, so the decision is made light by light.

-   **`Main::Draw`** (`0x1406444b0`) calls `NiCamera::CalculateAndDrawShadowCasterLights` (`0x1414cbb90`) unconditionally at
    `0x140644674` (`E8`, `Main::Draw +0x1C4`), after the scene lists are built and before `Main::unk_FPCulling` and `FUN_1414cbff0`
    (CS's `Main_RenderShadowMaps` thunk, `+0x30A`). Nothing between its return and `FUN_1414cbff0` touches the shadow lights.
-   **`CalculateAndDrawShadowCasterLights`**, in order:
    1.  `FUN_1414a4030` clears `shadowLightsAccum` (`ShadowSceneNode +0x230`, every slot's light `+0x520` = `0xFF`); the slot count
        (`0x14338c904`) and the light mask (`0x14338c90c`) are zeroed.
    2.  **The sun, when `DAT_14338c911` (the sun off) is clear:** its `drawFocusShadows` (`+0x558`) set to the focus setting, its
        `UpdateCamera` (vfunc 0x10), the focus camera (`BSShadowDirectionalLight::sub`, `0x1414f0480`), then
        **`FUN_1414a3fb0(node, sun, slot, shadowMapCount)` at `0x1414cbc66`**, which writes the light into its slots, then the
        full-frustum cull (`FUN_141511f30`, `0x1414cbcb6`). With the sun off, its `ReturnShadowmaps` (vfunc 0xC) instead.
    3.  `FUN_1414a0840` (`0x1414cbcea`), the main camera's list accumulation jobs queued, `JobList::Begin`.
    4.  **`CalculateActiveShadowCasterLights`** (`0x1414cc570`, called at `0x1414cbf41`), then `JobList::Finish`.
-   **`CalculateActiveShadowCasterLights`:** the sun's **`Accumulate` (vfunc 9) first** (`0x1414cc5e5`, sun on), then each of
    `activeShadowLights` (up to 4 kept) in turn: `UpdateCamera` (vfunc 0x10, which sets the light's camera and, for a spot light, creates its
    accumulator on first use) and the room test; for a kept light the focus host (`drawFocusShadows`), its portal walk (`FUN_1414a2530`),
    **`FUN_1414a3fb0(node, light, slot, 1)` at `0x1414cc7e1`**, the scissor, then its **`Accumulate`** (`0x1414cc9ce`). A light not kept
    gets its portal entry's visibility cleared and `ReturnShadowmaps`, which hands its slices back to the free mask (`0x142035798`).
-   **`Accumulate` registers synchronously:** `BSShadowDirectionalLight::Accumulate` (`0x141511c80`), `BSShadowFrustumLight::Accumulate`
    (`0x14151aa60`) and `BSShadowParabolicLight::Accumulate` (`0x14151b960`) set the accumulator's light index and bit (`+0x160`, `+0x164`)
    and cull each descriptor (`FUN_1414f0920`, which increments the slot count), registering as they go. Nothing registers into a shadow
    view before its light's `Accumulate`.
-   **So** a light's views (its descriptors' accumulators and batch renderers, its focus views) are known at its `FUN_1414a3fb0` call and
    not before (a spot light's accumulator may not exist until that frame's `UpdateCamera`); the frame-wide coverage (the revision's
    recording, the installed payload, the capacity) is known before `CalculateAndDrawShadowCasterLights`; and DCLF's own selection
    (`Features/LocalLights`' detour of `CalculateActiveShadowCasterLights`, which ports it after the engine's runs) is complete when
    `CalculateAndDrawShadowCasterLights` returns. The free mask, the descriptors' targets and slices and `0x142033498` are not written
    between that return and `Render` (writers of `0x142035798`: `ReturnShadowmaps`, `FUN_1414f01d0` from `SetShadowMapCount` and the
    destructor, and `RenderShadowmap`).
-   **DCLF** (`Features/Shadows/Engine/ShadowCasterPoints.cpp`): Main::Draw's call is hooked to begin the frame's views and decide their
    coverage before it and to list them from DCLF's selection after it; both `FUN_1414a3fb0` calls are hooked to cover the kept light's
    views and publish their renderers before its `Accumulate`. `FUN_1414a3fb0` has one other caller, `FUN_140973210`, not hooked.

## The sun's accumulation: what it writes, and who reads it

`NiCamera::CalculateAndDrawShadowCasterLights` (`0x1414cbb90`), for the sun, in order:

1.  `UpdateCamera` (vfunc 0x10).
2.  The focus accumulation, `BSShadowDirectionalLight::sub`, when enabled.
3.  The mask bookkeeping: `light+0x520 = count`, and the frame's shadow-light bit into `DAT_14338c90c`.
4.  The full-frustum cull, `FUN_141511f30(light, &DAT_14338c870, ...)`:
    -   Per process in `light+0x580`, it queues a cull job (`FUN_1414bf730`) on the scene-list job list,
        which fills that process's `objectArray` (`+0x128`, count `+0x138`).
    -   It closes with `FUN_1414bf320(..., 1, 1)`, which culls with no accumulator.
    -   The only consumer of these arrays found is `Accumulate`, which is passed
        `&fullFrustumCullingProcessArray`.

`BSShadowDirectionalLight::Accumulate` (vfunc 9, `0x141511c80`), per cascade:

1.  It sets the cascade accumulator's `+0x160` to the global shadow-light count + 1 and `+0x164` to
    `1 << count`, plus `+0x168` (the cascade index) and `+0x12E` (the volumetric flag).
2.  `FUN_1414f0920` calls `FUN_1414b47d0(accumulator)`, which is an empty stub.
3.  `FUN_1414bf320(..., 2)` walks each full-frustum process's `objectArray` against the cascade
    (`FUN_140e305c0`). `FUN_140e28af0` then hands every culled geometry to the accumulator's registration,
    `FUN_1414b2140`.
4.  It **increments the global shadow-light count** (`*param_3 += 1`). The sun's two cascades take two
    bits, and every later shadow light's bit is offset by them.

`FUN_1414b2140`, for every geometry handed to it:

-   It calls the mode's registration function. For shadows that is `FUN_1414b2a60`, which writes
    `property->lastAccumulatedFrameCount = gFrameCounter` and registers the passes.
-   Then **`property->lightData->activeLightMask |= accumulator+0x164`**. When `+0x160` is `0xFFFF`, it
    clears the mask to 0 instead.
-   The mask bit is set for every geometry that reaches the call, whether or not it produced a pass.

**Who reads `activeLightMask`: the main pass.** `BSLightingShaderProperty::GetRenderPasses` reads it
during the main camera's registration, which runs after the shadow lights accumulate:

-   `lastRenderPassState = (mode << 8) | activeLightMask` decides whether the pass list is rebuilt.
-   `FUN_1414fcdb0(lightData)` counts the non-directional shadow lights among the mask's bits
    (`pass->shadowLightCount`).
-   For the sun, it loops over `sunShadowDirLight->shadowMapCount` bits and stores the cascades this
    object is registered in into pass byte `+0x1D`. When that is 0, the pass descriptor loses
    `0x61C0`: ShadowDir (13), DefShadow (14) and the shadow light count (6-8).

So an object samples the sun's shadow mask in the main pass only if the sun's accumulation registered it
in a cascade that frame. Skipping that accumulation changes main-pass techniques unless those bits are
set some other way. `lastAccumulatedFrameCount` has no reader found yet.

**Who clears the masks.** `FUN_1414cb640`, which `Main::Draw` calls before the shadow lights accumulate (at
`0x140644d7e`, inside a block that runs only while the player's third-person 3D is drawn: it unhides it first, so in first
person there is no clear and the lights accumulate all the same), writes accumulator `0x14338c840`'s `+0x160` as the 8-byte `0xFFFF` (so `+0x164` becomes 0) while
`DAT_14338c911` is 0. It then registers culling process `0x14338c640`'s culled geometries through that accumulator
(`FUN_140e28af0`), and the registration zeroes their masks. Each shadow light then ORs its bits in.

**The main camera's registration reads the mask, then clears it.** Its accumulators also have `+0x160 = 0xFFFF`, so
`FUN_1414b2140` runs `GetRenderPasses` with the frame's bits and then zeroes the mask. Every registration of the same
geometry later in the frame (the reflections, the depth accumulations) reads 0. Measured with DCLF's sun entry
exclusion probe: ignoring this, a fifth of the later registrations disagreed.

**`FUN_1414b2140`'s early-outs**, before the mode's registration and the mask write. It returns at once when:

-   the skin instance is a `BSDismemberSkinInstance` whose byte `+0x98` is 0;
-   the geometry has no shader property (`+0x128`);
-   it has neither renderer data (`+0x138`) nor a skin instance, its vfunc `0x10` returns null, and its type
    byte (`+0x150`) is not `0xB`.

It writes no mask when the accumulator's `+0x160` is 0, or the property's `lightData` (`+0x70`) is null.

**The rest of the sun's accumulation** (AE 1.6.1170):

-   `Accumulate` sets each cascade's fields and calls `FUN_1414f0920`. That function calls `FUN_1414b47d0`
    (an empty stub), culls, registers, and increments the count (`*param_3 += 1`). Nothing else.
-   `FUN_140e28af0(process, accumulator)` registers everything a culling process kept: its `+0x128` list
    (count `+0x138`), then its bucketed lists. `FUN_1414bf320(ctx, 2)` gets there through `FUN_140e305c0` and
    `FUN_140e28f70`. `FUN_140e28f70` calls the process's vfunc `0xB8` for the first entry and `0xB0` for the
    rest, and with its `param_4` set it skips entries whose `+0xF4` bit 0 (app-culled) is set.
-   The array `FUN_140e305c0` hands `FUN_140e28f70` is the full-frustum process's `objectArray` (`+0x128`, a
    `BSTArray<NiPointer<NiAVObject>>`, size at `+0x138`), the same one for every cascade. The first entry's vfunc
    `0xB8` (`Process(camera, scene, visibleSet)`) is what sets the cascade process's frustum and planes up from the
    cascade's camera; with an empty array it keeps the previous cascade's.
-   The cascade's cull context (`FUN_1414bf2b0` builds it, `FUN_1414f0920` fills it): the accumulator (`+0x48` of the
    descriptor), the camera (`+0x40`), cull mode 3 or 4 (`light+0x47`), the custom planes from the descriptor's
    `+0xE0` block when its `+0x11F` is set, and `cameraRelatedUpdates` and `updateAccumulateFlag` both 0. The process
    is the static one at `0x14332bda0` (the context's `+0x5C` is 0).
-   The full-frustum cull (`FUN_141511f30`) queues one job per scene list (`FUN_1414bf730`), then runs
    `JobList__Begin` and `Finish` itself, so the render thread waits for it. Each process's `planes` are
    refreshed by that cull, from the light's full-frustum camera (`+0x578`).
-   The focus view does not depend on any of this. `sub` only builds the focus cameras, and `Render`'s third
    loop accumulates the focus view itself.

**Measured** (Riverwood, render thread per frame, `CS_DCLF_SUN_TIMING`):

| Part | Mean | Max |
| --- | --- | --- |
| Full-frustum cull | 0.044 ms | 0.23 ms |
| `Accumulate` | 0.69 ms | 1.25 ms |
| … of which registration (about 2,580 geometries) | 0.41 ms | |

The registration figure includes the probe's own timing overhead.

## The occlusion maps: precipitation, and Skylighting's

Two views draw the scene's depth from above into depth target 10 (`kPRECIPITATION_OCCLUSION_MAP`, 512x512): the
engine's precipitation mask, when there is a current or last precipitation object, and Community Shaders'
Skylighting height map, every exterior frame. Skylighting replaces the engine's call at `Main::Draw` +0x3A1 with its
own `RenderOcclusion`, which runs both through the engine's `Precipitation::SetupMask` and `RenderMask`, pointing the
occlusion camera in a new sky direction each frame and swapping target 10's texture for its own (`texOcclusion`).
Measured at Riverwood (clear weather, so Skylighting's alone), render thread per frame: `SetupMask` 0.43 ms,
`RenderMask` 0.34 ms.

-   **`SetupMask`** (`0x1404081c0`): the occlusion camera's projection (`FUN_140408520`), then for each of the six
    scene lists (`DAT_14338c870`, count `DAT_14338c880`, 0x18 bytes each) `FUN_1414bf320(ctx, 1)`: the list culled
    by `FUN_140e28f70` with the occlusion data's `BSGeometryListCullingProcess`, cull mode 3, and
    `cameraRelatedUpdates` 0, then registered through the occlusion accumulator (`FUN_140e28af0`), on the render
    thread. Without `cameraRelatedUpdates`, `BSFadeNode::OnVisible` (`0x141479f50`) updates no fade: with fades on
    (`0x142032dfd`) it goes into a node that is not settled (flags bit 15, `fadeAmount` and `currentFade` at 1) only
    while `currentFade > 0` and `fadeAmount != 0`. The map leaves out what the main camera's cull faded out, and the
    roots of a new cell before the main camera has seen them.
-   **The registration**, render mode `0x1C` (`FUN_1414b2c20` in the table at `0x14332b020`): the property's vfunc
    `0x2D` (`GetRenderPasses_Occlusion`), each pass inserted straight into batch group 14 with `FUN_1414f5090`. It
    never reaches `BSBatchRenderer::RegisterPass`. The accumulator's `+0x160` is 0, so no mask is written.
-   **The Lighting property's passes** (`0x1414afd50`, which Skylighting replaces): one Utility pass, technique
    `0x201A` (`0x2002` with model-space normals), `+1` with vertex colours, `+0x80` alpha-tested, `+0x8000000` for LOD
    objects, pass enum technique + `0x2B`. It needs `kZBufferWrite`, and it rejects skinned geometry, refraction,
    the terrain flags (multi-texture, `kNoLODLandBlend`, LOD landscape), eye reflection, tree animation, decals and
    flag bit 53. No fade, alpha-blend or `kCastShadows` test.
-   **Skylighting's replacement** (`Skylighting.cpp`, `GetPrecipitationOcclusionMapRenderPassesImpl`): a
    `RenderDepth` pass with `Vc`, `Texture` and `AlphaTest`, `LodObject` and `TreeAnim` as the property has them, for
    geometry with `kZBufferWrite` and a world-bound radius above 32. In its own view (`inOcclusion`) it keeps terrain
    and skinned trees, and drops references whose fade node's BSX flags mark them a ragdoll, editor marker, dynamic,
    addon, needing transform updates, magic particles, lights or breakable. Its camera's frustum covers one quarter
    of the square each frame (`SetViewFrustum`, `frameCount % 4`).
-   **`RenderMask`** (`0x140408380`): the camera into the renderer state, target 10 bound and cleared, then
    `FUN_1414a90f0(camera, accumulator, 0)`: the accumulator's `FinishAccumulatingPreResolveDepth` (vfunc `0x2A`),
    whose mode `0x1C` draw function (`FUN_1414b4750`) renders batch group 14. At that hook: render flags 0, viewport
    512x512, depth range [0, 1], `posAdjust` at the occlusion camera.
-   **Who reads them:** the rain particles read the precipitation map; Skylighting's `Prepass` compute (the probe
    update) reads `texOcclusion`.
-   **The cull mode is per pass:** the Utility shader sets it for each draw (0 for a two-sided property, 1 otherwise),
    so the renderer's state at the view's `FinishAccumulating` is the last pass's, and with nothing registered, whatever
    came before. DCLF's variant of the map takes back-face culling as the view's state.
-   **DCLF's variant** draws Skylighting's map (and the precipitation mask) itself when both run, and `SetupMask` is skipped:
    Skylighting sets the views up and `RenderMask` clears them before the engine's shadow maps (+0x2EC), and DCLF's shadow epoch
    draws them when those return; the call at +0x3A1 then renders only a map DCLF did not draw (`dclf-architecture.md`, "The
    occlusion maps, drawn by DCLF").

## The primary's cull: the scene lists

-   **The lists.** `DAT_14338c870` points at `DAT_14338c868` (6) `BSTArray<NiPointer<NiAVObject>>`, one per list job, filled
    round robin by `DrawWorld_BuildSceneLists` (`0x14064bc20`) with reference roots (the children of the cell category
    nodes 2, 3, 5, 6 and 7+; land, water and multibounds; in portal interiors, whole rooms), and `ObjectLODRoot`'s first
    two children whole. Every actor hangs under `ObjectLODRoot`'s second child, which is one entry. `DAT_14338c888` is an
    extra list only the first job culls (sky, weather, the LOD roots).
-   **The order in `CalculateAndDrawShadowCasterLights`** (`0x1414cbb90`): the sun's full-frustum cull
    (`FUN_141511f30(light, &DAT_14338c870, ...)`, the same lists), `FUN_1414a0840`, one job per list
    (`ListAccumulationJob`, `FirstListAccumulationJob` for list 0), `CalculateActiveShadowCasterLights` (the sun's
    `Accumulate` runs here, alongside the jobs), then `JobList::Finish` (`0x1414cbf4d`). Each job culls its list with its
    own `BSGeometryListCullingProcess` (`DAT_14338c8a0[i]`, `cameraRelatedUpdates` and `updateAccumulateFlag` set); the
    list's first entry goes through `Process2` (`0xB8`), which sets the frustum up, the rest through `Process1`.
-   **The registration** (`FUN_1414cbff0`) queues two jobs that walk every list process's output: the depth prepass's
    accumulator (`*0x14338c828`, render mode 0xC) and the main one (`*0x14338c830`, render mode 0, `+0x160` = `0xFFFF`).
-   **`BSFadeNode::OnVisible`** (`0x141479f50`), for a process with `cameraRelatedUpdates`: nothing but the recursion
    when fades are off (`0x142032dfd`) or the node is settled (flags bit 15 with `fadeAmount` (`+0x100`) and
    `currentFade` (`+0x130`) at 1, which no node at Riverwood was). Otherwise `FUN_14147a160(node, fadeAmount,
    {camera position, lodAdjust (+0x184)})` and the last-visible stamp (`+0x13C` = `0x142032e50`); for LOD type 6
    (`+0x153 & 0xF`) with `0x14332a254 == 0x141769578`, its own short step instead. It recurses into the children only
    while `currentFade > 0` and `fadeAmount != 0`. `BSLeafAnimNode::OnVisible` (`0x14147c9c0`) first runs
    `FUN_14147b110` and `FUN_14147a430` (the leaf LOD) when `0x142032dfc` is set; `BSTreeNode::OnVisible`
    (`0x14147d3c0`) also skips the node under a height test.
-   **`GetRenderPasses`' sun bits** (`0x1414adfb0`): ShadowDir is the light selection's output (`FUN_1414fcf80`: the
    incoming flag, and some mask bit naming the sun); DefShadow is the accumulator's deferred flag (`+0x178`) under alpha
    and fade conditions, cleared without ShadowDir or a shadow light; both are cleared for a property with no shadow
    passes unless flags `0x800c000100`. It copies `kSkinned` into descriptor bit 1 and `kProjectedUV` into bit 15.
-   **The pass draw** (`FUN_1414f2ad0`, from `SetupAndDrawPass` for unskinned geometry) switches on the geometry type
    (`+0x150`): a tri-shape (3) draws its renderer data (`+0x138`) with its triangle count (`+0x158`); a
    `BSMultiIndexTriShape` (7) the same, except a pass with accumulation hint 12, which draws its second index list
    (`altIndexBuffer`, `FUN_140e465d0`); `BSLODMultiIndexTriShape` (6) picks its LOD level's ranges. The second list's
    draw binds `*altIndexBuffer` (`+0x160`, its first qword the `ID3D11Buffer`) as R16 over the renderer data's vertex
    buffer and draws `altPrimCount` (`+0x168`) triangles from index 0. Hint 12 alone selects it; `useAdditionalTriList`
    is not read there.
-   **The registration table** (`0x14332b020`, a function per render mode, filled by `FUN_14147e2c0`): modes 0-11 take the
    main modes' registration (`FUN_1414b2330`), 0xC-0x11 the shadow modes' (`FUN_1414b2a60`), so the Z-prepass's
    accumulator (mode 0xC) registers as a shadow view does; 0x1C (the precipitation accumulator, both occlusion maps)
    takes `FUN_1414b2c20`, which inserts every pass of the property's `0x168` (the occlusion map's passes) into geometry
    group 14 with `FUN_1414f5090` at `0x1414b2c71`, and nothing else.
-   **The main modes' insertions** (`FUN_1414b2330`, by the pass's hint): hint 1 `FUN_1414f50b0(batch, pass, list)` at
    `0x1414b2478`; then `FUN_1414f5090(batch, pass, group, flag)` at `0x1414b24a2` (hint 2, group 3, only while
    `accumulator+0x12c` is set), `0x1414b24bd` (3, 4), `0x1414b24d8` (4, 5), `0x1414b24f3` (5, 6), `0x1414b250d` (6, 0),
    `0x1414b2528` (7, 1), `0x1414b2543` (9, 7), `0x1414b255e` (11, 9), `0x1414b2579` (13, 10), `0x1414b2591` (14, 11),
    `0x1414b25a9` (15, 8), `0x1414b25c1` (16, 12), `0x1414b25db` (17 and 18, 13); every other hint (8, 10, 12) through the
    batch renderer's `RegisterPass` (vfunc 0x10) at `0x1414b25f4`; a multi-index shape's layer (hint 12) into group 2 at
    `0x1414b2667`. The batch renderer is `accumulator+0x130`.
-   **The shadow modes' registration** (`FUN_1414b2a60`) asks only the geometry's own property for passes, so a multi-index
    shape's additional property never casts.
-   **The main modes' registration** (`FUN_1414b2330`, render modes 0-3 of the table at `0x14332b020`): after the
    geometry's own passes, for a multi-index shape (vfunc `0x1a8`) with an `additionalShaderProperty` (`+0x1b0`) it copies
    the main property's `lightData+0x20` byte into the additional property's, asks the additional property for its
    passes, sets each one's hint to 12 and inserts it into geometry group 2 with `FUN_1414f5090(batch, pass, 2, 1)` (not
    `RegisterPass`). Group 2 holds nothing else in the main registration (hint 2 goes to group 3, hint 3 to group 4).
    `FUN_1414b3bb0` draws it right after group 3, both with depth mode 3 (test and write); group 3's bias mode is
    `6 + b`, group 2's `8 + b`, both only while the `ToggleDepthBias` byte is set, else 0.
-   **A multi-index shape's ProjectedUV values** (`SetupGeometry`'s ProjectedUV block, for any of its passes with
    `kProjectedUV`): `TextureProj` is the shape's `materialProjection` (`+0x16c`, a 4x4 read column by column) rather
    than the computed projection, and `FUN_1414e00c0` takes `ProjectedUVParams` from `materialParams` (`+0x1bc`, with
    the same `(1 - a)` arithmetic) and `ProjectedUVParams2.xy` from `normalDampener` (`+0x1d0`) and `materialScale`
    (`+0x1cc`), leaving `zw` unwritten.
-   **The vanilla multi-index shapes** (Bleak Falls Barrow's and the glacial caves' walls and boulders, 95 at Bleak Falls
    Barrow): main property technique 0; additional property technique 14 (`multiIndexSparkle`: `kMultiIndexSnow`,
    `kProjectedUV`, `kDecal`); `useAdditionalTriList` 0, an alt list as long as the main one, no alpha property. At
    Yngvild's coast (icebergs, ice piles, glacier pieces) the main properties are technique 0 or multilayer parallax (hints
    15 and 0), and the layers are drawn (technique 14, hint 12); behind Bleak Falls Barrow's portals none of the shapes are.
-   **A layer pass's shadow bits:** none. Measured on every native hint-12 draw (pass descriptor `0E008201` against the
    `0E00E201` the main property's rule gives): ShadowDir needs the property's light mask to name the sun, and the
    registrations write masks only on the geometry's own property (`FUN_1414b2140`), so DefShadow is cleared with it.
-   **`SetupTechnique`'s samplers** by the remapped technique: 1 and 0x10 slots 4 and 5; 2 slot 6; 3 and 7 slot 3; 4 slots 3,
    4 and 12; 8 and 0x13 the land maps; 9 and 0x12 the LOD land maps; 0xb slots 4, 5 and 8; 0xe binds slot 10 with address mode 3 and filter 0. Slot n's filter mode is `0x14202ac6c + 4n`. The remap: 0x12 to 9
    unless `0x142032fdb`, 7 to 0 unless `0x142035500`.
-   **`Process1`** (`BSCullingProcess::Process1`, `0x140e28390`, the list processes' vtable slot `0x16`): the bound test
    (`TestBaseVisibility3`, or the compound frustum with portals), then the object's `OnVisible`, and `kAccumulated`
    (flags bit 26) set on what it passed and cleared on what it culled. `AppendVirtual(geometry, group)` appends
    straight to `objectArray` (`AppendNonAccum`) when the process is not grouping alphas and the group is -1, and queues
    it for an alpha group otherwise.
-   **`NiSwitchNode::OnVisible`** (`0x140d29700`) culls `children[index]` only, with group -1; when
    `childRevID[index] != revID` it first brings the child up to date (`UpdateDownwardPass`). The fields, at AE's
    offsets: flags `+0x128`, `index` `+0x12C`, `savedTime` `+0x130`, `revID` `+0x134`, and `childRevID`'s data `+0x140`
    and capacity `+0x148`. CommonLib's `NiSwitchNode::index` does not read `+0x12C` in this build.
-   **`BSTreeNode::OnVisible`** (`0x14147d3c0`): when the process's byte `+0x121` and `0x142032fb8` are set and
    `worldBound.center.z - *0x14332a2f0 > *0x142032fa0`, nothing at all (no update, no recursion). Otherwise
    `BSLeafAnimNode::OnVisible`, then, when `+0x180`'s `+0x12C` count is positive, `+0x153 = (+0x153 & 0xAF) | 0x20`
    and `+0x14C = 0`.
-   DCLF takes its references out of these lists (`drawcall-limit-fix.md`, "The primary's cull without DCLF's objects").

### Fade distance

-   **The fade value** (`FUN_14147b110(node, {camera position, lodAdjust}, &distance)`). The distance is from the camera
    to `worldBound.center` (`+0xE4`), written unscaled to the out parameter. Its scale is `lodAdjust` over the LOD
    type's divisor `0x142032e00[+0x153 & 0xF]`, or the constant `0x141ad2840` when the divisor is not positive (that
    case also sets flags bit 14 and `currentFade` = 1, except for type 6). With `x` the scaled distance, `n` and `f`
    the node's `+0x128` and `+0x12C` times `0x142032e48`: the value is 1 up to `n`, then `1 - (x - n) / (f - n)`, and
    always 1 while `0x142032dfc` is clear. It also writes the LOD metric `+0x144` (`x * 0x141aa6300`, or
    `0x14332a254` when that differs from `0x141769578`; the previous metric goes to `+0x148`).
-   **The fade update** (`FUN_14147a160(node, fadeAmount, camera)`). Flags bit 14 is the fade target (faded in).
    -   A faded-in node fades out below `0x142032e38`, and a faded-out one fades in above `0x142032e34`.
    -   A node last visible (`+0x13C`) more than 20 frames ago, not type 8, with `+0x134` below a constant, is reset to
        1 or 0 by that rule.
    -   A fade step runs only while the node was visible in the last frame (`0x142032e50 - +0x13C < 2`) or is type 8;
        otherwise the fade snaps to its end.
    -   `+0x109` bit 0 fades the node out whatever its distance.
    -   With flags bit 27 set, the update runs the leaf LOD update `FUN_14147a430` first.
-   **The leaf LOD** (`FUN_14147a430(node, distance)`). The level (`+0x152 & 0xF`) is 3, 2 or 1 as the unscaled
    distance passes two thresholds (`0x14332a268`/`0x14332a238` for a type-4 `BSTreeNode`, `0x14332a25c`/`0x14332a22c`
    otherwise, scaled by `+0xF0` through `logf`/`powf` when `+0x109` bit 1 is set). A node not visible for 20 frames
    snaps to the new level, settled (`+0x153 & 0x70` = `0x20`); otherwise the change is a cross-fade over frames
    (`+0x14C`).
-   DCLF tests a resident fade root's distance on the GPU (`drawcall-limit-fix.md`, "Resident entries", step 2).

### Fade state: inputs, outputs and their other readers

Decompiled 2026-10-01 for moving the fade and LOD state machine to the GPU.

-   **Pure.** `FUN_14147b110`, `FUN_14147a160`, `FUN_14147a430` and the three `OnVisible`s read only the node and globals,
    and write only the node. The one virtual call is `FUN_14147a430`'s `GetRTTI` (a type-4 `BSTreeNode` takes the tree
    thresholds).
-   **More state than the summary above:**
    -   `+0x134` is the world bound radius (`+0xF0`) captured by the placement snap below. `FUN_14147a160` resets a node
        not visible for 20 frames (not type 8) only while it is below a constant.
    -   `+0x140` is the last `fadeAmount`-driven fade. With flags bit 14 set and `fadeAmount` or `+0x140` not 1,
        `FUN_14147a160` steps `+0x130`/`+0x140` toward `fadeAmount` (or snaps above a threshold) and returns.
    -   `FUN_14147a430` keeps the previous level in `+0x152`'s high nibble, the transition state in `+0x153 & 0x70`
        (`0x20` settled, `0x10`/`0x30` blending, `0x80`/`0xA0`/`0xC0` the step's outcome), and the blend in `+0x14C`.
        With `+0x109` bit 1, the thresholds scale by `powf(c, logf(+0xF0 / r) * k)`.
-   **The node's construction** (`BSFadeNode::ctor_impl`, `0x141479ea0`): near and far `FLT_MAX`, `currentFade` 0
    (fades on) or 1, flags bits 14, 15 and 17 clear, `+0x153` `0x20` with type 0, `+0x134`, `+0x138`, `+0x13C`, `+0x144`
    and `+0x148` zero, `+0x14C` and `+0x140` 1, `+0x152` `0xF3`, `+0x109` `(x & 0xEA) | 2`, `+0x154` 1.
-   **At a reference's attach to its cell** (`FUN_1402d1280`, `FUN_1402d5090`):
    -   the LOD type (`+0x153` low nibble: 10 or 6 for some references);
    -   near and far from the bound (`FUN_14021f200` -> `FUN_14147a9b0`; also `TESModelDB::TESProcessor`);
    -   for the player, or a reference that never fades: flags bit 15 and `currentFade` 1;
    -   otherwise the **placement snap** `FUN_14147aa20(node, world camera)`: flags bit 14 and
        `currentFade` from the distance, `+0x134`, and `+0x13C` for types 1 and 4-6.

    All of it precedes DCLF's attach event, so a GPU state slot initialised from the node at join starts from it.
-   **Who writes `+0x13C`** (checked 2026-10-11 for T7b4's `<- FADE DOUBLE`):
    -   `BSFadeNode::OnVisible` (`0x141479f50`) stores the fade frame counter (`0x142032e50`) there after its fade update. It does so
        only when the process has `cameraRelatedUpdates`, and skips both the update and the store with fades off or the node settled
        at 1.
    -   `FUN_14147a160` only reads it.

    So a `+0x13C` that moved means an `OnVisible` stepped the fade. The animation gate `FUN_1402cff60` reads `currentFade > 0` from the
    node right after its call of `FUN_14147a160` (`0x1402d0044`), so with that call skipped, the gate reads whatever the node holds.
-   **`fadeAmount` is `NiAVObject+0x100`**, not a fade node field. Its stores are spread over 375 functions, none of
    them a user of the fade globals; the named ones are `NiAVObject::ctor` and `Actor::SetAlpha` (actors, which are not
    stood in).
    -   **Actors fade in through it every frame.** `Actor::SetAlpha` (vfunc `0xE1`, `0x1406c1a50`) writes the actor's 3D
        root's `+0x100` (and the player's first-person root's). `Actor::UpdateAlpha` (vfunc `0xE0`) calls it with
        `ActorProcess`'s alpha (middle-high `+0x134`), which `FUN_1406eb630` steps from 0 to 1 while the fade state
        (`+0x130`) is 1 or 3: after `ActorProcess::FadeIn` from `MoveToHigh`, `TESObjectREFR::Enable`, `Resurrect` and
        `SetLifeState`. An actor listed mid-fade has a `fadeAmount` that changes for about a second afterwards.
    -   DCLF follows it by event (`fadeAmountEvents`, `SceneStore::RefreshFadeAmount`). The fade parity's input check
        (`fade inputs parity`, `<- FADE INPUT`) compares every listed root's row with its node and names a writer
        without an event. The script and enable/disable fades of non-actors have shown none.
-   **`BSFadeNodeCuller::Process1`** (`0x1414e9a50`, the tree manager's culler) writes only flags bits 12 and 20 from its
    own frustum test: not fade state.
-   **The scene lists** (`0x14338c870`, count `0x14338c868`) are read by:
    -   `DrawWorld_BuildSceneLists` (fills them), `Main::Draw` (its `ClearLists` jobs), `FUN_14063eb50` (creation) and
        `FUN_14174a1e0` (teardown);
    -   `NiCamera::CalculateAndDrawShadowCasterLights`: the sun's full-frustum cull (`FUN_141511f30`, `FUN_141511650`)
        and the primary's list jobs and registration (`FUN_1414cbff0`);
    -   **`Precipitation::SetupMask`** (`0x1404081c0`), which culls and registers every list from the precipitation
        camera (`FUN_1414bf320(ctx, 1)`). A root taken out of the lists drops out of the precipitation occlusion mask.
-   **`activeLightMask` for DCLF's geometry** is written by DCLF itself: `SunAccumulation`'s `WriteMaskOnly` (a claimed
    caster's cascade bit, M1) and `ApplySunBits` (a removed entry's bits, at the main registration), plus the local
    shadow lights' own registrations. Each frame starts from `FUN_1414cb640`'s clear.

### The tree manager: distance, LOD switch and wind clock

-   **`FUN_140437e50`** (in `Main::Update`, for every tree the manager holds). It writes `+0x158`, the squared distance
    from the camera to the tree's world translation (`+0xA0`), and stores the tree LOD switch's index (`+0x180`'s
    `+0x12C`) from it: the full model within `+0x80` of the manager (squared), the LOD child past it. It sorts the near
    ones, and past `_LAB_14200d670` of them, forces the rest to the LOD child too. `FUN_140438840` is that tail alone.
-   **`FUN_1404381e0`** (the tree animation update, `BSFadeNodeCuller`, time-budgeted, under the manager's mutex). For
    each tree whose `kAccumulated` bit is set, it advances the wind timer `+0x164` by the frame time, refreshes
    `+0x158`, and within the manager's distance sets the amplitude `+0x15C` to a wave of `+0x78 * +0x164`
    (`FUN_140438950`) times the tree's base amplitude (its data's `+0xB0`). `BSLightingShader::SetupGeometry` copies
    `+0x164` to `+0x168` after every native tree draw.
-   **The two lists.**
    -   The tree list is `+0x20` (a `BSTArray`, size at `+0x30`). `FUN_140437200` adds a `BSTreeNode` to it, under the
        lock at `+0x10`.
    -   The animation list is `+0x50` (size at `+0x60`). `FUN_1404376f0` adds a `BSLeafAnimNode` to it, under the lock
        at `+0x18`. Each add takes a reference to the node by hand (`+0x8`).
    -   `FUN_140437840(manager, node)` removes a node from its list. A tree is searched for in `+0x20`, any other node
        in `+0x50`; the remove keeps the list's order and drops the reference. It is reached through a one-instruction
        thunk at `0x140282cc0`, from a vtable at `0x1417a0188`.
    -   `FUN_140437ae0` empties both lists (from `TES` and from the manager's teardown, `FUN_140437030`).
    -   The callers of the adds are `FUN_140192e90` and `FUN_140192fd0`, which pick the add by exact RTTI.
-   **Which trees the tree list's loop visits.** `FUN_140437e50` skips a tree unless its first child's multibound
    (`AsMultiBoundNode`, `+0x128`, its shape at `+0x18`) has the cull result 1 (`+0x10`) from the last cull, or the
    process's cull mode is 3.
    "Tree manager list (T7b5)" below has the loop's stores exactly (a near tree also gets the base LOD index, and the
    near list decides whose bones animate).
-   **The update time.** `FUN_140437d40` stores how long its last update took at `+0x7C`, in ms. The next update's
    budget is derived from it.
-   **In one load order:** at Riverwood the tree list was empty and the animation list held 2,562 nodes. `+0x80` was
    about 1e14 (no LOD distance in practice). `0x14200d640` was 1, so every LOD store is 0. The near cap
    (`0x14200d670`) was 40.

### Water reflections: the cube map

-   **Only the cube map draws geometry.** `TESWaterReflections::Update` (`0x140520570`, from `FUN_140644f60` /
    `FUN_140521c80` in `Main::Draw`, while the main cull jobs run) draws it for a reflection with the dynamic-cubemap
    flag. It draws 2 faces a frame, and all 6 under a menu, every `fCubeMapRefreshRate`. It then sorts the faces by
    their measured cost.
-   **Plane reflections are dead.** The constructor (`FUN_1405202b0`) zeroes their sorter and culling process, nothing
    renders one, and `Water.hlsl` samples only the cube map (t3) and SSR.
-   **The camera.** `BSCubeMapCamera` (constructor `FUN_1414ed1b0`, size `0x1C8`): roots at `+0x188` (count `+0x198`),
    two accumulators at `+0x1A0`/`+0x1A8`, a 90 degree frustum.
-   **Its roots** (`FUN_1405275e0`, the add-root) are only LOD land `*0x14315b898` and LOD objects `*0x14315b8a0`
    (which global is which is inferred), `gLODTrees`, and the Sky root, gated by `bReflectLODLand`,
    `bReflectLODObjects`, `bReflectLODTrees` and `bReflectSky`. A census saw exactly the Sky, LOD Trees and two unnamed
    roots, twice a frame. The `ShadowSceneNode` and the scene lists are never added.
-   **The cull.** The static `BSCullingProcess` (`0x14332bda0`) with cull mode 3, through `FUN_1414bf320`.
-   **The render modes:** `0x1B` with silhouettes, `0x19` without the sky, otherwise 0. Mode `0x19` registers only
    accumulation hints 6 and 7 (`FUN_1414b2b90`).
-   **The targets:** cube target 0 (`kREFLECTIONS`), depth 6.
-   **`Update` hides the player** and every water shape while a face draws, and gives the sky root `kAlwaysDraw`.
-   **The camera's place.** At the player's eye (`Actor::CalculateLOSLocation`, `kEye`), its far plane the main camera's.
-   **The face render** is `BSCubeMapCamera` vfunc `0x35` (`0x1A8`; `0x1414ed4e0`, a jump to `FUN_1414ed920`), called
    with a face mask, from `0x1405209c9`. Per face it:
    1.  binds the cube target's face (`FUN_140e4fe50(rtm, 0, 0, face, 0)`) and depth target 6, and clears;
    2.  sets depth mode 3 (test and write);
    3.  orients the camera to the face (`FUN_1414ed500`: the six cube rotations);
    4.  culls each root into the accumulator at `+0x1A0` (cull mode 3), with render mode `0x1B`, `0x19` or 0;
    5.  renders the accumulator (`FUN_1414a90f0(camera, accumulator, 8)`), with the raster cull mode at 2.

    There is no depth prepass: the face is one forward pass, colour and depth together.
-   **`Update`'s flags** (`+0x10`): bit 12 renders without the LOD roots (another root at `0x1431872c8`+0x100), bit 4 with
    silhouettes (0x1B); without bit 12 and `bReflectSky` the faces suppress the sky (0x19). The add-roots are at
    `0x14052080d` (LOD land, `0x14315b898`), `0x140520826` (LOD objects, `0x14315b8a0`), `0x14052083f` (LOD trees,
    `0x14315b880`) and `0x140520854` (the sky). It renders 2 faces an update (6 under a menu, `0x140fa37b0`), the last
    with clearRoots.
-   **A face's VS_PerFrame** (measured, 2026-10-09): the camera's matrices without jitter; c20 holds the projection's
    inverse and c24 the projection (FrameBuffer.hlsli's names are the other way round); c16
    (CameraPreviousViewProjUnjittered) and c41 (CameraPreviousPosAdjust, fDRClampOffset in w) are zero; c42-c44 the
    frame's (FrameParams, the dynamic resolution ratios).
-   CS Dynamic Cubemaps draws no geometry: it is a compute pass over the main colour and depth.

### Portals, rooms and occluders: the main camera's compound frustums

Reverse engineered 2026-10-09 (Ghidra, AE); DCLF's port is `Engine/PortalViews` (T5a).

-   **Who builds them.** `Main::Update` resets every occluder's state (`FUN_1401a4a00`: `+0x40` of the graph's and the
    rooms' occluders), then `FUN_1401a43c0(TES, list[0])` walks the ShadowSceneNode's portal graph (`ssn+0x228`) from
    the main camera into the one `BSPortalGraphEntry` every scene list's process shares (`+0x30190`). Lights
    (`FUN_1414a2530` -> `FUN_1414a6400`/`7150`, the light's own process at `light+0x128`) and the water process walk into
    their own entries. With no graph the entry is left as it was.
-   **The start.** Every room (`graph+0x40`, in order) whose `QPointWithin` (vfunc `0x3F`) holds the camera's near-plane
    centre (`eye + near * forward`) gets its own walk; only the first is flagged first (it clears
    `visibleUnboundSpace`). With none, one walk from unbound space. `entry+0x30` is that first room.
-   **The walk** (`FUN_1414a41e0`): a FIFO of rooms (null: unbound space, its portals the graph's list at `+0x28`).
    Each pop finalizes the room's frustum and, for each portal not yet crossed (keyed by its shared node, `+0x128`):
    the view-plane test (`BSOcclusionPlane::WithinFrustumDistFirst`, skipped when the frustum has `skipViewFrustum`),
    then the frustum's program on the portal's quad. The room beyond (`+0x118`, else `+0x120`) is queued, unless it is
    the start, and its frustum built from this one:
    -   new: AND(this frustum's copy, with each set the portal lies wholly inside, no edge clipped, turned off; the portal's
        set; the room's occluders). A parent of 26 operators or more is dropped (`skipViewFrustum` then);
    -   reached again: the stream reopened as an OR (`FUN_140e334c0`) and the new path ORed in, the parent whole.
-   **A portal's set** (`FUN_140e32710`): the quad's plane with the camera on its non-positive side, the four planes
    through the camera and each edge, slot 1 the zero plane; all six on, or none when the camera is within 5 units of
    the portal's plane. No near plane.
-   **Occluders.** Unbound space and the start room take theirs at the walk's start (`FUN_140e32920`): a plane when its
    quad meets the view, an occlusion box when the camera is outside it and it meets the view (`FUN_140e21750`). A room
    reached through a portal takes boxes (if they pass the parent's program) and planes only in state 1, which nothing in
    the walk sets: in the main camera's frustums the rooms' occlusion planes take no part. A plane is `8 s` (visible
    unless wholly inside its set); a linked pair of planes (`+0xF8..+0x110`, the neighbour accepted, the camera on the
    same side) is an AND/OR of four; a box is OR(not inside set 0, not inside set 1), its sets the silhouette's edge
    planes (the corners projected at 1/1024 pixel, a quickhull of at most 6) and its faces turned away, refined against
    the camera-relative frustum (`FUN_140e22220`).
-   **The program.** Builders write tokens (4 AND, 5 OR, 6 close, `7 s` portal test, `8 s` occluder test);
    `Finalize` (`FUN_140e33c70`) appends root, ACCEPT, REJECT and turns them into `{type, next if true, next if false}`
    with the jumps threaded to tests. Only the 5 and 6 tokens and the merge clear `finalized`. Evaluation is
    `BSCompoundFrustum::Process` (drawcall-limit-fix.md, "Roots with engine-drawn parts, culled by the engine").
-   **Who reads them.** `CalculateAndDrawShadowCasterLights` finalizes `entry+0x68` (unbound space) and deep-copies it
    into each list process (`+0x301A0`), without `skipViewFrustum`. `BSMultiBoundRoom::OnVisible` installs its room's
    frustum for its children; a room not in the map is not culled into at all (unless the cull mode is 1 or 3).
    `ShadowSceneNode::OnVisible` and `BSPortalSharedNode::OnVisible` build and use the portals' shared-node frustums
    during the jobs.
-   **State on the shapes** the walk reads and writes: corners (recomputed when dirty), each quad's edge-clipped flags
    (`+0xF0..+0xF3`: whichever test ran last, across frames), the per-frame state, a box's silhouette sets. Frustums
    from the static pool (`0x143284d00`, 32 slots) are not reset when taken: a portal's shared-node entry can hold stale
    operators.
-   **Plane states written outside the walk** (2026-10-22). The walk never writes a plane's state: `FUN_140e32920` writes only a
    box's, and `FUN_140e32860` only a box's too. The graph's other occluder tests do, lazily, after `Main::Update`'s reset:
    `FUN_1414a8ba0` (and its siblings `FUN_1414a8d30`, `FUN_1414a8ec0`), called from `HighActorCuller::Operate` and
    `FUN_140524a30`, judge each graph occluder still at state 0 with their own tests (a plane: `FUN_140e23920` with the camera's
    position and `FUN_140e242d0`; a box: `FUN_140e21750` then `FUN_140e21090`), write `(ok ^ 1) + 1`, and leave a judged plane's
    edge flags from their own test (`FUN_140e233b0`). A walk that runs after them takes a state-1 plane untested (with those
    flags) and skips a state-2 one, so unbound space's compound frustum depends on the order of the frame's jobs. The actor
    culler's test has no rectangle rescue: `FUN_140e23920` is `FUN_140e164e0` on the corners relative to the camera
    (`camera +0xA0`) against the camera-relative frustum (`0x1432846e0`), and `FUN_140e242d0` passes a plane with `+0xF4` set,
    else only with the camera on the negative side of the plane through corners 0, 1, 2 (the disassembly: true only when
    `dot(n, eye) - d < 0`; the decompiler shows otherwise). DCLF judges every graph plane that way at the start of its walk
    (`PortalViews::PlaneJudged`), and a start room's untested planes too; its portal parity rebuilds the program as the engine's
    walk built it (the engine's states and flags, `WithinFrustumDistFirst` for the rest) before counting a difference.
-   **Writers of a node's collision object and LOD type outside the hooked functions** (2026-10-23, `CS_DCLF_MIRROR_WATCH` with
    callers). `bhkWorld::InitHavok` (`0x140e86ad0`, a virtual at `+0x1B0`; `Hazard::InitHavok` calls it) moves a collision object
    from its second argument to its third: `SetCollisionObject` on the third, then the second's `+0x40` released and cleared inline
    (`0x140e86fb7`, `0x140e870c6`). The LOD type (`+0x153 & 0xF`) has 36 byte stores in the executable; outside the hooked
    `SetLodType`, LOD level and cell placements, and the constructors and `CreateClone`: Papyrus `Disable` with a fade
    (`FUN_140a3db90`, from `papyrus::ObjectReference::Disable` and `DisableNoWait`) sets 9 (`& 0xF9 | 9`) before queueing its
    `DisableFunctor`, and the script command `Enable` (`0x14034c9f0`) resets a 9 to the model type (`TESForm::GetModelType`).
-   **`WithinFrustumDistFirst`'s rectangle test uses the wrong axes** (`0x140e23e10`, 2026-10-23). The corners (`BSPortal::sub`,
    `0x140e230c0`) are `centre +- hx * column 0 +- hy * column 2` of the rotation at `+0x1C` (row-major); the test of the camera's
    foot point on the plane against the rectangle projects onto rows 1 and 2 (`+0x28..+0x30` against `hx`, `+0x34..+0x3C` against
    `hy`). For a plane turned only about the vertical by θ, the horizontal coordinate is `offset * sin 2θ`: an axis-aligned plane's
    rectangle is an infinite horizontal band at its height. Both uses are affected: the 5-unit test (a portal kept, an occlusion
    plane dropped: only conservative) and the rescue after a failed frustum test, which accepts a plane wholly outside the view
    whose foot point is ahead of the camera (along view plane 0's normal) and in the band. A plane so accepted failed
    `FUN_140e164e0` by its early exit, which writes no edge flags, so it keeps the last test's: with all four clipped, its occluder
    set is the quad's plane alone, and everything behind that infinite plane is culled.
-   **A shadow light's rooms** (`FUN_1414a2530` -> `FUN_1414a6400`; `CalculateActiveShadowCasterLights` walks a light only
    once kept, after testing its entry: the test reads the light's last walk). Nothing with no graph rooms, the NiLight
    hidden or its fade (`+0x134`) under 0.05, or the light on an object node (`BSLight +0x130`): the entry keeps what it had
    (the constructor's: `visibleUnboundSpace` 1, no rooms). Otherwise the entry is cleared, then: not portal-strict
    (`+0x47`), unbound space and every room whose bound meets the light's sphere (`CheckBound2`, vfunc 0x41); portal-strict,
    `Traverse` (frustums for a shadow light) from every room holding the camera's near point, each with `isFirst` 1, else
    from unbound space. The light's room process (`+0x128`) is a `BSParabolicCullingProcess` with its own orthographic
    camera at the light (frustum `-1 1 1 -1`, near 0.1, viewport `0 1 1 0.5`; the walk puts the NiLight's rotation and
    position on it), no view planes (mask 0), and a portal view test (vfunc 0xD8, `0x14151a120`) of the portal's sphere
    {centre, larger half extent} against the light's radius (`+0x30224`), with `+0x30200` set passing either side of the
    light's plane. The walk shares the shapes' occluder states and edge flags with the main camera's.

### Point lights' shadow culls

-   **`BSShadowParabolicLight::Accumulate`** (`0x14151b960`) fills a descriptor for `BSCullingProcess::Process`
    (`FUN_1414bf320(desc, mode, 0)`):
    -   the accumulators, the camera, the cull mode (`portalStrict + 3`), the radius;
    -   `+0x5C = 1`, which selects the static `BSParabolicCullingProcess` (`0x14335bfa0`; otherwise the static
        `BSCullingProcess`, `0x14332bda0`);
    -   either its root list (`sceneAccumArray`, mode 1, the list path `FUN_140e28f70`), or with none the scene's object
        root (`ShadowSceneNode` child 3, mode 0, the process's `Process` vfunc `0xB8`).
-   So a point light without its own list culls every loaded reference.
-   **`BSParabolicCullingProcess::Process1`** is `0x141519a90` (vtable slot `0x16`).
-   **`BSShadowDirectionalLight::Accumulate`** (`0x141511c80`) culls each cascade through `FUN_1414f0920`, with the
    static `BSCullingProcess` over its full-frustum processes' `objectArray`.
-   **The list path:** `FUN_140e28f70(process, list, camera, skipHidden, jobs)`. It calls `Process` (vfunc `0xB8`) on
    the first entry and `Process1` (`0xB0`) on each other one, skipping hidden entries when asked. A light reaches it
    with a `sceneAccumArray` (`+0x528`); skipHidden is `desc+0x61` and jobs is 0.
-   **`BSParabolicCullingProcess::Process1`** (`0x141519a90`), cull mode 3:
    -   a `kAlwaysDraw` node is visited untested;
    -   any other goes through `FUN_141519fe0`. It runs `FUN_14151a1e0(process, &worldBound)` and, when visible, stores
        the result at `+0x301F4` and calls `OnVisible`.
    -   `FUN_14151a1e0` is pure. It tests the bound against the light's sphere (centre `+0x30218`, radius `+0x30224`):
        visible when `(|c - centre| - r) - radius < 0`. It then tests the half-space `+0x30208`/`+0x30214`, with the second
        hemisphere's process at `+0x30200`, but only to say which hemisphere: a bound inside the sphere is visible either
        way (with no second hemisphere, only the degenerate `r == 0` on the plane is culled). So the test is the sphere.
    -   `kAccumulated` is set on a visited node when `+0x11D` (`updateAccumulateFlag`) is set: clear for point lights.
    -   Its other modes (cull mode `+0x30198`): 1 (`kAllPass`) visits untested, 2 (`kAllFail`) culls. A node with flag bit 12
        (unless `ignorePreprocess`) is culled when bit 20 is set, else visited untested. A `kAlwaysDraw` node is visited
        untested. With a compound frustum (`+0x301A0`, its `freeOp` `+0xBC` not 0) and a mode other than 3, a node must pass
        `FUN_14151a1e0` and `BSCompoundFrustum::Process` (the portals), with the active plane state saved and restored
        around it.
    -   The process is static and every point light's: its sphere is the current light's only during that light's
        `Accumulate`. The light's mask bit is its accumulators' `+0x164` (`+0x160` the shadow-light count + 1).
-   **`BSMultiBoundNode::OnVisible`** (`0x140e2d710`):
    -   it tests its multibound only when the node is not `kAlwaysDraw` and the cull mode is neither 1 nor 3 (mode 4
        fresh; other modes cached per frame on the shape);
    -   with `cullingMode == kAllPass` and a cull mode other than 3, it culls its children under mode 1 (no tests);
    -   under mode 3 it is a plain `NiNode`'s. Exterior cells are `BSMultiBoundNode`s; the object root is a `NiNode`
        with `kAlwaysDraw`.
-   **A portal-strict light** (`BSLight::portalStrict`, `+0x47`) culls with mode 4, and any other light with mode 3.
-   **`NiNode::OnVisible`** (`0x140d1e2c0`, `VTABLE_NiNode` slot 0x34): when the node's bound radius is not 0 or it is
    `kAlwaysDraw`, `NiAVObject::Cull(child, process, arg)` on each child. `BSFadeNode::OnVisible`, `NiBillboardNode`'s and
    `ShadowSceneNode`'s call it directly. `NiAVObject::Cull` (`0x140d1c570`) skips a hidden object (flag bit 0), else
    calls the process's `Process1` (vfunc `0xB0`).

## Engine side effects of the culls (T7a)

Reverse engineered 2026-10-11 (Ghidra, AE 1.6.1170) for DCLF's T7 stage, which stops the engine's scene culls
(`DrawWorld_BuildSceneLists`, the list jobs' `Process1`, the registrations). Method: every instruction in `.text` that
tests, sets or clears bits 20, 22 or 26 of `NiAVObject::flags` (`+0xF4`), as an immediate on the field or on a register
loaded from it, plus every reference to `gFrameCounter` (`0x14328cc6c`). Items marked *unverified* were not confirmed.

### Writer/reader table

| State | Writer (address, function) | Reader (address, function) | Meaning | If the scene cull stops |
| --- | --- | --- | --- | --- |
| `kNotVisible` (bit 20), actor 3D roots | `0x140729ba3`, `0x140729bb2` `HighActorCuller::Operate` (no 3D bound, or the occluder test `FUN_1414a8ba0` fails); `0x1414e9af0` set / `0x1414e9b02` clear `BSFadeNodeCuller::Process1` (6-plane frustum; also sets bit 12, preprocessed). Run from `FUN_1406452c0` (Main::Draw's camera setup, `0x1406453db`) over the high actors, gated by `bPreCullActors:General` (`0x142012a18`) | `0x1406a37b4` `Actor::ModifyAnimationUpdateData`; `0x14069eaab` `0x14069ea90` (Ghidra names it `Actor::UpdateFadeSettings`; it sets `bhkCharacterController` flag `0x40000`); `0x14077105b` `FUN_140771000` (`Job_Ragdoll_post_physics`: IK/look-at only when visible); `0x140776744` `UpdateRagdollPostPhysics`; `0x140771670` `FUN_140771520` (actor update: detection/process update gated on visible or `Main::IsRoomVisible`); `0x1407644db` `FUN_140764310` (VATS); `0x14064d481` `FUN_14064d3b0` (visible high actors sorted by distance, same Main::Draw step); `BSCullingProcess::Process1` (`0x140e28390`) and `BSParabolicCullingProcess::Process1` (a bit-12 node is culled when bit 20 is set, without a test); `0x1414c95c8` `FUN_1414c9560` (*unverified*: picks a random visible node) | the actor pre-cull's "off screen" | **nothing**: not a scene-cull write. It needs the main camera (`Process1`'s frustum set up in Main::Draw before the lists) and the occluder states that `Main::Update` resets |
| `BSShaderProperty::lastAccumulatedFrameCount` (`+0x80`) | `0x1414b2aef` `FUN_1414b2a60` (render modes `0xC`-`0x11`: the depth prepass accumulator `0x14338c828` and the shadow maps); `0x1414b2924` `FUN_1414b2810` (modes `0x12`, `0x13`); `0x1414b27bc` `FUN_1414b2790` (mode `0x17`). The main modes (`FUN_1414b2330`, 0-11) do not write it | `0x1406a37ef` `Actor::ModifyAnimationUpdateData` (only for a `kNotVisible` root without race flag bit 7: visible when `gFrameCounter - skin property +0x80 < fMaxFrameCounterDifferenceToConsiderVisible:Animation`, default 0.0667, so in effect equal; the skin property is `MiddleHighProcessData +0x178`); `BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask` (`0x1414af0fb`) and `BSGrassShaderProperty::GetRenderPasses_LocalMap` (`0x1414afb9a`): a stale stamp empties the property's pass queue (`FUN_14147b7e0`) | "this property was registered this frame" | the depth prepass's registration stops stamping, so an off-screen actor that still casts shadows keeps the stamp only from the shadow registrations; the actor branch is reached only when `kNotVisible` is set, which DCLF does not change. The pass-queue reset is self-contained (no registration, no `GetRenderPasses`) |
| `kRenderUse` (bit 22) | set: `0x1414a30b8` `BSGeometry::sub` (`0x1414a2fc0`), `0x1414a335c` `FUN_1414a31f0`, `0x1414a344c` `FUN_1414a33e0` (the `ShadowSceneNode` light assignment, after `FUN_1414fcea0` finalizes the geometry's light data), `0x140dd45a5` `BSStripParticleSystem::Do_UpdateSystem`; kept/cleared: `0x140d47c6d`/`0x140d47c73` `NiGeometry::UpdateWorldBound`, `0x140d37ef1`/`0x140d37ef7` `BSDynamicTriShape::UpdateWorldBound` (cleared when the bound moved) | those two `UpdateWorldBound`s; `0x1402cf1ec` `FUN_1402cf080` (on a fade root, from the managed-nodes update `FUN_1402b41a0`: re-place the reference in its cell, `FUN_1402c43f0(cell, ref, 1, 0)`, then clear at `0x1402cf23f`) | geometry: "light assignment valid for this bound"; fade root: a re-place request (*unverified*: no fade-root setter was found) | **nothing**: no cull writes it |
| `kAccumulated` (bit 26) | set/clear: `BSCullingProcess::Process1` (`0x140e2854b`/`0x140e28560`, `0x140e2863a`/`0x140e28654`, compound-frustum path), `FUN_140d3ff10` (`0x140d3ffec`/`0x140d40001`, the frustum test), `NiCullingProcess::Process1` (`0x140d40160`/`0x140d40189`), `BSParabolicCullingProcess::Process1` and `FUN_141519fe0` (only with `updateAccumulateFlag`); set on every listed root by `DrawWorld_BuildSceneLists` (`0x14064bef9`, `0x14064c16a`) | `0x140438316` `FUN_1404381e0` (tree wind clock); **`0x1402cff98` `FUN_1402cff60`**, the animated-reference visibility test used by `UpdateAnimationJob` (`0x1402c73f6`) and the managed-nodes update (`FUN_1402b41a0`, `0x1402b4230`; `FUN_1402b52e0`); **`0x141509571`/`0x1415095d2` `FUN_141509520`, `0x141509646` `FUN_141509620`, `0x14150a87e`, `0x14150a9b5`, `0x14150aa1f` `FUN_14150a840`**, the light receiver gathering under `FUN_1414a2530` | "passed the last cull" | animated references (controllers, managed nodes) stop updating unless fading; **dynamic lights** (`BSLight +0x46`) stop collecting receivers; trees' wind stops |
| `BSMultiBoundShape +0x10` (cull result 0 stale, 1 in, 2 out) and `BSMultiBoundNode::lastAccumTime` | `BSMultiBoundNode::OnVisible` (`0x140e2d710`), any process with cull mode other than 1 and 3 | the same frame's later culls; `FUN_140437e50` (`0x140437e50`, tree manager in `Main::Update`): a tree is visited only when its multibound's result is 1 | per-frame multibound cache | the tree list's distance/LOD loop skips every tree (the list was empty at Riverwood) |
| Portal graph entry (`list process 0 +0x30190`) | `FUN_1401a43c0` (`Main::Update`, the walk); `DrawWorld_BuildSceneLists` (`BSPortalGraphEntry::ClearVisibility` with no graph, and `AddVisibilityToMap` for each crossed portal's shared node) | `Main::IsRoomVisible` (`0x140649f20`) from the managed-nodes update (`0x1402b4268`), `Actor::UpdateRefLight` (`0x1402e1282`) and `FUN_140771520` (`0x140771687`); the shadow caster selection (`CalculateActiveShadowCasterLights`, `FUN_140e14300` on it); the scene list jobs | "which rooms and portal shared nodes the main camera sees" | the shared-node entries stop being added: `IsRoomVisible` of a `BSPortalSharedNode` turns false, and references under it stop animating. The room walk itself is `Main::Update`'s |
| Fade/LOD state (`+0x130`, `+0x13C`, `+0x140`, `+0x144`/`+0x148`, `+0x14C`, `+0x152`/`+0x153`, flags 14/15/27) | the three `OnVisible`s ("Fade state"); also **`FUN_1402cff60`**, which calls `FUN_14147a160` itself for a non-accumulated, unsettled fade root, and the placement snap `FUN_14147aa20` | `GetRenderPasses` (`+0x144`), the Lighting shader's fade, `FUN_1402cff60` (`+0x130 > 0`), `HighActorCuller::Operate` (actor root `+0x130`, flag bit 2 of `Actor +0x274`), `BSTreeNode` LOD (`+0x153`, `+0x14C`), the fade and LOD functions themselves (`+0x13C`) | fade and leaf/tree LOD | fades freeze; animated references then re-run the fade update from `FUN_1402cff60` |
| Light receivers (`BSLight` lists, the geometry's light data and `activeLightMask`) | `FUN_1414a1540` per `ShadowSceneNode` from `FUN_1414a2170` (Main::Draw, after the list build, before the cull jobs) and `CalculateActiveShadowCasterLights` (`0x1414cc7c9`), through `FUN_1414a2530` → `FUN_1414a7150` / `FUN_1414a6400` | the main and shadow registrations, the Lighting shader | a dynamic light takes a geometry only when it is `kAccumulated`, `kAlwaysDraw` (bit 11), or the light is on an object (`+0x130`): the previous frame's cull | dynamic lights light nothing the engine still draws |

### `Main::IsRoomVisible` and its callers (T7b3)

`Main::IsRoomVisible(main, node)` (`0x140649f20`, RELOCATION_ID 35608/36618) returns true in these cases:

-   no node;
-   the node is neither a `BSMultiBoundRoom` nor a `BSPortalSharedNode` (two `GetRTTI` calls);
-   there is no portal-graph entry (`*0x14338c8a0 +0x30190`, the scene lists' first process).

Otherwise the answer is the entry's visibility map (`FUN_140e14460`, a lookup under the read lock `0x1432846c8`). A
shared node that is not in the map is also visible when its portal (`+0x140`) has its entrance or exit room
(`+0x118`, `+0x120`) in the map.

**What fills the map.** The walk (`FUN_1401a43c0`, `Main::Update`, after the actor updates) clears it
(`ClearVisibility`) only when there is a graph. `Traverse` (`FUN_1414a41e0`) then adds each room it reaches, and each
portal it crosses as the portal's shared node, with an empty frustum. `DrawWorld_BuildSceneLists`, in `Main::Draw` in
the same frame, adds the shared node of every portal of each room on `multiBoundRoomAccumList`
(`BuildSharedCompoundFrustum`), so nothing arrives a frame late. Before the lists those shared nodes answer through
their portal's room. With no graph, the lists clear the map instead.

**Callers.** These are the only direct calls; the other references are chained unwind entries. Each passes the parent
(`+0x30`) of a reference's 3D.

-   `0x1402b4268` in `FUN_1402b41a0`, the managed-nodes update: `UpdateManagedNodesJob` and `UpdateAnimatedRefsJob`
    from `Job_Cell_animations`. It is reached only after `FUN_1402cff60` passes, and false skips the node's update.
-   `0x1402e1282` in `Actor::UpdateRefLight`, a virtual function: false skips the reference's light extra.
-   `0x140771687` in `FUN_140771520`, the actor update: only when the 3D is not `kNotVisible`; it feeds the
    detection and process update gate.

### Routes that do not go through the scene lists

-   **`ShadowSceneNode` children** (set in the world's init, `FUN_140643060`, `SetAt` vfunc `0x1E0`): 0-3 there, 5 the
    `BSParticleSystemManager` (`0x140643b23`), 6 the grass root (`GrassManager::ctor`, `0x1402010c0`, `kAlwaysDraw`).
    The lists take children 0 and 1 and child 2's first four children (the extra list) and child 3 (the object
    root). **Grass is in the lists:** children 6 and 7 go into the extra list (`0x14338c888`, at `0x14064c51c` and
    `0x14064c54e`). **Child 5 (the particle manager) is skipped:** the master particle systems' main cull is the portal
    graph's always-render list (below).
-   **Water** is in the lists (cell category 6, and the water system's node in list 0 for interiors). Its reflections
    use the water system's own processes (`FUN_140644f60`, `0x14338c648`).
-   **First person** has its own cull (`Main::unk_FPCulling`, `0x140645d30`: process `0x14338c640`, accumulator
    `0x14338c820`, the first-person root unhidden around it).
-   **`BSFadeNodeCuller::Process1`** and **`HighActorCuller`** are separate cullers and keep running.
-   **Not written by any cull:** `kRenderUse`, `kNotVisible`, face morphing, the skin palette.

### The master particle systems (AE 1.6.1170, Ghidra, round 3 phase 5)

-   **Ownership: one `BSMasterParticleSystem` per addon node type** (`MPSTorchFire01.nif`, `MPSCandleFlame01.nif`, ...), shared by
    every emitter of that type, not pooled. A reference's addon node is a `BSValueNode` (value `+0x12C`, the `BGSAddonNode`).
    `FUN_1402df8a0` / `BSValueNode::sub` (`0x140256700`), for an addon with the master flag (`+0x7A` bit 0), clears the node's
    `kHidden` and registers it with `BSParticleSystemManager::sub` (`0x140e2a2f0`) at the master index (`addon +0x68`): appended to
    the master's **emitter list** (`master +0x128`, count `+0x138`, a reference taken; `FUN_140e36150`) when it is loaded, otherwise
    queued on the manager's pending loads (`+0x128`; `FUN_140256390` requests the model, `FUN_140e2aa30` queues it with the cap
    `addon +0x78` and "always loaded" `addon +0x7A` bit 1). The value node holds the master at `+0x130`.
-   **Creation.** `Job_Post_reset` (`0x1406d3750`, the post-render stage's last job) runs `FUN_1402c74e0` → `FUN_140e2a6c0` (spin
    `0x143284b98`): each finished load's model root, **uncloned** (the NIF's `BSMasterParticleSystem`), its markers hidden
    (`FUN_140e2b000`), `+0x166` removable, `+0x158` index, `+0x164` cap, attached by the manager's `AttachChild` (vfunc `0x1A8`), the
    queued value nodes added as emitters.
-   **The manager** (`0x143284b88`) is `ShadowSceneNode` child 5 (`SetAt` at `0x140643b4d`, `kAlwaysDraw` at `0x140643aff`).
    `BSParticleSystemManager::AttachChild` (`0x140e29f90`, write lock `0x143284b90`) takes only a master with an index, grows the
    array and stores it at `children[index]` with the plain array set (`FUN_140d1ee40`); **it never sets `parent`**. The master lists
    its particle systems at `+0x140` (count `+0x150`).
-   **Cull.** `FUN_1402b7620` adds child 5 to a cell's portal graph's **`alwaysRenderChildren`** (`BSPortalGraph +0x58`, count `+0x68`;
    `FUN_140e152b0` at `0x1402b76bb`), from a cell's attach (`FUN_1402b7280`, interiors) and the sky cell's graph (`FUN_14019d210`,
    exteriors). List job 0 (`FirstListAccumulationJob`, `0x1414cc260`) culls that list (`0x1414cc308`, `FUN_1414bf320`) when the
    entry's `visibleUnboundSpace` (`+0x131`) is set, before the extra list: `NiNode::OnVisible` on the manager, each master (a bound
    test), its emitter nodes and particle systems. With `visibleUnboundSpace` clear (an interior room) the route is not traced
    (`ShadowSceneNode::OnVisible` culls children 4+ on its portal path, `0x1414a03b0`).
-   **Registration.** `DoSceneListAccumRegisterJob` (`0x1414bf7a0`) into the main accumulator (`0x14338c830`) only; a particle
    system's pass hint is 1, so `FUN_1414b2330` inserts it at `0x1414b2478` (the hint-1 list insertion `FUN_1414f50b0`). Not in main
    depth or any shadow view.
-   **Update.** `Job_Particles` (`0x1406d31a0`, the "render 1st person" stage, job list 10), under the manager's read lock, queues
    one `UpdateParticleSystemManagerJob` (`0x1402c7470`) per slot below `freeIdx` (`+0x122`): `FUN_140e2a520` → the master's
    `UpdateDownwardPass` (`0x140e35f40`): emitters pruned (`FUN_140e36630`: a value node only the master holds, or hidden; `+0x160`
    active = min(count, `+0x162`)); with none and no live particle it returns; else its clock (`+0x15C`) and each child's
    `NiParticleSystem::UpdateDownwardPass`. The particle data is rewritten every frame on workers, whatever the cull
    (`kAccumulated` is not read).
-   **Removal.** `FUN_140e2a520` drops a master that is removable, has **refcount < 3** (the array and the model's handle, no emitter)
    and no live particle (`0x140e2b430` == 0), with the plain array set at **`0x140e2a589`** (`FUN_140d1ee40(children, index, null)`),
    on the job thread: no vfunc, so no DetachChild override sees it. The world reset clears the whole array (`FUN_140648cd0` →
    `FUN_140d1d630(manager)` at `0x140649254`); `FUN_140e2a470` (cell changes) only resets the particles.
-   **Order.** Both stages finish inside the frame cycle (`FUN_1405fc210` at `0x14064694d` waits for all stages), so neither runs
    at the next frame's `BeginFrame` (`0x140644583`). *Unverified:* that the post-render stage cannot overlap Present.

### What DCLF must produce before the culls stop

1.  `kAccumulated` on every root and geometry it found visible last frame (the GPU visibility readback), or at least
    on animated references, trees and the receivers of dynamic lights.
2.  The `BSPortalSharedNode` entries of the main portal graph entry (`AddVisibilityToMap`), or a hook on
    `Main::IsRoomVisible` answered from `PortalViews`.
3.  The multibound cache result for tree multibounds (or `FUN_140437e50`'s list ported).
4.  Fade/LOD write-back for every root, and a check that `FUN_1402cff60`'s second fade update does not double-step a
    root DCLF fades.
5.  Light receivers for dynamic lights if any engine-drawn residue or engine reader of the lights' lists remains.

### T7b0: who sets kAccumulated, where the readers look, and when they run

Reverse engineered 2026-10-11 (Ghidra and capstone, AE 1.6.1170). It corrects two T7a rows: the receiver gathering also
reads the bit in `FUN_14150a610`, and `IsRoomVisible` of a shared node falls back to its rooms.

-   **Which processes set the bit (`updateAccumulateFlag`, `NiCullingProcess +0x11D`).** The base constructor
    (`FUN_140d3fc90`) stores the dword `+0x11C` = 1, so the flag starts false. Only three places write the byte:
    -   `FUN_14063eb50` (scene creation) at `0x14063f0be` (`mov word [rbx+0x11C], 0x101`), on every scene-list process
        (`DAT_14338c8a0[i]`, the main camera's `BSGeometryListCullingProcess`es). Their `Process1` sets and clears the
        bit on every node it reaches: the listed reference roots, their child nodes, and the geometry.
    -   The same function at `0x14063f27e`, on the water system's process `0x14338c648` (the main camera, which Main::Update
        sets up). `FUN_140644f60` -> `FUN_140521c80` culls the water shapes with it.
    -   `FUN_1414bf9f0` (from `FUN_1414bf320`/`FUN_1414bf510`) copies the cull context's `+0x62` into the static processes
        `0x14332bda0`/`0x14335bfa0` (mode 0). The context constructor `0x1414bf2b0` zeroes `+0x62`, and none of the
        17 callers sets it. Mode 1 uses the context's own process: list process 0 in `FirstListAccumulationJob`, and
        the sun's processes in `FUN_141511f30`, which keep the default.

    So the bit is set by **the main camera's scene-list culls, the water cull, and BuildSceneLists**. These do not set
    it: the sun (`FUN_141511f30` writes only `+0x11C`, at `0x14151202b`), point-light parabolic culls
    (`BSShadowParabolicLight::Accumulate`, mode 0), first person (`0x14338c640`, only `+0x11C`, at `0x14063f307`),
    the cube map, precipitation, the local map (`FUN_140972590`, mode 0), `BSFadeNodeCuller` and `HighActorCuller`.
    `DrawWorld_BuildSceneLists` sets the bit only on the cell roots (the object root's children, or the interior's
    `cell3D`: `0x14064bef9`, `0x14064c16a`), never on a reference root.
-   **Where each reader looks.**
    -   `FUN_1402cff60(obj)`: `obj` is the reference's 3D root, `ref->Get3D()` (vfunc `0x380`; `0x1402e5770` reads
        `loadedData +0x68`), in `UpdateAnimationJob` (`0x1402c73f6`), `FUN_1402b52e0` and `FUN_1402b41a0`. The fade
        node is `obj->AsFadeNode()` (vfunc `0x28`), so it is the same root, or null when the root is not a `BSFadeNode`.
        The logic:
        1.  Fade `+0x109 & 0x24`: true.
        2.  Root bit 26 set: true, unless the fade node's `currentFade` (`+0x130`) <= 0.
        3.  Otherwise, with root bit 29 set: true only when the fade node's bound radius (`+0xF0`) is 0.
        4.  Otherwise, a fade node with radius != 0 and fade flags bit 15 clear (not kIgnoreFade) runs
            `FUN_14147a160(fade, fadeAmount, main camera)` and returns `currentFade > 0`.
        5.  Anything else: false.

        `FUN_1402b41a0` also asks `Main::IsRoomVisible(root->parent)` unless fade `+0x109` has bit 2 or bit 5 (it clears
        bit 5 afterwards). It skips roots whose parent is the cell's Dynamic category node (`cell3D` child 4).
        `UpdateAnimationJob` and `FUN_1402b52e0` skip actors (form type `0x3E`) and trees (base form type `0x26`). They
        iterate the loaded cell's reference array (`loadedData +0x40`, count `+0x50`) and call `UpdateAnimation(delta)`
        (vfunc `0x3E8`, `FUN_1402f75a0`). `FUN_1402b41a0` calls the root's vfunc `0x168` (the selective downward
        update) instead.
    -   The tree wind clock (`FUN_1404381e0`, `0x140438316`) reads each `BSLeafAnimNode` in the manager's animation list:
        a node inside the tree's model, not the reference root.
    -   The receiver gathering reads **every level**, for a dynamic light (`param_4`) that is not on an object
        (`+0x130`):
        -   `FUN_141509520`: the node it is given, then that node's geometry;
        -   `FUN_14150a840`: the `ShadowSceneNode` children's multibound children and their children;
        -   `FUN_14150a610` (`0x14150a7c2`): each child before recursing into it. Missed by T7a.

        A geometry is collected only when the bit is set on it and on every node above it, from the cell root down. A
        `kAlwaysDraw` node (bit 11) passes without it.
-   **Order within a frame.** The serving thread (`JobListManager::ServingThread::Func2`) runs the stage job lists that
    `SetupJobLists` builds from the table at `0x142011920` (`{function, name}` pairs), in order. Their `JobListEnd` entries
    synchronise with the main thread through `BSThreadStagesManager` events (group 0: the jobs'; group 1: the main
    thread's; all reset at `Main::Update`'s start). The stages are `Main update cells`, `update player`,
    `prepare render`, `render scene`, `render 1st person`, `render end` and `post render`. In Main::Update:
    1.  `0x14064613f` `FUN_1405fc180` starts the cycle, only when not in a menu or paused. `Main update cells` runs `Job_TES_animation`
        (`UpdateAnimationJob` batches on job list 7) on workers.
    2.  `0x140646176` the tree manager `FUN_140437d40` (`FUN_140437e50`, then `FUN_1404381e0`), on the main thread,
        concurrently with step 1.
    3.  `0x140646187` `WaitForJobTask(0, 0x18)`: `UpdateAnimationJob` is done. **It never overlaps Main::Draw.**
    4.  `0x1406466cf`-`0x1406466f9` main events 1-4: `update player` and then `prepare render` run. Their actor
        animation (`ProcessActorAnimSG` -> `FUN_140771520` -> `IsRoomVisible`) waits on no later main event.
    5.  `0x1406467f1` the occluder reset, then `0x14064685f` the room walk `FUN_1401a43c0`.
    6.  `0x140646937` `FUN_140643c00` -> `FUN_140644360` -> Main::Draw (`0x14064447d`):
        -   `0x140644583` `FUN_1406452c0` (DCLF's BeginSceneFrame, `+0xD3`);
        -   the `BuildSceneLists` job (queued `0x1406445ff`, begun `0x14064461a`, finished `0x14064462e`), with the
            water cull `FUN_140644f60` beside it;
        -   `0x140644644` `FUN_1414a2170`, the light assignment;
        -   `0x140644674` `CalculateAndDrawShadowCasterLights`: the sun cull (`0x1414cbcb6`), then **main event 5**
            (`0x1414cbcd4`), then the primary's list cull jobs (`First`/`ListAccumulationJob`, begun `0x1414cbf3c`,
            alongside `CalculateActiveShadowCasterLights` `0x1414cbf41`, finished `0x1414cbf4d`).

        `render scene`'s `Job_Wait_For_Shadow_Culling` (`0x1406d2800`, `WaitForJobTask(1, 5)`) releases
        **`Job_Cell_animations`** (`FUN_14019c0f0`). That runs `UpdateManagedNodesJob` (exteriors) or
        `UpdateAnimatedRefsJob` (interiors), then `FUN_1402b3d90` -> `FUN_1402b41a0`, on workers, concurrently with the
        primary list culls and the rest of Main::Draw.
    7.  `0x14064694d` `FUN_1405fc210` waits for the whole cycle, so no stage crosses into the next frame.
-   **Which frame's bit each reader sees, and the earliest main-thread write it sees in the same frame:**

    | Reader | Sees | Write before |
    | --- | --- | --- |
    | `UpdateAnimationJob` | frame N-1's culls | `0x14064613f`. A write between that and `0x140646187` races the workers; a write at BeginSceneFrame is seen in frame N+1, the engine's own lag |
    | Tree clock `FUN_1404381e0` | frame N-1's | `0x140646176` (main thread) |
    | Light assignment `FUN_1414a2170` | cell roots N's (BuildSceneLists), everything below N-1's | `0x140644644`: BeginSceneFrame works, before the list job starts |
    | Shadow-light gathering in `CalculateActiveShadowCasterLights` | mixed: it runs alongside N's list culls | `0x1414cbf41`: BeginSceneFrame works |
    | Managed nodes (`FUN_1402b41a0` via `Job_Cell_animations`) | mixed N-1/N: from event 5, alongside the list culls that rewrite the roots | `0x1414cbcd4`: BeginSceneFrame works |

    One write at Main::Update's entry (before `0x14064613f`) reaches every reader in the same frame. BeginSceneFrame
    reaches every reader but the first two, which see it one frame later, as they see the engine's culls. The engine
    writes the flags word with plain read-modify-writes on job threads (`Process1`). A write while a list cull or
    `Job_Cell_animations` runs can lose bits, or make the cull lose them.
-   **`Main::IsRoomVisible` (`0x140649f20`).** It is true for a null node, or any node that is not a `BSMultiBoundRoom`
    or `BSPortalSharedNode`, or when list process 0's entry (`*DAT_14338c8a0 +0x30190`) is null. Otherwise it looks the
    node up in the entry's visibility map (`FUN_140e14460`: key a node pointer, value a compound frustum, under the
    global lock `0x1432846c8`). A shared node that is not in the map is still visible when its portal (`+0x140`) has an
    entrance (`+0x118`) or exit (`+0x120`) room in the map.
    -   **Who inserts.** The room walk (`FUN_1414a41e0`, from `FUN_1401a43c0`) inserts the rooms: the start room, and
        each room reached through a portal. BuildSceneLists (`0x14064c432`, through `BuildSharedCompoundFrustum`,
        `0x1414a4b1d`) inserts the shared node of every portal of a room in `multiBoundRoomAccumList`. So do
        `ShadowSceneNode::OnVisible` (`0x1414a0606`) and `BSPortalSharedNode::OnVisible` during the culls. Because of the
        fallback, losing BuildSceneLists' inserts leaves `IsRoomVisible` of a shared node true while an adjacent room
        is visible (T7a's "turns false" is wrong). Rooms come only from the walk, which `Main::Update` keeps running.
    -   **Callers.**
        -   The managed-nodes update (`0x1402b4268`, workers, after event 5): it sees this frame's walk and BuildSceneLists.
        -   `FUN_140771520` (`0x140771687`, from `ProcessActorAnimSG` and `FUN_1407394b0`, `prepare render` workers): not
            ordered against the walk at `0x14064685f`. It can see the last frame's map or a partly rebuilt one.
        -   `Actor::UpdateRefLight` (`0x1402e1282`, virtual): thread *unverified*.
    -   **`FUN_140e14300(a, b)`** (`CalculateActiveShadowCasterLights`, `0x1414cc76f`) reads only the entries' room arrays
        (`+0x18`, count `+0x28`) and `visibleUnboundSpace` (`+0x130`), which the walk writes. It does not use the map,
        so it needs no shared nodes.
    -   A `PortalViews`-backed hook needs the rooms the walk would insert. For a shared node, it can answer "either
        portal room visible".
-   **The stood-in clear (`SyncFadeOwnership`, `PrimaryCull.cpp:769-790`).** For an admitted, unmixed fade root it clears
    bit 26 once. No list process reaches the root afterwards (it is out of the lists), and BuildSceneLists and DCLF's
    `ListUpkeep` set only cell roots, so the bit stays clear. In `FUN_1402cff60` that moves the root from step 2 to
    steps 3-5:
    -   Fade `+0x109 & 0x24`: unaffected.
    -   A fading root (radius != 0, kIgnoreFade clear, bit 29 clear): `FUN_14147a160` runs on the node from the job
        threads, in `UpdateAnimationJob` and again in the managed-nodes update (the `AnimatedFade` hook observes it).
        `+0x13C` is no longer refreshed by an `OnVisible`, so the fade snaps to its end rather than stepping. The
        reference animates whenever `currentFade > 0`: within fade distance it animates on and off screen and does not
        freeze. But the engine writes the node's fade state again, against DCLF's write-back.
    -   **A never-fading root (kIgnoreFade, set at attach for references that never fade), a zero-radius one, or one
        with bit 29**: false. `UpdateAnimation` and the managed-node downward update both stop, so the object
        (windmill sails, water wheels, flags, Dwemer machinery: any non-actor, non-tree reference in the cell's
        animated array) **freezes in its last pose** from admission until the entry leaves admission.

    It is an existing defect for the never-fade case. T7b2's live writes (or not clearing the bit) fix it, and a count
    of admitted roots in the animated array by case (kIgnoreFade vs fading) measures it.

### Tree manager list (T7b5)

Reverse engineered 2026-10-11 (Ghidra, AE 1.6.1170) for DCLF's port of the tree list loop. It corrects "The tree manager"
above on the LOD stores and on which trees each list holds.

-   **The update, `FUN_140437d40(manager, process, paused)`** (Main::Update `0x140646176`, main thread, concurrently with
    `UpdateAnimationJob`). It runs only when `paused` is 0 and the manager's bytes `+0x1` and `+0x2` are set. It sets the
    time budget: `DAT_14313fa80` ticks = `fRam142029504 * clamp(DAT_141ad28e8 - (frame time * DAT_141ad2a30 - +0x7C), 0,
    _LAB_14200d688)`, 0 while `gSceneComplexCounter` is not 0, counted from `DAT_14313fa78` (`QueryPerformanceCounter`).
    Then:
    1.  `0x140437df1` `FUN_140437e50(manager, process)`, only when the process and its camera (`+0x18`) exist;
    2.  `0x140437dfc` `FUN_1404381e0(manager, frame time)`;
    3.  the near list emptied (`+0x48` = 0);
    4.  `+0x7C` = the update's time in ms.
-   **`FUN_140437e50`**, for each tree in the tree list (`+0x20`, size `+0x30`), read without the list's lock (`+0x10`). The
    local `BSCullingProcess` it builds and gives the process's frustum (`+0x20`) is not used. The camera is the process's
    camera's world translate (`+0xA0`).
    1.  `FUN_14147d640(tree)`: the first child (`*(+0x118)`), its vfunc `0x30` (`AsMultiBoundNode`). Null: skipped.
    2.  Visited when that node's `BSMultiBound` (`+0x128`) has a shape (`+0x18`) whose cull result (`+0x10`) is 1, or the
        process's cull mode (`+0x30198`) is 3. A tree not visited gets no store: its `+0x158` and its switch keep the last
        visit's.
    3.  `+0x158` = the squared distance from the camera to the tree's world translate (`+0xA0`).
    4.  **Near** when `manager +0x80 > +0x158` (strictly; a NaN is far): appended to the near list (`+0x38`, count `+0x48`).
        The LOD switch (`+0x180`, a `NiSwitchNode`: its children's size `+0x124`, their slots `+0x122`) gets the base index
        (`+0x12C`): 1 when `0x14200d640` is 0 and the switch has more than one child, else 0, stored only below the slot count.
    5.  **Far**: the same store, then with `0x142032dfc` (the fade LOD updates, `FadeFrame::lodUpdates`) the last child
        (`size - 1`), in range.

    Then the near list is sorted by `+0x158`, nearest first (`FUN_140439820`, a quicksort: not stable). Past the first
    `0x14200d670` (40), each tree at or past `+0x80 * 0x141ad2848` (0.25) gets the far stores (`FUN_140438840` is this tail
    alone). The list is then cut to 40. With `0x14200d640` = 1 (the default), a near tree shows child 0 (the full model)
    and a far one the last child.
-   **`FUN_1404381e0` reads both lists**, and checks the budget every fourth entry (it stops past it; it starts neither loop
    once the budget is spent).
    -   The animation list (`+0x50`, size `+0x60`, under the lock `+0x18`): a node whose reference (userData, `+0xF8`) has a
        base form (`+0x40`) and whose `kAccumulated` is set advances its clock (`+0x164 +=` frame time), refreshes `+0x158`,
        and within `+0x80` takes the gust `+0x15C` = `FUN_140438950(manager +0x78 * clock, 3)` * the form's `+0xB0`. It is
        counted at `+0x6C`.
    -   The near list (under `+0x10`): a tree whose form has bone data (`+0xB8`) advances its clock, runs
        `FUN_140438ed0` (the bones' wind over the tree's bone range `+0x174`/`+0x178` of `+0x1A0`, size `+0x1B0`, fewer while
        `+0x144` fades), takes the gust, sets flag bit 4 (`0x10`) and calls vfunc `0x168` (the selective downward update): the
        bones' world transforms. It is counted at `+0x68`. **The near list decides which trees' bones animate.**
-   **Which list a node is on.** `FUN_140192e90` (a `QueuedTree`, the reference's 3D at `+0x2C8`) and `FUN_140192fd0` pick by
    exact `NiRTTI`: `0x14332a2d8` (`BSTreeNode`, OnVisible `0x14147d3c0`, size `0x1B8`) to `FUN_140437200`, the tree list;
    `0x14332a2c8` (`BSLeafAnimNode`, OnVisible `0x14147c9c0`, size `0x170`) to `FUN_1404376f0`, the animation list.
    `FUN_140437200` also makes the node's fade type 4 (`FUN_14147aa00(node, 4)`) and sorts its bones (`+0x1A0`). The
    animation add copies the form's `+0xB0`/`+0xB4` to `+0x15C`/`+0x160`. The listed node is the reference's 3D root (its
    userData is the reference), so it is the fade root DCLF lists (T7b0's "a node inside the tree's model" is wrong).
-   **What the culls feed it:** the multibound cache for the tree list (any cull with a mode other than 1 and 3 writes it, in
    `BSMultiBoundNode::OnVisible`; the loop sees the last before `Main::Update`, normally the main camera's list cull of the
    last scene frame), and `kAccumulated` for the animation list (the main camera's list culls). Nothing else of the cull.
-   **DCLF's stand-in and the tree list.** `TreeAnimation` takes stood-in roots off the animation list only. A stood-in
    `BSTreeNode` stays on the tree list, under a multibound no cull reaches, so its result stays the last cull's: stale
    "in" keeps it visited (kept, its bones animated every frame within range), stale "out" freezes its bones. DCLF's
    skinned draws of its bones read those transforms. `<- TREE MANAGER` counts these trees by their stale result. Fixed (T7f
    preparation, `drawcall-limit-fix.md`): around the loop's call DCLF writes the stood-in trees' shape `+0x10` from its verdict and
    puts the engine's value back after it.
-   **The multibound cache's reuse** (`BSMultiBoundNode::OnVisible`, `0x140e2d710`). The node is tested only when it has children, is
    not `kAlwaysDraw` (flag bit 11) and the process's cull mode is not 1 or 3. Mode 4 tests the shape without touching the cache.
    Modes 0 and 2 compare `lastAccumTime` with the accumulation time (vfunc `0x118` of the object at `0x143274040`): when it moved
    they test the shape and store `+0x10` = 2 - inside, otherwise they reuse `+0x10` (1 in, 2 out; 0 makes them test again).
    So a value written outside a cull can serve a later cull of the same accumulation time: DCLF puts the engine's value back right
    after the tree loop.
-   **For T7f** (the culls off), the tree list needs the visited verdict for every `BSTreeNode` from DCLF. Either DCLF writes
    the multibound result (the shape's `+0x10`) at Main::Update's entry, or `FUN_140437e50` is replaced by DCLF's port,
    `TreeList`'s `Decide`, with the same stores. The animation list needs `kAccumulated` on its non-stood-in roots (T7b2's
    writes extended to them). Written: the multibound result around the loop's call, for every listed tree, behind
    `CS_DCLF_TREE_LIST_ALL` (`drawcall-limit-fix.md`, "T7f preparation, part 2").

### T7f preparation: the frame's camera, the height-test byte, parent pointers

Reverse engineered 2026-10-11 (Ghidra and capstone, AE 1.6.1170) for moving PrimaryCull's camera duties to BeginSceneFrame and for
DCLF's writes on the nodes between a geometry and its root.

-   **The main camera at BeginSceneFrame.** `FUN_1406452c0` (Main::Draw's camera setup, at `+0xD3`: DCLF's BeginSceneFrame runs right
    after it returns) reads the world camera (`Main::WorldRootNode +0x128`). It stores the camera's world translate in
    `0x14332a2e8`/`2ec`/`2f0`, so the tree height test's base (`0x14332a2f0`) is the camera's height. `CalculateAndDrawShadowCasterLights`
    uses `gWorldSceneGraph +0x128` (`0x14338c8e8`, PortalViews' camera). The list processes' camera (`+0x18`) is set before Main::Update's
    walk, which reads it. That they are one object is checked by DCLF's frame camera observer (`CS_DCLF_ACCUMULATED_PARITY`); not
    verified statically.
-   **The scene-list processes' `+0x121`** (the byte `BSTreeNode::OnVisible`, `0x14147d3c6`, tests for its height test). The writes of
    `[reg+0x121]` in `.text` are:
    -   the `NiCullingProcess` constructors (`0x140d3fd6a`, `0x140d3fed5`), which write 0;
    -   `FUN_1414bf9f0` (`0x1414bfba7`), which copies a cull context's `+0x5D` into the static mode-0 processes. A mode-1 context
        returns its own process (`+0x30`) without writing the byte;
    -   functions unrelated to culling processes (`TESLandTexture`, pathing, the save, the renderer).

    `FUN_14063eb50` (scene creation) writes only `+0x11C` as a word (`0x101`) on the list processes. So for the main camera's list
    processes the byte stays 0, and their height test is off. DCLF still reads it, at BeginSceneFrame.
-   **`NiNode`'s destructor** (`0x140d1e930`) sets each child's `parent` to null (and `parentIndex` to `0xFFFFFFFF`) before releasing
    it. So a live object's non-null parent pointer always names a live node. At Main::Update's entry no attach or detach runs, so
    DCLF walks a held geometry's parents up to its root there.
-   **The cell roots' `kAccumulated`.** `DrawWorld_BuildSceneLists` (`0x14064bc20`) sets it every frame on the object root's
    non-hidden children (`0x14064bef9`), or in an interior on the cell's `cell3D` alone (`0x14064c16a`). In both branches the object
    root's children 0 and 1 are also appended to list 0 (`FUN_14021cf40`). They are list entries, so the main list cull sets or clears
    their bit by its test: those two are not cell roots, and they lack the bit whenever they are out of view (DCLF's T7f dry run counted
    them as "cell roots without the bit"). No list cull visits the other children, so nothing clears theirs.
-   **What `Process1` leaves on a node (T7f, `kAccumulated`'s meaning).** `BSCullingProcess::Process1` (`0x140e28390`), with
    `updateAccumulateFlag`:
    -   **A zero radius without `kAlwaysDraw`:** the node is left alone (no test, no `OnVisible`, the bit unchanged).
    -   **`kAlwaysDraw` (bit 11), or no active planes:** `OnVisible`, then the bit is set.
    -   **Preprocessed (bit 12), unless `ignorePreprocess` or mode 4:** cleared if `kNotVisible`, else `OnVisible` and set.
    -   **Cull mode 2:** cleared. **Mode 1:** passes.
    -   **Otherwise:** the frustum (`FUN_140d3ff10`) or the room's compound frustum (`BSCompoundFrustum::Process`) sets or clears it.

    The bit is written after `OnVisible` returns, so a root's bit is written after its children's and whatever its `OnVisible` did. A
    faded-out `BSFadeNode` gets the bit even though nothing under it is visited. `NiCullingProcess::Process1` (`0x140d40160`) has the same
    shape, without the compound frustum.

    So a geometry's bit is the last cull that reached it. Under a failed node, a stopped `OnVisible` or an inactive switch or LOD child
    it is stale. The receivers test every level and never take such a geometry.

### T7f: the fade nodes' OnVisible stores and the actors' animation stamp

Reverse engineered 2026-10-11 (Ghidra, AE 1.6.1170) for the T7f checklist's items 4-6.

-   **What a fade node's `OnVisible` writes.**
    -   `BSFadeNode::OnVisible` (`0x141479f50`). With fades off (`0x142032dfd` clear), or settled (flag bit 15, `fadeAmount` and
        `currentFade` both 1.0, `0x141ad2870`), it goes straight to `NiNode::OnVisible` (`0x140d1e2c0`, the children). Otherwise, with
        the process's `cameraRelatedUpdates` (`+0x11C`):
        -   a LOD type 6 node (`+0x153 & 0xF`) while `0x14332a254 == 0x141769578` steps `currentFade` itself (or sets it to 1), sets
            flag bit 14, stores `+0x13C` and goes to the children whatever the fade;
        -   any other runs `FUN_14147a160` (the fade state machine) and stores `+0x13C` = `0x142032e50` (the fade frame counter).

        It then returns without the children when `currentFade <= 0` or `fadeAmount == 0` (the amount read before the update).
    -   `BSLeafAnimNode::OnVisible` (`0x14147c9c0`): with LOD updates (`0x142032dfc`) and camera updates, `FUN_14147b110` (the
        distance and level), `FUN_14147a430` (the leaf LOD transition: `+0x14C`, `+0x152`, `+0x153`) and `+0x13C`; then it jumps to
        `BSFadeNode::OnVisible` (a tail `jmp`).
    -   `BSTreeNode::OnVisible` (`0x14147d3c0`): the height test (process `+0x121`, `0x142032fb8`, world z `+0xEC` minus
        `0x14332a2f0` against `0x142032fa0`: above it nothing is visited), then a `call` of `BSLeafAnimNode::OnVisible`, then the LOD
        fix-up: with a LOD switch (`+0x180`) whose `+0x12C > 0`, `+0x153 = (+0x153 & 0xAF) | 0x20` and `+0x14C = 0`.

    These and the animation gate's `FUN_14147a160` (`0x1402d0044`) are the only engine writers of a fade root's state. The plan DCLF
    gives a root (`FadeState::PlanOf`) compares the vtable's `OnVisible` slot with the three addresses, so they are detoured at their
    entries, not replaced in the vtables (subclasses that inherit one keep its address).
-   **The actors' animation stamp** (`BSShaderProperty +0x80`, `lastAccumulatedFrameCount`).
    -   `gFrameCounter` is `0x14328cc6c`. Its one writer is `Renderer::End` (`0x140e4482b`), so it is constant from a frame's
        `Main::Update` to its end.
    -   The writers of `+0x80` stamp it with `gFrameCounter` right after `GetRenderPasses` (vfunc `0x158`) and **before** the passes reach
        the batch renderer's `RegisterPass` (vfunc 2, which DCLF's withholding replaces): `FUN_1414b2a60` (`0x1414b2aef`, render modes
        `0xC`-`0x11`: the depth prepass and the shadow maps), `FUN_1414b2810`, `FUN_1414b2790`. A withheld registration still stamps; an
        entry no cull visits (the sun's entry exclusion, the culls off) is not stamped.
    -   `Actor::ModifyAnimationUpdateData` (`0x1406a3750`, called through the actor vtables and `PlayerCharacter`'s) sets the update's
        "visible" byte (`+0x2C`) from `Get3D2()`: true unless the 3D has `kNotVisible` (bit 20, the actor pre-cull's) and the race's flag
        bit 7 is clear. Then it is `(float)(gFrameCounter - skin +0x80) < fMaxFrameCounterDifferenceToConsiderVisible` (`0x142013818`,
        cached in `0x14319a01c`), the skin property being `currentProcess +0x8 (middleHigh) +0x178` (`FUN_140713610`, a referenced read).
        The difference is a whole number of frames, so only a stamp of the same frame counts.
    -   So a stamp written at `Main::Update`'s entry, with that frame's counter, reaches the frame's actor animation like the engine's own.

### T7f item 8: the OnVisibles a skipped entry no longer runs

Reverse engineered 2026-10-12 (Ghidra, AE 1.6.1170) for the culls-off switch (`CS_DCLF_CULLS_OFF`,
[drawcall-limit-fix.md](./drawcall-limit-fix.md), "T7f item 8"). A cull reaches a node's or geometry's `OnVisible` (vtable slot
`0x34`) through `Process1`; an entry no cull visits runs none of them. What each one does besides the recursion:

| Class | `OnVisible` | What it does |
| --- | --- | --- |
| `NiNode`, and every subclass that keeps it (`BSFaceGenNiNode`, `BSValueNode`, ...) | `0x140d1e2c0` | the children (`Process1` on each); nothing else |
| `BSMultiBoundNode` | `0x140e2d710` | the multibound shape's cull cache (`+0x10`, "Tree manager list (T7b5)"), then the children |
| `NiSwitchNode` | `0x140d29700` | the selected child only |
| `NiBillboardNode` | `0x140d3f720` | with `cameraRelatedUpdates`, the turn to the process's camera (`0x140d3e650`, "Billboards: turned by the culls, per culling camera"), then `NiNode`'s |
| `BSOrderedNode` | `0x141503970` | a thunk: `lock xadd` of 1 on the process's `+0x301F0` (a per-process counter), then `NiNode::OnVisible` with the new count as its third argument (the alpha group its children's appends carry) |
| `BSGeometry` (`BSTriShape`'s slot) | `0x140d37f30` | the append (`AppendVirtual`) when the geometry has a shader property, under the TLS cull state `0x43` (`+0x768`) |
| `BSDynamicTriShape` | `0x140d37f30` | the same function |
| `BSSubIndexTriShape` | `0x140e312c0` | `BSDynamicTriShape`'s when its segment count is not 0 |
| `BSSegmentedTriShape` | `0x140d59740` | `NiAVObject::OnVisible` (`0x140d1c5a0`, the append) when `+0x144` is not 0 |
| `BGSDecalNode` | `0x1401fdf10` | `NiAVObject::Cull` on each decal's geometry (vfunc `0x148` of each `BSTempEffect` in `decals`), last first, when its bound radius is not 0 |
| `BSFadeNode`, `BSLeafAnimNode`, `BSTreeNode` | "T7f: the fade nodes' OnVisible stores" | the fade and LOD state, the height test |
| `BSMultiBoundRoom`, `BSPortalSharedNode`, `NiBSPNode` | `0x140e089f0`, `0x140e24740`, `0x140d4b8c0` | the portal and room structure (not under a reference's entry) |

-   **No geometry `OnVisible` writes anything a reader takes.** The appends are the registration's input; a skipped geometry is simply not
    registered.
-   **The slots DCLF itself replaces** (the startup line `[DCLF] culls off (T7f item 8): OnVisible slots`): `NiNode`'s is LocalLightCull's
    category filter (a hook in `CommunityShaders.dll` that ends in the engine's function), `NiBillboardNode`'s the billboards' observer under
    `CS_DCLF_SET_PARITY`. Subclasses that keep `NiNode`'s carry the engine's address in their own tables, so a plan compares with both.
-   **CommonLib's `VTABLE_*` ids resolve to AE's tables** at run time (`REL::Relocation`); the SE offsets beside them are not AE's.

## The material database: how a material is shared and released

`BSShaderProperty::SetMaterial` (`0x14147bff0`) never stores the material it is given. It asks the material manager
(the singleton pointer at `0x143187758`) for the database's copy, `FUN_1414f7790(manager, material, unique, flag)`:
under the database's spin lock it hashes the material (vfunc `0x20`), walks the bucket and compares with vfunc
`0x18`, and on a miss creates a copy (vfunc `0x08`, `0x10`) and inserts it. It returns the shared material with its
count (`+0x8`, `BSIntrusiveRefCounted`) incremented. The old material goes back through `FUN_1414f7a40(manager,
material)`, which decrements under the same lock and, at zero, takes the material out of the database before deleting
it.

So a reference to a property's material must be dropped through `FUN_1414f7a40`. A `BSTSmartPointer` deletes it
directly at zero and leaves the database pointing at freed memory: the next material that hashes to it crashes in
`FUN_1414f7790` on vfunc `0x18` (DCLF's material cache did this; `drawcall-limit-fix.md`, "The sun's cascades without
DCLF's objects"). Taking a reference is a plain atomic increment, as the engine does in `FUN_1414ac820`.

## Face morphing: the only writer of a face's positions

An NPC's head, mouth, eyes, brows, hair and beard are `BSDynamicTriShape`s under a `BSFaceGenNiNode`. Their
positions are not in a vertex buffer: they are `dynamicData` (`+0x160`), one float4 per vertex (`dataSize` is
exactly `vertexCount * 16`), guarded by a `BSSpinLock` at `+0x168` (lock `FUN_140d38c60`, unlock
`FUN_140d38cc0`). AE 1.6.1170.

-   **The writer.** The job stage "Face morphing" (`Job_Face_morphing`, `0x1406d36b0`, RELOCATION_ID
    38139/39096) is in the "Main post render" stage (`SetupJobLists`, job table `0x142011d60`: Face morphing,
    Sky, Shared particles, Update grass), after the scene and its shadow maps are drawn.
    `impl_Job_Face_morphing` (`0x140432f90`) queues one job per head on job list 12 (`FUN_1404334e0`, which
    calls `FUN_140432550(head, flags & 1)` at `+0x13`), then the stage calls `JobList::Finish`
    (`FUN_140cf6810`, at `0x1406d36fd`). `FUN_140432550` walks the head's direct children (`+0x118` data,
    `+0x122` count): it resets a shape to its base positions (`FUN_14042b4b0` -> `FUN_140d38d70`, without the
    lock), then applies each morph under its own lock and unlock (`FUN_14042fcc0`, `FUN_140430600`). So the
    lock keeps one copy from tearing, not a head: a reader holding it can see a head half morphed.
-   **Which heads.** `BSFaceGenNiNode::UpdateDownwardPass` queues a head (`FUN_1404330c0`, the list at
    `0x14313fa18`, count `0x14313fa28`, a spin flag at `0x14313fa30`) only while its face animates or is near.
    Between jobs the positions do not change.
-   **Everything else reads.** Decals (`1405f0ea0`, `1405f10c0`, `1405ef640`), hit tests (`140e54370`),
    `BSFaceGenBaseMorphExtraData` creation (`140431670`, `1404318c0`), and the draws. Particle geometry
    (`1414c77d0`, `1414c78a0`) is another use of `BSDynamicTriShape`, with its own writer.
-   **The draw.** `FUN_1414f3dc0` / `FUN_1414f4560` copy the whole `dynamicData` under the lock into the
    renderer's dynamic vertex ring (`FUN_140e46d60`: three 4 MB buffers mapped `WRITE_NO_OVERWRITE`, reused once a
    query says the GPU is past them) for every draw of every view, and bind it as stream 1. A skinned shape then
    draws its partitions (`BSDismemberSkinInstance::Unk_25` -> `NiSkinPartition::Unk_25`, `0x140d43a10`) with the
    partition's `buffData` as stream 0 and the ring as stream 1. All partitions of a shape share one vertex
    buffer in the shape's own vertex order (a head: 898 vertices, partitions of 102, 777 and 50 that overlap at
    their seams), so stream 1 is indexed by the same vertex index.
-   **The layout.** A face partition's `vertexDesc` has the position on stream 1 (bit 54) and nothing else
    there; stream 0 holds UV, colour and skinning (stride 20 for a head).
-   **Timing.** Frame N's render draws the morphs of frame N-1's post-render stage.

## Particle systems: the writer of their data, and the draw that builds their quads

AE 1.6.1170, decompiled for T7e (DCLF's particles: [drawcall-limit-fix.md](./drawcall-limit-fix.md), "Particles (T7e)").

-   **The classes.** `NiParticles : BSGeometry` (type byte `+0x150` = 1, `kParticles`; `BSStripParticleSystem` is type 2): its data
    `NiPSysData` at `+0x158`, four fade distances (ushort) at `+0x160..+0x166`, its vertex description `0x0840200004000051`
    (`+0x148`, set by the constructor, `NiParticles::CreateClone` `0x140d40210`: the position and colour on stream 1, offset 0 and
    16, stride 20; the texture coordinates on stream 0, stride 4). `NiParticleSystem` (vtable `0x1419c3898`) adds the modifier list
    (`+0x168`), `lastUpdate` (`+0x18c`), `isWorldspace` (`+0x192`). `NiMeshParticleSystem` (`0x1419c43a0`) and
    `BSStripParticleSystem` (`0x1419c4d70`) have vtables of their own. The data (`NiParticlesData`): `maxNumVertices` `+0x12`,
    positions `+0x30`, colours `+0x38` (NiColorA, optional), radii `+0x40`, sizes `+0x48`, rotation angles `+0x50` (optional),
    subtexture offsets `+0x60` and their count `+0x68`, the active count `+0x7c` (`GetActiveVertexCount`, vfunc 0x26), texture
    indices `+0x80`, two aspect flags `+0x88`/`+0x89` with their parameters `+0x6c..+0x78`; `NiPSysData::particleInfo` (`+0x90`, the
    velocities).
-   **The writer: the update pass.** `NiParticleSystem::UpdateDownwardPass` and `UpdateSelectedDownwardPass` (vtable slots `0x2C`
    `0x140dd48a0`, `0x2D` `0x140dd4910`; the selected one only with flag bit 2) run `FUN_140dd4db0` (UpdateWorldData: a world-space
    system's world is the identity with its parent's scale) and `BSDynamicTriShape`'s downward pass: the controllers, among them
    `NiPSysUpdateCtlr::Update` (`0x140ddd660`), which calls the system's vfunc `0x1c0` (`0x140dd4370`, a jump through vfunc `0x1c8`),
    `Do_UpdateSystem` (`0x140dd4380`): the modifiers' `Update`s write the data arrays and recompute the model bound (`+0x110`), then a
    callback (`0x143275998`, null in a vanilla game) - then the world transform and `UpdateWorldBound` (`0x140d37dc0`). Which thread:
    whatever updates the subtree. A reference's emitters (a candle's flame, a fire's smoke) update with the reference's 3D, in the cell
    animations' jobs (`Job_Cell_animations`, gated by the root's `kAccumulated`: `FUN_1402cff60`); the "Particles" job stage
    (`Job_Particles` `0x1406d31a0`, in the "render 1st person" stage of the job table, `0x142011cc0`) runs `FUN_1402c76c0`: one job a
    temporary effect (`0x1431374b0`, a `BSTArray<BSTempEffect>`, count `0x1431374c0`: `NiAVObject::Update` of the effect's 3D) and one
    a child of `BSParticleSystemManager` (`UpdateParticleSystemManagerJob` `0x1402c7470` -> `FUN_140e2a520`: the child's vfunc
    `0x160`), under the read lock `0x143284b90`. The "Shared particles" job (`0x1406d3720`) is in the post render stage.
-   **The draw.** `FUN_1414f2ad0` (the per-type draw of `SetupAndDrawPass`), type 1: count = `min(GetActiveVertexCount(), 0x800)`;
    maps `20 * 4 * count` bytes of the renderer's dynamic ring (`FUN_140e46cd0`, `FUN_140e46ed0`), fills them with `FUN_140e51320`, then
    `FUN_140e46f20`: index buffer `0x143286a40` (R16), stream 0 the static texture coordinates `0x143286a48` (stride 4), stream 1 the
    ring (stride 20), `DrawIndexed(count * 6)`. Both static buffers are made once at the renderer's initialisation (`FUN_140e4d280`):
    0x800 quads of `0,1,2 0,2,3`, and the corners `(0,1) (1,1) (1,0) (0,0)` (half2) of each.
-   **`FUN_140e51320`, the quads.** From the renderer's view data (`CameraStateData` `0x14202aee0`: `viewUp` `0x14202aef0`, `viewRight`
    `0x14202af00`, `viewForward` `0x14202af10`) and the eye (`0x14202aecc`, posAdjust): `A = R^T viewUp`, `B = R^T viewRight` (R the
    system's world rotation). The particle order is sorted by `dot(position, viewForward)` (model-space positions, a world direction:
    `FUN_140d4fcb0`), farthest first, with the quicksort `FUN_140d4ff00`. Per particle: its world position `R p s + T`; the fade
    `FUN_140d40ba0` (its distance d from the eye against the four distances: 0 below `+0x166`, rising to 1 at `+0x164`, falling from
    `+0x162` to 0 at `+0x160`; 1 with them all 0); the colour packed RGBA8 (each channel times 255 truncated, its low byte; alpha times the
    fade); its size `radius * size`. Without rotation angles: corners `c - (A+B)s, c - (A-B)s, c + (A+B)s, c + (A-B)s`, c the position
    minus the model bound's centre. With them: the size is 0 when `fade * alpha <= 0.05` (`0x1417743ec`); `(sin, cos)` of the angle
    (`FUN_14153b7a0`), `X = (cos+sin)s B + (cos-sin)s A`, `Y = (cos+sin)s A - (cos-sin)s B`, corners `c - X, c - Y, c + X, c + Y`. With the
    aspect flags (below) the corners follow the velocity. Position w is the texture index.
-   **The aspect flags** (`FUN_140e51320`'s path without rotation angles only: with them the flags are ignored). `+0x88` aligns the quad
    with the velocity (`NiPSysData::particleInfo[i].velocity`, `+0x90`, stride 0x20, in the system's space), `+0x89` stretches it by its
    speed; the parameters are `+0x6c` (the aspect, and the speed rule's first aspect), `+0x70` (the second aspect), `+0x74` and `+0x78`
    (the two speeds; CommonLib's names for `+0x6c..+0x78` are shifted). With either flag, per particle (in the sorted order): the
    velocity's projection on the view plane `P = (v.A) A + (v.B) B`; with `+0x89` the aspect is `lerp(+0x6c, +0x70, (clamp(|P|, +0x74,
    +0x78) - +0x74) / (+0x78 - +0x74))`; with `+0x88` A becomes `P` normalised (`XMVector3Normalize`: zero for a zero P, NaN for an
    infinite one) and B becomes `A x E`, E the view normal `B x A` computed once from the camera's; then `X = A/aspect + B`, `Y = A/aspect
    - B`, corners `c - X s, c - Y s, c + X s, c + Y s`. The new A and B, and the aspect, are **not reset per particle**: the next particle's
    projection uses them (the same plane while the axes stay orthonormal, but a zero velocity zeroes both axes, and every later particle of
    the draw collapses to a point). Without `+0x89` the aspect is `+0x6c`.
-   **The shader** (`BSEffectShaderProperty` on the system: `GetRenderPasses` `0x1414bd890` gives hint 1 whatever the alpha "for a
    particle system"; `FUN_1414bde90` sets `Particles` (`0x1000`) for a geometry whose vfunc 0x80 (`AsParticlesGeom`) answers and that is
    not a `BSStripParticleSystem` (`0x2013` for that), `TexCoordIndex | IndexedTexture` (`0x84`) with a texture and a non-zero
    subtexture count, never `Falloff`). `BSEffectShader::SetupGeometry` (`0x1414eab10`) with `Particles`: World's translation is
    `R c s + T` (the model bound's centre folded in, which the vertices had subtracted), PreviousWorld the same; with `IndexedTexture`,
    VS b11 = the data's subtexture offsets (count * 16 bytes, zero to 0x80 rows), which `Effect.hlsl`'s `TEXCOORD_INDEX` applies to the
    corner's coordinates (`u' = o.y u + o.x`, `v' = o.w v + o.z`).
-   **Precipitation** ("Particle: BSTriShape" in the census) is `BSParticleShaderProperty` (`GetRenderPasses` `0x1414c6890`, the Particle
    shader `0x14150da30`...) on its own geometry, the Precipitation feature's: not this path.

## Blood: weapon blood and skinned decals (AE 1.6.1170, 2026-10-12)

-   **Skinned decals** (`BSTempEffectGeometryDecal`: `Initialize` `0x1404a55b0`, `Attach` `0x1404a5800`, `Update` `0x1404a56e0`; the geometry is
    made by `FUN_1404a3ec0`). The hit's decal is a `BSTriShape` (`FUN_140d2d8f0`, 0x168 bytes, named `SkinnedDecal`) over a new renderer
    buffer (`FUN_140e45f70`) with the target geometry's skin instance (`+0x130`, shared: a `BSDismemberSkinInstance` of several partitions on an
    actor body), and a new `BSEffectShaderProperty` (`FUN_1414bd5e0`) with a `BSEffectShaderMaterial` whose source texture is the decal texture
    set's first; flags `kSkinned`, `kDynamicDecal`, `kZBufferTest` set and `kZBufferWrite` cleared; an `NiAlphaProperty` with blending on,
    DESTCOLOR / INVSRCALPHA (MultBlendDecal): hint 3, decal group 4. `Attach` puts it under the decal node (`Skinned Decal Node`), whose parent
    is the actor's `BGSDecalNode` (`FUN_1401fdfb0` lists the temp effect in its `decals`), or straight under the target geometry's parent
    without one. Its end fades it: `Update` calls the property's vfunc `0x188` (`SetMaterialAlpha`) with `1 - age / lifetime`, then detaches it
    (and the decal node with its last decal, counting the manager's `+0x10` down).
-   **World decals** (`BSTempEffectSimpleDecal`: `FUN_1404a9c70` clips and builds a `BSTriShape`, `FUN_140e52320`, into `+0x48`): their 3D is
    attached under the reference's `BGSDecalNode` (`AttachDecal`, `0x1401fdc30`: `NiNode::AttachChild`); `Update` (`0x1404ab3f0`) fades the
    last second through `FUN_1404ad450` (each geometry's property `SetMaterialAlpha`, vfunc `0x188`, when it differs) and detaches
    (`FUN_1401fdd50`). `BGSDecalNode::OnVisible` (`0x1401fdf10`) culls its decals' `Get3D` (vfunc `0x148`) last to first, not its children.
-   **Weapon blood** (`BSTempEffectWeaponBlood`): the weapon's `kWeaponBlood` effect geometry (`BloodEffects`, `ImperialSwordBlood`, ...),
    hidden until a hit (`FUN_1414ab580` shows it, clears the alpha's blend bits). Its start (`0x1404aeb10`) and `Update` (`0x1404aec00`) set the
    alpha threshold of every geometry under the weapon (`FUN_1414a92a0` -> `FUN_1414ab770`: `(1 - value) * 255`, the value growing then
    falling with the effect's age) and call `FUN_1414ec2a0(DAT_14200fbe8, DAT_14200fc00)`, which stores two game settings into the globals
    `0x1433dccf0` and `0x1433dccf4` (their only writer).
-   **`BSEffectShader::SetupGeometry`'s Blood** (`0x1414eab10`, the pass descriptor's bit 14): PS `AlphaTestRef` = (the pass geometry's alpha
    property (`+0x120`, `FUN_1414e8e80`) threshold `+0x32` / 255, `0x1433dccf0`, `0x1433dccf4`), whatever the property's alpha test.
    `Effect.hlsl`'s BLOOD takes its alpha from the texture's green and its colour from the blue against those three.
-   **The first-person root has a parent** in AE 1.6.1170 runs (2026-10-11 logs: `Skeleton.nif` under two unnamed nodes under `ObjectLODRoot`),
    contrary to what Load3D's decompile suggested (fp0); DCLF files its subtree under it whatever its parents (`MirrorCategoryNode`).

## Billboards: turned by the culls, per culling camera

AE 1.6.1170, decompiled for DCLF's billboards ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Billboards and aligned particles").

-   **Where.** `NiBillboardNode` (vtable `0x1417f0750`; `+0x128` userFlags, the face mode in bits 0-2) turns in its cull, not in its
    update. `NiBillboardNode::OnVisible` (slot `0x34`, `0x140d3f720`) calls `0x140d3e650` with the culling process's camera when the
    process has `cameraRelatedUpdates` (`+0x11c`), then `NiNode`'s OnVisible (its children). `NiBillboardNode::UpdateWorldData` (slot
    `0x30`, `0x140d3e510`) writes only the world translation and scale from the parent (the rotation stays the last cull's). So every leaf
    below a billboard holds the world of whichever camera culled it last; a shadow or reflection process with camera-related updates turns
    it to its own camera (a process without them leaves the last turn).
-   **`0x140d3e650`.** Rebuilds the node's world: rotation `R0 = parent.R * local.R`, translation `parent.R local.t parent.s + parent.t`,
    scale `parent.s * local.s` (no parent: the local); then `R = R0 * M` with M by mode, and runs each child's `UpdateDownwardPass`
    (vfunc `0x160`, NiUpdateData zeroed) so the leaves take it. The camera is its world rotation's columns (forward `+0x7c/88/94`, up
    `+0x80/8c/98`, right `+0x84/90/9c`) and translation `+0xa0`. Below `a = R0^T toward`, `u = R0^T up`, `r = R0^T right` (toward =
    -forward):
    -   **0 ALWAYS_FACE_CAMERA / 3 ALWAYS_FACE_CENTER**: `l = sqrt(r.y^2 + u.y^2)`; for `l <= 1e-6` (`0x141ad27f8`) the columns `-r, -u, a`;
        else with `c = u.y/l, s = -r.y/l` the columns `r c + u s`, `u c - r s`, `a` (turned about a until the right axis has no y).
    -   **2 RIGID_FACE_CAMERA / 4 RIGID_FACE_CENTER**: the columns `r, u, a`, so `R` is the camera's axes exactly.
    -   **1 ROTATE_ABOUT_UP**: `d = R0^T (eye - t) / scale`; with `l = sqrt(d.z^2 + d.x^2) >= 1e-12` (`0x1419afb08`) `M = [[d.z, 0, d.x], [0,
        1, 0], [-d.x, 0, d.z]] / l`, else the identity.
    -   **5 BSROTATE_ABOUT_UP**: R0 is replaced by the identity; `d = eye - t`; with `l = sqrt(d.x^2 + d.y^2) >= 1e-12` `M = [[-d.y, d.x, 0],
        [d.x, d.y, 0], [0, 0, l]] / l` (determinant -1: a mirror, as the engine has it), else the identity.
    -   The centre modes (3, 4) first turn the camera's axes with `FUN_140d3f240`: none (M the identity) when `|eye - t|^2 < 0.001`
        (`0x141ad280c`); `d` the unit direction to the eye; when `toward . d < 0.99999899` (`0x14193aca4`) the axes are rotated by the angle
        `acos(toward . d)` (pi at -1) about `normalize(d x toward)`, and toward becomes d.
-   **What reads it.** `SetupGeometry` reads the leaf's world as for any geometry. The camera rows `SetCameraData` gives the view matrix
    (`FUN_140e57ae0`, from the camera's columns: right, up, forward; VS_PerFrame's CameraView c0-c2 in the shader's row order) are the same
    floats, so a vertex stage can rebuild the turn from b12.

-   **A particle system under a billboard** (T7f item 7). The turn reaches it only through the children's `UpdateDownwardPass` that
    `0x140d3e650` runs: slot `0x2C` of `NiParticleSystem` (vtable `0x1419c3898`) is the system's update (`NiPSysUpdateCtlr`, the writer of its
    data, world and bound). `NiParticleSystem::UpdateWorldData` (slot `0x30`, `0x140dd4c90`) is empty. So the draw takes the
    system's world and particles as its last update (an update pass or a cull's turn) left them; nothing turns them per view afterwards.

## The empty fade node: the property's "no fade node" (T7f item 7)

AE 1.6.1170. `FUN_14147ac00` (static initialisation) allocates a `BSFadeNode` (0x158 bytes, NiAVObject flags `|= 0x8000`, `currentFade`
`+0x130` = 1) and holds it in the `NiPointer` at `0x14332a2a0`; `FUN_14147ae10` releases it at exit. It has no parent and no name.

-   **Who points at it.** `BSShaderProperty`'s constructor (`0x14147b580`) sets `fadeNode` (`+0x60`) to it. Measured (r1, the fade node references'
    report): terrain (`Land`, `Block (x, y)`), object LOD (`obj`, `objsnow`, `*-LargeRef`) and some references' geometry (havok clutter
    such as `SteelShield`, `Salmon`, `BasicTankard01`; `Scene Root:N` meshes) keep it. Which writer gives the others their `BSFadeNode` was
    not traced.
-   **Who reads it as "none".** `BSLightingShaderProperty::GetRenderPasses` (`0x1414ae146`: the refraction LOD fade then measures from the
    geometry's bound instead), `BSLightingShader::Func6` (`0x1414dd379`), `BSUtilityShader::SetupGeometry` (`0x1414fb720`), `FUN_1414dfec0`
    (`0x1414dfedc`) and `TESWaterSystem::AddWater` compare `fadeNode` with the pointer's value and take the null path.
-   **DCLF.** The fade-root listing (`SceneStore::ListFadeRoot`) and the fade node references take it as no node (`Engine::EmptyFadeNode`).
    Listed as a root, it was one root of thousands of geometries outside the scene graph: no parent walk reached it (T7f's "geometries whose
    walk missed their root"), and the fade write-back wrote it.

## Scene state writers: where DCLF takes its events

AE 1.6.1170. These are the choke points `SceneStore::InstallSceneEvents` detours for the delta walk
([dclf-event-driven-tables.md](./dclf-event-driven-tables.md), Phases 2 and 3).

-   **`BSFadeNode::currentFade`** is written in the cull by `BSFadeNode::OnVisible` (vtable slot `0x34`,
    `0x141479f50`): directly for one LOD mode, and through the fade update `FUN_14147a160` otherwise.
    `FUN_1402cff60` calls that update outside a cull.

    Eleven more writes store it inline, outside any function DCLF hooks: 0 or 1.0, followed by clearing or setting
    flag bit 14 (`0x4000` at `+0xF4`, a frame bit):
    -   the cells' placements: `FUN_1402bc1f0` (from the grid controller, on the main thread) and `FUN_1402bb690`
        (a job's), each after `FUN_1402d5090`;
    -   the sky cell's: `FUN_1402b9da0` (the clouds);
    -   two branches of `FUN_1402d1280` that skip the snap;
    -   `Explosion::Load3D`, `FUN_1401ce010`, `FUN_14028b9d0`, `FUN_1402c8420`, `FUN_1407d4440` and `FUN_1407ee3e0`.

    A scan of the image for that pattern finds exactly these eleven (2026-10-09; SceneWriterFramework patches them,
    `SWF_EVENT_FADE_RESET`).
-   **`BGSDecalNode::decals`** (`+0x128`, `BSTArray<NiPointer<BSTempEffect>>`) has one append and four erases, all of
    them BGSDecalNode's own:
    -   append: `FUN_1401fdfb0`. `BSTempEffectGeometryDecal::Attach` calls it after attaching the decal's 3D to its node.
    -   erases:
        -   `FUN_1401fdc80`, through `FUN_1401fe020`, from the decal manager;
        -   `FUN_1401fdcb0`, from `BSTempEffectGeometryDecal::Update`;
        -   `FUN_1401fdd50` and `FUN_1401fde40`, which also detach the decal's 3D.

    `BGSDecalNode::OnVisible` (`0x1401fdf10`) culls each decal's `Get3D` from last to first, and not the node's children.
-   **Child edits outside `NiNode`'s implementations.** These node classes implement the child edits themselves, so a
    detour of `NiNode`'s vtable slots `0x35` and `0x37`–`0x3C` misses them:
    -   `BSParticleSystemManager`: attach, detaches and `SetAt2`; it moves world-space particle systems, such as the
        waterfalls' splash and smoke;
    -   `BGSDecalNode`: detaches and sets; its decal erases call `DetachChild1` directly;
    -   `BSFaceGenNiNode`: attach.

    DCLF detours all eight (2026-10-09).
-   **`NiAVObject::SetCollisionObject`** (`FUN_140e8bd40`, `+0x40`) is called by `bhkWorld::InitHavok` and
    `FUN_140e8c600`, after the node's world attach.
-   **Lights and cameras** under the world root leave without a hooked detach. `ShadowSceneNode::RemoveLight`
    (`0x1414a1120`) only queues the light (`lightQueueRemove`), and the queue's processing detaches it by other means.
-   **`BSShaderProperty::SetFlags(flag, set)`** (`0x14147bee0`) sets or clears one bit of the 64-bit flags. It sets
    `lastRenderPassState = 0x7fffffff` when the bit changes, which makes `GetRenderPasses` rebuild the pass list.
    Community Shaders' own in-place flag writes are in `TruePBR`'s `LoadBinary`, before the property is attached.
-   **`BSShaderProperty::SetMaterial(material, unique)`** (`0x14147bff0`) takes the new material from the material
    manager (`FUN_1414f7790`, which shares one instance between properties unless `unique` is set or the property has
    controllers) and releases the old one (`FUN_1414f7a40`).
-   **Controllers.** `NiTimeController::SetTarget` (`0x140d33920`) removes the controller from its old target and
    calls `NiObjectNET::PrependController` (`FUN_140d268d0`): `controller->next = target->controllers`, then
    `target->controllers = controller` (`+0x18`). `NiObjectNET::LinkObject` sets the list directly when a NIF loads.
-   **Havok to the scene.** `FUN_140ea55a0(collisionObject, transform)` writes the node's (`collisionObject+0x10`)
    transform from its rigid body. Its callers:
    -   `bhkCollisionObject::Unk_2B` (`0x140e972c0`, `SetNodeTransformsFromWorldTransform`);
    -   the same slot of `bhkPCollisionObject` and `bhkSPCollisionObject`;
    -   `bhkBlendCollisionObject` (ragdolls);
    -   `FOIslandActivationListener`.

    A keyframed body takes the other branch of `Unk_2B`: `NiAVObject::RecalculateWorldTransform`, since its node
    drives the body. Dynamic clutter that never sleeps calls it every frame, about 10 nodes at Riverwood.
-   **`NiAVObject::SetMotionType`** (`0x140e87270`) runs a subtree visitor (`FUN_140e87df0`, operation 5). On a
    static reference's fixed body it returned true and left the motion `kFixed`.

## Object LOD: segments and where LOD is drawn

All addresses are for AE 1.6.1170 (dclf-lod.md has the DCLF side).

**The shapes.** Object LOD blocks hang under `TES::lodLandRoot`, each block a `BSMultiBoundNode` of `BSSubIndexTriShape`s
(geometry type 8; names `obj`, `objHD`, `objsnow`, with `-LargeRef` for the large-reference grid's). Their Lighting
properties carry `kLODObjects` (technique 13) or `kHDLODObjects` (technique 15). The engine never registers them into a
shadow mode: no shadow cull reaches the LOD root.

**Segments.** A shape has one segment per cell of its terrain node (`level²`, a `0x14`-byte record each: first index,
triangles, an enabled byte at `+0x08`, a run's triangles at `+0x0C`, a run-start byte at `+0x10`). The shape holds:
- the records at `+0x160`;
- their count at `+0x168`;
- the run count at `+0x16C`, which `OnVisible` reads (`BSSubIndexTriShape::OnVisible`, `0x140e312c0`: nothing when 0,
  else the plain geometry cull);
- a dirty byte at `+0x170`;
- a non-segmented byte at `+0x171`.

**The writers.**
- `FUN_140e31130(shape, segment)` shows a segment (enabled when it has triangles), and `FUN_140e31160` hides one. Both
  set the dirty byte.
- `FUN_140e310b0` shows the shape whole: one run from segment 0 over `triangleCount`. It clears the dirty byte.
- Their callers are the terrain manager's per-block updates, `FUN_1405110b0` (the active grid) and `FUN_1405112f0` (the
  large-reference grid, through `BSDistantObjectLargeRefExtraData`). For a node with `kInActiveGrid` (or
  `kInLargeRefGrid`), they hide the segment of every cell whose state is attached (`FUN_14019cdf0(tes, cell, 1)`: the
  cell's `+0x44` state 6 or 7) and show the rest. A node outside the grid is shown whole.
- Two other show-all callers (`FUN_1404fdcf0`, `FUN_140509550`) and the LOD block builder (`FUN_140e55790`, which also
  writes records directly: `FUN_140e30ff0`, `FUN_140e31060`) complete the list. `FUN_140e30fb0` is the
  `BSSubIndexLandTriShape` builder's (`FUN_141521890`).
- So object LOD hides itself cell by cell under the loaded cells.

**The draw.** `FUN_1414f2ad0`, the geometry draw's type switch, case 8:
1. It rebuilds the runs when dirty (`FUN_140e31180`: walking back, consecutive enabled segments merge into one run whose
   start is flagged with the run's triangles; an empty segment ahead of a run becomes its start).
2. It draws each run: `FUN_140e464d0(renderer, buffers, firstIndex, triangles)`, which is
   `DrawIndexed(triangles * 3, firstIndex, 0)`.

A non-segmented shape (`+0x171`) draws whole. The global byte `0x143284cc0` draws every shape whole (a debug switch).

**Where LOD is drawn.**
- **The main view.** It draws object LOD in its depth prepass (Utility) and in the deferred main pass, in write mode 1
  whether alpha-tested or not. Write mode 1 is the plain opaque group's: CS's deferred variant writes RGB only on every
  target, so the G-buffer's alphas keep what lies beneath. The alpha-tested group's write mode 10 writes every channel.
- **Water reflections.** `TESWaterReflections::Update` (`0x140520570`, via `FUN_140521c80`) renders a reflection view
  before the world render. It draws far more LOD than the main view at a vista: about 57 object LOD, 17 HD object LOD and
  50 terrain LOD draws a frame, through the same batch-renderer loops (write modes 1 and 11).
- **The main menu's backdrop.** `FUN_140972590` (`UI3DSceneManager`'s scene, under a menu's `PostDisplay`) draws LOD
  too, before gameplay.

## Terrain LOD: registration, constants and placement

All addresses are for AE 1.6.1170 (dclf-lod.md, "Terrain LOD", has the DCLF side).

**The shapes.** A terrain LOD block is a loaded BTR: a `BSMultiBoundNode` named `chunk` holding one `BSTriShape` `Land`,
under a level node (`4`, `8`, …) under `LandLOD` (`TES::lodLandRoot`), which hangs from `LODRoot` (a `BSClearZNode`). Its
Lighting property has `kLODLandscape` (technique 18, LODLandNoise; `SetupTechnique` draws it as LODLand, 9, unless the
noise global `0x142032fdb` is set). No shadow mode registers it.

**Registration.** `BSLightingShaderProperty::GetRenderPasses` gives its main pass accumulation hint 6. The main mode's
registration (`FUN_1414b2330`) maps hints to geometry groups with direct calls to `FUN_1414f5090` (not through
`BSBatchRenderer::RegisterPass`):

| hint | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 9 | 11 | 13 | 14 | 15 | 16 | 17, 18 | other |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| group | `FUN_1414f50b0` | 3 (only with `+0x12c`) | 4 | 5 | 6 | 0 | 1 | 7 | 9 | 10 | 11 | 8 | 12 | 13 | `RegisterPass` |

So terrain LOD (hint 6) is in group 0 and object LOD (hint 7) in group 1. A multi-index shape's layer goes into group 2
with hint 12.

**Where it is drawn.** The deferred pass (`FUN_1414b2d90`) draws groups 9, 8 and 1 with depth mode 4, then group 0 with
depth mode 3 (test and write). A group whose `+0x26` bit 0 is set draws its plain pass list through
`FUN_1414f19d0`, which calls `RenderPassImmediately` (`FUN_1414f3dc0`) at `0x1414f1acd`. Terrain LOD's colour draws come
from there, after object LOD's. Its depth prepass draw is a Utility pass through the usual batch loops.

**Technique state** (`BSLightingShader::SetupTechnique`, `0x1414db810`, case 9 and 0x12):
- Slots 0 and 1 (diffuse, normal): address mode 3, filter mode 1 (bilinear).
- `HighDetailRange` (VS PerTechnique) = (`x − posAdjust.x`, `y − posAdjust.y`, `w − 15`, `h − 15`) from the globals at
  `0x142033094`. `FUN_141480ab0` writes them from the loaded grid's bounds, four integers `(minX, maxY, maxX, minY)`:
  the centre in x and y, and the half extents in z and w. The 15 is `0x141ad28d0`. The vertex shader lowers the LOD land
  inside this range, under the loaded cells. The writers are the terrain manager's update (`FUN_14050e430`) and the
  map (`FUN_1409892e0`).

**Geometry state** (`BSLightingShader::SetupGeometry`, `0x1414dd040`, case 9, 0xd, 0xf and 0x12: all LOD):
`World` is the geometry's world transform less `posAdjust`; `PreviousWorld` is the **current** world transform less the
previous `posAdjust`, not `NiAVObject::previousWorld`. A chunk keeps the `previousWorld` it was loaded with (the origin)
when the terrain manager places it, so `previousWorld` is wrong for LOD and the engine never reads it.

**Textures.** The LOD diffuse maps (`Textures\Terrain\<world>\<world>.<level>.<x>.<y>.DDS`) can be
`DXGI_FORMAT_B8G8R8X8_UNORM`.

## Where the engine imports the world: threads (2026-10-09)

Ghidra, AE 1.6.1170; measured by DCLF's capture (`dclf-async-publication.md`: world attaches came from loader and job threads,
never the main thread, in motion).

-   **Loader threads** (`BSTaskManagerThread::Func2` 0x140dfe5f0, the IOManager's six): `QueuedReference` (and Actor,
    Character, Tree) slot 24 `FUN_14018e180` runs `ref->Load3D(true)` (vfunc +0x350): the NIF, the model, a parentless
    subtree.
-   **The completion pump** `FUN_140e014e0(IOManager)` finishes queued tasks (vfunc +0x30) on a time budget: from `Job_IO`
    (0x1406d3060, in the "Main render scene" job list: a job thread), or directly from `Main::Update` (0x140646467) in menu
    mode; the loading screen drains through `FUN_140e01800`. `QueuedReference`'s finish (0x14018e770) -> `FUN_1401a0920`:
    the 3D installed (`FUN_1402e5380`), the reference placed in its cell (`FUN_1402d1280` -> `AttachChild` into the
    category node, `FUN_1402d21f0`), an actor added to the process lists.
-   **`ShouldUseTaskQueue`** (0x1406d1ee0, ~130 sites): while other job threads run, shared-state work (attach into a live
    parent, Havok add/remove: tasks 4, 0x32, 0x46) is queued to BSTaskPool and drained by `Main::Update` at its end
    (`FUN_140654880` -> `BSTaskPool_HandleTask` 0x140656f30, a quiet point).
-   **Main thread proper:** the grid controller `FUN_14019b0b0` (cell attach and detach, synchronous `Load3D` fallbacks,
    the terrain update at grid changes), the BSTaskPool drain, the menu and load-screen pumps.
-   **LOD:** `BGSQueued{Object,Terrain}{Upgrade,Downgrade,InitialLoad}::Func2` (object upgrade `0x140513320`, terrain
    upgrade `0x140512c90`) run from the drain `FUN_140513840` at the end of the terrain manager update `FUN_14050e430`:
    `Job_Partial_Terrain_Manager_Update` (0x1406d3880, "Main update cells": a job thread) or the grid controller (main).
    An upgrade in one call attaches the new blocks (`FUN_1404fda90` / `FUN_140509550`), shows them (`FUN_140e310b0`) and
    retires the old block (`FUN_140510a10` -> detach `FUN_1404fd970` and release `FUN_140501d00`; terrain
    `FUN_140509770`/`FUN_14050d170`).
-   **Measured** (DCLF's `CS_DCLF_TIMELINE`, Riverwood in motion): the main thread's share is the grid controller's 0.01-0.08
    ms a frame; the I/O pump, the references' finishes, the LOD drains and the block retirements ran on job threads only.
-   **`TESWaterReflections::Update`** (0x140520570) sets kHidden on the player's 3D and on every water object's node
    (`TESWaterSystem` +0x20, count +0x30) before the cube map's faces and restores it after (0x14052068c / 0x140520a8c): a
    hide scoped to the faces, every frame the faces update.
-   **LOD shows:** `FUN_1405103e0` (store at 0x140510697) shows object LOD blocks and `FUN_140509b50` (0x140509b59) terrain
    blocks, outside the attach.
-   **Moving the rest off the main thread** meets Havok world edits, unsynchronised `ProcessLists`/`GarbageCollector`
    inserts, the per-reference owner lock (`+0x88`) and the grid controller's ordering; the main thread mostly waits on
    these jobs already.

## LOD swaps and their gates

Ghidra, AE 1.6.1170 (2026-10-11); the bytes of every site DCLF patches read from the binary. DCLF's side is
`Engine/LodGates.{h,cpp}` (T6b5, `CS_DCLF_LOD_GATES`).

**The quadtree node** (`0x50` bytes): `+0x00` the manager, `+0x08` the terrain block handle, `+0x10` the object block handle,
`+0x18` the tree handle, `+0x20`/`+0x28` the map's fixed-level terrain/object handles, `+0x30` the children (four contiguous
nodes, or null), `+0x38` the parent (not verified here), `+0x40` flags: the level is `(flags >> 21) & 0x3FC` (4 to 32); bit 0 skips a node's
retirement in `FUN_140510c20`; object bits `0x20` unloaded, `0x80` this level attached, `0x40` the children's; terrain `2`/`4`/`8`
likewise; `0x4000`/`0x8000` the node inside the two grid radii (`FUN_14050fe10`, every update).

**A handle**: `+0x0C` state and references (state bits `0x70000000`; loaded is state 3 or 4, the test
`((s & 0x70000000) + 0xD0000000) & 0xEFFFFFFF == 0`; the last reference moves `0x30000001` to `0x40000000` and frees it), `+0x28`
the block. A block is cached per node: a swap back while a reference lives finds the same handle.
- **Object block**: `+0x00` its quadtree node, `+0x08` its `NiNode` (attached to the LOD root `0x14315b8a0`), `+0x11` attached.
  Attach `FUN_1404fda90(block)` (only when `+0x11` is 0: `AttachChild`, the segments shown whole, `Update`); detach
  `FUN_1404fd970(block)` (`DetachChild` through vfunc `+0x1c0`, `+0x11` cleared). Release one reference `FUN_140501d00(handle)`.
- **Terrain block**: `+0x08` the land node and `+0x10` the water node (the nodes its show and hide store to), `+0x18` the
  container attached to the level node (`FUN_14050e830(level)`), `+0x20` loaded, `+0x21` attached, `+0x22` water registered.
  Attach `FUN_140509550(block)` (only loaded, unattached, with a container: the container attached, the water node detached from
  it and registered, `FUN_1405253a0`); detach `FUN_140509770(block)` (unregisters the water, `FUN_1405254d0`). Release
  `FUN_14050d170(handle)`.

**Retirement** `FUN_140510a10(node, bits)` (memory context `0x55` in the TLS block at `+0x768`): bit 1 terrain (detach when loaded
and the node's `(flags & 0x7F800000) <= 0x4000000`, release when non-null, flags `|= 2`), bit 2 objects (detach when loaded,
release, flags `|= 0x20`), `0x10` the map's handles, 4 trees. A null handle skips both its detach and its release: taking the
handle from the node first leaves the block attached and shown, its reference the taker's. `FUN_140510c20(node, bits)` runs it on
the node (unless flags bit 0) and recurses into every descendant (the call at `0x140510c3a`). Other callers retire without a
replacement: `FUN_1405103e0` (objects out of range, `0x140510499`/`0x140510534`, children through `FUN_140510c20`), the terrain
update `FUN_140510110`, `FUN_140510730`, `FUN_1405139b0`, `FUN_140513ea0`, a worldspace's unload `FUN_14050fb70` (`0x3F`).

**The swaps** (vtable `Func2`s), run by the drain `FUN_140513840` (vfunc `+0x10` per queued swap), called at `0x14050e7c8` at the
end of the terrain manager update `FUN_14050e430` under its recursive `BSSpinLock` `0x14315b8c8` (taken at `0x14050e5c4`, released
after the drain), on a job thread (`Job_Partial_Terrain_Manager_Update`) or the main thread (the grid controller, `0x14019bcbf`);
also from the map's `FUN_14050f1b0` (drained to empty).

| swap | attach | retire |
| --- | --- | --- |
| object upgrade `0x140513320` | each ready child's block, `0x14051339a` (`E8 F1 A6 FE FF`); its handle into the child's `+0x10` | `FUN_140510a10(parent, 2)` at `0x14051340c` (`E8 FF D5 FF FF`; `MOV EDX,2` at `0x140513407`) |
| terrain upgrade `0x140512c90` | each child's block (no state test), `0x140512ceb` (`E8 60 68 FF FF`) | `FUN_140510a10(parent, 1)` at `0x140512d57` (`E8 B4 DC FF FF`) |
| object downgrade `0x140513600` | the parent's block, `0x140513615` (`E8 76 A4 FE FF`); a different old parent handle released | `FUN_140510c20(child, 2)` per child at `0x140513671` (`E8 AA D5 FF FF`), four calls |
| terrain downgrade `0x140512f40` | the parent's block, `0x140512f55` (`E8 F6 65 FF FF`) | `FUN_140510c20(child, 1)` per child at `0x140512fb1` (`E8 6A DC FF FF`) |
| object initial load `0x140513740` | the node's own block, `0x140513789` (`E8 02 A3 FE FF`); its handle into the node's `+0x10` first (a different old one released), flags `&= ~0x160`, `\|= 0x80` | none |
| terrain initial load `0x140513080` | the item's block, `0x140513091` (`E8 BA 64 FF FF`), then its handle into the node's `+0x08`, flags `&= ~0x1A`, `\|= 4` | none |

**The initial loads** (vtables `0x1418690d8` objects, `0x141869030` terrain; `Func2` at `+0x10`). Queued by the per-node
dispatch `FUN_140513820` -> `FUN_140513ea0` (objects) / `FUN_1405139b0` (terrain) for a node marked unloaded (`0x20` / `2`) when
neither its parent's level is attached nor any ancestor holds a loaded handle: the first level of an area with nothing above it
loaded (every node at a worldspace's load; in play, an area coming into LOD range or back after the out-of-range retirement). With
an ancestor loaded the same dispatch queues an upgrade of that ancestor instead. Neither is queued while `TES` (`0x1431872c8`)
`+0x137` or `+0x138` is set (the same two bytes refuse a load screen in `FUN_1401a1150`). Under a load screen (`DAT_14315b6a0`,
set by `FUN_1401a1150(1, ..)` as it opens the "Loading Menu", cleared by its close after one last `FUN_14050e430(.., 1)`: a full
rebuild) the dispatch first retires the node's subtree (`FUN_140510c20(node, bits)`) and the ancestors up to the manager's level.
Nothing is retired by the `Func2` itself, and its attach is shown (the update's show stores keep it so).

**Shows and hides**: app-cull, bit 0 of `NiAVObject` `+0xF4`. Objects: `FUN_1405103e0` every update for a node with `0x80` (show at
`0x140510697`, `AND`; hide at `0x1405106a4`, `OR`). Terrain: `FUN_140510110` -> `FUN_140509b50` (shows `+0x08` and `+0x10`,
`0x140509b59`/`0x140509b69`) and `FUN_140509b20` (hides, `0x140509b29`/`0x140509b39`). The map: `FUN_140511680` (to the fixed
levels: attach and show, `0x1405117d0`; hide `0x140511808`), `FUN_140511860` (back: `0x14051192d`, `0x140511972`, detaching the
fixed-level blocks unless they are the node's own), `FUN_140511b40(node, show)` (every block, `MOV` at `0x140511c00`; from the
world map `FUN_1409892e0`/`FUN_140989440`). All of these are HiddenStores' patched stores.

**What tears the quadtree down** (where held blocks must go first):
- `FUN_14050ed60` (the manager deactivated: `FUN_140510c20(root, 0x2F/0x3F)`, the three block caches reset): from a worldspace's
  unload `FUN_14050e250` (its first call, `0x14050e2a4`; from `0x140304a5b`; then `FUN_14050fb70` per node and the node arrays
  freed), from `FUN_14050e430` switching managers (`0x14050e612`), and from cell transitions (`FUN_1401a59a0`, `FUN_140648cd0`,
  `FUN_14064a680`, `FUN_1401adce0`).
- `FUN_14050e430`'s LOD-off branch (`*manager != 0`): `FUN_140511860(root)` when active, then the cache flushes
  `FUN_1405099a0(0)` and `FUN_1404fdca0(0)` (a tail `jmp` at `0x14050e490`), outside the lock. The map's `FUN_140988c80`/
  `FUN_140988ea0` call both flushes too.
- `FUN_14050e430`'s early return when it is the current manager (`0x14315b6a8`) and either its `+0x36` byte is 0 or its
  worldspace's `+0xa0` bit `0x40` is set (an interior, LOD hidden): the LOD roots hidden (`0x14315b8a0`, `0x14315b898`, the trees' root) and a tail `jmp` to
  `FUN_140509950(0)` at `0x14050e4e4` (`E9 67 B4 FF FF`); no lock, no drain.
- The full rebuild: `FUN_14050f790(manager, root, .., .., 1)` at `0x14050e7ae` (`E8 DD 0F 00 00`; the manager's first update or
  one asked for, `*param_3 & 1`). Not every `FUN_14050f790(.., 1)`: the round-robin node update (`0x14050e6d1`) passes the node's
  `0x8000` bit, set for every node near the player.

**DCLF's gate** (T6b5): the attaches hide the incoming blocks first (only an unattached block: a reused one stays shown); the
swap's retirement call runs `FUN_140510a10` through a detour that takes each loaded, attached outgoing handle from its node (the
engine skips detach and release); the HiddenStores stubs at the four show stores (`0x140510697`, `0x140509b59`, `0x140509b69`,
`0x1405117d0`) skip a pending incoming node's show; the render thread's flip stores the app-cull bits; the next drain's head (under
the lock) detaches and releases the held blocks through the functions above, or only drops the reference when the node references
the handle again. Everything above that tears the quadtree down flushes the gates first. The initial loads' attaches are gated too
outside a load screen (a gate with no outgoing, emitted right after the attach); under one they pass through (the full rebuild
flushed every gate before that drain). A held block the engine attaches again (a swap back finds the cached handle) races the flip
per block: before the flip hides it, it stays shown as the node's current block and the scene is told (a kept notice); after,
it is the swapping gate's incoming. Known effects of a hold: both terrain
levels' LOD water registered; a held object block's segments are not refreshed.
