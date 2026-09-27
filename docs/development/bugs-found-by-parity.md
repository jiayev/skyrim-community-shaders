# Bugs found by DCLF's parity checks

Bugs found while Drawcall Limit Fix's parity checks compared DCLF's draws with the native ones: in other
Community Shaders features, and in the engine.

-   **Feature bugs** are each their own PR. DCLF works around none of them; the parity checks report them. The
    investigation that found the first two is in [drawcall-limit-fix.md](./drawcall-limit-fix.md), "Scene
    events before the walk".
-   **Engine bugs** cannot be fixed at the source by a PR. Where the native result is undefined, DCLF picks a
    deterministic rule, stated in the entry, and the parity checks compare against that rule.

# Community Shaders features

## TruePBR: a loader thread writes MATO data into materials that are being drawn

**Where:** `TESBoundObject_Clone3D` in `src/TruePBR.cpp` (installed on `TESObjectSTAT`'s vtable, slot
0x4A). It applies a static's material object (MATO) with `ApplyMaterialObjectData`.

**What happens.** For a static with a PBR MATO, the hook visits every geometry of the freshly cloned model
and applies the MATO's roughness, specular level, base colour scale and glint parameters to the geometry's
`BSLightingShaderMaterialPBR`. Two things are wrong:

-   **It writes into shared materials.** The cloned model's materials are shared with references already in
    the scene. The fork-before-write check only forks when the material already has a *different* owner
    (`prevOwnerRefID != 0`) with a different MATO. A material nobody has claimed yet has owner 0, so it is
    written in place, and every reference sharing it picks up this reference's MATO values.
-   **It writes from a loader thread while the frame renders.** `Clone3D` runs on a loader thread. The write
    lands in the middle of a frame, so draws of that material earlier in the frame use the old values, and
    later ones use the new values.

**Evidence** (temporary logging, a flight from the standard save):

```
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7b580 prevOwner 00000000 roughness 1 -> 0.7 forked false
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7ba00 prevOwner 00000000 roughness 1 -> 0.7 forked false
[4300] MATO apply thread 4300 ref 0005C108 material 0x251c9a7b880 prevOwner 00000000 roughness 1 -> 0.7 forked false
```

-   The render thread was 22340.
-   The three materials were already being drawn by other references, `DirtCliffsCornerIn01`,
    `DirtCliffsCornerOut01` and `L2_Roots` (refs 00029EC2, 00029EC3, 00029ECA), with roughness 1.
-   From that frame on, those references were drawn with ref 0005C108's roughness of 0.7.
-   Capture parity saw it as `PS PerMaterial 22` (`ParallaxOccData`, which TruePBR's `SetupMaterial` fills
    from the projected material's roughness and specular level): DCLF 1, native 0.7, on one frame around
    frame 171 after the save loads. It happened on about half of the runs, depending on load timing.

**Proposed fix:** fork before writing whenever the material is not exclusively this reference's:

-   Fork if another property already uses it, not only if another reference already claimed it. The
    material's reference count, or the ownership map `BSLightingShaderMaterialPBR::All` keeps, can tell.
-   A fresh copy is not drawn by anything until the clone is attached, so writing it from the loader thread
    is then safe.

**What to check in the PR:**

-   References that share a model but have no MATO, or a different one, keep their own values after a
    reference with a MATO loads next to them.
-   The number of materials forked on a cell load, which is a memory cost.

**DCLF side:** nothing to change. DCLF learns of the write through `NoteWritten`, which
`ApplyMaterialObjectData` already reports, and corrects its record on the next frame. The one-frame
difference is gone once the write no longer targets a drawn material. Capture parity reports any remaining
case as `mismatched materials: ... written after its last mismatch`.

## Subsurface Scattering: `IsBeastRace` stays set from the previous face or skin draw

**Where:** `SubsurfaceScattering::BSLightingShader_SetupSkin` in `src/Features/SubsurfaceScattering.cpp`,
called from its `BSLightingShader::SetupGeometry` hook.

**What happens.** In the deferred pass, the hook sets or clears
`ExtraShaderDescriptors::IsBeastRace` in the permutation buffer only when the property has `kFace` or
`kFaceGenRGBTint`. For every other draw it leaves the bit as the last such draw set it. `Lighting.hlsl`
(line 3016) writes the bit into `Masks.y` for the `SSS` + `SKIN` permutation, and `SeparableSSSCS.hlsl` uses
`Masks.y > 0` to choose the human or the beast scattering profile.

A skin draw without either flag therefore gets the profile of whichever face or tinted skin was drawn last.
Draws that aren't skin carry the stale bit too, but their permutation doesn't read it.

**Evidence.** Capture parity's permutation check ("permutation parity ... extra 4", `IsBeastRace` being
`1 << 2`) found draws where native had the bit set and DCLF did not. They were NPC skin (`MaleUnderwearBody1`,
`HandMaleBig3rd`, `MaleUnderwearBodyLightArmor`, refs 000977F5 and FF000E9B) and flora (`FloraThistle01`),
52-99 draws in an interval. It happens only on some runs, because it depends on draw order.

