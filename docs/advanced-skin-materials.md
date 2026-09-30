# Advanced Skin material system

For creation steps, see the [artist guide](advanced-skin-artist-guide.md). This document describes the material contract and implementation boundaries.

## Asset and assignment model

A material belongs to the mesh's `BSLightingShaderProperty`. Its typed extra data stores parameters; its ordinary `BSShaderTextureSet` stores texture paths. Preserve the native FaceGen (4) or FaceGenRGBTint (5) shader, flags, UV transform, skinning and face generation behavior. Environment mapping, multilayer parallax, decals and menu materials are incompatible.

`NiIntegerExtraData` named `CS_SkinVersion`, Integer Data `1`, enables the extension and identifies its storage format. This is a compatibility discriminator, not a feature tier. An absent marker uses native shading. Invalid, duplicate or unsupported fields produce diagnostics and disable the extension. Runtime finite floats are clamped; the authoring tool refuses invalid source data so it cannot silently rewrite it.

Parameters are optional and default as follows. Attach these blocks to the **shader property**, not the geometry or root.

| Extra-data name          | Type               | Default | Range   |
| ------------------------ | ------------------ | ------: | ------- |
| CS_SkinRoughness         | NiFloatExtraData   |     0.6 | 0–1     |
| CS_SkinReflectance       | NiFloatExtraData   |   0.028 | 0–0.08  |
| CS_SkinFuzz              | NiFloatExtraData   |    0.25 | 0–1     |
| CS_SkinDetailEnabled     | NiIntegerExtraData |       1 | 0 or 1  |
| CS_SkinDetailStrength    | NiFloatExtraData   |    0.25 | 0–1     |
| CS_SkinDetailTiling      | NiFloatExtraData   |      10 | 0.1–100 |
| CS_SkinSSSAmount         | NiFloatExtraData   |       1 | 0–1     |
| CS_SkinTransmission      | NiFloatExtraData   |     0.1 | 0–1     |
| CS_SkinTransmissionDepth | NiFloatExtraData   |     0.2 | 0.001–1 |
| CS_SkinWetResponse       | NiFloatExtraData   |       1 | 0–1     |

## Texture contract

Slot numbers below are zero-based **NIF array indices**, not ESP TX field numbers. All extension channels are sampled as linear data, including files tagged sRGB. Paths must be relative `textures/...dds` resources; optional `Data/` is normalized away. There is no suffix search, path-based material assignment, JSON registry or race-name special case.

| NIF slot      | Purpose                                     | Channels                                                                  | Native TXST field    |
| ------------- | ------------------------------------------- | ------------------------------------------------------------------------- | -------------------- |
| 0, 1, 3, 6, 7 | Native diffuse, normal and face inputs      | Native meanings preserved                                                 | Native mapping       |
| 2             | Native skin texture; transmission thickness | `thickness = 1 - saturate(R)`; white is thin                              | TX03                 |
| 4             | Optional wet surface                        | RGB tangent normal, A wet response mask                                   | TX05                 |
| 5             | Optional Skin controls / RFAOS              | R roughness multiplier, G fuzz multiplier, B AO, A reflectance multiplier | TX02                 |
| 8             | Optional detail                             | RGB tangent normal, A main-UV region strength                             | No native TXST field |

Controls multiply the corresponding material scalars. The resulting perceptual roughness is clamped to 0.02–1, F0 to 0–0.08. Missing controls use white multipliers; missing wet input uses a flat normal and white mask. AO affects indirect lighting. Old detail AO has no runtime meaning.

Extension resources use an independent 2D DDS loader. In particular, native slot 4 is routed as a cubemap by the engine; the Skin shader must not use that native resource. Standard RGBA8, BC1/2/3/7 DDS formats are supported, with BC7 or uncompressed RGBA8 preferred for masks. BC5 UNORM is supported only for detail: reconstruct positive Z and use mask 1. Signed BC5, arrays, volumes, cubemaps and unsupported formats are rejected. A broken explicit detail texture disables that detail input rather than substituting the global one.

Detail RGB samples transformed main UV multiplied by detail tiling, with a wrap sampler. Alpha samples transformed main UV separately, with native addressing and its own derivatives/mip selection. Thus mask regions do not move when tiling changes. Zero strength, black mask or disabled detail leaves the base normal unchanged. Tangent-space detail is composed using RNM; model-space base normals use a surface basis derived from world position and main UV.

