# Development Documentation

## Getting Started

- **[VSCode Setup](./vscode-setup.md)** - IDE configuration, extensions, and auto-deploy
- **[Shader Workflow](./shader-workflow.md)** - Fast shader iteration and deployment
- **[Nsight Perf](./nsight-perf.md)** - GPU hardware counters per render pass (Developer Tools, `NVPERF_SUPPORT`)
- **[OpenRenderGraph on DXVK](./render-graph.md)** - How features run render-graph work on DXVK's Vulkan device
- **[DCLF architecture](./dclf-architecture.md)** - Drawcall Limit Fix as it is: the frame by thread, the data from engine events to GPU draws, ownership, the code's layout, switches and parity gates
- **[DCLF status](./dclf-status.md)** - What DCLF draws, and what the engine still does on the render thread
- **[DCLF open defects](./dclf-open-defects.md)** - Defects and failing parity checks under investigation, with their evidence
- **[DCLF pool follow-ups](./dclf-pool-followups.md)** - What should move into the pooled versions next (latched values, staged commit uploads, animated material values), and what stays a frame-slot ring
- **[Drawcall Limit Fix](./drawcall-limit-fix.md)** - The historical record of how DCLF was built, with the measurements behind each decision
- **[Skyrim engine notes](./skyrim-engine-notes.md)** - What the decompiled game binary does, where features depend on it
- **[Crash catalog](./crash-catalog.md)** - Crashes met during development, their stacks and what is known about each
- **[DCLF: a GPU-driven frame](./dclf-gpu-driven-frame.md)** - Investigation: what taking DCLF's objects out of every engine per-view walk would require
- **[DCLF: object tables built from events](./dclf-event-driven-tables.md)** - Investigation: replacing the per-frame scene walk with persistent tables changed by engine events, the walk kept as a synchronous validation
- **[Bugs found by parity](./bugs-found-by-parity.md)** - Bugs in other features (each a separate PR) and in the engine (with DCLF's rule for each) that DCLF's parity checks found

## Subsystems

- **[Scene Settings Framework](./scene-settings-framework.md)** - Interior/time-of-day/weather/location setting overrides, the generated catalog, and what the port left out

## Quick Links

### Common Tasks

- **Fast shader deployment:** `cmake --build build/ALL --target COPY_SHADERS`
- **Full build with deployment:** `.\BuildRelease.bat ALL-WITH-AUTO-DEPLOYMENT`
- **Validate shader permutations:** `hlslkit-compile` (see `.claude/CLAUDE.md` "Shader validation")
- **Create a worktree with submodules + local preset:** `pwsh ./tools/new-worktree.ps1 -Name my-branch`
- **Install optional git alias:** `pwsh ./tools/install-worktree-alias.ps1`

### Build Presets

- `ALL` - Standard build (no auto-deployment)
- `ALL-WITH-AUTO-DEPLOYMENT` - Build + deploy to game directory
- `Dev` - Optimized DLL + AIO folder (`BuildDev.bat`)
- `Dev-Fast` - Fastest iteration: Ninja, /Od, DLL only (`BuildDevFast.bat`)

See `CMakePresets.json` for all available presets.

## Worktrees

Use `tools/new-worktree.ps1` when creating a new worktree for development. The script:

- Creates the worktree under a sibling `<repo>.worktrees/` directory by default
- Reuses an existing local branch or creates a new one from `HEAD`
- Runs `git submodule update --init --recursive` in the new worktree
- Copies `CMakeUserPresets.json` from the main checkout if it exists there
- Does not overwrite an existing `CMakeUserPresets.json` unless `-ForcePresetCopy` is passed

Examples:

- `pwsh ./tools/new-worktree.ps1 -Name reproj_fixes`
- `pwsh ./tools/new-worktree.ps1 -Name shader-debug -StartPoint dev`
- `pwsh ./tools/new-worktree.ps1 -Name clean-build -NoSubmodules`

If you want a Git-native command, install the optional repo-local alias:

- `pwsh ./tools/install-worktree-alias.ps1`
- Then use `git new-worktree reproj_fixes`

The alias is installed into local Git config by default, so it does not affect other users unless they opt in.

## Build Targets

| Target            | Builds DLL | Copies Shaders | Use Case               |
| ----------------- | ---------- | -------------- | ---------------------- |
| `COPY_SHADERS`    | ❌         | ✅             | Fast shader iteration  |
| `DEPLOY_ALL`      | ✅         | ✅             | Full deployment (auto) |
| `prepare_shaders` | ❌         | ✅ (AIO only)  | CI shader validation   |

With `AUTO_PLUGIN_DEPLOYMENT`, the plugin's own build copies its DLL and PDB to every deploy target, so building just
the `CommunityShaders` target deploys it too; `DEPLOY_ALL` adds the rest of the package. The deploy targets are taken
from `CommunityShadersOutputDir` when CMake configures and cached (`CS_DEPLOY_DIRS`), so a later reconfigure from a
shell without the variable keeps them.

## Contributing

When adding new features or documentation, please keep development docs organized under `docs/development/`.
