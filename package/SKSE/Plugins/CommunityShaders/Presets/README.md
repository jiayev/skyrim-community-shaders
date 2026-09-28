# Unified Presets

Drop packs under this folder as `Presets/<PackId>/`.

Metadata (`<PackId>.json` or `preset.json`) is **optional**. A classic ENB folder dropped here works as-is.

## Supported layouts

### Classic ENB drop (no metadata)
Same shape as Nexus ENB presets (e.g. NAT.ENB):

```
Presets/NAT.ENB - .../
  enbseries.ini
  enbseries/enbeffect.fx
  enblocal.ini          (optional)
  ReadMe.txt            (optional — used as description)
  Data/                 (optional game assets)
  01_LOW .../           (optional quality variants)
```

### Nested Effects11 (with or without metadata)
```
Presets/MyPack/
  MyPack.json           (optional metadata)
  effects11/            (enbseries.ini + enbseries/)
  logo.png / cover.png / gallery/
```

### CS Presets pack

Scene Manager **Export preset** writes a pack in this layout; it can sit beside `effects11/` in the same pack:

```
Presets/MyPack/
  MyPack.json                        (metadata, merged on re-export)
  InteriorOnly/MyPack_<Feature>.json
  TimeOfDay/<Period>/MyPack_<Feature>.json
  Weather/<spid>/[<Period>/]MyPack_<Feature>.json
  Locations/<formKey>/[<Period>/]MyPack_<Feature>.json
  logo.png / cover.png / gallery/
```

Only the **active** pack's scene files load. Applying another pack swaps the Scene Manager layer to it; a pack without scene files clears it. Your own Scene Manager edits stay in `SceneSettings/SceneManager.json` and always apply on top.

Export writes `version` (`MAJOR.MINOR.PATCH`) and `periodTransitionHours` into the metadata alongside the display fields below.

## Discovery

Packs are found under both the Data (VFS) path and the Community Shaders mod's own folder, so packs from other MO2 mods and packs created mid-session both show up. Folders starting with `_` or `.` are ignored.

## Metadata (optional, Presets packs)

When present, `<PackId>.json` can set name, author, version, description, tags, artwork paths (`logo` / `cover` / `screenshots`, relative to the pack folder), and backend paths (`effects11.path`, `backends`). Without it the UI uses the folder name, ReadMe text, and inferred E11/CS Presets content and artwork.
