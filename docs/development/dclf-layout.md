# DCLF code layout

This is the layout Drawcall Limit Fix's code is moving to, the rules it keeps, and the order of the move. The present
architecture is in [dclf-architecture.md](./dclf-architecture.md).

## The target

```
src/Features/DrawcallLimitFix.{h,cpp}   the shell: CS's one integration point (Feature overrides, hook thunks, menu)
src/Features/DrawcallLimitFix/
  Api/            the extension API: the only DCLF headers other CS features include
  Core/           what every feature builds on, with no feature knowledge
    Runtime/        the building blocks (KeptState, SlotTable, EventQueue, Retirement, ...) and the scene executor
    Engine/         engine access, the read window, the releases, the render-thread budget, hook helpers, event channels
    Settings/       the switches and toggles
    Scene/          the coordinator's core: mirror, tracked entries, slots, walk, set and claims, publication, pump
    Handoff/        taking objects from the engine: primary cull, scene lists, pass capture
    Shading/        the material port framework, bindings, constants, lighting descriptors
    Gpu/            frame values, pooled backings, GPU resources and textures, layouts, pipelines
    Render/         the snapshot builder, revisions, payloads, the epoch framework, common passes, the frame driver
  Features/       one folder per engine feature DCLF ports (Main, Forward, FirstPerson, Reflection, OcclusionMaps,
                  Shadows/{Common,Sun,Local}, Portals, GpuOcclusion, LocalLights, Actors, Faces, Particles, Billboards,
                  Decals, Effects, Landscape, Fades, Lod, TreeLod, TreeWind, TextureTransforms, HiddenNodes, SwitchNodes)
  Integrations/   one folder per CS feature DCLF supports, written against Api/ (TruePBR, AdvancedSkin, ...)
  Diagnostics/    the observer infrastructure and the parity checks that span features
```

A feature's folder owns everything about that feature:
- its engine hooks;
- its coordinator state;
- its table columns and records;
- its frame-value pool;
- its GPU stage and HLSL;
- its toggles;
- its parity checks and report lines.

Features register at install, before DCLF's threads start. Their callbacks run per phase or per chunk, never per object.
The bits that shaders share stay compile-time: each feature declares its own, and a core manifest checks that they don't
overlap.

## The extension points

A feature is an `Api::Feature`: a name and an install function. It is listed in `Features/FeatureList.cpp`, the one place
that shows what DCLF does. The shell's `PostPostLoad` calls `Extensions::InstallFeatures` before DCLF's threads start.
Install registers the feature's parts through `Api::InstallContext`. The registry (`Core/Extensions/Registry`) is
read-only after that.

