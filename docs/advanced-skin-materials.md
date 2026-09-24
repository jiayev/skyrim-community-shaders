# Advanced Skin asset materials (v1)

This implementation replaces the old Skin profile/rule engine. Path selectors,
glob matching, geometry-signature origin inference, material inheritance trees,
and `_rfaos` / `_wet` discovery are no longer runtime inputs. Existing global
wetness settings remain. This intentionally changes the appearance of assets
that depended on legacy rules.

## Supported scope

| Source / workflow                                                 | Current implementation                                                                                              |
| ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `CSSkinMaterial` on the rendered geometry                         | Complete material plus stable surface UUID                                                                          |
| Direct native `BGSTextureSet` on non-head FaceGenRGBTint material | Exact record assignment; no texture-path comparison                                                                 |
| Head TXST selection                                               | Capture the engine-selected record on the specific property during head-part preparation (SE/AE)                    |
| Converted alternate textures                                      | Capture native TXST → runtime texture-set conversion; accept only the set actually retained by the material (SE/AE) |
| Race and NPC adjustments                                          | Exact current race / actor-base FormKey, parameters only                                                            |
| Local edits                                                       | Surface, NPC base, or NPC base × surface; unsaved preview is separate                                               |
| NIF publisher                                                     | SSE 20.2.0.7, user 12, stream 100; BSTriShape, BSDynamicTriShape and BSSubIndexTriShape with skin shaders           |
| ESP publisher                                                     | Explicit ARMA NAM0/NAM1 assignment through xEdit; copies complete native TXST and ARMA records                      |

An unknown TXST source does not disable NIF materials or race/NPC parameter
adjustments. It does prevent record-based material selection and record-based
user surface identity. The editor reports this boundary. Neither head-part names
nor an actor's list of referenced TXST records are substitutes for a source receipt.

Head preparation receipts survive ordinary property clones when the copied
feature, texture set and normal texture agree. They are replaced on the next
head preparation or successful property texture-set application. Material,
texture-set or normal-texture replacement invalidates a head receipt. Generated
face diffuse textures do not identify TXST and are not used for that check.
Conversion receipts retain their texture set; changing its paths invalidates the
receipt. A path snapshot only detects mutation; it cannot discover a source.
Both receipt caches are bounded at 4096 entries and retain referenced objects to
prevent address reuse. Cache-only references are pruned during periodic prepass
maintenance. Eviction falls back to the remaining known sources.

The extra source hooks are disabled on VR. FaceGen/appearance pipelines that
bypass the verified engine entries remain unconfirmed; this is not a claim of
full third-party or pre-generated FaceGen coverage.

## Creator workflow

Run from the repository using Python 3.10+ with Tk:

```powershell
python tools/skin_materials.py gui
```

Open a NIF, explicitly select a skin geometry block, edit its parameters and two
optional texture resources, then **Publish NIF copy**. Output must be a new file;
the source and existing output files are protected. The publisher re-reads the
output and verifies the material. Existing blocks retain their indices; unedited
block bodies and the root footer are preserved verbatim.

Re-exporting an authored surface preserves its UUID. For weight variants such
as `_0` / `_1`, explicitly reuse the same UUID in both selected surfaces. For an
independent asset, select **Independent asset** to generate a new one. There is
no automatic pairing by filename or shape name. Repeated publication appends a
new extra-data block; old unreferenced blocks are left intact to avoid renumbering
references in opaque blocks.

Material drafts can be saved, reopened and copied into other explicitly selected
surfaces. They are authoring inputs, not runtime presets. The game editor exports
drafts to `Data/Shaders/Skin/Authoring/<name>.skinmaterial.json`.
Include any newly authored DDS resources in the mod at the exact Data-relative
paths assigned by the material; the NIF publisher does not copy texture files.

For a record assignment, place `tools/Advanced Skin - Publish ARMA Material.pas`
in xEdit's Edit Scripts directory and apply it to exactly one ARMA record. Choose
male or female, a material draft, and a new plugin name/staging location. The
script takes the winning original TXST, clones its complete native contents,
creates an ARMA override and changes only the selected NAM0/NAM1 reference. It
verifies the new reference before exporting a new directory containing:

```text
MySkin.esp
Shaders/Skin/Materials/MySkin.esp.skin.json
```

The script does not create empty native texture slots as an inheritance device.
Its scope is the selected ARMA field, which may be shared by several actors or
armors. It does not create an NPC-specific armor distribution. Review that scope
in xEdit before installing the staging directory as a mod. An `.incomplete`
directory after an error is not a finished package. The xEdit script has been
statically reviewed against xEdit's scripting adapters; it has not been executed
in xEdit in this change.

