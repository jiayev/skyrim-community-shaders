# External mod requirements

Drawcall Limit Fix (DCLF) keeps a mirror of the scene objects it draws. Engine hooks and SceneWriterFramework (SWF) events keep
that mirror up to date. When a plugin writes straight into an engine object, no hook sees the write, and DCLF draws stale state
until something else recaptures the object.

DCLF works around the plugins it knows about. Each workaround costs something: a per-frame check, an extra copy, or a case it still
gets wrong. This page lists, per mod, what the mod would need to change so DCLF can draw it correctly without those workarounds. The
changes are small and do not affect the game without DCLF.

## Reporting writes through SWF

A plugin connects to SWF with `swf_loader.h` (`extern/SceneWriterFramework/include/swf/`). SWF is optional: when no provider is
loaded, `SWF_Connect` returns null and the plugin carries on as before.

```c
#include "swf/swf_loader.h"

static const SWF_Api* swf = SWF_Connect(2);  /* abiMinor 2 for Write; NotifyWritten needs 0 */

/* After a direct store, on the thread that made it: */
if (swf)
    swf->NotifyWritten(geometry, SWF_FIELD_SKIN);
```

There are two calls:

- `NotifyWritten(object, fields)` reports a store the plugin already made. `swf.h` lists which object each `SWF_FIELD_*` expects.
- `Write(kind, object, value)` makes the store for the plugin and reports it (`SWF_WRITE_HIDDEN`, `SWF_WRITE_SHADER_PROPERTY`,
  `SWF_WRITE_ALPHA_PROPERTY`, `SWF_WRITE_ALPHA_FLAGS`). Prefer it where a `kind` exists.

Writes to objects that are not attached to the world yet need no report: DCLF captures an object whole when it is attached.

## RaceMenu (skee64)

Based on the public skee64 source (`SKSE64Plugins/skee64`). The current release may differ in details.

### Body morphs

`MorphFileCache::ApplyMorph` deep-copies the skin partition, and `NIOVTaskUpdateSkinPartition::Run` (`BodyMorphInterface.cpp`)
writes the morphed vertices and stores the new partition into the skin instance.

**Needed:**

1. After the new partition is stored, call `NotifyWritten(geometry, SWF_FIELD_SKIN)` for every geometry that uses the skin
   instance, including the overlays on it. The task holds only the skin instance today, so `ApplyMorph` would pass the geometry
   along.
2. Give the new partition a vertex buffer DCLF can share with the game: `D3D11_USAGE_DEFAULT`, no CPU access, created with its
   data. Do not change its contents after the partition is stored; a later morph already makes a new partition. With
   `bBodyMorphRebind=1` (the default), skee creates `D3D11_USAGE_DYNAMIC` buffers with CPU write access instead. DCLF's renderer
   cannot import those.

**Workarounds this removes:**

- (1) removes the per-frame partition check (`SkinPartitionMoved` in `FrameValues.cpp`). DCLF compares each sampled skin's
  partition against the one it planned, every frame. Without it, a morph showed only after a cell change or reload.
- (2) removes `MorphedPartitionCopy` (`SceneCapture.cpp`). DCLF builds its own copy of each morphed partition from the CPU-side
  vertices, which costs a second copy of the vertex data in video memory. A mod that rewrites a dynamic buffer in place without
  skee's `SHAPEDATA` marker is still drawn by the game.

### Overlay relinking

`OverlayInterface::RelinkOverlay` (`OverlayInterface.cpp`) stores a cloned skin instance into an overlay that is already attached.

**Needed:** after the store, call `NotifyWritten(overlay, SWF_FIELD_SKIN)`.

**Workaround this removes:** the same per-frame partition check, which also catches the relink.

### Override values (NiOverride)

`SetShaderProperty` (`ShaderUtilities.cpp`) stores override values straight into the property or its material:

- emissive colour and multiple;
- tint colour;
- alpha;
- glossiness and specular strength;
- the lighting effects;
- an effect shader's emissive values.

The engine runs no setter afterwards.

**Needed:** after storing a value, call `NotifyWritten(shaderProperty, SWF_FIELD_MATERIAL)`. One call per property is enough after
several values. The texture and texture-set overrides need nothing; they already go through engine calls that DCLF hooks.

**Workaround this removes, and the case still wrong without it:**

- DCLF picks up the values only when a texture change follows them, as it does in a preset load. A value changed on its own, such
  as a RaceMenu colour or alpha slider, stays at its last capture in DCLF until the actor's 3D is attached again.
- The material-capture alpha refresh (`DrainMaterialCaptures`) becomes a fallback rather than the only path. It stays for other
  writers.

### Overlay alpha flags

The overlay install writes `alphaFlags` and `alphaThreshold` before the overlay is attached, which needs nothing.

**Needed only if** a later version changes the flags on an attached overlay: use `Write(SWF_WRITE_ALPHA_FLAGS, alphaProperty,
flags)`.

### Overlay fade node (recommended)

An overlay's shader property keeps the fade node of the template it was copied from, which belongs to no ancestor of the overlay.
The pointer can outlive the node it names. DCLF does not hold it, and fades the overlay with the actor's own fade node.

**Recommended:** when attaching an overlay, set its property's fade node to the actor's fade node, as the engine does for the
actor's own shapes. This also makes the game's own renderer fade the overlay with the actor.

### Not RaceMenu's to change

- **Face overlays** (`Face [Ovl*]`) are copies of the head's `BSDynamicTriShape`. DCLF does not draw dynamic tri-shapes yet, so the
  game draws them.
- **Shown flags and armour.** Overlays share their base body's skin instance. DCLF now applies a skin's partition visibility to
  every shape on it, so armour over a painted body hides the paint too. This is DCLF's own fix, not a workaround.

### Summary

| Change in skee | Where | DCLF workaround it removes |
| --- | --- | --- |
| `NotifyWritten(geometry, SWF_FIELD_SKIN)` after the partition swap, for each sharer | `NIOVTaskUpdateSkinPartition::Run` | Per-frame partition check |
| DEFAULT, non-CPU vertex buffer for a new partition, left unchanged | `NIOVTaskUpdateSkinPartition::Run` (`bBodyMorphRebind`) | `MorphedPartitionCopy` |
| `NotifyWritten(overlay, SWF_FIELD_SKIN)` after relinking | `OverlayInterface::RelinkOverlay` | Per-frame partition check |
| `NotifyWritten(property, SWF_FIELD_MATERIAL)` after override values | `SetShaderProperty` | Stale values after a slider change |
| `Write(SWF_WRITE_ALPHA_FLAGS, ...)` for flags on attached overlays | none today | none (future-proofing) |
| Overlay property's fade node set to the actor's | overlay install | Unpinned stale fade node |