| Contract | Header | What a feature gets |
| --- | --- | --- |
| Frame-value pool | `Api/FrameValues.h` | rows sampled every frame on DCLF's pool into a pooled backing the frame reads by descriptor (`FrameValues::PoolIndex`); its own frame-start reads (`frameStart`), and the rows once the producer is done (`FrameValues::ProducerDone`) |
| GPU stage | `Api/GpuStages.h` | a compute pass at a named slot of an epoch (`DepthSegmentBeforeBuild`, `DepthSegmentNextFrameState`), on a chosen queue, its programs set up with the scene's buffers, and optionally the segment's latch word |
| Payload product | `Api/Payload.h` | data built with every main payload on the builder, carried with it, and installed with it on the render thread |
| Scene resources | `Api/SceneResources.h` | state held with the scene's tables: versioned buffers registered with the scene's set, their graph registration, each revision's requirement, and the slots they hold of the objects (`SceneFit`) |
| Commit hook | `Api/Commits.h` | its part of each commit's frame data (depth, colour, shadow), staged into the adopted versions on the render thread |
| Frame binding | `Api/FrameBindings.h` | a buffer every draw reads at a declared texture register, given by the frame records (main, shadow, the builds' embedded addresses) and checked by the named-address parity |
| Switches and toggles | `Api/Settings.h` | its `CS_DCLF_*` switches, listed where it says beside Core's, and its live toggles, in the menu and the packed word |
| Report lines | `Api/Reports.h` | its lines at a named point of the periodic report |
| Stand-in observer | `Api/EngineEvents.h` | the primary cull's stood-in roots as they change, and Main::Update's entry; whether the primary cull is installed |

Each contract's thread, timing and lifetime is documented in its header. A feature's install also installs its engine
hooks and subscribes its observers (`Diagnostics::Subscribe`, under Core's `Observe()` flags).

The headers in `Api/` name only standard and ORG types. The contexts their callbacks receive are opaque there. Core
defines them in companions under `Core/Extensions/` (`FrameValuePools.h`, `GpuStages.h`, `PayloadProducts.h`,
`SceneResources.h`, `Commits.h`, `FrameBindings.h`, `Reports.h`, `EngineEvents.h`), which features inside DCLF include
for typed access to Core's types. The switches and toggles register with `Core/Settings`, which owns them.

The shaders' shared constants stay compile-time: a feature declares its own (a frame binding's register beside its HLSL
declaration), and `Features/Manifest.cpp` checks them against Core's and each other's.

`Features/TextureTransforms/` was the first feature built on them: a pool, a stage and a product. `Features/TreeWind/` was
the second: a pool with frame-start reads, scene resources with their commit hook and frame binding, a next-frame stage on
the compute queue, its switches and toggle, a report line, a stand-in observer with its engine hooks, and its parity. The
extension points still to come, as the next features need them:
- coordinator components and table column families (the trees' listing and table family are still the coordinator's);
- per-object slot rows (`SceneFit` and `ObjectFits` still name the trees' slot);
- object contributors (a record's or a lighting descriptor's per-object fields and flags, technique eligibility);
- more engine event channels;
- material contributors;
- view kinds;
- the shared-bit manifest (object and record flags).

## The rules

`tools/dclf-layout-check.py` checks these.

1. **Only the shell tests `CS_HAS_DCLF`.**
   - CMake defines it when the render graph and ORGModuleServices are present, and otherwise leaves the DCLF tree out of
     the build.
   - No file in the tree tests a dependency macro.
2. **The rest of CS reaches DCLF only through the shell.** Later it will also go through `Api/`.
3. **Includes point one way.**
   - `Api/` includes `Api/`.
   - `Core/` includes `Api/` and `Core/`.
   - A feature includes `Api/`, `Core/`, its own folder, and the features it declares as dependencies.
   - An integration includes `Api/` and `Core/`.
   - `Diagnostics/` may be included from anywhere. Production code reaches it only through its observer contract (`Hooks.h`,
     `Observation`, `Policy`), which the parity rework will redefine.
4. **Old includes may only shrink.** Includes into the folders that haven't moved yet (`Engine/`, `Scene/`) are
   counted against a baseline total, which each step lowers. The same goes for the functions over the complexity thresholds
   (120 lines or cyclomatic complexity 40).

## The order

Each step is a move-only commit, then a commit that extracts the code. It is verified with:
- the build, both with and without the render graph;
- the CPU tests;
- a normal run, plus `PERSISTENT_PARITY` and `SET_PARITY` runs;
- the motion timing run, for steps that change code.

The steps:
1. Put the dependency guard in one place, and add the layout check. *Done.*
2. `Core/Runtime`, `Core/Engine`, `Core/Settings`, `Core/Scene/SceneWake`. *Done.*
3. Split `IndirectDraws` into `Core/Render`. *Done.*
4. Split `Coordinator.h` into the coordinator's core plus one component per concern. *Done (Scene/Coordinator/State/).*
5. Two pilot features, `TextureTransforms` then `TreeWind`. Together they define the first extension points. *Done.*
6. The other features, least entangled first and `Fades` last. Then the integrations, then `Main` with what remains of Core.