The **Plugin package** window can add exact TXST materials or sparse race/NPC
adjustments to a new or existing package. Only checked adjustment fields are
written. It validates the document, not the existence of records in a game load
order. Record types, loaded plugins and ESL local-ID ranges are checked again by
the runtime. A package's owner must be an installed plugin; generating a JSON
package alone does not create that plugin. Re-export packages after compacting
FormIDs.

## Game editor

Open **CS Editor → Skin Editor**, or **Advanced Skin → Surface Material Editor**.
Both use the same selection, draft and preview state.

Select the player, console reference or crosshair target, then select an actual
skin geometry. The display shows the base material source, identity, applied
adjustments and missing texture channels. Choose one scope:

-   This material surface: NIF UUID, or a confirmed TXST if there is no valid UUID.
-   This NPC: all actors using that base; parameters only.
-   This NPC's surface: the conjunction of those two identities.

Parameter checkboxes store explicit edits, including values equal to defaults.
Complete material editing starts from the base before race/NPC/user parameter
adjustments. Preview starts enabled and can be toggled for comparison. It
substitutes the draft for the selected scope's saved entry at that scope's normal
priority; more specific edits can still win. Unchecking an existing override
therefore previews inheritance correctly, and saving has the same result.
Cancel restores the saved entry. Saving changes only the local user file;
it does not write an author's NIF or plugin package. A target without the required
stable identity is preview-only until authored with the offline tool.

**Restore Parameter Values** stages removal of the selected scope's parameter
patch. **Use Author Material** stages removal of its complete replacement; other
applicable user scopes and race/NPC adjustments still apply. Both can be previewed
and cancelled before saving. Save or cancel a dirty draft before changing the
selected surface/scope. Identity changes disable that draft's preview and saving
until it is reloaded.

**Import Material Draft** reads the named file in `Data/Shaders/Skin/Authoring`
into complete-material mode. **Export Material Draft** writes the edited base
with this scope's explicitly checked parameter changes; it does not bake the
resolved race/NPC adjustments into the material. NPC-only edits can export a
draft but cannot import a complete replacement into their runtime scope.

## Runtime contract

Only a `NiStringExtraData` named `CSSkinMaterial` on the actual `BSGeometry` is
read. Parent nodes and shader properties are not searched. Its UTF-8 string is:

```json
{
    "schemaVersion": 1,
    "surfaceId": "84b6db16-90be-4d38-b0c7-dcc68430cc3c",
    "material": {
        "name": "Example",
        "parameters": {
            "SkinMainRoughness": 0.7,
            "SkinSecondRoughness": 0.35,
            "SkinSpecularTexMultiplier": 1.0,
            "SecondarySpecularStrength": 0.15,
            "F0": 0.0278,
            "BaseColorMultiplier": 1.0,
            "PhysicalMainRoughnessMultiplier": 1.3,
            "PhysicalSecondRoughnessMultiplier": 0.75,
            "PhysicalSpecularStrength": 1.0,
            "ExtraEdgeRoughness": 0.25,
            "EnableSkinDetail": true,
            "SkinDetailStrength": 0.25,
            "SkinDetailTiling": 10.0,
            "BodyTilingMultiplier": 2.0,
            "Translucency": 0.1,
            "sssWidth": 0.2,
            "UseSSS": true,
            "FuzzStrength": 1.0,
            "FuzzRoughness": 0.35,
            "FuzzF0": 0.045
        },
        "textures": { "rfaos": null, "wetness": null }
    }
}
```

The canonical field names, frozen defaults and ranges are in
`src/Features/Skin/SkinParameters.def`,
shared by C++ validation, the game editor and the Python publisher.

Complete material selection is:

```text
default → valid NIF → confirmed TXST package → user surface → user NPC × surface
```

Then apply parameters:

```text
race → NPC → user surface → user NPC → user NPC × surface
```

Each plugin has one package named exactly `<ownerPlugin>.skin.json`. Packages are
ordered by the engine's `TESDataHandler::files` traversal, with its active file
last, not by filenames or FormID indices. Full and light plugins retain their
actual interleaving. For each target, a later owner's whole entry replaces an
earlier owner's entry. Duplicate targets within one package are excluded from
that package; array order cannot break ties. Invalid entries are diagnosed and
excluded, allowing a valid lower source to apply. A syntactically invalid file
retains its previous snapshot; deleting a file removes it. Manual reload also
re-reads NIF metadata and retries texture loads.

