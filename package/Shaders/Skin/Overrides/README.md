# Advanced Skin layered materials — Authoring Guide

中文制作指南：[README.zh-CN.md](./README.zh-CN.md)

> For modders and artists. This guide documents the current `src/Features/Skin.h` /
> `src/Features/Skin.cpp` implementation; every key name, matching rule, and merge order
> is checked against the code. If something disagrees with in-game behavior, trust the
> Community Shaders log (`[Advanced Skin] ...`).

Advanced Skin's override system is driven by JSON. It splits "what parameters and extra
textures each skin geometry finally uses" into **Base → Surface → Appearance → Local**
layers, merged in order; each layer only needs to contain the parts you want to change.

---

## 1. What the system can and cannot do

### ✅ What it can do directly

-   **Override parameters**: every numeric `SkinProfile` field (roughness, F0, SSS, fuzz,
    skin detail — see §4) matched to precise targets by NIF / shape / texture / race / NPC /
    slot.
-   **Assign extra textures**: the RFAOS (extra roughness/fuzz mask/AO/specular) and wetness
    maps, with paths given directly in JSON.
-   **Layered inheritance**: put shared settings in a base material / Surface, per-character
    personality in Appearance, and a single-NPC special case in Local; later writes override
    earlier ones, children inherit from parents.
-   **Conditional matching**: selectors and classifiers (`all`/`any`/`not` recursive logic),
    with glob wildcards (`*`, `?`; `**` behaves as consecutive `*` and crosses path separators).
-   **Hot reload**: edits apply while the game runs (§2).

### ❌ What it cannot do directly

-   **It does NOT "assign" diffuse / normal textures to geometry.** `diffuse` and `normal`
    remain governed by the ESP / NIF / `BSTextureSet`. In JSON, diffuse / normal only act as
    **matching conditions** (`diffuse` / `diffusePrefix` / `diffuseGlob`, etc.) to _hit_
    geometry — they do not replace it with other textures.
-   **It does NOT write back to the engine's shared material or `BSTextureSet`.** JSON only
    binds the two extra RFAOS / wetness slots and leaves the original material alone.
-   **It does NOT allow per-NPC "wetness constant" overrides.** The global wetness settings
    (`ExtraSkinWetness`, `WetParams`, sweat thresholds, etc.) are global settings, not part of
    `SkinProfile`, and cannot be adjusted through override JSON.

> In one sentence: **texture assignment comes from ESP/NIF/TextureSet; JSON is only
> responsible for "parameters + RFAOS/wetness" and "who these settings hit".**

---

## 2. File paths, User overrides, hot reload, in-game UI

### File locations

| Type                                           | Path                                      |
| ---------------------------------------------- | ----------------------------------------- |
| Mod overrides                                  | `Data\Shaders\Skin\Overrides\*.json`      |
| User overrides (loaded later, higher priority) | `Data\Shaders\Skin\Overrides\User\*.json` |

-   Only regular files with a `.json` extension are read (extension is case-insensitive).
-   The directory is scanned and sorted by lower-cased filename; **mod files load first, then
    `User` files**.
-   User overrides written through the in-game UI persist to `User\SkinOverrides.user.json`,
    which only stores the legacy `nif` / `baseid` sections.

### Hot reload

At runtime the directory is rescanned roughly every 60 frames; as soon as any file's
modification-time set changes, the whole override set is rebuilt and per-geometry caches are
invalidated (`revision` increments). So **save the JSON and it takes effect in-game — no
restart needed**.

### In-game "quick mode"

The Advanced Skin settings panel → **NIF Overrides** section provides:

-   **Pick Crosshair** / **Use Console Selection** (target the actor under the crosshair or
    the console selection);
-   automatic derivation of all NIF keys and the BaseID key for the target, plus whether the
    target has a skin material;
-   **New NIF Override** / **New BaseID Override** open the override editor, with the merge
    chain shown as `Chain: Default -> Race(...) -> ... override`.

