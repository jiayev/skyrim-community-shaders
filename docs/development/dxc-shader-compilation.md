# Community Shaders on DXC: design (2026-10-12, not started)

Goal: compile CS's replacement game shaders (`src/ShaderCache.cpp`, `SShaderCache::CompileShader`) with DXC instead of FXC.
FXC takes 14-15 s per Lighting pixel variant and the startup set is about 3,400 variants.

## Constraints found
- DXC cannot emit DXBC; the DXVK fork (`extern/dxvk`) creates D3D11 shaders only from DXBC (DXBC -> dxbc-spirv IR -> SPIR-V,
  `DxvkIrShader`). It already has `DxvkSpirvShader` (`src/dxvk/dxvk_shader_spirv.{h,cpp}`) for prebuilt SPIR-V, used by internal
  shaders only.
- Lighting, Utility, Effect and DistantTree already compile under DXC for DCLF (`ShaderPrograms.cpp`: `vs_6_6`/`ps_6_6`, `-HV 2018`,
  shifts b+0/t+200/s+400/u+500), but only under the `DCLF_*` defines.
- The engine keeps the VS blob and calls `CreateInputLayout` with it; DXVK reads its ISGN (register = Vulkan location). Whatever
  CS produces must stay a DXBC container with an ISGN chunk.
- VS and PS are replaced independently, the missing half falling back to vanilla (`Hooks.cpp`), so a DXC VS runs with a vanilla
  DXBC PS and vice versa. DXC numbers locations by declaration without FXC's register packing: linking must be by semantic.

## Design
- **Container:** a DXBC container passed to the normal `Create*Shader` calls: `ISGN`/`OSGN` synthesised from SPIR-V reflection,
  `SPRV` (the SPIR-V), `SPRM` (version, stage, binding convention, flags). No call site changes; one blob per variant on disk.
- **DXVK** (new fork file, e.g. `src/d3d11/d3d11_shader_spirv.cpp`, in `FORK.md`):
  - `CreateShaderModule` branches on an `SPRV` chunk; key from the chunk's MD5.
  - Reflection maps the shifted binding ranges back to slots (one shared header with DCLF's shifts), fills `DxvkBindingInfo` and
    the `D3D11BindingMask`, rejects anything else (`$Globals`, heap bindings, UAV counters).
  - Sampler rewrite pass: samplers become heap indices in per-stage push data (DXVK rejects plain sampler descriptors).
  - Semantic I/O linking when either stage of a pair is ingested (the `SemanticIo` machinery D3D9 uses); vanilla pairs keep
    register linking.
  - `D3D11CommonShader` from a ready `Rc<DxvkShader>`; a version export CS checks at startup.
- **DXC args:** the ORG wrapper's `-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout`, plus `-HV 2018 -fspv-reflect
  -fvk-use-dx-position-w -fspv-preserve-interface`, the four shifts, an unknown-image-format option for typed UAVs; never
  `-fvk-invert-y`.
- **CS:**
  - a dedicated `org::services::ShaderCompiler` on CS's own workers (wrapper additions: include list for hot reload, memory-cache
    opt-out, per-thread `IDxcCompiler3`);
  - SPIR-V reflection replaces `D3DReflect` (a neutral struct for `ReflectConstantBuffers`);
  - new cache extensions and the DXC version + argument hash in `Info.ini`;
  - Effects11 stays on FXC; `CS_NATIVE_D3D11=1` forces FXC.
- **Policy (proposed, to confirm):** one process-wide `ShaderCompiler = FXC | DXC` setting, no per-shader FXC fallback (a DXC
  failure is a failed shader). A parity mode compares FXC and DXC renders of the same frame and the reflection per variant.

## HLSL risks
- Never-compiled branches will hit the usual DXC rejects (loop variables after the loop, ambiguous min/max, resources in structs,
  non-static globals).
- Silent differences: float-to-int saturation, min/max NaN, contraction without `precise`, float controls and DXVK's per-vendor
  workarounds lost, no `PARTIAL_PRECISION`/`AVOID_FLOW_CONTROL`.
- Estimate: days for the core four shaders; 1-2 weeks for all ~50 top-level shaders.

## Stages
0. Offline: DXC on the slowest Lighting VS/PS variants with CS's defines: time against FXC, `spirv-val`, reflected offsets against
   `D3DReflect`. Pass: clearly faster (target 3x or more) and offsets equal. No DXVK change.
1. DXVK ingestion (~2 weeks), tested with a standalone D3D11 sample: DXC triangle, mixed DXBC/DXC pairs both ways, packed
   interpolants, comparison sampler, typed UAV.
2. CS (~1-2 weeks): wrapper extensions, container writer and reflection, cache format, the setting. Prototype gate: one Lighting
   VS+PS in game, reflection identical, pixel diff within threshold, hot reload works.
3. All Lighting/Utility/Effect/DistantTree variants: cache build time, frame-time A/B, parity captures.
4. The remaining shader types, ordered by time x count.
5. DXC default; FXC for A/B and native D3D11; then `Utils/D3D.cpp`.

Overall about 5-8 engineer-weeks. Critical files: `extern/dxvk/src/d3d11/d3d11_device.cpp` (`CreateShaderModule`,
`CreateInputLayout`), `d3d11_shader.{h,cpp}`, `extern/dxvk/src/dxvk/dxvk_shader_spirv.cpp`, `src/ShaderCache.cpp`,
`extern/ORGModuleServices/src/ShaderCompiler.cpp`, `src/Hooks.cpp`.
