# Advanced Skin layered materials

Place JSON files in `Data\Shaders\Skin\Overrides`. Files in `User` are loaded
after regular files. The runtime polls the directory and applies edits without a
game restart.

The resolved material of every skin geometry has a fixed composition order:

```text
Base Material -> Surface winner chain -> Appearance winner chain -> Local winner chain
```

Each domain selects one winner. A winner inherits through one parent chain; it
does not depend on several unrelated rules being applied in an accidental order.

## Complete example

This applies one texture set to every geometry classified as an UBE body, shares
one parameter set across every part of Lydia, and finally replaces only Lydia's
UBE-body RFAOS texture:

```json
{
    "materials": {
        "advanced-skin:default": {
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
    "appearanceInstances": {
        "appearance:nord": {
            "parameters": {
                "F0": 0.03
            }
        },
        "appearance:lydia": {
            "parent": "appearance:nord",
            "parameters": {
                "SkinMainRoughness": 0.62,
                "SkinDetailStrength": 0.4
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
            "match": { "tag": "body:ube" },
            "use": "skin:ube_body"
        },
        {
            "id": "appearance:nord",
            "domain": "appearance",
            "match": { "race": "NordRace" },
            "use": "appearance:nord"
        },
        {
            "id": "appearance:lydia",
            "domain": "appearance",
            "match": { "baseid": "000A2C8E" },
            "use": "appearance:lydia"
        },
        {
            "id": "local:lydia_ube_body",
            "domain": "local",
            "match": { "baseid": "000A2C8E", "tag": "body:ube" },
            "use": "lydia:ube_body"
        }
    ]
}
```

Lydia's head and body receive the same Appearance parameters. Their textures
remain independently inherited from their Surface chains because omitted texture
channels mean inherit.

## Instance rules

-   `surfaceInstances` may inherit a base material or another Surface instance.
-   `appearanceInstances` may inherit only another Appearance instance.
-   `localInstances` may inherit only another Local instance.
-   A missing field inherits. A texture string replaces the channel. `null` or an
    empty texture string explicitly disables the channel.
-   Missing parents, cross-domain parents, cycles, excessive depth, and bindings to
    invalid instances are rejected and logged.
-   A Local binding must contain at least one actor selector and one surface
    selector.

## Selectors and classifiers

All populated fields in a selector must match. Matching is case-insensitive;
paths use forward slashes. Supported fields are:

-   `nif`, `nifGlob`, `shape`, `shapeGlob`;
-   `diffuse`, `diffusePrefix`, `diffuseGlob`, `normal`, `normalPrefix`, `normalGlob`;
-   `baseid` (NPC base), `referenceid` (placed actor), `race`, `sex`;
-   `armor`, `armorAddon`, `slot`, `shaderFeature`, `tag`.

`armor` and `armorAddon` accept an editor ID or FormID. `slot` uses names such as
`head`, `body`, `hands`, and `feet`. `shaderFeature` currently exposes
`facegen` and `facegenrgbtint` by name. Classifiers support recursive `all`, `any`,
and `not`, with the same selector leaves. Globs support `*` and `?`; `**` behaves
as consecutive `*` and therefore also crosses path separators.

Runtime ARMO/ARMA/slot identity is resolved from the actor's active Biped object
whose `partClone` owns the drawn geometry. FaceGen geometry commonly has no ARMA;
classify it by shape, source NIF, textures, or shader feature instead.

## Priority

Bindings are ordered from low to high precedence by:

1. regular file, then `User` file;
2. `priority` (file default, optionally overridden per binding);
3. selector specificity;
4. normalized source filename;
5. order within that file.

The last matching binding wins independently in Surface, Appearance, and Local.
The three domains never compete for one global winner.

## Compatibility

The previous `nif`, `baseid`, `profiles`, texture-only `materials`, and `rules`
format remains readable. Legacy rules are assigned to Surface, Appearance, or
Local according to their selector kinds and applied as low-priority compatibility
layers. Automatic `_rfaos.dds` / `_wet.dds` discovery is disabled by default. It
can be re-enabled with the Advanced Skin compatibility checkbox, but explicit
Surface/Local instance textures are recommended. When enabled, discovery probes
Skyrim's resource system before loading, so a missing path returned as an engine
placeholder is not marked present. It never writes paths into the engine's shared
material or `BSTextureSet`.

The shader receives explicit `HasRfaos` and `HasWetness` flags. Black fallback
textures are always bound, so texture dimensions are not used as presence flags.
