# Drawcall Limit Fix: open defects

Defects found while investigating the "road lit as if in direct sunlight" report, at the save on the road by
the large tree. That report's cause was that every DCLF sampler was an empty descriptor (fixed; see
"Every DCLF draw sampled through an empty sampler descriptor" in [drawcall-limit-fix.md](./drawcall-limit-fix.md)).
Each is reproducible with the probes named below.

## The far cascade's remaining differences

The cascades' caster sets were fixed (see "The far cascade: DCLF drew the wrong set of casters" in
[drawcall-limit-fix.md](./drawcall-limit-fix.md)); the far cascade matches native within the scene's drift
across a live toggle. What is left, at the road save:

-   **0-3 extra casters per far-cascade report**: fixed by the sun's entry rule (see "Shadow-only casters,
    and the sun's entry rule" in [drawcall-limit-fix.md](./drawcall-limit-fix.md)). What remains is 1-2
    carried items a frame, pruned by the engine at a node below their actor's entry, with no visible effect.

The near cascade's step and Volumetric Shadows' are fixed: see "Shadow views draw with the view's rasterizer
state" and "The volumetric lighting copy is the engine's alone" in [drawcall-limit-fix.md](./drawcall-limit-fix.md).

## BasicRHI's `frontCCW` under Vulkan, outside DCLF

BasicRHI's `frontCCW` now has D3D's meaning on both backends (see "Front faces" in
[drawcall-limit-fix.md](./drawcall-limit-fix.md)). SARP and BasicRenderer set it true for glTF content; their
D3D12 behaviour is unchanged, and they have not been run under Vulkan since the mapping changed back.

## Opaque pipelines write alpha that the engine's opaque draws leave alone

**Evidence.** The write masks of the blend state bound at native deferred Lighting draws (Whiterun road save,
one interval):

| Targets 0-7 write mask | Engine write mode | Draws |
| --- | --- | --- |
| `7 F F 7 7 7 7 7` | 1 (opaque) | 16805 |
| `F F F F F F F F` | 10 (opaque decals) | 1846 |
| `7b Fb Fb 7b 7b 7b 7b 7b` | 1, blend mode 1 | 1065 |

Write mode 1 writes only RGB to target 0 (`kMAIN`) and targets 3-7 (Albedo, Specular, Reflectance, Masks,
Masks2); Community Shaders' `Deferred::OverrideBlendStates` copies target 0's mask to targets 3-7. DCLF's
opaque pipelines use the default blend state, which writes all four channels, so they write the pixel
shader's alpha where the engine keeps the target's cleared value. At the road: target 0's alpha is 1.0
natively and 0.0 under DCLF (the road's vertex alpha is 0).

Nothing found reads those alphas today (`DeferredCompositeCS` and Subsurface Scattering read RGB only), so
the image does not change. It is still a parity gap.

**Fix:** build opaque pipelines from the engine's blend state for their write mode, as decal pipelines
already do (`DrawPipelines::ReadEngineState`).

## DCLF's draws appear not to carry the TAA / upscaler jitter

**Probably explained by the empty samplers; to re-measure.** A sampler that neither filters nor selects
mips returns the same texel for sub-pixel movement, which is what this measured.

**Evidence (inferred).** With `CS_DCLF_TARGET_PROBE` at one road pixel, every G-buffer target DCLF writes
holds the same value frame after frame (normal, roughness, AO to four decimals), while the native draw's
values move every frame. The camera is still, so the native variation is the sub-pixel jitter; DCLF's lack
of it suggests its vertex stage runs without the jitter. The colour epoch replays the Z-prepass's
vertex-stage constants (`prepassVS`), which come from the native depth pass.

**Why it matters:** temporal AA and the upscalers accumulate over the jitter sequence, so unjittered objects
lose their anti-aliasing and resolve detail, and may shimmer against jittered native objects.

**Next step:** compare the projection that DCLF's epochs pack (`VS_PerFrame.ViewProj`) with the jittered
one the native main pass binds, on the same frame.

## The VSM soft shadow differs at the road

**To re-measure after the sampler fix:** the VSM lookup samples through `LinearSampler`.

**Evidence.** Pixel shader telemetry (`CS_DCLF_SHADOW_DEBUG_OUTPUT`, `ShadowSampling::GetLightingShadow` at
the road pixel): DCLF 0.0005, native 1.0, from the same permutation, the same t18 VSM and the same t98
cascade data (`CS_DCLF_SHADOWS=0`, so the shadow maps match native). It only feeds soft lighting and coat
terms, so it is not the brightness, and it is not explained yet.

**Next step:** the VSM lookup inputs per pixel: the light-space position, the cascade picked and the moments
read, native against DCLF.

## Probes added for this investigation

-   `CS_DCLF_SHADOWMAP_PROBE=1`: the sun's cascade texture and the VSM copy, summarised per slice and mip.
-   `CS_DCLF_CASCADE_PROBE=1`: per shadow view, DCLF's culled caster set against the engine's registrations
    (recorded at `PassCapture::Withhold`), the difference by cull reason, radius and depth, and the engine's
    cull volumes, camera, ancestry and caster-rule inputs for both sets.
-   `CS_DCLF_SHADOWMASK_PROBE=x,y`: the engine's shadow mask at a pixel.
-   `CS_DCLF_TARGET_PROBE=x,y`: every bound G-buffer target, averaged over a 64 x 64 block, where the opaque
    pass ends, DCLF on or off.
-   `CS_DCLF_SHADOW_DEBUG_OUTPUT=1`: a global shader define (`DCLF_SHADOW_DEBUG`) that makes Lighting.hlsl
    write chosen intermediate values into the Diffuse target, native and DCLF alike, cached apart.
    Changing the deployed Lighting.hlsl makes Community Shaders recompile its whole shader cache on the next
    launch, and DCLF does not start until that finishes.
-   The colour epoch's report now names frame textures a pipeline reads that the commit could not resolve.
-   The scene report counts the objects whose pass descriptor carries the sun's shadow mask bits and the
    objects with derived descriptors.