**Also to check.** The actor is taken from `geometry->GetUserData()`, and `isBeastRace` defaults to `true`
when that isn't an `Actor`. Skinned body parts may not carry the actor in their own user data (it is usually
on a node above them). If so, every such draw is treated as beast race. Walk up the parents to the owning
reference, as `CaptureParity`'s `Describe` does, and default to human.

**Proposed fix:**

-   In the deferred pass, decide the bit for every lighting draw.
-   Skin (the draws that compile the `SKIN` permutation): set it from the owning actor's race keyword, found
    by walking up to the reference.
-   Everything else: clear it.

The value then depends only on the draw's own object.

**DCLF side, a follow-up to this PR:** DCLF never sets `IsBeastRace` (its permutation's extra field is
`InWorld` plus `SuppressExternalEmittance`), so it matches today only on human skin. Once SSS decides the bit
per draw, DCLF has to model it:

-   the owning actor's race, recorded per object for skin objects;
-   a per-object permutation bit, like `kObjectSuppressExternalEmittance`.

Capture parity's permutation check will then compare it on every skin draw.

## Advanced Skin: the RFAOS and wetness textures are never loaded

**Where:** `Skin::SetupExtraTexture` in `src/Features/Skin.cpp`, called the first time its `SetupMaterial` hook
sees a FaceGen or FaceGenRGBTint material.

**What happens.** The function derives a `_rfaos.dds` and a `_wet.dds` path from the material's specular or
normal map. It writes those paths into the texture set's environment and multilayer slots. But the two textures
it keeps for the material, `rfaosTexture` and `wetnessTexture`, are set to the engine's default white texture,
and nothing replaces them with the files at those paths. The hook then binds them at t71 and t74.
`Lighting.hlsl` treats a texture of 32 texels or less on a side as absent (`hasSkinExtra`, `hasSkinWetness`),
so the per-material roughness, fuzz, AO and specular (t71) and the wetness mask and normal (t74) are never
used.

**Evidence** (a temporary log in capture parity's skin check, Riverwood): every FaceGen material drawn bound
the same view at t71 and t74, a 16x16 texture. The first was the player's head (`MaleHeadNord`, ref 00000014);
no other view appeared. It is not even the view the engine's `defaultTextureWhite` resolves to (1x1), so which
texture ends up bound is worth checking in the PR too.

**Proposed fix:** load the two files when they exist, the way the engine loads a texture set's textures
(through the texture manager, with the material's usual residency), and fall back to the black default
otherwise. Keep the material's entry keyed by its hash key, as now.

**DCLF side:** nothing to change. DCLF binds what `Skin::MaterialTexturesOf` returns for the material, so it
follows the fix. Capture parity's `skin parity` line compares t71 and t74 on every FaceGen draw it checks.

# The engine

## Hair with ProjectedUV shades another object's snow

**Where:** `BSLightingShader::SetupGeometry` (AE `0x1414dd040`), its ProjectedUV block (skyrim-engine-notes.md,
"ProjectedUV and MTLand"). It writes `TextureProj` (VS PerGeometry 6) and `ProjectedUVParams` 1-3 (PS
PerGeometry 12-14) for a pass with descriptor bit 15 (ProjectedUV), **for every technique but Hair (6)**.

**What happens.** Hair parts can carry the ProjectedUV flag. The Riverwood NPCs' hairlines, brows and beards
do: decal-flagged, blended, drawn in the engine's blended decal group (accumulation hint 3). Their pass has
bit 15, so the Lighting permutation is the projected one and its pixel shader reads those constants, but
`SetupGeometry` skips them for Hair. The draw shades its snow or moss projection from whatever the previous
projected draw left in the constant buffers: another object's parameters, which depend on draw order.

**Evidence** (capture parity, `CS_DCLF_OWNERSHIP=0 CS_DCLF_CAPTURE_PARITY=1`, Riverwood): 1,500-2,000
per-geometry mismatches per 300 frames, all on those four variables and all on hair, with native values such
as `TextureProj` 2.35e-38 and -9.0e26, and `ProjectedUVParams` 6.2e21, next to plausible leftovers (0.4264488 on
two different hairlines in one frame).

**DCLF's rule:** hair draws without the projection. `DrawnPassDescriptor` (`LightingDescriptors.h`,
`HairProjection`) drops bit 15 from a Hair pass, so DCLF's pipeline is the unprojected permutation and writes
no projected constants. Capture parity leaves that bit out of its descriptor and permutation comparisons for
such passes, and is then OK (0 mismatched) at Riverwood.
