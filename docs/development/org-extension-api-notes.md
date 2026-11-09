# ORG extension API notes

What other SKSE plugins will need from CS once the renderer runs through OpenRenderGraph without D3D11. This is a
collection of requirements, not a design: each port in [org-full-port.md](./org-full-port.md) adds what it finds here,
and the API is designed from this list later.

Until that API exists, the DXVK build stays the default, and plugins that call D3D11 keep working there. In the
DXVK-free build, a plugin that calls D3D11 reaches a tripwire handle, which names the plugin's module in the log
(plan task P4.1). Each one found that way gets an entry below.

For what DCLF needs from mods that write engine objects directly (a different problem: scene state, not GPU calls), see
[external-mod-requirements.md](./external-mod-requirements.md).

## How to add an entry

One entry per need. Give:

- **Need:** what the plugin has to do, in GPU terms.
- **Who:** the plugins known to need it, and how they do it today (the D3D11 calls or hooks).
- **Where it fits:** the frame anchor or ORG concept it would map to.
- **Found by:** the plan task or tripwire report that found it.

## Needs

### An overlay pass after the UI

- **Need:** draw a 2D overlay (usually Dear ImGui) over the finished frame.
- **Who:** ImGui-based SKSE menus and HUDs that hook `IDXGISwapChain::Present` and draw with `imgui_impl_dx11`.
- **Where it fits:** a pass in the Present anchor, after CS's own overlay (plan P1.6, P2.2). The plugin records its ImGui
  draw data and CS draws it, or it gets a render pass on the back buffer.
- **Found by:** survey for the plan (2026-11-09).

### Present notification

- **Need:** run CPU code once per presented frame, and know the back buffer's size and format.
- **Who:** the same Present-hook plugins, plus frame limiters and capture tools.
- **Where it fits:** a callback from the ORG present stage. The swap chain is BasicRHI's, so there is no
  `IDXGISwapChain` to hook in the DXVK-free build.
- **Found by:** survey for the plan (2026-11-09).

### Texture creation and upload

- **Need:** create a texture, fill it from the CPU, and have the engine or a UI sample it.
- **Who:** plugins that make icons, HUD textures or dynamic textures with `ID3D11Device::CreateTexture2D` and
  `UpdateSubresource`.
- **Where it fits:** an ORG texture with an upload, handed to the engine as a tripwire handle that wraps it (P4.1, P4.3).
- **Found by:** survey for the plan (2026-11-09).

### Render to texture

- **Need:** render into a texture the engine later samples on a character.
- **Who:** RaceMenu/SKEE overlay and tint-mask baking.
- **Where it fits:** an ORG render pass the plugin attaches to a frame anchor, with its output imported as an engine
  texture.
- **Found by:** survey for the plan (2026-11-09).

### Reading named engine targets

- **Need:** read the scene colour, depth, normals or motion vectors at a known point in the frame.
- **Who:** post-processing and screenshot plugins that read `BSGraphics::Renderer` render targets.
- **Where it fits:** named graph resources at the frame anchors (P2.2), readable by a plugin's pass.
- **Found by:** survey for the plan (2026-11-09).

### Attaching passes to the frame

- **Need:** run compute or raster work at a defined point in the frame, with declared inputs and outputs.
- **Who:** any plugin that adds a GPU effect.
- **Where it fits:** the frame anchors from P2.2 (ReflectionsPrepass, EarlyPrepass, Prepass, DeferredPasses, Tonemap,
  PostProcessing, Present). Each anchor's ordering rule is recorded here when it is built.
- **Found by:** survey for the plan (2026-11-09).
