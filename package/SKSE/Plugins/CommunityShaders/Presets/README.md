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

### Nested Effects11 + CSPP (with or without metadata)
```
Presets/MyPack/
  MyPack.json           (optional metadata)
  effects11/            (enbseries.ini + enbseries/)
  cspp.json
  logo.png / cover.png / gallery/
```

## Scene Manager exports (canonical)

Scene Manager **Export preset** is the source of truth for Scene Settings packs. It writes into the live tree:

```
SceneSettings/
  <Name>.json                 # presetMetadata — identity + optional Presets fields
  <Name>/                     # optional artwork (same filenames as unified packs)
    logo.png
    cover.png
    gallery/
  InteriorOnly/<Name>_*.json
  TimeOfDay/.../<Name>_*.json
  Weather/<spid>/.../<Name>_*.json
  Locations/<formKey>/.../<Name>_*.json
```

### `presetMetadata` (SceneSettings/`<Name>`.json)

Required:

| Key | Meaning |
|-----|---------|
| `name` | Preset identity (must match filename stem / overwrite prefix) |
| `version` | `MAJOR.MINOR.PATCH` |
| `periodTransitionHours` | Written by export from the current Scene Manager value |

Optional (same keys as unified Presets packs; artwork paths relative to `SceneSettings/`):

| Key | Meaning |
|-----|---------|
| `author` | Display author |
| `description` | Display blurb |
| `tags` | String array |
| `logo` / `cover` / `screenshots` | e.g. `"MyPreset/logo.png"` |

The Scene Manager export dialog can fill these and **Browse** images; chosen files are copied into `SceneSettings/<Name>/` (cover as poster art, screenshots under `gallery/`).

If artwork paths are omitted, the Presets browser still picks up `SceneSettings/<Name>/logo.png`, `cover.png`, and `gallery/` automatically.

The Presets tab lists these exports with an **SM** badge. Apply / Reload Overwrites rescans SceneSettings; it does **not** exclusively switch away from other Scene Manager overwrite files.

## Metadata (optional, Presets packs)

When present, `<PackId>.json` can set name, author, version, description, tags, artwork paths, and backend paths. Without it the UI uses the folder name, ReadMe text, and inferred E11/CSPP/artwork.