An empty detail slot inherits the global texture, strength and tiling, including the global body tiling multiplier outside the head. A custom detail slot uses its material strength and tiling without global multiplication. The global detail switch and material enable-detail switch both gate detail. The shipped global normal is BC5 UNORM with no AO channel.

Post-process SSS continues to receive the existing scalar `baseColor.a * SSSAmount` through the existing render target. It retains the existing SSS feature and profile. The native skin texture drives only the direct-light transmission term, controlled independently by transmission strength and depth. No render-target channel is added.

## Native assignment and source tracking

Nonempty extension inputs from the **actually applied** TXST replace NIF slots 4 and 5. Empty TXST extension inputs inherit the original NIF baseline, never the previous TXST. Native texture slots keep native replacement behavior. Slot 8 and typed parameters remain in the NIF. Unmarked assets do not reinterpret extension slots; a character instance can explicitly enable a material using neutral defaults.

Source tracking observes property PostLink, property clones, native TXST conversion and head-part preparation. Conversion receipts refer to the actual generated texture-set object retained by the material. Path snapshots only invalidate receipts after mutation; matching paths never discover a source. Head receipts carry the actual head part, NPC and selected TXST. An unknown replacement of the normal or texture set invalidates a head receipt. Tint generation does not define material identity.

SE 1.5.97 and AE 1.6.1170 native entries were inspected in Ghidra for conversion, head preparation, PostLink and texture loading. Address-library pairs used for the three detours are 20905/21361, 26259/26838 and 26260/26839. Property hooks use the existing CommonLib virtual interfaces. VR does not install these three detours or expose persistent character targets; third-party pipelines that bypass the observed engine entries remain preview-only where provenance is unavailable.

## Character customization and persistence

Players and NPCs use the same per-actor instances. Logical parts are Face, Body, Hands and Feet, with explicitly identified first-person and third-person surfaces. Face identity comes from observed head-part preparation. Body identity comes from actual biped clone ancestry and ARMO/ARMA assignment. A material type or node name alone does not establish a part.

Each part can retain separate sparse overrides for selected surfaces. Guards contain plugin-local race/head-part/ARMO/ARMA identities, sex, source material location, view, full bounded UV/topology/normal-space layout data, native UV transform, and base DDS paths/content fingerprints. Positions and generated tint are excluded. Changed appearances pause affected surfaces; matching context resumes them. Explicit rebind replaces the selected material slot's guard and retains unrelated surfaces.

Committed settings use Data/SKSE/Plugins/CommunityShaders/AdvancedSkin/State.json. The editor can open another configuration or save a copy to an explicit writable location; Location.json remembers that choice. **Save and apply** writes immediately, stages and reads back output, checks disk conflicts, and retains recovery backups before publishing the runtime snapshot. Failure retains the previous committed state and draft. **Undo last saved character edit** is another persisted operation.

The Player key is shared by playthroughs using this configuration. Loading an older save or starting a new game does not restore historical Skin settings. Choose a scheme explicitly to switch settings. NPC keys identify placed actor references by originating plugin and local ID, never all instances of an NPC base. Dynamically generated references receive session-only settings; read/load/menu lifecycle changes clear those instances. Lifecycle changes invalidate runtime handles and previews while preserving external persistent state.

No Skin SKSE serialization callbacks are registered. Old Skin co-save records are not automatically imported and other plugins' co-saves are untouched. Configuration limits are 64 MiB, 1024 actors, four parts, 32 surface records per part, 1024-byte resource/reference strings and 4 MiB of encoded layout per surface. Invalid or unsupported files are rejected, not replaced with empty data. **Restore latest valid backup** explicitly restores a validated publication backup.

**Copy Skin settings from player** captures committed effective settings, including inherited NIF/TXST inputs, while preserving global-detail fallback. It never copies preview edits, wetness, native color/normal/thickness textures, race or morphs. Receivers get independent values and their own adaptation guards. **Use as copy source** and **Apply copied scheme** support other source actors; named multi-part schemes use the same transfer pipeline.