This UI is handy for quickly locating key names; it ultimately writes into `User` overrides.
For release, still prefer tidy, hand-written JSON files.

---

## 3. Simple format: `nif` and `baseid`

### `nif` (keyed by NIF / shape)

```json
{
    "nif": {
        "actors/character/actresscharacternord/characternord.nif": {
            "SkinMainRoughness": 0.62,
            "EnableSkinDetail": true
        }
    }
}
```

Keys are normalized: lower-cased, `\` converted to `/`, and a leading `meshes/`,
`data/meshes/`, or the part before a `/meshes/` segment stripped. So `meshes\actors\...nif`
and `actors/...nif` are equivalent. The values are a set of `SkinProfile` fields (only write
what you change; `null` is skipped on merge).

### `baseid` (keyed by NPC base FormID)

```json
{
    "baseid": {
        "000A2C8E": {
            "Translucency": 0.2,
            "sssWidth": 0.28
        }
    }
}
```

Keys are normalized: an optional `0x`/`0X` prefix is stripped and hex letters are
upper-cased. **This stores the NPC's (`TESNPC`) raw FormID** (8 hex digits; when the plugin
lands above `0x00000xxx`, the high 2 index digits are included).

> ⚠️ **Load-order risk**: BaseID is the **raw FormID**, not a stable `PluginName:FormID`.
> If the NPC's plugin load order changes (and for ESL `FE`-flagged FormIDs), this key breaks.
> Record the target NPC's plugin and FormID before release.

---

## 4. `SkinProfile` field reference (complete)

Everywhere a "parameter" can be written (base-material `parameters`, instance `parameters`,
`profileOverrides`, legacy partials), the following fields are used. All are optional;
omitted fields inherit from the layer above. Defaults come from the `SkinProfile` default
constructor in `Skin.h`.

| Field (exact case)                  | Default | Meaning                                                                                 |
| ----------------------------------- | ------- | --------------------------------------------------------------------------------------- |
| `SkinMainRoughness`                 | 0.7     | First-lobe (stratum corneum) micro-roughness                                            |
| `SkinSecondRoughness`               | 0.35    | Second-lobe (epidermis) reflection smoothness; usually 30–50% lower than primary        |
| `SkinSpecularTexMultiplier`         | 1.0     | Multiplier for the vanilla specular map (applied to the first lobe's roughness)         |
| `SecondarySpecularStrength`         | 0.15    | Secondary specular highlight intensity                                                  |
| `F0`                                | 0.0278  | Fresnel reflectance (F0)                                                                |
| `BaseColorMultiplier`               | 1.0     | Base color texture multiplier                                                           |
| `PhysicalMainRoughnessMultiplier`   | 1.3     | Physical main roughness texture multiplier                                              |
| `PhysicalSecondRoughnessMultiplier` | 0.75    | Physical second roughness texture multiplier                                            |
| `PhysicalSpecularStrength`          | 1.0     | Physical specular strength (note: a _different_ field from `SkinSpecularTexMultiplier`) |
| `ExtraEdgeRoughness`                | 0.25    | Extra roughness at edges (approximates facial peach fuzz)                               |
| `EnableSkinDetail`                  | true    | Enable the skin detail texture                                                          |
| `SkinDetailStrength`                | 0.25    | Skin detail strength                                                                    |
| `SkinDetailTiling`                  | 10.0    | Skin detail tiling                                                                      |
| `BodyTilingMultiplier`              | 2.0     | Body tiling multiplier (to match the face)                                              |
| `Translucency`                      | 0.1     | SSS transmittance translucency                                                          |
| `sssWidth`                          | 0.2     | SSS transmittance width                                                                 |
| `UseSSS`                            | true    | Enable SSS transmittance                                                                |
| `FuzzStrength`                      | 1.0     | Fuzz strength                                                                           |
| `FuzzRoughness`                     | 0.35    | Fuzz roughness                                                                          |
| `FuzzF0`                            | 0.045   | Fuzz F0                                                                                 |

> `SkinProfile` has **no** wetness field; wetness strength, thresholds, and noise parameters
> are global settings and cannot be overridden per NPC.

---

## 5. Recommended layered model: Base → Surface → Appearance → Local

### 5.1 Top-level sections (JSON top-level keys)

| Top-level key         | Meaning                                                                                    |
| --------------------- | ------------------------------------------------------------------------------------------ |
| `nif` / `baseid`      | Legacy compatibility layer (§3), still readable                                            |
| `profiles`            | Named parameter pool (referenced by legacy `rules`' `profile`)                             |
| `materials`           | Overloaded: a texture-only payload (legacy), or a "base material" when it has `parameters` |
| `surfaceInstances`    | Surface-domain instances                                                                   |
| `appearanceInstances` | Appearance-domain instances                                                                |
| `localInstances`      | Local-domain instances                                                                     |
| `classifiers`         | tag → recursive classifier expression                                                      |
| `rules`               | Legacy unified rules (compatibility, low priority)                                         |
| `bindings`            | Modern bindings (recommended)                                                              |
| `priority`            | Optional file-level default priority for that file's `rules` / `bindings`                  |

> Note: there is **no** top-level `baseMaterials` key. Base materials are `materials` entries
> with a `parameters` object; the entry named `advanced-skin:default` is treated as the global
> root override, merged into `DefaultProfile` before every other layer.

### 5.2 The three domains

| Domain     | What it cares about                                                       | What it does not handle / ignores                             |
| ---------- | ------------------------------------------------------------------------- | ------------------------------------------------------------- |
| Surface    | Mesh surface: NIF, shape, diffuse/normal, armor, slot, shaderFeature, tag | Actor identity                                                |
| Appearance | Actor identity: baseid, referenceid, race, sex                            | Mesh surface                                                  |
| Local      | Actor + surface (both selector kinds required)                            | A single side (missing actor or surface selector is rejected) |

The `domain` string (in `bindings`) is case-insensitive: `surface` / `appearance` / `local`.

### 5.3 Material instances (inheritance)

```json
{
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "parameters": { "SkinDetailTiling": 12.0 },
            "textures": {
                "rfaos": "textures/actors/character/female/femalebody_1_rfaos.dds",
                "wetness": "textures/actors/character/female/femalebody_1_wet.dds"
            }
        }
    }
}
```

-   `parent` is single-parent; a Surface instance may inherit a base material or another
    Surface instance; Appearance may only inherit Appearance; Local only Local.
-   Merge order is **parent → child** (root applied first, leaf last; later overrides earlier
    for the same field).
-   Validation: a missing parent, cross-domain parent, cycle, or depth beyond 32 invalidates
    the instance (logged); bindings to invalid instances are dropped.
-   `advanced-skin:default` (or any base-material id) is a valid root in the Surface domain;
    reaching it applies its `parameters` and stops the walk.

### 5.4 `bindings` (recommended selection mechanism)

```json
{
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

-   Required: `match` (object), `domain` (string), `use` (string pointing at a same-domain
    instance).
-   Each domain picks one "winner" independently: walk the sorted bindings; the **last matching
    binding** wins. The three domains never compete for a single global winner.
-   A Local binding **must contain at least one actor selector field and one surface selector
    field**, otherwise it is ignored.

### 5.5 Selector fields (exact keys)

Every **populated** field must match (AND). Matching is case-insensitive; paths use forward
slashes. Unknown or non-string fields invalidate the **entire selector** (the rule/binding is
dropped).

| Field           | Normalization                        | Match rule                                    |
| --------------- | ------------------------------------ | --------------------------------------------- |
| `nif`           | strip meshes prefix, lower-case, `/` | equals `context.nif` or `context.legacyNif`   |
| `nifGlob`       | same                                 | glob vs `nif` or `legacyNif` (`*`, `?`)       |
| `shape`         | lower-case                           | equals geometry name                          |
| `shapeGlob`     | lower-case                           | glob                                          |
| `baseid`        | strip 0x, upper-case                 | equals NPC raw FormID                         |
| `referenceid`   | same                                 | equals placed-reference FormID                |
| `race`          | lower-case                           | equals race EditorID                          |
| `normal`        | texture-key normalize                | equals normal texture path                    |
| `normalPrefix`  | same                                 | normal path `startswith`                      |
| `normalGlob`    | same                                 | glob                                          |
| `diffuse`       | same                                 | equals diffuse texture path                   |
| `diffusePrefix` | same                                 | diffuse path `startswith`                     |
| `diffuseGlob`   | same                                 | glob                                          |
| `armor`         | lower-case                           | equals armor EditorID **or** hex FormID       |
| `armorAddon`    | lower-case                           | equals ArmorAddon EditorID **or** FormID      |
| `slot`          | lower-case                           | equals slot name (see below)                  |
| `sex`           | lower-case                           | `female` / `male`                             |
| `shaderFeature` | lower-case                           | `facegen` / `facegenrgbtint` / numeric string |
| `tag`           | lower-case                           | `context.tags` contains the tag               |

**Slot names** (BIPED slot → lower-case string, complete list):
`head, hair, body, hands, forearms, amulet, ring, feet, calves, shield, tail, longhair,
circlet, ears, modmouth, modneck, modchestprimary, modback, modmisc1, modpelvisprimary,
decapitatehead, decapitate, modpelvissecondary, modlegright, modlegleft, modfacejewelry,
modchestsecondary, modshoulder, modarmleft, modarmright, modmisc2, fx01, handtohandmelee,
onehandsword, onehanddagger, onehandaxe, onehandmace, twohandmelee, bow, staff, crossbow, quiver`

**shaderFeature**: only face geometry gets named values `facegen` / `facegenrgbtint`;
other materials stringify to a numeric string (e.g. `6`). FaceGen geometry usually has no
ArmorAddon, so classify it by shape / source NIF / textures / shaderFeature instead.
ARMO/ARMA/slot identity comes from the active Biped part that owns the drawn geometry.

### 5.6 Classifiers (tags)

```json
{
    "classifiers": {
        "body:ube": {
            "any": [
                { "shape": "UBEBody" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "normalPrefix": "textures/ube/" }
            ]
        }
    }
}
```

-   A classifier is a recursive expression: `all` (AND, array), `any` (OR, array), `not`
    (negation), or a selector leaf (the same 19 selector fields).
-   A leaf with no populated fields evaluates to false.
-   Classifiers may reference other tags; evaluation iterates to a fixed point. Derived tags
    enter `context.tags` and can then be selected with `"tag"`.

### 5.7 Priority and ordering

`bindings` / `rules` are sorted by the following tuple **ascending** (later wins):

1. regular files first, `User` files after;
2. `priority` (file default, optionally overridden per item);
3. selector specificity (how many fields are populated);
4. normalized source filename;
5. order within the file (`sourceOrder`).

Each domain takes the **last matching binding** as the winner; the three domains do not
compete. Legacy `nif`/`baseid` use "same key: User overwrites mod value" (user wins), with no
priority/specificity involvement.

### 5.8 Fixed runtime merge order

```
DefaultProfile (+ advanced-skin:default base material)   ← root
  → legacy NIF partial (Surface)
  → legacy rules → Surface domain
  → Surface winner instance chain
  → legacy race profile (only when a race→profile is bound in the UI)
  → legacy BaseID partial (Appearance)
  → legacy rules → Appearance domain
  → Appearance winner instance chain
  → legacy rules → Local domain
  → Local winner instance chain
```

Same-name parameters: later writes override earlier writes. Texture channels inherit by
default; only `Disabled` / `Path` mutate the value.

---

## 6. RFAOS and wetness texture authoring

Advanced Skin binds two extra textures (slots 71 / 74), controlled by the explicit
`HasRfaos` / `HasWetness` flags; a black texture is always bound as fallback (**texture
dimensions are never used to infer presence**).

### Texture channel three states

| JSON value       | Effect                                          |
| ---------------- | ----------------------------------------------- |
| key omitted      | **Inherit** (keep the previous / default value) |
| `null` or `""`   | **Disabled** (clear the path, no RFAOS/wetness) |
| non-empty string | **Path** (bind that path)                       |

Paths are normalized: lower-cased, `\`→`/`, a leading `data/` or everything before a
`/textures/` segment stripped, keeping the `textures/...` tail. Non-string values are
ignored with a warning.

### RFAOS (extra roughness / fuzz mask / AO / specular)

`rfaos` packs four channels into one image; the current shader's exact channels are:

| Channel | Meaning           |
| ------- | ----------------- |
| **R**   | Roughness         |
| **G**   | Fuzz mask         |
| **B**   | Ambient Occlusion |
| **A**   | Specular          |

Place roughness, fuzz mask, AO, and specular into the RGBA channels respectively.

### wetness (mask / normal — two modes)

`wetness` is more than a mask. It chooses between two modes per pixel (decided in
`package/Shaders/Lighting.hlsl`):

-   **Mode A (mask/height; the shader derives the normal)**: when `G` and `B` are both 0, **or**
    `RGB` is grayscale and `A ≈ 1` (code: `A >= 0.99`), the image is treated as a **single
    mask/height map**:
    -   `R` = wet mask / height field, fed to `CalculateNormalFromHeight` so the shader derives
        the normal;
    -   `G`/`B` are ignored (kept 0 or equal to R).
-   **Mode B (wet normal + mask)**: otherwise (`G`/`B` non-zero, or RGB not grayscale / A not
    full), it is treated as a normal map:
    -   `RGB` = wet (water-flow) normal;
    -   `A` = wet mask.

The exact decision (excerpt from `Lighting.hlsl`):

```text
if (G == 0 && B == 0) or (R == G == B and A >= 0.99):
    R acts as mask/height; shader derives the normal with CalculateNormalFromHeight
else:
    RGB = wet normal, A = wet mask
```

Authoring tip: for a uniform wetness / water film, export a grayscale (R) mask with A ≈ 1 —
simple and cheap; for directional water flow or ripples that need local normal detail, use
the "normal RGB + A mask" mode.

### Load failure

If an explicit path cannot be resolved (loose file or BSA), the log prints
`[Advanced Skin] Failed to load configured RFAOS texture: ...` and silently falls back to the
black texture ("looks like no RFAOS" rather than a crash). Use this log to chase typos.

> Compatibility note: automatic `_rfaos.dds` / `_wet.dds` filename discovery
> (`EnableLegacyExtraTextureDiscovery`) is **off** by default. When enabled, it looks for
> `_rfaos.dds` / `_wet.dds` next to `_s.dds` / `_n.dds` / `_msn.dds`, and does **not** write
> back to the shared material / `BSTextureSet`. Explicit Surface/Local instance textures are
> recommended for shipped work.

---

## 7. Complete valid JSON examples

> All of the following are **strict JSON with no comments** and can be used as-is.

### 7.1 Generic UBE body (custom Base Material + Surface material)

```json
{
    "priority": 10,
    "materials": {
        "my_ube:base": {
            "parameters": {
                "SkinMainRoughness": 0.62,
                "SkinSecondRoughness": 0.3,
                "F0": 0.028,
                "BaseColorMultiplier": 1.05,
                "UseSSS": true,
                "Translucency": 0.12,
                "sssWidth": 0.2
            }
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "my_ube:base",
            "parameters": {
                "SkinDetailTiling": 12.0,
                "BodyTilingMultiplier": 2.0
            },
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "classifiers": {
        "body:ube": {
            "any": [
                { "shape": "UBEBody" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "normalPrefix": "textures/ube/" }
            ]
        }
    },
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

> Here the UBE-specific parameters live in a custom base material `my_ube:base` (any name
> works), inherited by `skin:ube_body` via `parent`, so it does **not** affect non-UBE skin.
> Do not put them in `advanced-skin:default` — that acts as a global root override and would
> leak onto every skin.

### 7.1.1 How the system auto-detects UBE / how tags take effect

**A tag is not metadata written into the NIF.** A tag is derived at runtime by a classifier,
evaluated against each geometry's `GeometryContext` (NIF, shape, diffuse/normal paths,
armor, slot, race, sex, etc.), then inserted into `context.tags`. "Detecting UBE" means:
write a classifier that expresses "what counts as UBE" as a decidable condition over
`GeometryContext`; then use a Surface binding that matches only the `tag` to connect matches
to the UBE instance.

**Contract-style publication (provider JSON)**: a UBE author only needs to ship one provider
JSON that defines a recognition contract stable for third parties. Preferred by reliability:

1. **`normalPrefix` / `diffusePrefix` (first choice, most stable)**: enforce a canonical
   texture directory prefix, e.g. all UBE body normals must live under `textures/ube/`.
2. **`nifGlob` (second choice)**: enforce a NIF path glob, e.g. `actors/character/**/ube*.nif`.
3. **`shapeGlob` (third choice)**: enforce a geometry-name glob, e.g. `UBEBody`.

Note: do **not** enumerate NIFs one by one (`nifGlob` is more general than a pile of `nif`
entries), and do **not** rely only on `slot: "body"` — the `body` slot also matches non-UBE
bodies and cannot distinguish UBE from other body types.

```json
{
    "classifiers": {
        "body:ube": {
            "any": [
                { "normalPrefix": "textures/ube/" },
                { "diffusePrefix": "textures/ube/" },
                { "nifGlob": "actors/character/**/ube*.nif" },
                { "shapeGlob": "UBEBody" }
            ]
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "bindings": [
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        }
    ]
}
```

**How third-party extensions plug in**:

-   If a third-party UBE skin / armor extension **keeps the canonical path convention**
    (textures/NIF/shape follow the same convention), the provider's classifier matches
    automatically; they need to do nothing.
-   If they **change paths**, they should **add a new binding** pointing `use` at the existing
    `skin:ube_body` instance (or a self-built instance that inherits it), and they must **not**
    redefine a classifier with the same `body:ube` name in another file.

> ⚠️ Important: a classifier with the same tag **replaces** the earlier definition in the
> later-loaded file (the parser assigns `parsedClassifiers[tag] = expression` directly — it
> does not merge). So redefining `body:ube` across files makes "only the last one wins" and
> silently overwrites the earlier recognition logic, causing hard-to-predict behavior. The
> right approach: the provider defines `body:ube` exactly once; everyone else hooks it with a
> **new binding**.

**What if the UBE has no stable convention**: if a UBE has no stable path / shape / texture
naming convention at all, the system **cannot "guess" it is UBE from topology** — matching is
based purely on observable facts in `GeometryContext`, and without a convention there is no
decidable condition. In that case you must **establish a convention first** (e.g. a uniform
`textures/ube/` prefix); only later could NIF ExtraData or an explicit registry add
recognition, but the current version does not support those.

**How to verify**:

1. In-game, use **Pick Crosshair** on the target and inspect the actual derived **NIF keys**
   (and whether it has a skin material) to confirm your `nifGlob`/prefix matches reality.
2. Temporarily set an obvious parameter (e.g. `SkinMainRoughness` to 0 or 1) to observe
   whether it hits, then revert.
3. The current UI **does not show the list of classifier-derived tags** — you can only
   indirectly verify a tag hit via whether the parameter/texture takes effect.

### 7.2 Lydia (BaseID `000A2C8E`) — Appearance parameters + Local body texture

```json
{
    "priority": 10,
    "appearanceInstances": {
        "appearance:lydia": {
            "parameters": {
                "SkinMainRoughness": 0.55,
                "SkinDetailStrength": 0.4,
                "Translucency": 0.2,
                "sssWidth": 0.28,
                "FuzzStrength": 1.2
            }
        }
    },
    "surfaceInstances": {
        "skin:ube_body": {
            "parent": "advanced-skin:default",
            "textures": {
                "rfaos": "textures/skin/ube_body_rfaos.dds",
                "wetness": "textures/skin/ube_body_wet.dds"
            }
        }
    },
    "localInstances": {
        "lydia:ube_body": {
            "textures": {
                "rfaos": "textures/skin/lydia_body_rfaos.dds"
            }
        }
    },
    "classifiers": {
        "body:ube": {
            "any": [{ "shape": "UBEBody" }, { "normalPrefix": "textures/ube/" }]
        }
    },
    "bindings": [
        {
            "id": "appearance:lydia",
            "domain": "appearance",
            "use": "appearance:lydia",
            "match": { "baseid": "000A2C8E" }
        },
        {
            "id": "surface:ube_body",
            "domain": "surface",
            "use": "skin:ube_body",
            "match": { "tag": "body:ube" }
        },
        {
            "id": "local:lydia_ube_body",
            "domain": "local",
            "use": "lydia:ube_body",
            "match": { "baseid": "000A2C8E", "tag": "body:ube" }
        }
    ]
}
```

Explanation: Lydia's head and body share one Appearance parameter set; `skin:ube_body`
inherits the built-in root `advanced-skin:default` directly (no need to redefine it in this
file — the global `DefaultProfile` already acts as the root), and body textures still inherit
from their Surface chains; `local:lydia_ube_body` only swaps Lydia's UBE-body RFAOS for a
dedicated texture (omitting `wetness` means "inherit", not "disable").

---

## 8. Troubleshooting

| Symptom                                | Possible cause                                                                 | Fix                                                                                     |
| -------------------------------------- | ------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------- |
| Override does nothing                  | Not strict JSON; wrong directory; typo                                         | Validate JSON; confirm `Overrides` or `Overrides\User`; check the `[Advanced Skin]` log |
| A rule/binding is dropped              | Selector has an unknown or non-string field                                    | Use only the documented 19 fields; check spelling                                       |
| Local binding does nothing             | Missing actor or surface selector                                              | Give both `baseid`/`race`/`sex` and `nif`/`shape`/`tag` etc.                            |
| Instance invalid                       | Missing parent, cross-domain parent, cycle, depth > 32                         | Check the `parent` domain and hierarchy                                                 |
| BaseID does not hit                    | Load order changed / FormID mismatch                                           | Reconfirm the NPC raw FormID (§3 risk)                                                  |
| NIF override intermittently fails      | Source NIF overridden; ambiguous origin (same signature from multiple sources) | Prefer shape / diffuse / tag; verify the source NIF                                     |
| RFAOS/wetness "absent"                 | Wrong path / missing texture                                                   | Check the log `Failed to load configured ...`; verify the `textures/` tail              |
| Wetness strength won't change via JSON | Wetness params are global, not in SkinProfile                                  | Change global settings, not override JSON                                               |
| JSON edits don't apply                 | Hot reload runs ~60 frames; cache not invalidated                              | Wait 1–2 s; reopen panel / reload geometry if needed                                    |

## 9. Release checklist

-   [ ] JSON passes strict validation (no comments, no trailing commas).
-   [ ] Files live in `Data\Shaders\Skin\Overrides\` (not `User`, which is user overrides).
-   [ ] Target geometry's diffuse / normal are correctly assigned by ESP/NIF/`BSTextureSet`.
-   [ ] `rfaos` / `wetness` paths start with `textures/`, use forward slashes, no `data/` prefix.
-   [ ] NPC BaseID is the **raw FormID**; record the plugin and load-order dependency.
-   [ ] Every binding's `domain`/`use` points at a same-domain instance; Local bindings include
        an actor + surface selector.
-   [ ] Instance `parent` is same-domain, acyclic, depth ≤ 32.
-   [ ] Selectors use only the documented 19 legal fields.
-   [ ] Log confirms `[Advanced Skin] Loaded ... surface/... appearance/... local material(s) ...`
        with no `[Advanced Skin]` warnings/errors.
-   [ ] In-game, verify the hit and merge chain (`Chain: ...`) with Pick Crosshair.
