# Development Documentation

## Getting Started

- **[VSCode Setup](./vscode-setup.md)** - IDE configuration, extensions, and auto-deploy
- **[Shader Workflow](./shader-workflow.md)** - Fast shader iteration and deployment
- **[Nsight Perf](./nsight-perf.md)** - GPU hardware counters per render pass, D3D11 and DXVK/Vulkan backends (Developer Tools, `NVPERF_SUPPORT`, `CS_NVPERF_*`)
- **[OpenRenderGraph on DXVK](./render-graph.md)** - How features run render-graph work on DXVK's Vulkan device
- **[Render graph schedule](./render-graph-schedule.md)** - The persistent schedule inspector window and the `communityshaders.schedule` DevBench tool: batches per queue, barriers, cross-queue waits and their causes
- **[DCLF architecture](./dclf-architecture.md)** - Drawcall Limit Fix's design: the frame by thread, the scene from engine events to GPU draws, ownership and withholding, shading, fades, object classes and views
- **[DCLF layout](./dclf-layout.md)** - DCLF's code layout: the folders, the extension points, the view and schedule contracts, and the rules `tools/dclf-layout-check.py` enforces
- **[DCLF validation](./dclf-validation.md)** - DCLF's switches, parity checks, diagnostics and probes, how to validate a change, crash triage, and the bugs parity found
- **[DCLF open defects](./dclf-open-defects.md)** - Defects and failing parity checks under investigation, with their evidence
- **[DCLF roadmap](./dclf-roadmap.md)** - The work left, by priority: engine work still on the render thread, the scene lane, GPU cost, coverage, integrations and tooling
- **[Memory usage](./memory-usage.md)** - What occupies VRAM in the main menu, by owner and resource, for choosing ORG aliasing port targets
- **[Skyrim engine notes](./skyrim-engine-notes.md)** - What the decompiled game binary does, where features depend on it

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