Record keys contain the original owning plugin and local hex ID. No EditorID or
runtime load-order prefix is stored. User edits are kept in:

```text
Data/Shaders/Skin/User/SkinMaterials.user.json
```

The user document is `{schemaVersion: 1, edits: [...]}`. Each edit has `target`,
`parameters`, and an optional complete `material`. A target contains `surfaceId`
or `txst`, optionally combined with `npc`; NPC-only targets cannot replace a
material. FormKey objects contain `plugin` and `localFormId`. Unknown fields,
duplicate JSON keys, unknown versions, non-finite/out-of-range parameters and
invalid target forms are rejected.

Texture paths are explicit `textures/...dds` resources relative to Data, with
slashes/case normalized. Null disables a channel; complete materials have no
inherit state. A valid material with a missing texture retains its parameters
and disables only that channel. The loader probes the engine resource stream,
checks DDS metadata, rejects non-2D/array/cube resources and known fallback SRVs,
then binds the loaded resource without modifying the native texture set.

Limits: 64 KiB per NIF payload, 4 MiB per JSON file, 4096 entries per array, 4096
package files, 64 MiB aggregate input JSON, 16 JSON nesting levels and 256 MiB per
DDS resource. Runtime geometry caches hold strong references, are capped at 4096
geometries and expire after 600 unobserved frames. Texture-pair caches are capped
at 1024. File scans occur in prepass, not per draw; material parsing and resource
loads occur only on cold/invalidation paths. The shared b7 buffer is updated for
every skin draw. The wetness calculation and t71/t72/t74 bindings are preserved.

## Migration

**Import legacy parameters** extracts candidate payloads for review. It does not
execute or convert old selectors, parent chains or texture discovery. Each field
must be explicitly checked before import. Full race profiles are labelled as all
20 fields; equality with a default is not used to infer author intent. Extra
texture resources must be assigned explicitly in the new material.

## Verification and remaining acceptance work

Performed without a plugin build or shader compilation:

-   Static C++/header/API review and formatting.
-   13 Python data tests for typed/sparse parameters, complete materials, path
    rejection, duplicate keys/targets, legacy import, NIF opaque-block preservation,
    UUID reuse/regeneration, malformed streams and staging overwrite protection.
-   Byte-identical no-op round trip of the repository's existing WaterMesh.nif.
-   Translation extraction/orphan/order checks and `git diff --check`.

No in-game, xEdit, NifSkope or creator-GUI execution has been performed. Before
release, verify SE/AE rendering, real body/head NIF publication, BSA and loose DDS
loading, texture failure behavior, shared TXST scope, clone/re-equip/race changes,
preview cancellation and sequential draws with distinct profiles. Source hooks
have been statically checked in Ghidra for SE 1.5.97 and AE 1.6.1170; runtime
acceptance remains necessary before claiming those paths are game-tested.

| Entry                            | Address Library SE / AE | Verified addresses SE / AE |
| -------------------------------- | ----------------------- | -------------------------- |
| Native TXST conversion           | 20905 / 21361           | `1402d19a0` / `1403270c0`  |
| Property texture-set application | 99865 / 106510          | `1412c5ab0` / `1414ad7d0`  |
| Prepare head part                | 26259 / 26838           | `1403d2a60` / `14042bd90`  |
| Apply selected head normal       | 26260 / 26839           | `1403d2fc0` / `14042c410`  |

The normal update passes the selected `BGSTextureSet` and actual property together.
A thread-local head-preparation context carries that pair to the property's
final `PrecacheTextures` call (vtable slot `0x3A`), after tint/detail updates. A
preparation without a selected record clears any prior head assignment. Ordinary
cloning uses the lighting property's `CreateClone` slot `0x17`; texture path
mutation uses `BSShaderTextureSet::SetTexturePath` slot `0x27`. The property
texture-set function forwards to the material's `OnLoadTextureSet`; the adapter
checks the resulting material set rather than assuming the requested set won.

The ordering adapter was checked against SE `CompileFiles` at `14016e660` and
its `140174710` loader loop, which traverses the list at `TESDataHandler + 0xD60`
before loading the active file. Mixed ESP/ESL package conflicts still need an
in-game acceptance check.

Format/API references:
[nifxml](https://github.com/niftools/nifxml/blob/develop/nif.xml),
[xEdit TES5 records](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsTES5.pas),
[xEdit JSON example](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Build/Edit%20Scripts/JSON%20-%20Demo.pas),
[xEdit file scripting adapter](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/xEdit/JvI/xejviScriptAdapterFile.pas).