Automatic transfer requires matching part/view, unambiguous source values, layout and UV transform. Source and receiver need not share race, head-part, ARMA or native texture paths. Unsupported or missing parts open a review rather than silently applying a subset. Manual mappings require receiver preview and explicit texture-mapping confirmation. Selected surface updates commit together; unrelated parts remain unchanged. Schemes list DDS inputs in the transfer review, and DDS files remain dependencies when sharing.

In-place DDS edits require **Reload skin resources**, which refreshes content fingerprints. No filesystem watcher is installed. Configurations apply to every game session using the selected directory; mod-manager profile isolation is not assumed.

## Editor and publication

The CS Editor's **Skin Editor** works on source files, with a separate temporary preview on explicitly selected loaded geometry. Parameters use localized controls and DDS file pickers. Undo/redo covers draft parameter and texture edits. Switching away from an unsaved document or character draft asks whether to keep editing or discard it. Closing the editor cancels preview.

Source provenance includes the NIF path, property block and file fingerprint. **Edit source NIF** requires the loaded snapshot to match. Otherwise the user opens a source explicitly. Live FaceGen, morph, pose or skinning buffers are never serialized as asset geometry.

The narrow writer supports SSE 20.2.0.7, user version 12, stream 100, little endian, without block groups. It preserves existing block indices, strings, unknown blocks, native shader tails, geometry, weights, partitions and controllers. It appends typed extras and texture sets, replacing only selected property data. **Make independent** clones a shared property and changes one supported shape's property link. Unsupported or invalid materials remain read-only. Unreferenced superseded blocks are deliberately preserved rather than rewriting unknown references.

Saving compares the source bytes, reparses output, verifies changed material values and unchanged blocks, stages and reads back all files, then publishes with recovery backups. Multi-file output stages everything before replacing anything and attempts rollback on failure; it is not a filesystem transaction. Recovery failures are reported with paths. A pre-existing `.cs-skin.tmp` is never silently overwritten.

**Save NIF with Skin textures** chooses a NIF beneath the destination mod's `meshes` directory and lists the NIF and all explicit extension DDS dependencies before writing. `textures` is placed alongside `meshes`. Native diffuse, normal and skin textures remain dependencies of the original mod. Batch output uses individually selected NIFs, property blocks and destinations. It does not enumerate assets using name patterns.

Material presets are ordinary material-only NIFs. The shipped `meshes/CS/Skin/SkinMaterial.nif` is a template, not a renderable actor mesh. Preset import lets the author select parameters and textures. Built-in texture tools combine grayscale controls or detail RGB plus an independently authored UV mask and generate linear DDS mipmaps. No artist Python installation or JSON copy/paste is required. An explicit, approximate importer can recover complete materials from old JSON exports; it does not import assignment rules, detail AO or old wet encoding.

## Code organization and localization

-   `Material/`: typed contract, defaults, validation and sparse overrides.
-   `Runtime/`: native source receipts, Actor target description, textures, rendering and wetness.
-   `Persistence/`: bounded external configurations, portable schemes and runtime lifecycle.
-   `Editor/`: NIF preservation, publication, texture tools, legacy import and UI.

Skin UI and errors use the existing `feature.skin.*` translation namespace, inline English defaults, generated `en.json` and `zh_CN.json`. Wire names and file field names remain stable technical identifiers. Other languages use the project's normal English fallback. CS global settings retain their existing settings persistence; JSON is not an asset material authoring format.

## Validation boundary

This change has only static review and asset-data checks. No project build, shader compilation or in-game acceptance run was performed. Before release, verify SE/AE loading and cloning, actual TXST replacement, custom detail versus global inheritance, mask UV/tiling at multiple mip levels, zero-mask base-normal preservation, wetness, native SSS versus transmission, shared-property save/reopen, source conflicts and failed publication recovery. Exercise RaceMenu race/sex/head/body changes, ordinary morphs, first-person hands, matching-context resume, explicit rebind, immediate external save, failed publication, copy/undo, scheme import, older saves, new games and dynamic NPC session boundaries. VR and pipelines without verified provenance must remain visibly limited.

Format reference: [nifly's native shader serialization](https://github.com/ousnius/nifly/blob/main/src/Shaders.cpp) and [NIF object serialization](https://github.com/ousnius/nifly/blob/main/src/Objects.cpp).
