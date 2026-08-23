# Advanced Skin material rules

Place one or more `.json` files in `Data\Shaders\Skin\Overrides`. Files in the
`User` subdirectory are applied after files in the parent directory.

The rule format shares selection, ordering, reload, and caching with skin profile
overrides, while keeping numeric profiles and texture materials as separate named
payloads:

```json
{
    "priority": 0,
    "profiles": {
        "example:soft_skin": {
            "SkinMainRoughness": 0.75,
            "SecondarySpecularStrength": 0.12
        }
    },
    "materials": {
        "example:female_body": {
            "rfaos": "textures/actors/character/female/femalebody_rfaos.dds",
            "wetness": "textures/actors/character/female/femalebody_wet.dds"
        }
    },
    "rules": [
        {
            "id": "example:female_body",
            "match": {
                "nif": "actors/character/character assets/femalebody_1.nif",
                "shape": "FemaleBody"
            },
            "profile": "example:soft_skin",
            "profileOverrides": {
                "SkinDetailStrength": 0.3
            },
            "material": "example:female_body",
            "textures": {
                "wetness": null
            }
        }
    ]
}
```

## Selectors

Every populated field in `match` must match. An empty `match` object is a global
rule. Matching is case-insensitive and path separators are normalized.

-   `nif`: source NIF path, relative to `meshes`. Advanced Skin records this while
    `BSStream` loads the model. If stream identity is unavailable, the base form's
    `MODL` path is used. Use `shape` when identical geometry is shared by multiple
    source NIFs and its source cannot be distinguished after cloning.
-   `shape`: exact `NiTriShape`/`BSTriShape` node name.
-   `baseid`: eight-digit actor base form ID, with or without `0x`.
-   `race`: race editor ID.
-   `normal`: current normal-map path.
-   `diffuse`: current diffuse-map path.

## Payloads and inheritance

`profile` can reference either a partial profile declared in a JSON `profiles`
object or a full named profile from the Advanced Skin settings UI. A JSON named
profile with the same case-insensitive name takes precedence. `profileOverrides`
is an inline partial applied after the named profile.

`material` references a `materials` entry. `textures` is an inline material
override applied after it. For each texture channel:

-   omitted: inherit the value produced by earlier rules;
-   a string: bind that texture path;
-   `null` (or an empty string): explicitly disable the channel and bind black.

Before rules are evaluated, the existing `_rfaos.dds` and `_wet.dds` filename
discovery supplies the inherited values. Existing `nif` and `baseid` profile-only
JSON objects remain supported and are applied before the new rules.

## Ordering

Matching rules are applied from low to high precedence:

1. regular override files, then files in `User`;
2. lower `priority`, then higher `priority` (a rule-level value overrides the
   file-level value);
3. fewer populated selector fields, then more specific selectors;
4. normalized source filename;
5. order within the file.

Later rules overwrite only the channels or profile fields they provide. Use
namespaced profile, material, and rule IDs to avoid collisions between mods.
