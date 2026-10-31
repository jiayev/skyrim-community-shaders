# Drawcall Limit Fix: test build notes

Drawcall Limit Fix (DCLF) is a Beta feature of this Community Shaders build. It takes over drawing most of the world: statics,
terrain, trees, grass, LOD, actors, effects and their shadows. It draws them from the GPU in large indirect batches instead of one
engine draw call per object, which removes the game's CPU cost per object in busy scenes.

This is a test build. Expect bugs, and please report them (see "What to send" below).

## Requirements

- **Skyrim Special Edition 1.6.1170** (the current Steam build). On any other game version, DCLF turns itself off and the game
  renders as usual. The menu says why.
- **A Vulkan driver with `VK_EXT_descriptor_heap` and `VK_EXT_device_generated_commands`.** Community Shaders renders through
  Vulkan in this build (DXVK, bundled). So far DCLF has only been tested on NVIDIA (RTX 50 series, driver 621). If your driver
  lacks either extension, DCLF turns itself off and the game renders as usual.
- **RenderDoc turns DCLF off** while it is loaded: its capture layer hides the descriptor heap extension.

If DCLF is unavailable, the Drawcall Limit Fix page in the Community Shaders menu shows "Unavailable this session" and the reason.

## Turning it off

DCLF can be switched off and on while playing, with no restart: use the feature's on/off switch on its page in the Community
Shaders menu. When it is off, the game draws everything itself, as it does without DCLF.

The "What DCLF draws" section switches individual object classes back to the game's own drawing. If one kind of object looks
wrong (trees, grass, effects, shadows), switching its class off shows whether DCLF causes the problem. Save Settings keeps
your choices.

## Known issues

- **One frame with objects missing** after a resolution or upscaler change, or after a change of the render target formats.
  After a format change the game can also pause for a few seconds while the new pipelines compile.
- **Light Limit Fix's local shadow cache stays off** while DCLF is loaded. The two would choose different shadow-casting lights,
  so Light Limit Fix's menu shows the cache as unavailable.
- **Frame generation (FSR frame generation, DLSS-G) is untested** with DCLF. Please mention it in a report if you use it.
- **Occasional crashes** are under investigation, most often during long walks through busy areas with cells loading, and at
  load. A crash log helps a lot (see below).
- **Plugins that change scene objects directly** (node flags, fades, materials or alpha set without the game's own functions)
  can leave DCLF drawing an object as it was before the change. The compatibility monitor below finds these.

## The compatibility monitor

On DCLF's menu page, "Watch for changes other plugins make" turns on a check that runs while you play. It compares a slice of the
scene each frame with what DCLF holds, and lists every object that changed with no hook DCLF sees. Each entry names the
object's reference and its plugin. It does not change what is drawn. It costs a little CPU, so leave it off unless you are
testing.

"Write report" saves `CommunityShaders-DCLF-Compat-<date>-<time>.txt` next to the log. The report has the build, your game
version, every offender with its reference and plugin, your load order and your SKSE plugins.

## What to send

Send these files from `Documents\My Games\Skyrim Special Edition\SKSE\`:

- `CommunityShaders.log` from the session with the problem. Send it before you start the game again, because the next start
  replaces it.
- After a crash: `CommunityShaders.dmp` if there is one, and your crash logger's log (for example Crash Logger's).
- After the game froze or the screen went black: any `gpu-crash-*.nv-gpudmp` and `.nvdbg` files with that session's time.
- For a drawing problem tied to a mod: a compatibility monitor report written after you saw the problem.

Please also say where you were (the cell or location), what you did just before, and whether turning DCLF off makes the
problem go away.
