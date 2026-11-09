# Engine D3D11 surface

Where Skyrim SE 1.5.97 and AE 1.6.1170 issue D3D11, for the native ORG port ([org-full-port.md](./org-full-port.md),
Phase R). Found with Ghidra (GhidraMCP over HTTP) and the Address Library on 2026-11-09. IDs are Address Library IDs.
_(inferred)_ marks conclusions not checked on every path. AE 1.7.104 is not verified anywhere in this file.

Functions confirmed here were renamed in the Ghidra databases (CommonLib-style names, only where the name was `FUN_`).

## What changes the plan

- **Stencil is used widely:** water (per-type bits, EQUAL), Lighting/Utility fades (GREATER, ref = fade x 255 at pass
  hint 10), Utility (writes 0xff), the depth prepass (per-object ref), particles (EQUAL 0xff), menu 3D (bit 0, then an
  ImageSpace NOT_EQUAL), and Scaleform masks (its own DSV, INCR/DECR). P0.1 is required, with a dynamic reference.
- **Only VS, PS and CS.** No geometry, hull or domain shaders and no stream-out. 8 DrawIndexed sites, 1
  DrawIndexedInstanced (grass), 1 Dispatch; vertex shaders never read textures.
- **Constant buffers are pooled by size:** one `ID3D11Buffer` serves many shaders and is rewritten with WRITE_DISCARD
  before each use, so a buffer pointer does not identify a shader. The constants path (P5.7) latches per use.
- **The dynamic ring** is three 4 MiB buffers, mapped with NO_OVERWRITE at unaligned offsets; on wrap the CPU spins on
  an event query. Two particle draws build input layouts inline and cache them forever.
- **The loading screen renders and presents on a job thread** (`JobListManager::ServingThread`, mode 2) under
  `Renderer::Lock`, a recursive critical section the main frame holds from Begin to End. Its UI includes Scaleform and
  the loading-screen 3D model (full accumulator and ImageSpace path). ORG submission and present must be safe from that
  thread under the same lock.
- **Resources are created and released without the lock**, on loader threads. Tripwire handles (P4.1) must accept
  Release from any thread and defer the GPU free until the frames that used the resource retire.
- **A window resize recreates every render target** (114 targets and 12 depth-stencils on SE).
- **DCLF does not claim menu geometry.** Menu, map and loading-screen 3D use their own accumulators; their passes reach
  `BSBatchRenderer::RegisterPass` but DCLF withholds only main, view and shadow renderers.
- **ImageSpace has 159 slots:** 32 composite effects, 124 shaders (17 compute), 3 empty. CS replaces 39 today; 68 have
  a CS source whose descriptor entry is commented out (vanilla DXBC runs); **17 have no CS source.**
- **Scaleform bypasses BSGraphics** with its own device and context. Its 58-slot renderer vtable, 55 DXBC blobs and the
  HLSL source of its shaders (embedded in the exe) are mapped below, so its shader port is a transcription.
- **Bink has no YUV shader:** the CPU writes RGBA8 tiles that a copy shader draws.

## RE lane 1: BSGraphics draw, state and constant-buffer choke points (SE 1.5.97 / AE 1.6.1170)

Sources: GhidraMCP decompiles of both executables, Address Library bins, CommonLibSSE-NG headers
(`RendererShadowState.h`, `BSShader.h`, `Renderer.h`) and DCLF sources. SE addresses are confirmed unless
marked. AE twins were matched by Ghidra function hash (marked **hash**), or by ID order plus instruction
count and a decompile check of the distinctive calls (marked **checked**). Anything marked _(inferred)_ was
not proven.

Notation. `ctx` is the immediate-context global (SE `0x143027ea0` / AE `0x1432887b0`). `RSS` is
RendererShadowState (SE `0x143027eb0`, ID 524773 / AE `0x14202ab70`, ID 388819). The AE layout of RSS is the
same as SE's: the AE SetDirtyStates reads depth mode at +0x88, stencil mode at +0x90 and topology at +0x358.
D3D11 context vtable: 0x38 VSSetConstantBuffers, 0x40 PSSetShaderResources, 0x50 PSSetSamplers, 0x60
DrawIndexed, 0x70 Map, 0x78 Unmap, 0x80 PSSetConstantBuffers, 0x88 IASetInputLayout, 0x90
IASetVertexBuffers, 0x98 IASetIndexBuffer, 0xa0 DrawIndexedInstanced, 0xc0 IASetPrimitiveTopology, 0xe0
End, 0xe8 GetData, 0x108 OMSetRenderTargets, 0x118 OMSetBlendState, 0x120 OMSetDepthStencilState, 0x148
Dispatch, 0x158 RSSetState, 0x160 RSSetViewports, 0x168 RSSetScissorRects, 0x190 ClearRenderTargetView,
0x1a8 ClearDepthStencilView, 0x218 CSSetShaderResources, 0x220 CSSetUnorderedAccessViews, 0x228
CSSetShader, 0x230 CSSetSamplers, 0x238 CSSetConstantBuffers, 0x370 ClearState.

Ghidra renames I made: SE `0x140d6bfe0` BSGraphics::Renderer::DrawTriShape, `0x140d6c1e0`
BSGraphics::Renderer::DrawInstancedTriShape (the name CS uses in GrassOptimizations), `0x140d6c8a0`
BSGraphics::Renderer::MapDynamicBuffer, `0x140d6cbe0` BSGraphics::Renderer::DrawParticleShaderTriShape.
AE: `0x140e464d0` DrawTriShape, `0x140e46d60` MapDynamicBuffer, `0x140e47180` DrawParticleShaderTriShape.
Nothing was saved to the Ghidra project.

---

### R1 Draw functions

#### Whole-engine summary of draw calls through `ctx`

Across all 110 functions that reference `ctx`, the only draw calls are **8 DrawIndexed sites, 1
DrawIndexedInstanced site and 1 Dispatch site**. The engine has no `Draw`, `DrawInstanced`, indirect draw,
DrawAuto or stream-output call. Every draw goes through the functions below. All of them take 16-bit
indices: IASetIndexBuffer always uses format 0x39 (`R16_UINT`) at offset 0. Vertex strides come from the
`VertexDesc` nibbles: `(desc & 0xF) * 4` for stream 0 and `((desc >> 4) & 0xF) * 4` for stream 1. Each
draw function first latches `RSS.vertexDesc` (+0x340, setting dirty bit 0x400 when it changes) and
`RSS.topology` (+0x358, dirty bit 0x800), then calls `SetDirtyStates(false)`, then issues the IA calls and
the draw.

The geometry-type dispatcher is SE `0x141307160` (ID 100847, Ghidra name "BSRenderPass::FUN") / AE
`0x1414f2ad0` (ID 107637). It switches on `BSGeometry::type` (byte at geometry+0x150) for a BSRenderPass
(`pass->geometry` at +0x10):

| type | BSGeometry::Type                     | path                                                                                                                                                                                                    |
| ---- | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1    | kParticles                           | 75483 (static particle-quad geometry), 75486 (map ring), PackParticleData, 75487 (unmap), 75488, then 75489. At most 0x800 particles per draw (clamped).                                                |
| 2    | kStripParticles                      | FUN_140d76d80 builds vertices into scratch 0x143283bb0 and indices into 0x143477bb0, then 75491                                                                                                         |
| 3    | kTriShape                            | 75477                                                                                                                                                                                                   |
| 4    | kDynamicTriShape                     | once per frame (frame counter `DAT_14302c8dc` compared with geometry+0x174): 75486 map, FUN_14130a030 copies the dynamic data, 75487 unmap. Then 75488 → 75489 with the offset stored at geometry+0x178 |
| 5    | kMeshLODTriShape                     | 75477 (index range from FUN_141330240/FUN_1413300c0)                                                                                                                                                    |
| 6    | kLODMultiIndexTriShape               | 75478 with its alternative index buffer when `pass->accumulationHint` (+0x1c) == 12, else 75477                                                                                                         |
| 7    | kMultiIndexTriShape                  | same as type 6                                                                                                                                                                                          |
| 8    | kSubIndexTriShape                    | a loop of 75477 calls over its segments (0x14-byte records)                                                                                                                                             |
| 9    | kSubIndexLandTriShape                | loop of 75477 per land quadrant; between draws it rebinds `RSS.PSTexture[0..5]` and `[7..12]` from `DAT_14302c8e8`                                                                                      |
| 10   | kMultiStreamInstanceTriShape (grass) | per instance group: GetID3D11Resource(type 7) writes the group index into a 16-byte CB, VSSetConstantBuffers(**b7**), then 75479                                                                        |
| 11   | kParticleShaderDynamicTriShape       | 75490                                                                                                                                                                                                   |
| 12   | kLines                               | 75494                                                                                                                                                                                                   |
| 13   | kDynamicLines                        | 75497 (map), FUN_14130a030 (pack), 75498 (unmap), 75499                                                                                                                                                 |

The draw functions are also exposed through **BSShaderResourceManager's vtable** (SE vtable starting
around `0x14186bf80`, COL at `0x141991de0`). Its slots are thunks `lea rcx,[0x143028490]; jmp <fn>`:
`0x141327fd0` jumps to 75477 and `0x141327ff0` to 75489. Each draw function also has a DATA reference in
a table at `0x143570e..0x143570fd`, which looks like the same interface or an Address Library-style table
_(inferred)_.

#### Per-function table

| SE ID | SE addr     | AE ID | AE addr                                  | Signature (decompiled)                                                                                                         | Geometry and D3D11 calls                                                                                                                                                                                                                                              | Main callers                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ----- | ----------- | ----- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 75477 | 0x140d6bfe0 | 77263 | 0x140e464d0 (**hash**)                   | `DrawTriShape(Renderer*, TriShape* {vb@0, ib@8, desc@0x10}, uint startIndex, uint triCount)`                                   | Static TriShape. TRIANGLELIST (4). IASetIndexBuffer(ib), IASetVertexBuffers(0, 1, vb, stride0, 0), DrawIndexed(tri*3, start, 0)                                                                                                                                       | Dispatcher types 3, 5-9 (8 call sites). FUN_1410a2370 (4 sites; a tiled texture/screenshot renderer that sets its own VS/PS _(inferred)_). BSShaderResourceManager thunk. In AE, also the inlined ImageSpaceManager fullscreen-quad draw FUN_141482940 (ID 453509), which draws `ImageSpaceManager+0x48`'s screen BSTriShape. **Imagespace passes are DrawTriShape on a fullscreen-quad TriShape** (SE path inferred to be the same) |
| 75478 | 0x140d6c0e0 | 77264 | 0x140e465d0 (**hash**)                   | `(Renderer*, TriShape*, uint start, uint triCount, ID3D11Buffer** altIB)`                                                      | As 75477, but the index buffer is `*altIB`                                                                                                                                                                                                                            | Dispatcher types 6 and 7 when the pass hint is 12                                                                                                                                                                                                                                                                                                                                                                                    |
| 75479 | 0x140d6c1e0 | 77265 | 0x140e466d0 (**hash**, named fDrawGrass) | `DrawInstancedTriShape(Renderer*, TriShape*, uint start, uint triCount, uint instanceCount, VertexDesc, VertexBuffer* instVB)` | Instanced. TRIANGLELIST. Two streams: 0 = mesh VB, 1 = `*instVB`, with strides from the desc nibbles. DrawIndexedInstanced(tri*3, instanceCount, start, 0, 0). This is the engine's only instanced draw                                                               | Dispatcher type 10 (grass). CS hooks it (GrassOptimizations.cpp:551)                                                                                                                                                                                                                                                                                                                                                                 |
| 75484 | 0x140d6c8a0 | 77271 | 0x140e46d60 (**checked**)                | `void* MapDynamicBuffer(Renderer*, uint size, uint* outOffset)`                                                                | The dynamic ring (described below)                                                                                                                                                                                                                                    | 75486, 75490, 75491 (x2), 75496 (d5d0), RenderPassImmediately_Standard (0x141308970)                                                                                                                                                                                                                                                                                                                                                 |
| 75485 | 0x140d6c9e0 | 77272 | 0x140e46ea0 (**hash**)                   | `Unmap...(Renderer*)`                                                                                                          | Unmap(ring[cur]). 75485, 75487, 75497 and 75498 (0x140d6d5f0) are byte-identical                                                                                                                                                                                      | RenderPassImmediately_Standard                                                                                                                                                                                                                                                                                                                                                                                                       |
| 75487 | 0x140d6ca30 | 77274 | 0x140e46ef0 (**hash**)                   | `UnmapDynamicTriShapeDynamicData(Renderer*, ...)`                                                                              | Unmap(ring[cur])                                                                                                                                                                                                                                                      | Dispatcher types 1 and 4                                                                                                                                                                                                                                                                                                                                                                                                             |
| 75489 | 0x140d6cab0 | 77276 | 0x140e47050 (**hash**)                   | `DrawDynamicTriShape(Renderer*, {vb, ib, desc}*, DrawData*, uint start, uint triCount, uint dynOffset)`                        | CPU-dynamic. TRIANGLELIST. Stream 0 = static vb (stride0), stream 1 = ring[cur] at `dynOffset` (stride1). DrawIndexed(tri*3, start, 0)                                                                                                                                | 75488 (0x140d6ca60, which copies the 0x20-byte DrawData: vb, ib, desc, offset@+0x18) for dispatcher types 1 and 4. BSShaderResourceManager thunk 0x141327ff0                                                                                                                                                                                                                                                                         |
| 75490 | 0x140d6cbe0 | 77277 | 0x140e47180 (**checked**)                | `DrawParticleShaderTriShape(Renderer*, const void* verts, uint vertCount)`                                                     | Copies `vertCount*0x30` bytes into the ring, then Unmap. TRIANGLELIST. **Clears the 0x400 bit, runs SetDirtyStates, then binds its own inline input layout** (below). IB = static quad IB 0x143025f38. DrawIndexed((vertCount/4)*6, 0, 0): quads, at most 2048 per IB | Dispatcher type 11 (BSParticleShader GPU-style emitters)                                                                                                                                                                                                                                                                                                                                                                             |
| 75491 | 0x140d6ce60 | 77278 | 0x140e47420 (**checked**)                | `(Renderer*, const void* verts, uint vertCount, const uint16* idx, uint idxCount)`                                             | Copies the vertices (stride 0x28) **and the indices** into the ring (two MapDynamicBuffer calls). IB = ring[cur] at the index offset. **TRIANGLESTRIP (5)**. Inline input layout (below). DrawIndexed(idxCount, 0, 0)                                                 | Dispatcher type 2 (strip particles: trails and ribbons)                                                                                                                                                                                                                                                                                                                                                                              |
| 75494 | 0x140d6d310 | 77282 | 0x140e478f0 (**hash**)                   | `DrawLineShape(Renderer*, LineShape*{vb, ib, desc}, uint lineCount)`                                                           | **LINELIST (2)**. DrawIndexed(lines*2, 0, 0)                                                                                                                                                                                                                          | Dispatcher type 12                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 75499 | 0x140d6d620 | 77289 | 0x140e47bf0 (**hash**)                   | `DrawDynamicLineShape(Renderer*, {vb, ib, desc, dynOffset@+0x18}*, uint start, uint lineCount)`                                | LINELIST. Stream 0 static, stream 1 = ring at dynOffset. DrawIndexed(lines*2, start, 0). Resets the offset to -1 afterwards                                                                                                                                           | Dispatcher type 13                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 75500 | 0x140d6d750 | 77290 | 0x140e47d20 (**hash**)                   | `(Renderer*, {vb, ib, desc}*, uint dynOffset, uint start, uint lineCount)`                                                     | As 75499, with an explicit offset                                                                                                                                                                                                                                     | No direct code callers: DATA reference only (interface table 0x143570fcc) _(inferred: BSShaderResourceManager/Ni line renderer)_                                                                                                                                                                                                                                                                                                     |

Helpers: 75483 `0x140d6c7e0` returns a static (guarded) "particle quad" DynamicTriShape descriptor:
vb = `0x143025f40`, ib = `0x143025f38`, desc `0x0840200004000051`. Stream 0 stride is 4 (a half2 corner UV)
and stream 1 stride is 20 (packed particle data from the ring). 75486 `0x140d6ca10` and 75496 `0x140d6d5d0`
are `MapDynamicTriShapeDynamicData(Renderer*, shape, DrawData*, ?, size)`: they call MapDynamicBuffer with
the size (DrawData+0x1c when 0) and store the offset at DrawData+0x18.

#### Inline input layouts (built with `device->CreateInputLayout` against the _currently bound_ VS bytecode, `RSS.currentVertexShader`+0x68 and size at +0x10, the first time only, then cached forever)

| Fn    | Cache global SE | Elements (semantic, index, DXGI format, slot, offset, per-vertex)                                                                                                                                                               | Stride |
| ----- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| 75490 | 0x143025f48     | POSITION0 R32G32B32A32_FLOAT(2) @0x00; NORMAL0 R32G32B32A32_FLOAT(2) @0x10; TEXCOORD0 R32G32B32_FLOAT(6) @0x20; TEXCOORD1 format **0x20** (R8G8B8A8_SINT by DXGI numbering, confirmed in the disassembly as an immediate) @0x2c | 0x30   |
| 75491 | 0x143025f50     | POSITION0 R32G32B32_FLOAT(6) @0x00; TEXCOORD0 R32G32B32_FLOAT @0x0c; NORMAL0 R32G32B32_FLOAT @0x18; COLOR0 R8G8B8A8_UNORM(0x1c) @0x24                                                                                           | 0x28   |

After binding its layout, each function looks up `(VS->vertexDesc@+0x48 & RSS.vertexDesc)` in the
input-layout cache and inserts its layout under that key (75588 `0x140d71830`). It then sets dirty bit
0x400 again, so the next normal draw rebinds the cached layout. **Pitfall:** the layout is validated only
against whichever VS happened to be bound on first use.

The general layout builder for the cache is 75583 (see R11). It maps VertexDesc to elements as follows.
Attribute i lives in stream 0 if bit 44+i is set, otherwise in stream 1 if bit 54+i is set. Its offset is
`(desc >> (2+4*i)) & 0x3c`. The elements:

- POSITION: R32G32B32A32_FLOAT.
- TEXCOORD0 (UV): R16G16_FLOAT.
- TEXCOORD1: R16G16B16A16_FLOAT.
- NORMAL, BINORMAL and COLOR: R8G8B8A8_UNORM.
- BLENDWEIGHT: R16G16B16A16_FLOAT, plus BLENDINDICES R8G8B8A8_UNORM at +8.
- TEXCOORD2/3 (landscape data): R8G8B8A8_UNORM at +0/+4.
- TEXCOORD2 (eye data): R32_FLOAT.
- TEXCOORD4..7 (instance data): R16G16B16A16_FLOAT at +0/+8/+0x10/+0x18, **per-instance, step 1**.

The cache is a hash map at SE `0x141e07140` (buckets `0x141e07160`, mask `0x141e07144`). Descriptor
`0x300000000407` is allowed to map to a null layout.

#### The dynamic ring (75484 MapDynamicBuffer), exactly

Globals (SE / AE):

| What                    | SE          | AE          |
| ----------------------- | ----------- | ----------- |
| `ID3D11Buffer* ring[3]` | 0x143025f18 | 0x143286a20 |
| `uint cur`              | 0x143025f30 | 0x143286a38 |
| `uint offset`           | 0x143025f34 | 0x143286a3c |
| `bool safe[3]`          | 0x143026164 | 0x143286a78 |
| `ID3D11Query* fence[3]` | 0x143026168 | 0x143286a60 |

Creation is 75593 SE `0x140d72350` / AE 77401 `0x140e4d280` (**checked**). It creates 3 buffers of
**4 MiB** each (ByteWidth 0x400000, USAGE_DYNAMIC, BIND_VERTEX|BIND_INDEX, CPU_ACCESS_WRITE), 3
`D3D11_QUERY_EVENT` queries, and sets `safe[i]=1`. In the same function it also creates:

- the **quad index buffer** `0x143025f38`: 0x6000 bytes, DEFAULT usage, BIND_INDEX, 2048 quads of (0,1,2,0,2,3);
- the **particle corner VB** `0x143025f40`: 0x8000 bytes, BIND_VERTEX, half2 corners (0,1)(1,1)(1,0)(0,0) for 2048 quads.

Algorithm of `MapDynamicBuffer(size, &outOffset)`:

1. `newEnd = offset + size`. If `newEnd > 0x400000`, the current buffer is retired: `safe[cur]=0`,
   `ctx->End(fence[cur])`, `cur = (cur+1) % 3`, `offset = 0`, `newEnd = size`.
2. If `!safe[cur]`, it spins: `GetData(fence[cur], &done, 4, flags)` with flags = 1 (DONOTFLUSH) on the
   first try and 0 afterwards, with `Sleep(1)` between tries, until S_OK and `done != 0`. Then
   `safe[cur] = (done == 1)`. **This is a CPU-blocking GPU fence wait on the render thread.**
3. It calls `ctx->Map(ring[cur], 0, D3D11_MAP_WRITE_NO_OVERWRITE (5), 0, &m)`. **NO_OVERWRITE is used
   every time, including the first use after a wrap; WRITE_DISCARD is never used.**
4. It sets `outOffset = offset`, `offset = newEnd`, and returns `m.pData + outOffset`.

There is **no alignment** of the offsets, and no allocation larger than 4 MiB is possible. Users unmap
right after writing (75485/75487/75497/75498). They bind ring[cur] as stream 1 (75489, 75499, 75500), as
stream 0 (75490, 75491), or as the index buffer (75491).

---

### R2 SetDirtyStates, ApplyState and RendererShadowState

`BSGraphics::SetDirtyStates(bool isCompute)`: SE 75580 `0x140d705b0`, AE 77386 `0x140e4b5b0`.
DCLF calls it with false (EngineAccess.h:155). When `isCompute` is true, the input-layout step is skipped
and bit 0x400 is kept pending. After the flags are processed, the whole flag word is cleared to 0 (or to
`flags & 0x400` when isCompute).

#### RSS fields read by SetDirtyStates (offsets from RSS; SE absolute = 0x143027eb0 + off)

| Off                                | Field                                        | Used for                                                                                                                                                 |
| ---------------------------------- | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0x00                               | stateUpdateFlags                             | the dirty bits                                                                                                                                           |
| 0x04                               | PSResourceModifiedBits                       | PSSetShaderResources, one slot per set bit, from PSTexture[] (+0x140)                                                                                    |
| 0x08                               | PSSamplerModifiedBits                        | PSSetSamplers from `samplers[PSTextureAddressMode[i] (+0xbc)*5 + PSTextureFilterMode[i] (+0xfc)]`                                                        |
| 0x0c                               | CSResourceModifiedBits                       | CSSetShaderResources from CSTexture[] (+0x240)                                                                                                           |
| 0x10                               | CSSamplerModifiedBits                        | CSSetSamplers from CSTextureAddressMode (+0x1c0) and CSTextureFilterMode (+0x200)                                                                        |
| 0x14                               | CSUAVModifiedBits                            | CSSetUnorderedAccessViews(i, 1, &CSUAV[i] (+0x300), NULL initial counts)                                                                                 |
| 0x18                               | renderTargets[8] (RT indices, -1 terminated) | RTVs at `RendererData+0xa58+idx*0x30` (Renderer+0xa68)                                                                                                   |
| 0x38/0x3c                          | depthStencil / depthStencilSlice             | DSV at `RendererData+0x1fb0+(ds*0x13+slice)*8`, or `+0x1ff0` for the read-only DSV when `RendererData+0x22` (readOnlyDepth) is set                       |
| 0x40/0x44                          | cubeMapRenderTarget / view                   | the cube RTV `RendererData+0x26d0+(rt*8+face)*8` replaces the RT list when != -1                                                                         |
| 0x48[8]                            | setRenderTargetMode[]                        | 0 = clear (ClearRenderTargetView with `RendererData+0x2768` clearColor), then set to 4                                                                   |
| 0x68                               | setDepthStencilMode                          | 0 or 6: clear depth+stencil; 1: depth; 2: stencil. Clear value `DAT_1415232d8` (1.0f), stencil 0. Then set to 4. Modes <3 and 6 also clear readOnlyDepth |
| 0x6c                               | setCubeMapRenderTargetMode                   | cube clear                                                                                                                                               |
| 0x70                               | viewPort (D3D11_VIEWPORT)                    | RSSetViewports(1). MaxDepth (+0x84) is adjusted by the depth bias                                                                                        |
| 0x88                               | depthStencilDepthMode                        | DS-state table row                                                                                                                                       |
| 0x90                               | depthStencilStencilMode                      | DS-state table column                                                                                                                                    |
| 0x94                               | stencilRef                                   | OMSetDepthStencilState ref                                                                                                                               |
| 0x98                               | rasterStateFillMode                          | raster table                                                                                                                                             |
| 0x9c                               | rasterStateCullMode                          | raster table                                                                                                                                             |
| 0xa0                               | rasterStateDepthBiasMode                     | raster table, plus the viewport MaxDepth offset `DAT_143026180[mode]`                                                                                    |
| 0xa4                               | rasterStateScissorMode                       | raster table                                                                                                                                             |
| 0xa8                               | alphaBlendMode                               | blend table                                                                                                                                              |
| 0xac                               | alphaBlendAlphaToCoverage                    | blend table                                                                                                                                              |
| 0xb0                               | alphaBlendWriteMode                          | blend table                                                                                                                                              |
| 0xb4                               | alphaTestEnabled                             | alpha-test CB (0 when disabled)                                                                                                                          |
| 0xb8                               | alphaTestRef                                 | alpha-test CB                                                                                                                                            |
| 0x340                              | vertexDesc                                   | input layout                                                                                                                                             |
| 0x348                              | currentVertexShader                          | VS+0x48 desc mask, VS+0x68 bytecode for layout creation                                                                                                  |
| 0x358                              | topology                                     | IASetPrimitiveTopology                                                                                                                                   |
| 0x5c0 / 0x5c4 (SE 0x143028470/474) | saved viewport Min/MaxDepth                  | viewport depth reset (BSGraphics::Renderer::SetMinandMaxViewportDepth 75456 writes it)                                                                   |
| 0x5d0 (SE 0x143028480)             | alphaBlendModeExtra                          | last blend index (MRT variant)                                                                                                                           |

#### Dirty-bit table

| Bit                      | CommonLib name                                                     | Flushes                                                                                                          | Notes                                                                                                                         |
| ------------------------ | ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| 0x1                      | DIRTY_RENDERTARGET                                                 | pending RT/DSV clears, then OMSetRenderTargets(n, rtvs, dsv)                                                     | Clears are deferred to the flush. Viewport is not touched                                                                     |
| 0x2                      | DIRTY_VIEWPORT                                                     | RSSetViewports(1, &RSS.viewPort)                                                                                 | also set internally by 0x40                                                                                                   |
| 0x4 / 0x8                | DEPTH_MODE / STENCILREF_MODE                                       | OMSetDepthStencilState(DS[depthMode*0x28 + stencilMode], stencilRef)                                             |                                                                                                                               |
| 0x10, 0x20, 0x40, 0x1000 | UNKNOWN1 (fill), CULL, DEPTH_BIAS, UNKNOWN2 (scissor _(inferred)_) | RSSetState(RS[((fill*3+cull)*12+bias)*2+scissor])                                                                | With 0x40, the viewport depth range is reset from the saved values and `MaxDepth -= biasViewportOffset[bias]`, which sets 0x2 |
| 0x80                     | ALPHA_BLEND                                                        | OMSetBlendState(BS[(((mode*2+a2c)*13+writeMode)*2)+extra], blendFactor `0x141e07168`, 0xffffffff)                |                                                                                                                               |
| 0x100 / 0x200            | ALPHA_TEST_REF / ENABLE                                            | Map(alphaTestCB SE 0x143027a28 / AE 0x143288328, WRITE_DISCARD), write `enabled ? ref : 0.0` as one float, Unmap | The CB is bound **PS b11** by SetPerFrameBuffers 75570                                                                        |
| 0x400                    | VERTEX_DESC                                                        | look up or create the input layout for `VS->vertexDesc & RSS.vertexDesc`, then IASetInputLayout                  | skipped when isCompute                                                                                                        |
| 0x800                    | PRIMITIVE_TOPO                                                     | IASetPrimitiveTopology(RSS.topology)                                                                             |                                                                                                                               |
| 0x2000                   | VRPREVIEW                                                          | not in flat SE/AE                                                                                                |                                                                                                                               |

**Not flushed by SetDirtyStates:**

- **VS/PS/CS shaders** are bound immediately by `Renderer_SetShader` 75550 (VSSetShader) and
  `Renderer_SetPixelShader` 75555 (PSSetShader). CSSetShader happens only in 75532.
- **Constant buffers** are bound immediately by the shaders' Setup* functions (R3).
- **VS textures and samplers**: the engine never calls VSSetShaderResources or VSSetSamplers through
  `ctx`.
- **Scissor rects** are set immediately by 75564 `0x140d70100` (RSSetScissorRects).

#### State tables (all created at init through `RendererData+0x38`, i.e. the device)

| Table                       | SE / AE                   | Size                                                                               | Created by (SE ID/addr, AE ID/addr)                                 |
| --------------------------- | ------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Depth-stencil               | 0x1430261b0 / 0x143286ab0 | 6 depth modes x 40 stencil modes = 240                                             | 75828 0x140d88c90 / 77650 0x140e657b0 (**hash**)                    |
| Rasterizer                  | 0x143026930 / 0x143287230 | 2 fill (SOLID, WIREFRAME) x 3 cull (NONE, BACK, FRONT) x 12 bias x 2 scissor = 144 | 75830 0x140d89230 / 77652 0x140e65d50 (**checked** by xref)         |
| Bias viewport offsets       | 0x143026180               | float[12]                                                                          | 75829 0x140d891f0 / 77651 (**hash**)                                |
| Blend                       | 0x143026db0 / 0x1432876b0 | 7 modes x 2 a2c x 13 write masks x 2 MRT variants = 364                            | 75827 InitD3D 0x140d88720 / 77649 0x140e65260 (**checked** by xref) |
| Samplers (PS and CS shared) | 0x143027910 / 0x143288210 | 6 address x 5 filter = 30                                                          | 75831 0x140d89480 / 77653 0x140e65fa0 (**checked**)                 |
| Release all                 |                           |                                                                                    | 75832 0x140d896b0 / 77654 (**hash**)                                |

Depth modes:

| Mode | DepthEnable / Write / Func      |
| ---- | ------------------------------- |
| 0    | off / no write / ALWAYS         |
| 1    | test / no write / LESS_EQUAL    |
| 2    | test / write / ALWAYS           |
| 3    | test / write / LESS_EQUAL       |
| 4    | test / no write / EQUAL         |
| 5    | test / no write / GREATER_EQUAL |

Blend modes:

| Mode | Blend                                          |
| ---- | ---------------------------------------------- |
| 0    | off                                            |
| 1    | SRC_ALPHA / INV_SRC_ALPHA                      |
| 2    | SRC_ALPHA / ONE                                |
| 3    | DEST_COLOR / INV_SRC_ALPHA (alpha: DEST_ALPHA) |
| 4    | DEST_COLOR / ZERO                              |
| 5    | ONE / ONE                                      |
| 6    | ONE / INV_SRC_ALPHA                            |

Write-mask column, 13 entries: none, RGB, RGBA, RG, BA, R, G, B, A, then four MRT combinations (9: RT0 B
and RT1 RGB; 10: RT0 RGBA and RT1 none; 11/12: RT0 RGBA with RT1 none or RGBA, RT2+ none). The "extra"
variant copies RT0's blend to RT1+ when the write mask index is below 9.

Sampler address modes: CLAMP; U-clamp/V-wrap; U-wrap/V-clamp; WRAP; BORDER; MIRROR. Sampler filters:
POINT (MaxLOD 0); MIN_MAG_LINEAR_MIP_POINT (MaxLOD 0); LINEAR; ANISO x16; COMPARISON_MIN_MAG_LINEAR_MIP_POINT
(the shadow comparison sampler).

#### Stencil: used, and the plan needs it

Stencil modes (column index into the DS table). All fail ops are KEEP, and back face = front face.

| Mode  | Enable | Read / Write mask   | Func      | Pass op                              | Meaning              |
| ----- | ------ | ------------------- | --------- | ------------------------------------ | -------------------- |
| 0     | off    | ff/ff               | ALWAYS    | KEEP                                 | default              |
| 1     | on     | ff/ff               | ALWAYS    | REPLACE                              | write ref            |
| 2-9   | on     | ff / 01,02,04,..,80 | ALWAYS    | REPLACE                              | write one bit        |
| 10    | on     | ff/ff               | ALWAYS    | INCR                                 |                      |
| 11    | on     | ff/ff               | GREATER   | KEEP                                 | "ref > stencil" test |
| 12    | on     | ff/ff               | EQUAL     | KEEP                                 |                      |
| 13-20 | on     | 01..80 / ff         | EQUAL     | KEEP                                 | test one bit         |
| 21-27 | on     | 02..80 / ff         | EQUAL     | REPLACE                              |                      |
| 28    | on     | 01/ff               | NOT_EQUAL | REPLACE                              |                      |
| 29    | on     | ff/ff               | NOT_EQUAL | KEEP                                 |                      |
| 30    | on     | 01/ff               | NOT_EQUAL | KEEP                                 |                      |
| 31-38 | on     | ff,02..80 / ff      | EQUAL     | INCR                                 |                      |
| 39    | on     | ff/00               | EQUAL     | REPLACE (write mask 0, so read-only) |                      |

Code that sets `RSS.depthStencilStencilMode` (+0x90) and `stencilRef` (+0x94), confirmed in SE and AE:

| Setter                                                      | SE ID / addr                                                                                                                                                                                       | AE ID / addr          | Modes and refs                                                                                                                                                                                                   | Purpose                                                                                                                                                    |
| ----------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Main::RenderDepth                                           | 100421 / 0x1412e3520                                                                                                                                                                               | 107139 / 0x1414ccb30  | resets to 0/0xff. In the depth prepass, after the first-person view and when `DAT_141e0de43` is set, it loops over the list `DAT_1431d0fa8[DAT_1431d0fb8]` and draws each accumulator with **mode 1, ref = i+1** | marks per-object stencil IDs in the main depth/stencil _(the list's identity is not proven: likely the per-object masks the later imagespace passes read)_ |
| BSLightingShader::SetupGeometry                             | 0x1412f2bb0                                                                                                                                                                                        | 0x1414dd040           | **mode 11 (GREATER), ref = (fade alpha * k) & 0xff** when `pass->accumulationHint == 10`. The fade value comes from property+0x130 or +0x14c (LOD flag bit 0x80 at pass+0x1e)                                    | stencil-based fade or dissolve for that render mode _(inferred)_                                                                                           |
| BSUtilityShader::SetupGeometry                              | 0x14130ec70                                                                                                                                                                                        | 0x1414fae40           | the same mode-11 fade rule. Also **mode 1, ref 0xff** when `(flags & 0x1200) == 0x1200`                                                                                                                          | depth/stencil-writing utility passes                                                                                                                       |
| BSUtilityShader::RestoreTechnique                           | 0x14130dd80                                                                                                                                                                                        | 0x1414f9f50           | reset to 0                                                                                                                                                                                                       |                                                                                                                                                            |
| BSWaterShader::SetupGeometry                                | 0x1412f6a70 (62 refs)                                                                                                                                                                              | 0x1414e1890 (64 refs) | modes 2-9 (write water bit N, ref = bit), 14-20 and 32-38 (equal test and incr on the water's bit, from `water+0x8c` as 2..0x80), mode 30 ref 1, modes 14-20 with refs 3, 5, 9, 0x11, 0x21, 0x41, 0x81           | per-water-type stencil bits (water occlusion and reflection masking)                                                                                       |
| BSParticleShader::SetupGeometry                             | 0x141320380                                                                                                                                                                                        | 0x14150ccc0           | **mode 39, ref 0xff** when the technique is 4 or 5 and `DAT_141e0dff8 == 2`                                                                                                                                      | particles only where stencil == 0xff                                                                                                                       |
| Menu 3D render (Inventory3DManager::Render and other menus) | 51855 / 0x1408d2390                                                                                                                                                                                | 52727 / 0x140972590   | **mode 2** (write bit 0) while drawing the item, then **mode 28** (NOT_EQUAL bit 0) for imagespace effect 0x55                                                                                                   | inventory and menu item mask                                                                                                                               |
| ImageSpaceEffectWaterDisplacement::Func2                    | 0x141303da0                                                                                                                                                                                        | 0x1414ef370           | resets to 0                                                                                                                                                                                                      |                                                                                                                                                            |
| FUN_1412f0bb0                                               | 0x1412f0bb0                                                                                                                                                                                        | 0x1414da970           | reset                                                                                                                                                                                                            |                                                                                                                                                            |
| Restore functions                                           | (code at 0x1412f3876, 0x1412f5e65, 0x1412f803a, 0x141310377, 0x141320d59: Lighting RestoreGeometry, Water RestoreTechnique and RestoreGeometry, Utility RestoreGeometry, Particle RestoreGeometry) |                       | write 0/0xff                                                                                                                                                                                                     |                                                                                                                                                            |

#### Geometry, hull and domain shaders

**None.** No GSSetShader, HSSetShader, DSSetShader, their CB/SRV/sampler setters, or SOSetTargets are
called through `ctx`. Among the 32 device-global users there is no CreateGeometryShader, CreateHullShader
or CreateDomainShader (device vtable 0x68/0x70/0x80/0x88). Only VS, PS and CS exist. (VR has a stereo
geometry-shader path per CommonLib comments; it is not present in flat SE/AE.)

#### ApplyState: SE 75462 `0x140d6a330`, AE 77247 `0x140e44850` (Ghidra-named in both)

`Renderer::ApplyState(Renderer* this, bool rtOnly)` is a partial flush. It handles only:

- bit 1: RT, DSV and clears, using `this+0xa68` RTVs, `this+0x1fc0/0x2000` DSVs and `this+0x2778` clear
  color. It supports only depth-stencil modes 0, 1 and 2, not 6.
- when `!rtOnly`: bits 0xc (DS state), 0x1070 (raster, including the depth-bias viewport tweak), 2
  (viewport) and 0x80 (blend, factor `0x141e07178`).

It flushes **no** textures, samplers, UAVs, input layout, topology or alpha test, and it **does not clear
the dirty flags**, so the next SetDirtyStates reapplies them. It is used before clears and by passes that
draw without SetDirtyStates (for example the menu-3D path calls it after SetRenderTarget).

Other immediate state calls: Renderer::Begin 75460 does **ClearState** every frame, re-inits the RSS flags
and binds the per-frame CB to VS and PS b12. SetPerFrameBuffers 75570 binds PS b11 (alpha test) and VS/PS
b12.

---

### R3 Constant-buffer contract

#### Global constant buffers (75592: SE `0x140d720d0`, AE 77400 `0x140e4d000`, **hash**)

All are USAGE_DYNAMIC, BIND_CONSTANT_BUFFER, CPU_ACCESS_WRITE, and are written with Map(WRITE_DISCARD).

| SE global                    | AE global        | Size                 | Use                                                                                                            |
| ---------------------------- | ---------------- | -------------------- | -------------------------------------------------------------------------------------------------------------- |
| 0x143027a08[4]               | (in 0x14328xxxx) | 0xf00 each           | 4 rotating "custom" CBs handed out by GetID3D11Resource 75561 (Ghidra name is wrong: it is a custom-CB mapper) |
| 0x143027a30 + i*8, i = 1..19 |                  | 0x10*i (up to 0x130) | **VS PerTechnique (b0) size-class pool**                                                                       |
| 0x143027ad0 + i*8, i = 1..9  |                  | up to 0x90           | **VS PerMaterial (b1) pool**                                                                                   |
| 0x143027b20 + i*8, i = 1..27 |                  | up to 0x1b0          | **VS PerGeometry (b2) pool**                                                                                   |
| 0x143027c00 + i*8, i = 1..19 |                  | up to 0x130          | PS PerTechnique pool _(inferred from symmetry and the read by 75582)_                                          |
| 0x143027ca0 + i*8, i = 1..19 |                  | up to 0x130          | PS PerMaterial pool _(inferred)_                                                                               |
| 0x143027d40 + i*8, i = 1..39 |                  | up to 0x270          | PS PerGeometry pool _(inferred)_                                                                               |
| 0x143027a28                  | 0x143288328      | 0x10                 | alpha-test ref, PS b11                                                                                         |
| 0x143027e80                  | 0x143288780      | 0x240                | compute-shader CB0 (75562 writes it, 75532 binds it to CS b0)                                                  |
| 0x143027e88                  | 0x143288788      | 0x2d0                | per-frame, VS and PS b12, written by 75472 "UpdateCameraData" (CS name; AE 77258)                              |
| 0x143027e90                  | 0x143288790      | 0x10                 | instance-group CB, VS b7 (GetID3D11Resource type 7)                                                            |

**Important for the design:** `VertexShader/PixelShader::constantBuffers[level].buffer` (VS+0x18/0x28/0x38,
PS+0x10/0x20/0x30) points into the **shared size-class pools**. Every shader whose group has the same size
shares one ID3D11Buffer. The serializer 75581 `0x140d70d00` writes the pool index (VS tech index < 0x14,
mat < 10, geom < 0x1c) into the shader cache. Buffer identity therefore does not identify a shader. Each
Setup* call rewrites the buffer with WRITE_DISCARD immediately before the draws it applies to. The write
offsets come from the shader's `constantTable` (int8 per constant: VS+0x50, PS+0x40), and CommonLib's
`SetVSConstant` and `SetPSConstant` model them.

#### Per shader type: which vtable functions map which groups (SE; vtable slots 2-7 = SetupTechnique, RestoreTechnique, SetupMaterial, RestoreMaterial, SetupGeometry, RestoreGeometry)

| Shader (SE vtable)                          | SetupTechnique                 | SetupMaterial                | SetupGeometry                                                                                                                                                                                                                                       | Other slots                                                                                | Maps through `ctx` |
| ------------------------------------------- | ------------------------------ | ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------ |
| Lighting (0x141864de8)                      | 0x1412f1990: VS b0 and PS b0   | 0x1412f2020: VS b1 and PS b1 | 0x1412f2bb0: VS b2 and PS b2                                                                                                                                                                                                                        | bones via 14131f630                                                                        | 6                  |
| Effect (0x1418665e0)                        | 0x1412fecb0: VS b0 and PS b0   | 0x1412ff0b0: VS b1 and PS b1 | 0x1412ff500: VS b2 and PS b2, plus a **custom VS b11** of 0x80 float4 from GetID3D11Resource (geometry+0x158 data)                                                                                                                                  |                                                                                            | 6                  |
| Utility (0x1418685b0)                       | 0x14130df90: VS b0 and PS b0   | 0x14130e890: VS b1 and PS b1 | 0x14130ec70: VS b2 and PS b2                                                                                                                                                                                                                        |                                                                                            | 6                  |
| Water (0x141865360)                         | 0x1412f6000: VS b0 and PS b0   | 0x1412f63d0: VS b1 and PS b1 | 0x1412f6a70: VS b2 and PS b2                                                                                                                                                                                                                        |                                                                                            | 6                  |
| Sky (0x141865c70)                           | 0x1412fa900: none (state only) | none                         | 0x1412fabb0: VS b2 and PS b2                                                                                                                                                                                                                        |                                                                                            | 2                  |
| Grass (0x14185d470)                         | 0x1412ce060: none              | 0x1412ce200: none            | 0x1412ce260: VS b2                                                                                                                                                                                                                                  | 0x1412ceb90: **VS b8** per-instance from GetID3D11Resource; dispatcher type 10 binds VS b7 | 1                  |
| DistantTree (0x141866a18)                   | 0x1413014c0: VS b0 and PS b0   | none                         | 0x141301990: VS b2                                                                                                                                                                                                                                  |                                                                                            | 3                  |
| Particle (0x14186a4b8)                      | 0x141320190: VS b0             | none                         | 0x141320380: VS b2 and PS b2                                                                                                                                                                                                                        |                                                                                            | 3                  |
| BloodSplatter (0x14185eea8)                 | 0x1412d8bf0: none              | none                         | 0x1412d8da0: VS b2 and PS b2                                                                                                                                                                                                                        |                                                                                            | 2                  |
| ImageSpace base (0x141868118)               | 0x14130c870                    | none                         | 0x14130c980 (Ghidra mislabels it GetTechniqueName): VS b2 and PS b2                                                                                                                                                                                 |                                                                                            | 2                  |
| ImageSpace subclasses                       |                                |                              | 37 per-effect setup functions (0x1413309e0 to 0x141332970, for example ISCompositeLensFlare 0x141331130). Each fills a GetID3D11Resource custom CB and binds **the same buffer to VS b2 and PS b2**. WaterFlow SetupGeometry 0x14132bf60 maps PS b2 |                                                                                            | 1 (WaterFlow)      |
| Skinning (NiBoneMatrixSetterI, 0x14131f630) |                                |                              | VS **b10** bones and **b9** previous bones from GetID3D11Resource (types 10 and 9)                                                                                                                                                                  |                                                                                            | 0 (via 75561)      |

#### Map sites through `ctx`: 46 calls in 32 functions, with 91 Unmap calls

Renderer level (8):

- 75472 per-frame b12
- 75514 `0x140d6e4c0`, Map(texture, mapType). Callers: save-game screenshot (1405c0890 from Win32FileType::WriteSaveFile) and the tiled renderer 1410a2220/1410a2840
- 75516 `0x140d6e530`. Caller: FUN_1412dd140, which fills a 4096-float lookup texture
- 75518 `0x140d6e5a0` (no code callers)
- 75561 GetID3D11Resource (custom-CB pool)
- 75562 compute CB (callers 1412dc200, 1412dc3a0, 1412e8470, 1412ea7e0, DispatchCopySubRegion, 1412eb530, 1412ed610 x4, 1412e4470, 1412eb610)
- SetDirtyStates, alpha test
- MapDynamicBuffer, the ring

Shader level (38): the table above (Lighting 6, Effect 6, Utility 6, Water 6, Sky 2, Grass 1, DistantTree
3, Particle 3, BloodSplatter 2, ImageSpace base 2, WaterFlow 1).

Indirect Map users: about 40 functions call GetID3D11Resource (37 imagespace setups, Grass b8, Effect b11,
bones b9/b10, dispatcher b7), all of them binding with `VSSetConstantBuffers`/`PSSetConstantBuffers`
right after an Unmap. Through `ctx` there are 64 VSSetConstantBuffers calls, 55 PSSetConstantBuffers
calls, 1 CSSetConstantBuffers call and **no UpdateSubresource** calls.

Register map in use: b0, b1 and b2 (groups); b7 (instance group); b8 (grass instances); b9 and b10
(bones); VS b11 (Effect custom); PS b11 (alpha test); b12 (per-frame); CS b0. CS features add their own
registers (DCLF notes b4 permutation, b5 SharedData, b6 FeatureData).

#### What DCLF already covers

- **ConstantMirror** (`Core/Engine/Capture/ConstantMirror.h`) is generic. It hooks Map, Unmap and
  UpdateSubresource on the immediate context and keeps CPU copies of _watched_ buffers. The watched
  buffers are the per-frame b12 (ViewEpoch.cpp:273, ShadowCapture.cpp:219, Uploads.cpp:245 reads ViewProj
  from VS b12 c8), whatever is bound at the non-per-draw VS/PS registers at capture
  (MainEpochs.cpp:483), and frame buffers read through SRVs (Uploads.cpp:389, ViewEpoch.cpp:431).
- Per-draw registers that DCLF decodes itself rather than mirrors (`Core/Extensions/FrameBindings.h`
  kCorePerDrawVS/PS): **Lighting b0-b2, b4, VS b9/b10, PS b11**. PulledSource.cpp and DrawPipelines.h
  rebuild PerTechnique, PerMaterial and PerGeometry rows for Lighting, and Utility's PerMaterial for
  shadows.
- **Not decoded:** Effect, Water, Sky, Grass (b7/b8), DistantTree, Particle, BloodSplatter, ImageSpace
  (custom CB b2), compute CB0, VS b11. These still go to D3D11 unchanged.

---

### R11 Device-global users and creation functions

Device global SE `0x143025f08` (524729) / AE `0x143286a10` (411348). It and `ctx` are both written once by
75589 `0x140d718d0` (D3D11CreateDeviceAndSwapChain plus CreateDXGIFactory, which also stores context and
device in RendererData). **32 functions read the device global (confirmed count).** The state tables are
created through `*(RendererData+0x38)`, the same device, not the global.

| SE ID                 | SE addr                                 | AE ID / addr                    | Signature or role                                                             | Device call(s)                                                                                                   | Result stored at                                                                                            |
| --------------------- | --------------------------------------- | ------------------------------- | ----------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| 75445                 | 0x140d68dd0                             | ~77635 0x140e63840 _(inferred)_ | Renderer::Init(this, RendererInitOSData*, ApplicationWindowProperties*, HWND) | CreateQuery x2                                                                                                   | timing/occlusion queries                                                                                    |
| 75457                 | 0x140d69ea0                             |                                 | Renderer::CreateSwapChain(Renderer*, HWND*, bool)                             | QueryInterface(DXGI), factory CreateSwapChain                                                                    | `this+0x70+i*0x50` RendererWindow.swapChain                                                                 |
| 75467                 | 0x140d6a870                             | 77253 0x140e44d90 _(inferred)_  | create render target (2D)                                                     | CreateTexture2D x2, CreateRenderTargetView, CreateShaderResourceView x2, CreateUnorderedAccessView               | RendererData+0xa58 + rt*0x30: tex@0, copy@+8, RTV@+0x10, SRV@+0x18, copy SRV@+0x20, UAV at `(rt+0x38)*0x30` |
| 75469                 | 0x140d6aca0                             |                                 | create depth-stencil target                                                   | CreateDepthStencilView x2 (normal and read-only), CreateShaderResourceView x2 (depth and stencil)                | RendererData+0x1fb8 block: tex, DSV `+0x1fc0+i*8`, RO-DSV `+0x2000+i*8`, SRVs +0x2040/+0x2048               |
| 75470                 | 0x140d6aef0                             |                                 | create 3D target                                                              | CreateTexture3D, SRV, UAV                                                                                        | RendererData+0x2718 (tex), +0x2728 (SRV), UAV at (rt+0x139)*0x20                                            |
| 75471                 | 0x140d6b0c0                             |                                 | create cubemap target                                                         | CreateRenderTargetView per face, SRV                                                                             | RendererData+0x26d8 (tex), RTVs at (+0x4dc+face)*8, SRV RendererData+10000                                  |
| 75507                 | 0x140d6dbc0                             | 77299 0x140e48190               | Renderer::CreateRenderTexture(w, h)                                           | CreateTexture2D, SRV                                                                                             | new NiTexture::RendererData (SRV@+0x10)                                                                     |
| 75509                 | 0x140d6dd70                             |                                 | texture from DDS memory                                                       | FUN_140d7dc50(device, ...) (DDS loader)                                                                          | returned                                                                                                    |
| 75510                 | 0x140d6de00                             | 77302 0x140e48560 (**hash**)    | SRV for a render target                                                       | CreateShaderResourceView                                                                                         | `out+0x10`                                                                                                  |
| 75511                 | 0x140d6df00                             | 77303 (CS uses this ID)         | create 2D texture (dynamic)                                                   | CreateTexture2D, SRV                                                                                             | texture object +0x10                                                                                        |
| 75513                 | 0x140d6e2d0                             |                                 | create 1D texture                                                             | CreateTexture1D, SRV                                                                                             | texture object +0x10                                                                                        |
| 75526                 | 0x140d6eb70                             | 77320 0x140e49890 (**hash**)    | `(?, ulonglong, longlong)`                                                    | FUN_140d87b20(device, ctx, ...) (texture save/convert helper that uses the immediate context)                    |                                                                                                             |
| 75533 / 75534 / 75535 | 0x140d6f020 / 0x140d6f070 / 0x140d6f0c0 |                                 | create UAV for a buffer or texture wrapper                                    | CreateUnorderedAccessView                                                                                        | `wrapper+8`                                                                                                 |
| 75546                 | 0x140d6f440                             | 77346 0x140e4a150 _(inferred)_  | create SRV (from a description)                                               | CreateShaderResourceView                                                                                         | out param                                                                                                   |
| 75547                 | 0x140d6f6e0                             | 77348 0x140e4a3e0 (**checked**) | load VS from stream: `VertexShader* (?, stream)`                              | CreateVertexShader(bytecode = VS+0x68, size)                                                                     | `VertexShader+0x08` (VS struct 0x68 + bytecode; groups +0x18..; vertexDesc +0x48; constantTable +0x50)      |
| 75548                 | 0x140d6f8a0                             | 77349 0x140e4a5a0 (**checked**) | create VS from bytes                                                          | CreateVertexShader                                                                                               | VS+0x08                                                                                                     |
| 75551                 | 0x140d6fa00                             | 77352 0x140e4a700 (**checked**) | create PS from bytes `(?, bytes, size)`                                       | CreatePixelShader                                                                                                | `PixelShader+0x08` (groups +0x10..; constantTable +0x40)                                                    |
| 75552                 | 0x140d6fab0                             | 77353 0x140e4a7b0 (**checked**) | load PS from stream                                                           | CreatePixelShader                                                                                                | PS+0x08                                                                                                     |
| 75556                 | 0x140d6fda0                             | 77357 0x140e4aaa0 (**checked**) | load CS from stream                                                           | CreateComputeShader(bytecode = CS+0x90)                                                                          | `ComputeShader+0x60`                                                                                        |
| 75583                 | 0x140d70f90                             | 77389 0x140e4bf20 (**checked**) | `ID3D11InputLayout* CreateInputLayout(VertexDesc)` (layout rules in R1)       | CreateInputLayout against RSS.currentVertexShader                                                                | returned, then inserted into the cache 0x141e07140                                                          |
| 75585                 | 0x140d71590                             |                                 | create query                                                                  | CreateQuery                                                                                                      | `out`                                                                                                       |
| 75589                 | 0x140d718d0                             | ~77396 0x140e4c830 _(inferred)_ | device creation                                                               | CreateDXGIFactory, D3D11CreateDeviceAndSwapChain, then QueryInterface on the device                              | device global, `ctx`, RendererData context field                                                            |
| 75590                 | 0x140d71d20                             | 77397 0x140e4cc50 (**hash**)    | create render-window views (**not a query**)                                  | CreateRenderTargetView and SRV from the swap-chain buffer                                                        | `RendererData+0x78/+0x80 + window*0x50` (RendererWindow.renderView/resourceView)                            |
| 75592                 | 0x140d720d0                             | 77400 0x140e4d000 (**hash**)    | create all global CBs (R3)                                                    | CreateBuffer x(4+19+9+27+19+19+39+4)                                                                             | the globals in the R3 table                                                                                 |
| 75593                 | 0x140d72350                             | 77401 0x140e4d280 (**checked**) | create the dynamic ring, fences, quad IB and particle VB                      | CreateBuffer x5, CreateQuery(EVENT) x3                                                                           | 0x143025f18[3], 0x143026168[3], 0x143025f38, 0x143025f40                                                    |
| 75595                 | 0x140d72810                             | 77403 0x140e4d5f0 (**hash**)    | `(longlong)`: device QueryInterface                                           | QueryInterface (GUID 0x1417907e8)                                                                                | out                                                                                                         |
| 75596                 | 0x140d728c0                             | 77404 0x140e4d6a0 (**hash**)    | create query                                                                  | CreateQuery                                                                                                      | out                                                                                                         |
| 75608                 | 0x140d73660                             |                                 | `(ID3D11Query** pair)`                                                        | CreateQuery x2                                                                                                   | `pair[0]`, `pair[1]` (start/end timer pair _(inferred)_)                                                    |
| 75490 / 75491         | (R1)                                    | 77277 / 77278                   | inline layouts                                                                | CreateInputLayout                                                                                                | 0x143025f48 / 0x143025f50                                                                                   |
| (state objects)       | 75827-75831                             | 77649-77653                     | R2 table                                                                      | CreateBlendState, CreateDepthStencilState, CreateRasterizerState, CreateSamplerState through `RendererData+0x38` | the R2 tables                                                                                               |

**Scaleform has its own D3D11 path.** `BSGraphics::Renderer::GetScaleformRendererParameters` (SE 75576
`0x140d70400`, AE 77378) returns Renderer+0x48/+0x50 (device and context). FUN_140edb730 and FUN_140edb8a0
pass them to the Scaleform HAL init (vtable +0x1a8). The UI then issues D3D11 calls directly, outside
BSGraphics and SetDirtyStates.

---

### Open questions

1. Identity of the list Main::RenderDepth loops over with stencil ref = i+1 (`DAT_1431d0fa8`, count
   `DAT_1431d0fb8`, gated by `DAT_141e0de43`), and which later pass reads those IDs. Likely candidates are
   the per-object imagespace masks.
2. Exact meaning of `BSRenderPass::accumulationHint` 10 (stencil fade, mode 11) and 12 (the
   alternative-index-buffer and depth path). The mapping to BSShaderAccumulator render modes is not
   proven.
3. The bit 0x10 = fill and 0x1000 = scissor assignment is inferred from the raster index order.
   CommonLib calls them UNKNOWN1 and UNKNOWN2.
4. Format 0x20 for 75490's TEXCOORD1 (R8G8B8A8_SINT) is unusual. Check the shader's input signature.
5. The PS size-class pools (0x143027c00, 0x143027ca0, 0x143027d40) are mapped to tech, material and
   geometry by symmetry only. 75582 `0x140d70e60` reads 0x143027c08 and is presumably the PS serializer.
   Confirm.
6. AE twins not matched by hash: InitD3D 77649 (by xref), raster 77652, samplers 77653 (by xref), Init
   ~77635, device creation ~77396, 75546 ~77346. Also the AE addresses of the size-class pools and the
   custom-CB ring (only the AE alpha, compute, per-frame and type-7 CBs were confirmed).
7. The BSShaderResourceManager interface: which external subsystems call DrawTriShape and
   DrawDynamicTriShape through its vtable thunks (`0x141327fd0`/`0x141327ff0`), and whether 75500 is
   reachable in normal play.
8. Threading. All `ctx` use in these functions is assumed to be on the render (main) thread; nothing
   here takes a lock. Shader Setup* functions keep the current technique in **TLS** (`gs:[0x58][tlsIndex]
    - 0x9f0`), and 75483 uses a TLS static-init guard (+0x2a08), so some per-thread render state exists.
      Not checked: whether any job thread calls Map, or the texture creators (75511, 75513, 75514) off the
      render thread (save-screenshot and texture loading are candidates).
9. AE adds a direct DrawTriShape caller (FUN_141482940, ID 453509, an ImageSpaceManager fullscreen quad).
   Confirm that SE reaches the equivalent through the dispatcher or an inline path.

## RE lane 2: ImageSpace, Scaleform, Bink (R4, R5, R10)

Addresses are image addresses; IDs are Address Library IDs. _(inferred)_ marks a conclusion not checked against every
call path. RT names are CommonLib `RENDER_TARGET` SE indices. AE has 116 render targets against SE's 114; the AE
constants checked so far match SE, so the two extra are probably appended _(inferred)_: check AE indices before use.

### R4 ImageSpaceManager

| What                                    | SE ID  | SE addr     | AE ID             | AE addr     | Notes                                                                                                                                                                                                                            |
| --------------------------------------- | ------ | ----------- | ----------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ctor                                    | -      | 0x141296ed0 | -                 | -           | 0x220 bytes. Effects in a `NiTPrimitiveArray` at +0x20 (data +0x28), capacity 159.                                                                                                                                               |
| Init                                    | -      | 0x1412970a0 | -                 | -           | CreateEffects, then the full-screen quad (+0x48, built by 0x14129d650), then Setup. Sets +0x5C = kMAIN and +0x60 = kMAIN_COPY (2), the ping-pong partner.                                                                        |
| CreateEffects                           | 99045  | 0x141298480 | 105696            | 0x141482da0 | Creates all 159 slots plus the helper structs at +0x168..+0x208. Many AE ctors are inlined.                                                                                                                                      |
| SetupEffects                            | 99021  | 0x141297340 | 105672            | 0x141481ab0 | Calls Setup (vfunc 2) on slots 0..33 only. Each composite wires its own child slots.                                                                                                                                             |
| CountActiveEOFEffects                   | 99049  | 0x14129d8f0 | inlined in 105674 | -           | Scans slots 0..7. +0x3C active count, +0x40 last active index (-1 if none). Gated by `gEnableEOFImageSpace` (SE 0x141e0e184, console `ToggleEOFImageSpace`).                                                                     |
| RenderEndOfFrameEffects(src, dst, flag) | 99023  | 0x141297410 | 105674            | 0x141481b80 | The end-of-frame chain (below). CS hooks the tonemap call at +0x1EA/+0x230 (SE) and +0x178 (AE).                                                                                                                                 |
| RenderPreTonemapComposites(src, dst)    | 99024  | 0x141297910 | 105675            | 0x141481f60 | SIL composite (+0x1d8), bloom/downsample/IB lens flares (+0x180), VL/lens-flare composite (+0x190); ISCopy back to `dst` if needed.                                                                                              |
| RenderEffect(mgr, idx, src, dst, param) | 99025  | 0x1412979e0 | 105676            | 0x141481ff0 | Calls RenderEffectImpl.                                                                                                                                                                                                          |
| RenderEffectImpl                        | -      | 0x141297b40 | inlined           | -           | `SetTexture(eff, 0, dst)`, `SetTexture(eff, 1, src)`, `Render(quad)`. Quad +0x50 with dynamic resolution (+0x58), else +0x48. Texture slot 0 is the output; slots 1 and up are inputs.                                           |
| RenderWorldPostProcess(dst)             | 100430 | 0x1412e49a0 | ?                 | -           | Tail of RenderPlayerView (35560) when `DAT_1431d0e36 && gEnableEOFImageSpace`. Unbinds RT0-3 and the DSV; VL: ApplyVL (+0x170) and the VL CS blur (+0x188); lens flares (100274), `99024(kMAIN, kMAIN)`, `99023(kMAIN, dst, 1)`. |
| Menu 3D render                          | 51855  | 0x1408d2390 | ?                 | -           | Clears with SimpleColor (+0x1e0). With an override base data (+0xb0): `99023(kMAIN, kFRAMEBUFFER, 0)`; otherwise TAA, then ISCopy.                                                                                               |

**End-of-frame chain (99023), in order.** Each active effect renders `src` into the other target of the pair (kMAIN and
kMAIN_COPY), then the pair swaps.

1. Active slots in 0..7: WorldMap 0, Refraction 1, FXAA 2, DOF 3, (4 empty), RadialBlur 5, FullScreenBlur 6, GetHit 7.
   When `usesLDR` (+0x210) is set, slots 3..7 wait until after the tonemap.
2. Tonemap: HDR (slot 30) if the HDR flag is set (SE `DAT_141e0eb24` / AE `DAT_142033d84`, toggled by the console function
   at 0x14031ed50), else ISCopy (34). It writes `dst` directly when nothing follows; otherwise kIMAGESPACE_TEMP_COPY (0x29).
   "Something follows" means TAA is on, a deferred LDR effect is active, or the dynamic-resolution scale is not 1.0.
3. TAA (helper +0x1f0, 100540 at 0x1412eeb90, shaders 151-153): TEMP_COPY to `dst` or kIMAGESPACE_TEMP_COPY2 (0x2a). When
   `dst` is kMENUBG it sets +0x30 = 1.
4. If +0x211 is set: ISCopy into kSAVE_GAME_SCREENSHOT (0x4b); the frame counter goes to +0x218.
5. LDR post slots 3..7, ping-ponging TEMP_COPY and TEMP_COPY2.
6. Dynamic-resolution upsample (helper +0x1f8, 100548 at 0x1412f03f0, shader 154) to `dst`.

**How composites are wired.** The generic `ImageSpaceEffect::Render` (0x1412fc000) walks the children. Each child has an
output index and input indices into the effect's texture list: 0 the effect's output, 1 its source, 2 and up borrowed
textures. Helpers: AddChild 0x1412fc2b0, set one input 0x1412fc3d0, set two inputs 0x1412fc470.

| Slot | Effect (Render / Setup, SE)                   | Children (slot)                                                                                                           | Textures and RTs                                                                                                |
| ---- | --------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 0    | WorldMap (generic / 0x141311ad0)              | 43, or 44 NoSkyBlur when `DAT_141e106d0`                                                                                  | (src, depth) to out                                                                                             |
| 1    | Refraction (0x14130d4c0 / 0x14130d350)        | 39                                                                                                                        | (src, kREFRACTION_NORMALS) to out                                                                               |
| 3    | DepthOfField (0x1412f90d0 / 0x1412f9410)      | DownSample4 58; Blur7 composite 11, or the CS blur helper +0x188 (112/113) when `DAT_141e10448`; then one of 45 / 46 / 47 | kHDR_DOWNSAMPLE0 (kLDR_DOWNSAMPLE0 under LDR), swap kHDR_BLURSWAP / kLDR_BLURSWAP, main depth, kUNDERWATER_MASK |
| 5    | RadialBlur (generic / 0x14130d720)            | 52 / 53 / 54 by quality `DAT_141e10650`                                                                                   | src to out                                                                                                      |
| 6    | FullScreenBlur (0x14132a370 / 0x141329df0)    | 58, Blur composites 9-15, 58, ISCopy 34                                                                                   | kHDR_DOWNSAMPLE0, kHDR_BLURSWAP                                                                                 |
| 7    | GetHit (generic / 0x141328b80)                | ISCopy 34 to kGETHIT_BUFFER; Blur5 composite 10 twice (kGETHIT_BLURSWAP); DoubleVision 40                                 | (src, blurred) to out                                                                                           |
| 8    | Map (generic / 0x14132a9c0)                   | ISMap 42                                                                                                                  | src to out; drawn by the map menu _(inferred)_                                                                  |
| 9-29 | Blur (generic / 0x141329260)                  | Blur 65-71, NonHDR 72-78 or BrightPass 79-85, H then V                                                                    | src, temp, out; BrightPass also reads average luminance                                                         |
| 31   | WaterDisplacement (0x141303da0 / 0x1413035c0) | 86-94                                                                                                                     | via 99025 from 31385 (water system)                                                                             |
| 32   | VolumetricLighting (generic / 0x1412f86b0)    | 97                                                                                                                        | (depth, DS slot 2) to kVOLUMETRIC_LIGHTING_HALF_RES / _QUARTER_RES; created only when `DAT_141e10400`           |
| 33   | Noise                                         | 95, 96                                                                                                                    | not traced                                                                                                      |

**HDR (slot 30) Render:**

1. DownSample4 (58): scene to kHDR_DOWNSAMPLE0 (26).
2. Luminance: 4RGB2Lum (60), then 16Lum (59) passes, each 4x smaller, into kHDR_DOWNSAMPLE1 and up. The final pair
   alternates (`_DAT_143233530`) so last frame's value survives.
3. If a previous value exists (`DAT_141e0eb50 >= 0`), 16LightAdapt (64) adapts it; the result persists in the
   `ImageSpaceTexture` at `DAT_141e0eb48`.
4. Bloom: BrightPassBlur composite at slot 23 + clamp(radius - 1, 0, 6), radius from ImageSpaceData +0xcc. Inputs:
   downsample0, kHDR_BLURSWAP, luminance. Output kHDR_BLOOM (24).
5. Tonemap: 55, or 56 (Fade) when mgr+0x118 > 0. Inputs bloom, scene, luminance.

Slots 57, 61 and 63 are created but not used by this Render _(inferred)_.

**Helper structs on the manager** (driven by the frame code, not the effect loop):

| Field  | Ctor (SE)   | Slots                            | Render entry (SE ID / addr) and caller                                                             |
| ------ | ----------- | -------------------------------- | -------------------------------------------------------------------------------------------------- |
| +0x168 | 0x1412e7b80 | 102 ApplyReflections             | not traced                                                                                         |
| +0x170 | 0x1412e7d90 | 103                              | 100475 / 0x1412e7db0, from 100430                                                                  |
| +0x178 | 0x1412e8270 | 104 BasicCopy                    | not traced                                                                                         |
| +0x180 | 0x1412e82f0 | 105, 119-122, 124                | 100481 / 0x1412e8470, from 99024; kIBLENSFLARES_* RTs                                              |
| +0x188 | 0x1412ea1a0 | 106-113 (CS blurs)               | 100486 / 0x1412ea290 (VL), 100488 / 0x1412ea340 (DOF)                                              |
| +0x190 | 0x1412ea8e0 | 114-116                          | 100493 / 0x1412ea900, from 99024                                                                   |
| +0x198 | 0x1412eabc0 | 117                              | RenderWorld 100424 (snow: kMAIN to kSNOW_SWAP) and RenderWaterEffects                              |
| +0x1a0 | 0x1412eade0 | 118                              | debug only                                                                                         |
| +0x1a8 | 0x1412eaf20 | 123                              | -                                                                                                  |
| +0x1b0 | 0x1412eb050 | 125-127                          | unused in vanilla                                                                                  |
| +0x1b8 | 0x1412eb4c0 | 128-129                          | -                                                                                                  |
| +0x1c0 | 0x1412eb6e0 | 130-131                          | -                                                                                                  |
| +0x1c8 | 0x1412ebc50 | SAO: 132, 133, 136, 138-144      | `RenderSSAO` 100520 from RenderWorld 100424: (kMAIN, kMAIN), and (kSNOW_SWAP, kMAIN) with snow SSS |
| +0x1d0 | 0x1412ed420 | 134, 135, 137, 145 (SAO compute) | 100526 / 0x1412ed610, from 35560                                                                   |
| +0x1d8 | 0x1412edba0 | 146 / 147                        | 100528 / 0x1412edd30, from 99024                                                                   |
| +0x1e0 | 0x1412ee460 | 148 / 149                        | 100531, from 51855                                                                                 |
| +0x1e8 | 0x1412ee8a0 | 150 SnowSSS                      | from RenderWorld when `DAT_141e0e2d0`                                                              |
| +0x1f0 | 0x1412eeb20 | 151-153 TAA                      | 100540                                                                                             |
| +0x1f8 | 0x1412f0360 | 154                              | 100548                                                                                             |
| +0x200 | 0x1412f0590 | 155 WaterBlend                   | not traced                                                                                         |
| +0x208 | 0x1412f0b90 | 156 UnderwaterMask               | not traced                                                                                         |

**Other 99025 call sites:**

| Caller (SE ID) | Effect               | src to dst                                                          |
| -------------- | -------------------- | ------------------------------------------------------------------- |
| 16094          | 98 LocalMap          | kLOCAL_MAP_SWAP to kLOCAL_MAP                                       |
| 31385          | 31 WaterDisplacement | in place                                                            |
| 35556          | ISCopy               | to kFRAMEBUFFER; kFRAMEBUFFER to kSCREENSHOT; kFRAMEBUFFER to kMAIN |
| 79947          | ISCopy               | kMAIN to kFRAMEBUFFER                                               |
| 100412         | ISCopy               | kMENUBG to kFRAMEBUFFER (menu background)                           |
| 100524         | ISCopy               | 0x2e+n to dst                                                       |
| 51855          | ISCopy               | TAA output to kFRAMEBUFFER                                          |

**Every effect.** Kind is compute when the ctor sets +0x1A0 = 0x101. Source file is +0x128 (`originalShaderName`); CS
loads `Data/Shaders/<name>.hlsl`. CS status: **active** = the +0x120 name is in CS's uncommented descriptor map
(`GetImagespaceShaderDescriptor`) and the file exists in package/Shaders; **src-unmapped** = the file exists but the map
entry is commented out, so vanilla DXBC runs; **DXBC only** = CS has no source. Totals: 39 active, 68 src-unmapped,
**17 DXBC only** (slots 2, 100, 117, 121, 122, 125-129, 134, 135, 137, 143-145, 147), 32 composites, 3 empty. Of the
DXBC-only ones, a normal vanilla frame reaches FXAA (when enabled), the SAO raw AO and blur paths, CopySubRegionCS (snow
and water) and the lens-flare draw _(inferred)_.

| #   | Enum                                   | Shader/effect name (+0x120)                              | Source file (+0x128)                   | SE ctor     | SE ctor ID | SE vtable                          | AE ctor (ID)         | Kind                              | CS status                                        |
| --- | -------------------------------------- | -------------------------------------------------------- | -------------------------------------- | ----------- | ---------- | ---------------------------------- | -------------------- | --------------------------------- | ------------------------------------------------ |
| 0   | WorldMap                               | ImageSpaceEffectWorldMap                                 |                                        | 0x141311a30 | 101015     | 0x141868978                        | 0x1414fdad0 (107799) | composite (ImageSpaceEffect)      | n/a                                              |
| 1   | Refraction                             | ImageSpaceEffectRefraction                               |                                        | 0x14130d250 | 100959     | 0x141868290                        | 0x1414f8f20 (107741) | composite (ImageSpaceEffect)      | n/a                                              |
| 2   | ISFXAA                                 | BSImagespaceShaderFXAA                                   | ISFXAA                                 | 0x1412a3680 | 99077      | 0x141853788                        | 0x14148d420 (105724) | raster                            | DXBC only                                        |
| 3   | DepthOfField                           | ImageSpaceEffectDepthOfField                             |                                        | 0x1412f8e10 | 100620     | 0x141865ad0                        | 0x1414e3f80 (107383) | composite (ImageSpaceEffect)      | n/a                                              |
| 4   | (empty)                                | -                                                        | -                                      | -           | -          | -                                  | -                    | -                                 | -                                                |
| 5   | RadialBlur                             | ImageSpaceEffectRadialBlur                               |                                        | 0x14130d660 | 100966     | 0x1418683d8                        | 0x1414f95b0 (107748) | composite (ImageSpaceEffect)      | n/a                                              |
| 6   | FullScreenBlur                         | ImageSpaceEffectFullScreenBlur                           |                                        | 0x141329c50 | 101561     | 0x14186c2e0                        | 0x141516f80 (108559) | composite (ImageSpaceEffect)      | n/a                                              |
| 7   | GetHit                                 | ImageSpaceEffectGetHit                                   |                                        | 0x141328970 | 101549     | 0x14186c150                        | 0x141515ba0 (108547) | composite (ImageSpaceEffect)      | n/a                                              |
| 8   | Map                                    | ImageSpaceEffectMap                                      |                                        | 0x14132a920 | 101574     | 0x14186c360                        | 0x141517d80 (108572) | composite (ImageSpaceEffect)      | n/a                                              |
| 9   | Blur3                                  | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 10  | Blur5                                  | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 11  | Blur7                                  | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 12  | Blur9                                  | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 13  | Blur11                                 | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 14  | Blur13                                 | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 15  | Blur15                                 | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 16  | BlurNonHDR3                            | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 17  | BlurNonHDR5                            | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 18  | BlurNonHDR7                            | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 19  | BlurNonHDR9                            | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 20  | BlurNonHDR11                           | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 21  | BlurNonHDR13                           | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 22  | BlurNonHDR15                           | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 23  | BlurBrightPass3                        | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 24  | BlurBrightPass5                        | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 25  | BlurBrightPass7                        | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 26  | BlurBrightPass9                        | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 27  | BlurBrightPass11                       | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 28  | BlurBrightPass13                       | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 29  | BlurBrightPass15                       | ImageSpaceEffectBlur                                     |                                        | 0x141329140 | 101557     | 0x14186c270                        | 0x141516460 (108555) | composite (2-pass separable blur) | n/a                                              |
| 30  | HDR                                    | ImageSpaceEffectHDR                                      |                                        | 0x1412e68a0 | 100462     | 0x141861e90                        | 0x1414d0540 (107180) | composite (ImageSpaceEffect)      | n/a                                              |
| 31  | WaterDisplacement                      | ImageSpaceEffectWaterDisplacement                        |                                        | 0x141303420 | 100796     | 0x141866f38                        | 0x1414ee9c0 (107579) | composite (ImageSpaceEffect)      | n/a                                              |
| 32  | VolumetricLighting                     | ImageSpaceEffectVolumetricLighting                       |                                        | 0x1412f85f0 | 100613     | 0x141865578                        | 0x1414e34f0 (107375) | composite (ImageSpaceEffect)      | n/a                                              |
| 33  | Noise                                  | ImageSpaceEffectNoise                                    |                                        | 0x14132abd0 | 101580     | (not resolved)                     | 0x141518280 (108578) | composite (ImageSpaceEffect)      | n/a                                              |
| 34  | ISCopy                                 | BSImagespaceShaderCopy                                   | ISCopy                                 | 0x1412a1b00 | 99066      | 0x141852678                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 35  | ISCopyDynamicFetchDisabled             | BSImagespaceShaderCopyDynamicFetchDisabled               | ISCopy                                 | 0x1412a1d00 | 99068      | 0x141852798                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 36  | ISCopyScaleBias                        | BSImagespaceShaderCopyScaleBias                          | ISCopy                                 | 0x1412a1e00 | 99069      | 0x1418528f8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 37  | ISCopyCustomViewport                   | BSImagespaceShaderCopyCustomViewport                     | ISCopy                                 | 0x1412a1bf0 | 99067      | 0x141852ce8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 38  | ISCopyGrayScale                        | BSImagespaceShaderGreyScale                              | ISCopy                                 | 0x1412a3ab0 | 99078      | 0x141852b98                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 39  | ISRefraction                           | BSImagespaceShaderRefraction                             | ISRefraction                           | 0x1412b1a80 | 99157      | 0x141855288                        | 0x141499490 (105791) | raster                            | HLSL (active)                                    |
| 40  | ISDoubleVision                         | BSImagespaceShaderDoubleVision                           | ISDoubleVision                         | 0x1412a3480 | 99076      | 0x141853628                        | 0x14148d240 (105723) | raster                            | HLSL (active)                                    |
| 41  | ISCopyTextureMask                      | BSImagespaceShaderTextureMask                            | ISCopy                                 | 0x1412b1c50 | 99158      | 0x141852a48                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 42  | ISMap                                  | BSImagespaceShaderMap                                    | ISMap                                  | 0x1412aead0 | 99140      | 0x1418547c0                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 43  | ISWorldMap                             | BSImagespaceShaderWorldMap                               | ISWorldMap                             | 0x1412b39d0 | 99170      | 0x1418548f0                        | 0x14149ad60 (105800) | raster                            | HLSL file, not in descriptor map                 |
| 44  | ISWorldMapNoSkyBlur                    | BSImagespaceShaderWorldMapNoSkyBlur                      | ISWorldMap                             | 0x1412b3c10 | 99171      | 0x141854a50                        | 0x14149af80 (105801) | raster                            | HLSL file, not in descriptor map                 |
| 45  | ISDepthOfField                         | BSImagespaceShaderDepthOfField                           | ISDepthOfField                         | 0x1412a1f50 | 99070      | 0x141852e28                        | 0x14148bf00 (105717) | raster                            | HLSL (active)                                    |
| 46  | ISDepthOfFieldFogged                   | BSImagespaceShaderDepthOfFieldFogged                     | ISDepthOfField                         | 0x1412a2240 | 99071      | 0x141852f98                        | 0x14148c1b0 (105718) | raster                            | HLSL (active)                                    |
| 47  | ISDepthOfFieldMaskedFogged             | BSImagespaceShaderDepthOfFieldMaskedFogged               | ISDepthOfField                         | 0x1412a25b0 | 99072      | 0x1418530f0                        | 0x14148c4d0 (105719) | raster                            | HLSL (active)                                    |
| 48  | (empty)                                | -                                                        | -                                      | -           | -          | -                                  | -                    | -                                 | -                                                |
| 49  | ISDistantBlur                          | BSImagespaceShaderDistantBlur                            | ISDepthOfField                         | 0x1412a29e0 | 99073      | 0x141853260                        | 0x14148c890 (105720) | raster                            | HLSL (active)                                    |
| 50  | ISDistantBlurFogged                    | BSImagespaceShaderDistantBlurFogged                      | ISDepthOfField                         | 0x1412a2ce0 | 99074      | 0x141853398                        | 0x14148cb50 (105721) | raster                            | HLSL (active)                                    |
| 51  | ISDistantBlurMaskedFogged              | BSImagespaceShaderDistantBlurMaskedFogged                | ISDepthOfField                         | 0x1412a3050 | 99075      | 0x1418534d8                        | 0x14148ce70 (105722) | raster                            | HLSL (active)                                    |
| 52  | ISRadialBlur                           | BSImagespaceShaderRadialBlur                             | ISRadialBlur                           | 0x1412b0ba0 | 99150      | 0x141854eb8                        | 0x141498660 (105784) | raster                            | HLSL (active)                                    |
| 53  | ISRadialBlurMedium                     | BSImagespaceShaderRadialBlurMedium                       | ISRadialBlur                           | 0x1412b0f20 | 99152      | 0x141855008                        | 0x1414989c0 (105786) | raster                            | HLSL (active)                                    |
| 54  | ISRadialBlurHigh                       | BSImagespaceShaderRadialBlurHigh                         | ISRadialBlur                           | 0x1412b0d60 | 99151      | 0x141855148                        | 0x141498810 (105785) | raster                            | HLSL (active)                                    |
| 55  | ISHDRTonemapBlendCinematic             | BSImagespaceShaderHDRTonemapBlendCinematic               | ISHDR                                  | 0x1412a5f30 | 99087      | 0x1418543a8                        | 0x14148f7e0 (105733) | raster                            | HLSL (active)                                    |
| 56  | ISHDRTonemapBlendCinematicFade         | BSImagespaceShaderHDRTonemapBlendCinematicFade           | ISHDR                                  | 0x1412a61f0 | 99088      | 0x141854520                        | 0x14148fa60 (105734) | raster                            | HLSL (active)                                    |
| 57  | ISHDRDownSample16                      | BSImagespaceShaderHDRDownSample16                        | ISHDR                                  | 0x1412a3c00 | 99079      | 0x141853ce8                        | 0x14148d820 (105725) | raster                            | HLSL (active)                                    |
| 58  | ISHDRDownSample4                       | BSImagespaceShaderHDRDownSample4                         | ISHDR                                  | 0x1412a5340 | 99083      | 0x1418538f0                        | 0x14148ecf0 (105729) | raster                            | HLSL (active)                                    |
| 59  | ISHDRDownSample16Lum                   | BSImagespaceShaderHDRDownSample16Lum                     | ISHDR                                  | 0x1412a47e0 | 99081      | 0x141853a48                        | 0x14148e2c0 (105727) | raster                            | HLSL (active)                                    |
| 60  | ISHDRDownSample4RGB2Lum                | BSImagespaceShaderHDRDownSample4RGB2Lum                  | ISHDR                                  | 0x1412a5c70 | 99086      | 0x141853ba0                        | 0x14148f560 (105732) | raster                            | HLSL (active)                                    |
| 61  | ISHDRDownSample4LumClamp               | BSImagespaceShaderHDRDownSample4LumClamp                 | ISHDR                                  | 0x1412a5970 | 99085      | 0x141853e28                        | 0x14148f2a0 (105731) | raster                            | HLSL (active)                                    |
| 62  | ISHDRDownSample4LightAdapt             | BSImagespaceShaderHDRDownSample4LightAdapt               | ISHDR                                  | 0x1412a5600 | 99084      | 0x1418540e0                        | 0x14148ef80 (105730) | raster                            | HLSL (active)                                    |
| 63  | ISHDRDownSample16LumClamp              | BSImagespaceShaderHDRDownSample16LumClamp                | ISHDR                                  | 0x1412a4d70 | 99082      | 0x141853f90                        | 0x14148e7c0 (105728) | raster                            | HLSL (active)                                    |
| 64  | ISHDRDownSample16LightAdapt            | BSImagespaceShaderHDRDownSample16LightAdapt              | ISHDR                                  | 0x1412a4190 | 99080      | 0x141854258                        | 0x14148dd20 (105726) | raster                            | HLSL (active)                                    |
| 65  | ISBlur3                                | BSImagespaceShaderBlur3                                  | ISBlur                                 | 0x14129f200 | 99055      | 0x141850be8                        | 0x1414899b0 (105706) | raster                            | HLSL file, not in descriptor map                 |
| 66  | ISBlur5                                | BSImagespaceShaderBlur5                                  | ISBlur                                 | 0x14129f440 | 99056      | 0x141850d70                        | 0x141489be0 (105707) | raster                            | HLSL file, not in descriptor map                 |
| 67  | ISBlur7                                | BSImagespaceShaderBlur7                                  | ISBlur                                 | 0x14129f700 | 99057      | 0x141850eb0                        | 0x141489e60 (105708) | raster                            | HLSL file, not in descriptor map                 |
| 68  | ISBlur9                                | BSImagespaceShaderBlur9                                  | ISBlur                                 | 0x14129fa30 | 99058      | 0x141850ff0                        | 0x14148a150 (105709) | raster                            | HLSL file, not in descriptor map                 |
| 69  | ISBlur11                               | BSImagespaceShaderBlur11                                 | ISBlur                                 | 0x14129e410 | 99052      | 0x141851138                        | 0x141488d20 (105703) | raster                            | HLSL file, not in descriptor map                 |
| 70  | ISBlur13                               | BSImagespaceShaderBlur13                                 | ISBlur                                 | 0x14129e840 | 99053      | 0x141851290                        | 0x1414890e0 (105704) | raster                            | HLSL file, not in descriptor map                 |
| 71  | ISBlur15                               | BSImagespaceShaderBlur15                                 | ISBlur                                 | 0x14129ece0 | 99054      | 0x1418513e8                        | 0x141489510 (105705) | raster                            | HLSL file, not in descriptor map                 |
| 72  | ISNonHDRBlur3                          | BSImageSpaceShaderNonHDRBlur3                            | ISBlur                                 | 0x1412affc0 | 99146      | 0x141851540                        | 0x141497b50 (105780) | raster                            | HLSL file, not in descriptor map                 |
| 73  | ISNonHDRBlur5                          | BSImageSpaceShaderNonHDRBlur5                            | ISBlur                                 | 0x1412b0200 | 99147      | 0x141851670                        | 0x141497d80 (105781) | raster                            | HLSL file, not in descriptor map                 |
| 74  | ISNonHDRBlur7                          | BSImageSpaceShaderNonHDRBlur7                            | ISBlur                                 | 0x1412b04c0 | 99148      | 0x1418517a0                        | 0x141498010 (105782) | raster                            | HLSL file, not in descriptor map                 |
| 75  | ISNonHDRBlur9                          | BSImageSpaceShaderNonHDRBlur9                            | ISBlur                                 | 0x1412b07f0 | 99149      | 0x1418518d0                        | 0x141498300 (105783) | raster                            | HLSL file, not in descriptor map                 |
| 76  | ISNonHDRBlur11                         | BSImageSpaceShaderNonHDRBlur11                           | ISBlur                                 | 0x1412af1d0 | 99143      | 0x141851a00                        | 0x141496ed0 (105777) | raster                            | HLSL file, not in descriptor map                 |
| 77  | ISNonHDRBlur13                         | BSImageSpaceShaderNonHDRBlur13                           | ISBlur                                 | 0x1412af600 | 99144      | 0x141851b30                        | 0x141497290 (105778) | raster                            | HLSL file, not in descriptor map                 |
| 78  | ISNonHDRBlur15                         | BSImageSpaceShaderNonHDRBlur15                           | ISBlur                                 | 0x1412afaa0 | 99145      | 0x141851c60                        | 0x1414976c0 (105779) | raster                            | HLSL file, not in descriptor map                 |
| 79  | ISBrightPassBlur3                      | BSImagespaceShaderBrightPassBlur3                        | ISBlur                                 | 0x1412a0d30 | 99062      | 0x141851d90                        | 0x14148b270 (105713) | raster                            | HLSL file, not in descriptor map                 |
| 80  | ISBrightPassBlur5                      | BSImagespaceShaderBrightPassBlur5                        | ISBlur                                 | 0x1412a0ff0 | 99063      | 0x141851ef8                        | 0x14148b4f0 (105714) | raster                            | HLSL file, not in descriptor map                 |
| 81  | ISBrightPassBlur7                      | BSImagespaceShaderBrightPassBlur7                        | ISBlur                                 | 0x1412a1320 | 99064      | 0x141852038                        | 0x14148b7e0 (105715) | raster                            | HLSL file, not in descriptor map                 |
| 82  | ISBrightPassBlur9                      | BSImagespaceShaderBrightPassBlur9                        | ISBlur                                 | 0x1412a16d0 | 99065      | 0x141852178                        | 0x14148bb40 (105716) | raster                            | HLSL file, not in descriptor map                 |
| 83  | ISBrightPassBlur11                     | BSImagespaceShaderBrightPassBlur11                       | ISBlur                                 | 0x14129fde0 | 99059      | 0x1418522b8                        | 0x14148a4b0 (105710) | raster                            | HLSL file, not in descriptor map                 |
| 84  | ISBrightPassBlur13                     | BSImagespaceShaderBrightPassBlur13                       | ISBlur                                 | 0x1412a0280 | 99060      | 0x1418523f8                        | 0x14148a8e0 (105711) | raster                            | HLSL file, not in descriptor map                 |
| 85  | ISBrightPassBlur15                     | BSImagespaceShaderBrightPassBlur15                       | ISBlur                                 | 0x1412a07a0 | 99061      | 0x141852538                        | 0x14148ad70 (105712) | raster                            | HLSL file, not in descriptor map                 |
| 86  | ISWaterDisplacementClearSimulation     | BSISWaterDisplacementClearSimulation                     | ISWaterDisplacement                    | 0x1412b25e0 | 99161      | 0x1418553b8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 87  | ISWaterDisplacementTexOffset           | BSISWaterDisplacementTexOffset                           | ISWaterDisplacement                    | 0x1412b29e0 | 99164      | 0x141855538                        | 0x14149a040 (105795) | raster                            | HLSL file, not in descriptor map                 |
| 88  | ISWaterDisplacementWadingRipple        | BSISWaterDisplacementWadingRipple                        | ISWaterDisplacement                    | 0x1412b2ca0 | 99165      | 0x1418556e8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 89  | ISWaterDisplacementRainRipple          | BSISWaterDisplacementRainRipple                          | ISWaterDisplacement                    | 0x1412b2940 | 99163      | 0x141855878                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 90  | ISWaterWadingHeightmap                 | BSISWaterWadingHeightmap                                 | ISWaterDisplacement                    | 0x1412b3710 | 99169      | 0x1418559d0                        | 0x14149aae0 (105799) | raster                            | HLSL file, not in descriptor map                 |
| 91  | ISWaterRainHeightmap                   | BSISWaterRainHeightmap                                   | ISWaterDisplacement                    | 0x1412b3190 | 99167      | 0x141855b30                        | 0x14149a5e0 (105797) | raster                            | HLSL file, not in descriptor map                 |
| 92  | ISWaterBlendHeightmaps                 | BSISWaterBlendHeightmaps                                 | ISWaterDisplacement                    | 0x1412b22f0 | 99160      | 0x141855c70                        | 0x141499b10 (105793) | raster                            | HLSL file, not in descriptor map                 |
| 93  | ISWaterSmoothHeightmap                 | BSISWaterSmoothHeightmap                                 | ISWaterDisplacement                    | 0x1412b3450 | 99168      | 0x141855de0                        | 0x14149a860 (105798) | raster                            | HLSL file, not in descriptor map                 |
| 94  | ISWaterDisplacementNormals             | BSISWaterDisplacementNormals                             | ISWaterDisplacement                    | 0x1412b2680 | 99162      | 0x141855f30                        | 0x141499dc0 (105794) | raster                            | HLSL file, not in descriptor map                 |
| 95  | ISNoiseScrollAndBlend                  | BSImagespaceShaderNoiseScrollAndBlend                    | ISNoise                                | 0x1412aef10 | 99142      | 0x141854b98                        | 0x141496c50 (105776) | raster                            | HLSL file, not in descriptor map                 |
| 96  | ISNoiseNormalmap                       | BSImagespaceShaderNoiseNormalmap                         | ISNoise                                | 0x1412aec50 | 99141      | 0x141854d68                        | 0x1414969d0 (105775) | raster                            | HLSL file, not in descriptor map                 |
| 97  | ISVolumetricLighting                   | BSImageSpaceShaderVolumetricLighting                     | ISVolumetricLighting                   | 0x1412b1da0 | 99159      | 0x141856088                        | 0x141499650 (105792) | raster                            | HLSL (active)                                    |
| 98  | ISLocalMap                             | BSImagespaceShaderLocalMap                               | ISLocalMap                             | 0x1412ae950 | 99139      | 0x141854680                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 99  | ISAlphaBlend                           | BSImagespaceShaderAlphaBlend                             | ISAlphaBlend                           | 0x14129d960 | 99050      | 0x1418508c8                        | 0x141488380 (105701) | raster                            | HLSL file, not in descriptor map                 |
| 100 | ISLensFlare                            | BSLensFlare                                              | (null)                                 | 0x1412d9570 | 100273     | VTABLE_BSImagespaceShaderLensFlare | inlined in AE 105696 | raster                            | DXBC only (no originalShaderName; not in CS map) |
| 101 | ISLensFlareVisibility                  | BGSLensFlareVisibilityPass                               | LensFlare                              | 0x1412daf70 | 100284     | VTABLE_BGSLensFlareVisibilityPass  | inlined in AE 105696 | raster                            | HLSL (active)                                    |
| 102 | ISApplyReflections                     | BSImagespaceShaderApplyReflections                       | ISApplyReflections                     | 0x14129e210 | 99051      | 0x1418564c0                        | 0x141488b30 (105702) | raster                            | HLSL file, not in descriptor map                 |
| 103 | ISApplyVolumetricLighting              | BSImagespaceShaderISApplyVolumetricLighting              | ISApplyVolumetricLighting              | 0x1412a64f0 | 99089      | 0x141856658                        | 0x14148fd20 (105735) | raster                            | HLSL (active)                                    |
| 104 | ISBasicCopy                            | BSImagespaceShaderISBasicCopy                            | ISBasicCopy                            | 0x1412a6770 | 99090      | 0x141856808                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 105 | ISBlur                                 | BSImagespaceShaderISBlur                                 | ISBlur                                 | 0x1412a68b0 | 99091      | 0x141856938                        | 0x14148ff80 (105736) | raster                            | HLSL file, not in descriptor map                 |
| 106 | ISVolumetricLightingBlurHCS            | BSImagespaceShaderISVolumetricLightingBlurHCS            | ISBlurCS                               | 0x1412ae230 | 99136      | 0x141856aa8                        | 0x141496310 (105772) | compute                           | HLSL file, not in descriptor map                 |
| 107 | ISVolumetricLightingBlurVCS            | BSImagespaceShaderISVolumetricLightingBlurVCS            | ISBlurCS                               | 0x1412ae480 | 99137      | 0x141856cc8                        | 0x141496540 (105773) | compute                           | HLSL file, not in descriptor map                 |
| 108 | ISReflectionBlurHCS                    | BSImagespaceShaderReflectionBlurHCS                      | ISBlurCS                               | 0x1412b10e0 | 99153      | 0x141856e30                        | 0x141498b70 (105787) | compute                           | HLSL (active)                                    |
| 109 | ISReflectionBlurVCS                    | BSImagespaceShaderReflectionBlurVCS                      | ISBlurCS                               | 0x1412b1330 | 99154      | 0x141856f70                        | 0x141498da0 (105788) | compute                           | HLSL (active)                                    |
| 110 | ISParallaxMaskBlurHCS                  | BSImagespaceShaderISParallaxMaskBlurHCS                  | ISBlurCS                               | 0x1412a9ed0 | 99111      | 0x1418570b0                        | 0x141492e40 (105754) | compute                           | HLSL file, not in descriptor map                 |
| 111 | ISParallaxMaskBlurVCS                  | BSImagespaceShaderISParallaxMaskBlurVCS                  | ISBlurCS                               | 0x1412aa120 | 99112      | 0x141857210                        | 0x141493070 (105755) | compute                           | HLSL file, not in descriptor map                 |
| 112 | ISDepthOfFieldBlurHCS                  | BSImagespaceShaderISDepthOfFieldBlurHCS                  | ISBlurCS                               | 0x1412a74f0 | 99097      | 0x141857350                        | 0x1414909b0 (105741) | compute                           | HLSL file, not in descriptor map                 |
| 113 | ISDepthOfFieldBlurVCS                  | BSImagespaceShaderISDepthOfFieldBlurVCS                  | ISBlurCS                               | 0x1412a7740 | 99098      | 0x1418574a0                        | 0x141490be0 (105742) | compute                           | HLSL file, not in descriptor map                 |
| 114 | ISCompositeVolumetricLighting          | BSImagespaceShaderISCompositeVolumetricLighting          | ISCompositeLensFlareVolumetricLighting | 0x1412a6f60 | 99094      | 0x1418575e8                        | 0x1414905d0 (105739) | raster                            | HLSL (active)                                    |
| 115 | ISCompositeLensFlare                   | BSImagespaceShaderISCompositeLensFlare                   | ISCompositeLensFlareVolumetricLighting | 0x1412a6be0 | 99092      | 0x1418577b0                        | 0x141490270 (105737) | raster                            | HLSL (active)                                    |
| 116 | ISCompositeLensFlareVolumetricLighting | BSImagespaceShaderISCompositeLensFlareVolumetricLighting | ISCompositeLensFlareVolumetricLighting | 0x1412a6da0 | 99093      | 0x141857900                        | 0x141490420 (105738) | raster                            | HLSL (active)                                    |
| 117 | ISCopySubRegionCS                      | BSImagespaceShaderISCopySubRegionCS                      | ISCopySubRegionCS                      | 0x1412a7120 | 99095      | 0x141857a40                        | 0x141490780 (105740) | compute                           | DXBC only                                        |
| 118 | ISDebugSnow                            | BSImagespaceShaderISDebugSnow                            | ISDebugSnow                            | 0x1412a7370 | 99096      | 0x141857bb8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 119 | ISDownsample                           | BSImagespaceShaderISDownsample                           | ISDownsample                           | 0x1412a7b50 | 99100      | 0x141857d00                        | 0x141490fc0 (105744) | raster                            | HLSL (active)                                    |
| 120 | ISDownsampleIgnoreBrightest            | BSImagespaceShaderISDownsampleIgnoreBrightest            | ISDownsample                           | 0x1412a8170 | 99102      | 0x141857eb0                        | 0x141491560 (105746) | raster                            | HLSL (active)                                    |
| 121 | ISDownsampleCS                         | BSImagespaceShaderISDownsampleCS                         | ISDownsampleCS                         | 0x1412a7e00 | 99101      | 0x141858010                        | 0x141491240 (105745) | compute                           | DXBC only                                        |
| 122 | ISDownsampleIgnoreBrightestCS          | BSImagespaceShaderISDownsampleIgnoreBrightestCS          | ISDownsampleCS                         | 0x1412a8430 | 99103      | 0x141858190                        | 0x1414917e0 (105747) | compute                           | DXBC only                                        |
| 123 | ISExp                                  | BSImagespaceShaderISExp                                  | ISExp                                  | 0x1412a87b0 | 99104      | 0x1418582e0                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 124 | ISIBLensFlares                         | BSImagespaceShaderISIBLensFlares                         | ISIBLensFlare                          | 0x1412a8930 | 99105      | 0x141858410                        | 0x141491b10 (105748) | raster                            | HLSL file, not in descriptor map                 |
| 125 | ISLightingComposite                    | BSImagespaceShaderISLightingComposite                    | ISLigthingComposite                    | 0x1412a8f00 | 99106      | 0x1418586b0                        | 0x141491fe0 (105749) | raster                            | DXBC only                                        |
| 126 | ISLightingCompositeNoDirectionalLight  | BSImagespaceShaderISLightingCompositeNoDirectionalLight  | ISLigthingComposite                    | 0x1412a95e0 | 99108      | 0x141858888                        | 0x141492620 (105751) | raster                            | DXBC only                                        |
| 127 | ISLightingCompositeMenu                | BSImagespaceShaderISLightingCompositeMenu                | ISLigthingComposite                    | 0x1412a9270 | 99107      | 0x1418589e8                        | 0x141492300 (105750) | raster                            | DXBC only                                        |
| 128 | ISPerlinNoiseCS                        | BSImagespaceShaderISPerlinNoiseCS                        | ISPerlinNoiseCS                        | 0x1412aa4f0 | 99114      | 0x141858b38                        | inlined in AE 105696 | compute                           | DXBC only                                        |
| 129 | ISPerlinNoise2DCS                      | BSImagespaceShaderISPerlinNoise2DCS                      | ISPerlinNoiseCS                        | 0x1412aa370 | 99113      | 0x141858c98                        | inlined in AE 105696 | compute                           | DXBC only                                        |
| 130 | ISReflectionsRayTracing                | BSImagespaceShaderReflectionsRayTracing                  | ISReflectionsRayTracing                | 0x1412b1800 | 99156      | 0x141858de8                        | 0x141499230 (105790) | raster                            | HLSL (active)                                    |
| 131 | ISReflectionsDebugSpecMask             | BSImagespaceShaderReflectionsDebugSpecMask               | ISReflectionsRayTracing                | 0x1412b1580 | 99155      | 0x141858f90                        | 0x141498fd0 (105789) | raster                            | HLSL file, not in descriptor map                 |
| 132 | ISSAOBlurH                             | BSImagespaceShaderISSAOBlurH                             | ISSAOBlur                              | 0x1412aa670 | 99115      | 0x1418590f0                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 133 | ISSAOBlurV                             | BSImagespaceShaderISSAOBlurV                             | ISSAOBlur                              | 0x1412aa9c0 | 99117      | 0x141859248                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 134 | ISSAOBlurHCS                           | BSImagespaceShaderISSAOBlurHCS                           | ISSAOBlurCS                            | 0x1412aa7f0 | 99116      | 0x141859380                        | 0x1414932a0 (105756) | compute                           | DXBC only                                        |
| 135 | ISSAOBlurVCS                           | BSImagespaceShaderISSAOBlurVCS                           | ISSAOBlurCS                            | 0x1412aab40 | 99118      | 0x1418594d0                        | 0x141493460 (105757) | compute                           | DXBC only                                        |
| 136 | ISSAOCameraZ                           | BSImagespaceShaderISSAOCameraZ                           | ISSAOCameraZ                           | 0x1412aad10 | 99119      | 0x141859608                        | 0x141493620 (105758) | raster                            | HLSL file, not in descriptor map                 |
| 137 | ISSAOCameraZAndMipsCS                  | BSImagespaceShaderISSAOCameraZAndMipsCS                  | ISSAOCameraZAndMipsCS                  | 0x1412aaed0 | 99120      | 0x141859748                        | 0x1414937d0 (105759) | compute                           | DXBC only                                        |
| 138 | ISSAOCompositeSAO                      | BSImagespaceShaderISSAOCompositeSAO                      | ISSAOComposite                         | 0x1412ab680 | 99122      | 0x1418598b8                        | 0x141493ed0 (105761) | raster                            | HLSL (active)                                    |
| 139 | ISSAOCompositeFog                      | BSImagespaceShaderISSAOCompositeFog                      | ISSAOComposite                         | 0x1412ab120 | 99121      | 0x141859ae8                        | 0x141493a00 (105760) | raster                            | HLSL (active)                                    |
| 140 | ISSAOCompositeSAOFog                   | BSImagespaceShaderISSAOCompositeSAOFog                   | ISSAOComposite                         | 0x1412abbe0 | 99123      | 0x141859c38                        | 0x1414943a0 (105762) | raster                            | HLSL (active)                                    |
| 141 | ISMinify                               | BSImagespaceShaderISMinify                               | ISSAOMinify                            | 0x1412a9950 | 99109      | 0x141859d78                        | 0x141492940 (105752) | raster                            | HLSL (active)                                    |
| 142 | ISMinifyContrast                       | BSImagespaceShaderISMinifyContrast                       | ISSAOMinify                            | 0x1412a9c10 | 99110      | 0x141859f08                        | 0x141492bc0 (105753) | raster                            | HLSL (active)                                    |
| 143 | ISSAORawAO                             | BSImagespaceShaderISSAORawAO                             | ISSAORawAO                             | 0x1412ac140 | 99124      | 0x14185a060                        | 0x141494870 (105763) | raster                            | DXBC only                                        |
| 144 | ISSAORawAONoTemporal                   | BSImagespaceShaderISSAORawAONoTemporal                   | ISSAORawAO                             | 0x1412ac770 | 99126      | 0x14185a208                        | 0x141494e20 (105765) | raster                            | DXBC only                                        |
| 145 | ISSAORawAOCS                           | BSImagespaceShaderISSAORawAOCS                           | ISSAORawAOCS                           | 0x1412ac470 | 99125      | 0x14185a348                        | 0x141494b60 (105764) | compute                           | DXBC only                                        |
| 146 | ISSILComposite                         | BSImagespaceShaderISSILComposite                         | ISSILComposite                         | 0x1412acaa0 | 99127      | 0x14185a490                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 147 | ISSILRawInd                            | BSImagespaceShaderISSILRawInd                            | ISSILRawInd                            | 0x1412acc20 | 99128      | 0x14185a5d8                        | 0x141495110 (105766) | raster                            | DXBC only                                        |
| 148 | ISSimpleColor                          | BSImagespaceShaderISSimpleColor                          | ISSimpleColor                          | 0x1412acf50 | 99129      | 0x14185a748                        | 0x141495400 (105767) | raster                            | HLSL file, not in descriptor map                 |
| 149 | ISDisplayDepth                         | BSImagespaceShaderISDisplayDepth                         | ISSimpleColor                          | 0x1412a7990 | 99099      | 0x14185a890                        | 0x141490e10 (105743) | raster                            | HLSL file, not in descriptor map                 |
| 150 | ISSnowSSS                              | BSImagespaceShaderISSnowSSS                              | ISSnowSSS                              | 0x1412ad110 | 99130      | 0x14185a9d8                        | 0x1414955b0 (105768) | raster                            | HLSL file, not in descriptor map                 |
| 151 | ISTemporalAA                           | BSImagespaceShaderISTemporalAA                           | ISTemporalAA                           | 0x1412ad350 | 99131      | 0x14185ab38                        | 0x1414957d0 (105769) | raster                            | HLSL (active)                                    |
| 152 | ISTemporalAA_UI                        | BSImagespaceShaderISTemporalAA_UI                        | ISTemporalAA                           | 0x1412ad770 | 99132      | 0x14185ad90                        | 0x141495b90 (105770) | raster                            | HLSL file, not in descriptor map                 |
| 153 | ISTemporalAA_Water                     | BSImagespaceShaderISTemporalAA_Water                     | ISTemporalAA                           | 0x1412adba0 | 99133      | 0x14185aed0                        | 0x141495f50 (105771) | raster                            | HLSL file, not in descriptor map                 |
| 154 | ISUpsampleDynamicResolution            | BSImagespaceShaderISUpsampleDynamicResolution            | ISUpsampleDynamicResolution            | 0x1412ae0b0 | 99135      | 0x14185b020                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 155 | ISWaterBlend                           | BSImagespaceShaderISWaterBlend                           | ISWaterBlend                           | 0x1412ae6d0 | 99138      | 0x14185b188                        | 0x141496770 (105774) | raster                            | HLSL (active)                                    |
| 156 | ISUnderwaterMask                       | BSImagespaceShaderISUnderwaterMask                       | Water                                  | 0x1412adfd0 | 99134      | 0x14185b2e8                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 157 | ISWaterFlow                            | BSImagespaceShaderWaterFlow                              | ISWaterFlow                            | 0x1412b2e20 | 99166      | 0x141856328                        | inlined in AE 105696 | raster                            | HLSL file, not in descriptor map                 |
| 158 | (empty)                                | -                                                        | -                                      | -           | -          | -                                  | -                    | -                                 | -                                                |

### R5 Scaleform renderer (GFx 3.3 GRendererD3D1x on D3D11)

Scaleform bypasses BSGraphics entirely: `Renderer::GetScaleformRendererParameters` (SE 0x140d70400) hands its renderer
the raw device and context, and every UI draw is its own D3D11 call.

| What                             | SE ID | SE addr     | AE ID | AE addr     | Notes                                                                                                                       |
| -------------------------------- | ----- | ----------- | ----- | ----------- | --------------------------------------------------------------------------------------------------------------------------- |
| ctor                             | 82790 | 0x140f5a740 | 84906 | 0x141044e20 | 0x8D8 bytes. Final vtable SE 0x1417dbf90, AE 0x141a258c0; base vtable 0x1417d8d30 / 0x141a255a0. No RTTI.                   |
| factory                          | 82831 | 0x140f5d450 | 84952 | 0x141047dc0 | Allocates from the GFx heap.                                                                                                |
| SetDependentVideoMode            | 82918 | 0x140f633d0 | 85033 | 0x14104dfe0 | Slot 53.                                                                                                                    |
| CreateShaders and states         | 82863 | 0x140f5fae0 | 84984 | 0x14104a670 | Releases everything via 82894 on failure.                                                                                   |
| BSScaleformManager renderer init | 80603 | 0x140edb730 | ?     | -           | Factory, `GetScaleformRendererParameters`, `SetDependentVideoMode(dev, ctx, 0)`, GFxRenderConfig (0x140f63d20, flags 0x11). |

**Members:** +0x490 device, +0x498 context, +0xC0 video mode set, +0xE0 the renderer's own DSV (mask stencil), +0x150..
VS, +0x248.. and +0x2D8.. PS, +0x110 input layout, +0x1B8 and +0x3B0 constant buffers (0x80 each), +0x3E0..+0x458 blend
states (2x7), +0x410 color-write-off, +0x3B8..+0x3D0 samplers (4), +0x458/+0x460 rasterizer states, +0x468..+0x488
depth-stencil states (5), +0x4A0 in-mask flag, +0x4A4 stencil ref, +0x620 texture list, +0x640 CRITICAL_SECTION, +0x720
blend mode, +0x740 display RT.

**Vtable (58 slots).** CommonLib's `GRenderer.h` numbers its last five virtuals two too low (the binary has two extra
no-op slots, 46 and 47).

| Slot | SE addr / ID        | AE addr / ID        | Function                                                                |
| ---- | ------------------- | ------------------- | ----------------------------------------------------------------------- |
| 0    | 0x140f5b900 / 82809 | 0x141046190 / 84931 | dtor                                                                    |
| 1    | 0x140f5f430 / 82852 | 0x141049fd0 / 84973 | GetRenderCaps                                                           |
| 2    | 0x140f5d480 / 82832 | 0x141047df0 / 84953 | CreateTexture (takes the critical section; GTexture vtable 0x1417dbe40) |
| 3    | 0x140f5d530 / 82833 | 0x141047ea0 / 84954 | CreateTextureYUV                                                        |
| 4    | 0x140f449d0 / 82359 | 0x14102ce50 / 84446 | BeginFrame (no-op)                                                      |
| 5    | 0x140f44af0 / 82363 | 0x14102cf70 / 84450 | EndFrame                                                                |
| 6    | 0x140f5d3b0 / 82830 | 0x141047d20 / 84951 | CreateRenderTarget                                                      |
| 7    | 0x140f63550 / 82919 | 0x14104e170 / 85034 | SetDisplayRenderTarget                                                  |
| 8    | 0x140f61830 / 82887 | 0x14104c410 / 85004 | PushRenderTarget                                                        |
| 9    | 0x140f61270 / 82882 | 0x14104be10 / 85001 | PopRenderTarget                                                         |
| 10   | 0x140f61c70 / 82888 | 0x14104c990 / 85005 | PushTempRenderTarget                                                    |
| 11   | 0x140f62470 / 82895 | 0x14104d1f0 / 85012 | ReleaseTempRenderTargets                                                |
| 12   | 0x140f5c2b0 / 82819 | 0x141046be0 / 84942 | BeginDisplay                                                            |
| 13   | 0x140f5f230 / 82841 | 0x141049d50 / 84962 | EndDisplay                                                              |
| 14   | 0x140f636c0 / 82922 | 0x14104e230 / 85038 | SetMatrix                                                               |
| 15   | 0x140f63820 / 82929 | 0x14104e390 / 85045 | SetUserMatrix                                                           |
| 16   | 0x140f633a0 / 82916 | 0x14104dfb0 / 85031 | SetCxform                                                               |
| 17   | 0x140f61770 / 82886 | 0x14104c300 / 85003 | PushBlendMode                                                           |
| 18   | 0x140f61120 / 82881 | 0x14104bd10 / 85000 | PopBlendMode                                                            |
| 19   | 0x140f44e50 / 82368 | 0x14102d340 / 84455 | PushUserData                                                            |
| 20   | 0x140f44e40 / 82367 | 0x14102d330 / 84454 | PopUserData                                                             |
| 21   | 0x140f63730 / 82924 | 0x14104e2a0 / 85040 | SetPerspective3D                                                        |
| 22   | 0x140f63960 / 82933 | 0x14104e4e0 / 85051 | SetView3D                                                               |
| 23   | 0x140f639b0 / 82935 | 0x14104e530 / 85053 | SetWorld3D                                                              |
| 24   | 0x140f44c30 / 82366 | 0x14102d130 / 84453 | MakeViewAndPersp3D                                                      |
| 25   | 0x140f44fe0 / 82377 | 0x14102d4d0 / 84464 | SetStereoParams                                                         |
| 26   | 0x140f44fd0 / 82376 | 0x14102d4c0 / 84463 | SetStereoDisplay                                                        |
| 27   | 0x140f638a0 / 82930 | 0x14104e410 / 85048 | SetVertexData                                                           |
| 28   | 0x140f63670 / 82921 | 0x14104e1e0 / 85037 | SetIndexData                                                            |
| 29   | 0x140f62080 / 82889 | 0x14104cdf0 / 85006 | ReleaseCachedData                                                       |
| 30   | 0x140f5ecd0 / 82838 | 0x141049690 / 84959 | DrawIndexedTriList                                                      |
| 31   | 0x140f5ee80 / 82839 | 0x141049940 / 84960 | DrawLineStrip                                                           |
| 32   | 0x140f60c80 / 82875 | 0x14104b970 / 84995 | LineStyleDisable                                                        |
| 33   | 0x140f60c40 / 82874 | 0x14104b930 / 84994 | LineStyleColor                                                          |
| 34   | 0x140f5f3a0 / 82845 | 0x141049ec0 / 84966 | FillStyleDisable                                                        |
| 35   | 0x140f5f360 / 82844 | 0x141049e80 / 84965 | FillStyleColor                                                          |
| 36   | 0x140f5f340 / 82843 | 0x141049e60 / 84964 | FillStyleBitmap                                                         |
| 37   | 0x140f5f3b0 / 82846 | 0x141049ed0 / 84967 | FillStyleGouraud                                                        |
| 38   | 0x140f5d660 / 82835 | 0x141048060 / 84956 | DrawBitmaps                                                             |
| 39   | 0x140f44ae0 / 82362 | 0x14102cf60 / 84449 | DrawDistanceFieldBitmaps (no-op)                                        |
| 40   | 0x140f5cb40 / 82820 | 0x141047440 / 84943 | BeginSubmitMask                                                         |
| 41   | 0x140f5f280 / 82842 | 0x141049da0 / 84963 | EndSubmitMask                                                           |
| 42   | 0x140f5d5a0 / 82834 | 0x141047fa0 / 84955 | DisableMask                                                             |
| 43   | 0x140f5ce70 / 82825 | 0x141047770 / 84948 | CheckFilterSupport                                                      |
| 44   | 0x140f5db10 / 82836 | 0x141048470 / 84957 | DrawBlurRect (blur, glow, drop shadow)                                  |
| 45   | 0x140f5e840 / 82837 | 0x1410491e0 / 84958 | DrawColorMatrixRect                                                     |
| 46   | 0x140f44fa0 / 82374 | 0x14102d490 / 84461 | no-op (extra GFx 3.3 virtual)                                           |
| 47   | 0x140f44f90 / 82373 | 0x14102d480 / 84460 | no-op (extra GFx 3.3 virtual)                                           |
| 48   | 0x140f5f460 / 82853 | 0x14104a000 / 84974 | GetRenderStats                                                          |
| 49   | 0x140f5f4c0 / 82856 | 0x14104a060 / 84977 | GetStats                                                                |
| 50   | 0x140f62240 / 82893 | 0x14104cfc0 / 85010 | ReleaseResources                                                        |
| 51   | 0x140f446a0 / 82357 | 0x14102cb30 / 84444 | AddEventHandler                                                         |
| 52   | 0x140f44e70 / 82370 | 0x14102d360 / 84457 | RemoveEventHandler                                                      |
| 53   | 0x140f633d0 / 82918 | 0x14104dfe0 / 85033 | SetDependentVideoMode                                                   |
| 54   | 0x140f62ec0 / 82908 | 0x14104dcb0 / 85025 | ResetVideoMode                                                          |
| 55   | 0x140f5ce60 / 82824 | 0x141047760 / 84947 | CheckDisplayStatus                                                      |
| 56   | 0x140f5f410 / 82850 | 0x141049fb0 / 84971 | GetDevice                                                               |
| 57   | 0x140f5f3f0 / 82848 | 0x141049f90 / 84969 | GetDeviceContext                                                        |

**Shaders: 55 DXBC blobs (51 unique), all SM 4.0** (HLSL compiler 9.29.952.3111):

| Table          | Pointers            | Sizes               | Slots            | Enable bytes (.bss) |
| -------------- | ------------------- | ------------------- | ---------------- | ------------------- |
| VS-A           | 0x141e0ad58         | 0x1417d4258         | 11 (slot 5 null) | 0x143036f80         |
| VS-B           | 0x141e0adb0         | 0x1417d42b0         | 2 (slot 0 null)  | 0x143036e90         |
| PS-A (fills)   | 0x141e0ab80 + 0x248 | 0x1417d40b0 + 0x248 | 18               | 0x143036fe0         |
| PS-B (filters) | 0x141e0a708 + 0x2D8 | 0x1417d40b8 + 0x2D8 | 27 (slot 0 null) | 0x143036bc8         |

Input layout descs are at 0x141e0a990. Blob addresses (size):

- **VS-A:** 0x1417c4a70 (0x330), c3450 (0x348), c4da0 (0x394), c5140 (0x3f0), c5530 (0x454), null, c3ab0 (0x35c),
  c30d0 (0x374), c3e10 (0x3c0), c41d0 (0x41c), c45f0 (0x480).
- **VS-B:** null, 0x1417c37a0 (0x310).
- **PS-A:** 0x1417d2490 (0x208), c74c0 (0x2fc), c77c0 (0x354), d26a0 (0x354), d2a00 (0x318), d2d20 (0x370), d3090
  (0x574), c1670 (0x5cc), d3610 (0x5ec), d3c00 (0x644), c5d90 (0x2c0), c6a30 (0x264), c6ca0 (0x3bc), c5990 (0x3fc),
  c6050 (0x304), c6360 (0x2c4), c6630 (0x400), c7060 (0x454).
- **PS-B:** null, 0x1417c8ef0 (0x6cc), c95c0 (0x7f8), cac70 (0x724), cb3a0 (0x850), ca5c0 (0x6ac), c9dc0 (0x7f8, dup of
  2), cc440 (0x704), cbbf0 (0x850, dup of 4), ccb50 (0x6c0), cd210 (0x768), ce7f0 (0x718), cef10 (0x7c0), ce110 (0x6dc),
  cd980 (0x784), cfeb0 (0x734), cf6d0 (0x7dc), d05f0 (0x588), d0b80 (0x588, dup of 17), d1110 (0x5e0), d16f0 (0x5e0, dup
  of 19), c7b20 (0x464), c8450 (0x520), c7f90 (0x4bc), c8970 (0x578), d1cd0 (0x3ac), d2080 (0x404).

**The exe also embeds Scaleform's HLSL source:** 53 `void main` strings at 0x1417d44b0-0x1417dbc70 (`cbuffer
PSConstants {cxadd, cxmul, fsize, ...}`, `cbuffer VSConstants {float4x4 mvp}`), pointer tables near 0x141e0aab8 and
0x141e0ab58. No D3DCompile import or call was found, so they are probably unused leftovers _(inferred)_. The ORG port
can transcribe them; pair them with the blobs (compile and diff) first.

**State:**

- **Masks** use the renderer's own DSV stencil. BeginSubmitMask turns colour writes off (+0x410). Mode 0 runs
  `ClearDepthStencilView(+0xE0, STENCIL, 0)` and uses DS +0x470 with ref 1; mode 1 increments (DS +0x478, ref++), mode 2
  decrements (DS +0x480, ref--). EndSubmitMask restores the blend state and uses DS +0x488 (EQUAL, ref). DisableMask
  uses DS +0x468 with ref 0. Ops: ALWAYS/EQUAL with KEEP/INCR_SAT/INCR/DECR.
- **Blend:** the 15 GFx modes map through table 0x1417d4468 to 7 states, built from two D3D11_BLEND_DESC arrays at
  0x1417c1d48 and 0x1417c2588.
- **Samplers:** 4, point or linear by wrap or clamp. **Rasterizer:** solid, cull none, MSAA variant by flag +0xD4 bit 2.
- **Render to texture:** CreateRT, Push/PopRT and PushTempRT; the filters render through pooled temp RTs. Menu 3D
  content arrives through BSScaleformExternalTexture.
- **Textures:** uploads use UpdateSubresource and Map/Unmap.
- **Threads:** CreateTexture and CreateTextureYUV lock the critical section at +0x640, so they serve GFx loader threads
  _(inferred)_. Drawing takes no lock and runs on whichever thread calls Display(): the main thread and, _(inferred)_,
  the loading-screen job thread (38085).

### R10 Bink

**There is no YUV plane and no YUV shader.** `BinkCopyToBufferRect` converts on the CPU into 32-bit RGBA tiles, and a
plain textured-quad VS/PS draws them. The P5.11 work is an RGBA8 upload ring plus a blit.

| What                 | SE ID         | SE addr                   | AE ID            | AE addr                   | Notes                                                                                                                                                                                                                                     |
| -------------------- | ------------- | ------------------------- | ---------------- | ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Open                 | 87888         | 0x1410a0f80               | 90257            | 0x141195a10               | Opens `Data\Video\<name>`. Flag 0x2000 (preload) for small files or when forced; flag 0x8000000 (BINKNOTHREADEDIO, _inferred_) when the calling thread does not own `DAT_143047330`.                                                      |
| Open + scale + tiles | 87889         | 0x1410a10f0               | ?                | -                         | `CreateTileTextures(w, h, 4, 0x100, scale, x, y)`                                                                                                                                                                                         |
| CreateTileTextures   | 87904         | 0x1410a1910               | 90275            | 0x1411965e0               | 256x256 tiles, power-of-two edge tiles. Each: `Renderer::CreateTexture` (0x140d6df00) R8G8B8A8_UNORM, DYNAMIC, SRV, CPU write, 1 mip, plus a quad TriShape (87910). Creates the VS (blob 0x141e0bcd0, 0x238) and PS (0x141e0bf10, 0x2b8). |
| DecodeFrameToTiles   | 87911         | 0x1410a2840               | inlined in 90259 | -                         | `BinkDoFrame`; per tile: lock 75514 (Map WRITE_DISCARD), `BinkCopyToBufferRect(..., x, y, w, h, 3 \| copyAll << 31)` (surface 3 = BINKSURFACE32, _inferred_), unlock 75515.                                                               |
| Lock / unlock        | 75514 / 75515 | 0x140d6e4c0 / 0x140d6e510 | 77306 / 77307    | 0x140e48c10 / 0x140e48c60 | Map/Unmap on the context global                                                                                                                                                                                                           |
| DrawTiles            | 87907         | 0x1410a2370               | 90279            | 0x141197050               | Sets VS/PS; per tile binds PS t0 and calls DrawTriShape.                                                                                                                                                                                  |
| Player tick          | 87890         | 0x1410a1210               | 90259            | 0x141195ca0               | `BinkWait`, decode, `BinkNextFrame`; RT0 = kFRAMEBUFFER, ApplyState, `Renderer::Begin`, DrawTiles, `Renderer::End` (presents). Sleeps 1 ms when no frame is ready.                                                                        |
| Play loop            | 87887         | 0x1410a0e40               | ?                | -                         | Blocking `do { tick } while`, then BinkClose and free tiles.                                                                                                                                                                              |
| Free tiles           | 87905         | 0x1410a20e0               | 90276            | 0x141196dc0               |                                                                                                                                                                                                                                           |
| Close                | 87891         | 0x1410a13a0               | 90260            | 0x141195f30               |                                                                                                                                                                                                                                           |
| PlaySequenceThread   | 87896         | 0x1410a14d0               | ?                | -                         | Movies can play on a dedicated thread _(inferred)_.                                                                                                                                                                                       |

Bink's shaders are plain copy shaders (POSITION/TEXCOORD VS, TexBase/SampBase PS), DXBC in .data with no CS HLSL. The
ORG present stage must accept presents from the blocking loop and the sequence thread.

### Open questions (lane 2)

1. AE render-target indices (116 vs 114): confirm the two extra are appended.
2. Bink colour order: BINKSURFACE32's byte order may not match R8G8B8A8_UNORM; dump the PS at 0x141e0bf10 for a swizzle.
3. Scaleform enable tables: what fills the `.bss` enable bytes, so which blobs are created at runtime.
4. Pair the 53 embedded HLSL sources with the 55 blobs before porting.
5. No callers found for slots 8 (Map), 33 (Noise), 49-51 (DistantBlur) and 57/61/63.
6. Which console command or INI sets the HDR flag (SE `DAT_141e0eb24`), and what mgr+0x118 means.
7. Missing AE IDs: 100430, 51855, 80603, 87887, 87889 and the helper render functions.
8. Whether Scaleform's Display() runs on the loading-screen job thread (38085).
9. Which DXBC-only shaders a default vanilla frame runs (FXAA default, SAO raster vs compute): a runtime census with CS's
   developer-mode active-shader tracking would answer it.

## RE lane 3: threads, swap chain, engine passes around DCLF, menus (R6-R9, R12)

Sources: Ghidra SE 1.5.97 and AE 1.6.1170 (GhidraMCP), Address Library bins, CommonLibSSE-NG in the repo, the repo's
hooks (src/Hooks.cpp, src/Features/Upscaling.cpp, src/Features/DrawcallLimitFix*.cpp, DCLF Core/Handoff, Features/Reflection,
Features/FirstPerson), CreationEngineRaytracing/src/Hooks.cpp.

Conventions:

- "SE ID / AE ID" are Address Library IDs. An AE address is always from the AE database, and was checked in the AE Ghidra
  program unless the row says _inferred_.
- Offsets into `RendererData` are from CommonLib's `RendererData*` (= `Renderer + 0x10` on flat SE/AE). Offsets into
  `Renderer` are from the singleton (SE 0x143028490, AE 0x1432887c0).
- D3D11/DXGI vtable slots are written as `method (slot, byte offset)`.
- **(inferred)** marks a conclusion drawn from code shape or naming, not traced end to end.

Ghidra renames made this session (only `FUN_` names were replaced, both programs saved):

| SE addr     | AE addr                                                | New name                                                |
| ----------- | ------------------------------------------------------ | ------------------------------------------------------- |
| 0x140d718d0 | 0x140e4c830                                            | `BSGraphics::Renderer::CreateDeviceAndSwapChain`        |
| 0x140d72810 | (inlined in AE Init)                                   | `BSGraphics::Renderer::InitD3D`                         |
| 0x140d71d20 | 0x140e4cc50                                            | `BSGraphics::Renderer::CreateSwapChainViews`            |
| 0x140d720d0 | 0x140e4d000                                            | `BSGraphics::Renderer::CreateConstantBufferPools`       |
| 0x140d72350 | 0x140e4d280                                            | `BSGraphics::Renderer::CreateDynamicBuffers`            |
| 0x140d6b8d0 | 0x140e45d30                                            | `BSGraphics::Renderer::CreateTriShape`                  |
| 0x140d6c320 | 0x140e46810                                            | `BSGraphics::Renderer::ReleaseTriShape`                 |
| 0x140d6a810 | 0x140e44d30                                            | `BSGraphics::Renderer::ClearDepthStencil`               |
| 0x140d6dd70 | 0x140e484c0                                            | `BSGraphics::Renderer::CreateTextureFromDDS`            |
| 0x141305610 | (AE already named `BSShadowParabolicLight::sub`, left) | `BSShadowLight::RenderShadowmap`                        |
| 0x141302370 | 0x1414ed920                                            | `BSCubeMapCamera::RenderFaces`                          |
| 0x1408d2390 | 0x140972590                                            | `UI3DSceneManager::Render`                              |
| 0x14063fa50 | 0x1406d2120                                            | `JobListManager::ServingThread::RunLoadingScreen`       |
| 0x140576410 | 0x1405fc050                                            | `JobListManager::ServingThread::SetLoadingScreenActive` |

Existing wrong names to watch for (not renamed, they are not `FUN_`): SE 0x140d69b70 is labelled
`BSPointerHandleManager::IsValid` but is `Renderer::ResetWindow` (CommonLib 75454). AE 0x1414f0cf0 is labelled
`BSShadowParabolicLight::sub` but is `BSShadowLight::RenderShadowmap` (all shadow light types). SE 0x1412e3520's call at +0x18E
is labelled `BSShaderAccumulator::RenderFirstPersonView_End`; DCLF calls it the world-drawn point.

---

### R6. Threads and lifetime

#### R6.1 Which threads touch D3D11

The engine's render thread is the **main thread**: `Main::Draw` / the frame function run there, inside `Renderer::Lock`.
These run D3D11 work on other threads:

| Thread                                   | What it does with D3D11                                                                                                         | Lock                                      | Entry (SE ID / addr, AE ID / addr)                                                                                                                       |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JobListManager **ServingThread**, mode 2 | The whole loading-screen loop: `Begin`, clears, UI (Scaleform menus and `MistMenu`'s 3D scene), `End` = **Present**             | `Renderer::Lock` per iteration            | 38085 / 0x14063fa50, 39039 / 0x1406d2120                                                                                                                 |
| Same thread, mode 2 entry                | `MoviePlayer::WaitForSequenceToEnd` before the loop when a movie is up (so Bink can draw and Present here) **(inferred)**       | none seen                                 | in 38085 / 39039                                                                                                                                         |
| BSResource loader / IO threads           | `CreateTexture2D` + `CreateShaderResourceView` with initial data (DDS), `CreateBuffer` (immutable VB/IB) for every NIF geometry | none (relies on the free-threaded device) | textures 101532 / 0x141328370, 108531 / 0x141515810; geometry 75473 / 0x140d6b8d0, 77259 / 0x140e45d30 **(thread inferred: called from NiStream loads)** |
| Any thread dropping the last ref         | `ID3D11Buffer::Release` on VB/IB (TriShape) and texture releases                                                                | none                                      | 75480 / 0x140d6c320, 77267 / 0x140e46810                                                                                                                 |
| Main thread (MoviePlayer)                | Bink: Map/Unmap dynamic textures, draw quad, `Begin`/`End` (Present)                                                            | none in the Bink function                 | 87890 / 0x1410a1210, 90259 / 0x141195ca0                                                                                                                 |

The ServingThread (`JobListManager::ServingThread`, 0x78-byte object, singleton SE 0x142f26668 / AE 0x1431771d0):

| Field | Meaning                                                              |
| ----- | -------------------------------------------------------------------- |
| +0x58 | auto event: wake                                                     |
| +0x60 | event: done (set when a mode's work returns)                         |
| +0x68 | requested state (loop condition: `IsMode(2)` reads it)               |
| +0x6c | current mode: 0 idle, 1 run job lists, 2 run the loading-screen loop |
| +0x70 | quit flag; +0x72 running flag                                        |

Thread function: SE 0x140576cd0 `ServingThread::Func2` (AE 35423 / 0x1405fc870). It waits on +0x58, then runs
`ServingThread::sub` (mode 1, the frame's job lists) or `RunLoadingScreen` (mode 2), then sets +0x60.

| Function                              | SE ID / addr        | AE ID / addr        | Notes                                                                                                                                                                                                                                                     |
| ------------------------------------- | ------------------- | ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| IsMode(n) (reads +0x68)               | 34550 / 0x1405763b0 | 35405 / 0x1405fbff0 | the loading loop's `while`                                                                                                                                                                                                                                |
| IsActive(n) (reads +0x6c)             | 34551 / 0x1405763e0 | 35406 / 0x1405fc020 | called from the main frame and many menus                                                                                                                                                                                                                 |
| SetLoadingScreenActive(bool)          | 34552 / 0x140576410 | 35407 / 0x1405fc050 | true: mode 2, SetEvent(+0x58). false: +0x68 = 0, then pump `FUN_1405b5fc0` while `WaitForSingleObjectEx(+0x60, 1ms)`, mode = 0. It releases `UI`'s spin lock (UI +0x158, owner thread id, +0x15c count) if this thread holds it, and takes it again after |
| StartJobLists                         | 34553 / 0x140576540 | 35408 / 0x1405fc180 | mode 1, from the main frame 35565                                                                                                                                                                                                                         |
| WaitJobLists                          | 34554 / 0x1405765d0 | 35409 / 0x1405fc210 | from 35565                                                                                                                                                                                                                                                |
| Load coordinator that starts/stops it | 13214 / 0x140157710 | 13363 / 0x1401a1150 | calls SetLoadingScreenActive(true) before the load, (false) after                                                                                                                                                                                         |

#### R6.2 The loading-screen loop (38085 / 39039)

```
if (MoviePlayer active) MoviePlayer::WaitForSequenceToEnd();
while (IsMode(2) && !BSWin32SystemUtility->quitting(+0x115)) {
    pump window messages (FUN_14133e590 / AE FUN_14152cc30)
    if (!DAT_142f4ecf0) {                      // normal path
        UI: advance / process messages (FUN_140ebe630, FUN_140ebf260)
        drain a deferred task queue (FUN_1408dad30 -> FUN_1408dae20)
        Renderer::Lock
        State: ++counter at +0x110 (FUN_140d7cf50)
        Renderer::Begin(0)                      // ClearState, per-frame CB, kFRAMEBUFFER <- window 0 views
        DAT_14302c8e5 = 1                       // "in loading screen" flag; FinishAccumulating reads it
        SetClearColors(0,0,0,0)
        SetRenderTarget(0, kMAIN, SRTM_CLEAR)
        SetRenderTarget(1, kMOTION_VECTOR, SRTM_CLEAR)
        SetRenderTarget(2, kNORMAL_TAAMASK_SSRMASK(_SWAP), SRTM_CLEAR)
        SetCurrentDepthStencilTarget(kMAIN, SRTM_CLEAR)
        ApplyState(false)                       // the clears happen here
        UI::Render (SE 79947 / 0x140ebf510, AE 82084 / 0x140fa4f00)
        deferred release flush (SE 0x1403db780, AE 0x1404355b0)
        DAT_14302c8e5 = 0
        Renderer::End                           // Present
        State counter (FUN_140d7cf50)
        Renderer::Unlock
    } else {
        FUN_1408ccb70(); SetRenderTarget(0, kFRAMEBUFFER, SRTM_CLEAR); ApplyState  // no Present
    }
    music update, BSInputDeviceManager::PollInputDevices
}
```

`UI::Render` (79947/82084) draws every visible menu: `PostDisplay` (IMenu vfunc 0x30) on each, top-down from the first
opaque one. During loading the visible menus are LoadingMenu (Scaleform) and **MistMenu**, whose `PostDisplay` calls
`UI3DSceneManager::Render(scheme 4)`. `LoadingMenu::ProcessMessage` (AE 0x140935020) sets UI3DSceneManager scheme 4. So the
loading-screen 3D model goes through BSShaderAccumulator, BSBatchRenderer and ImageSpace **on the ServingThread**
**(inferred from the call graph; not runtime-checked)**. If no menu writes `kFRAMEBUFFER`, UI::Render runs ImageSpace
effect 0x22 (copy to the framebuffer).

Hand-off: the main thread calls `SetLoadingScreenActive(true)` and goes on loading. The two threads exclude each other
only through `Renderer::Lock` (the main thread is not rendering frames during the load). On stop, the main thread
waits for the loop to finish its iteration and return. Plan impact: ORG's Present and graph submission must accept a
second thread, or the loading loop must be moved or marshalled to one thread.

#### R6.3 Renderer::Lock / Unlock

| Function           | SE ID / addr        | AE ID / addr        |
| ------------------ | ------------------- | ------------------- |
| `Renderer::Lock`   | 75458 / 0x140d6a080 | 77243 / 0x140e44550 |
| `Renderer::Unlock` | 75459 / 0x140d6a0a0 | 77244 / 0x140e44570 |

- A Win32 `CRITICAL_SECTION` at `Renderer + 0x2790` (SE) / `+0x27F0` (AE 1.6.1130 and later), CommonLib `GetLock()`. It is
  **recursive**, with no timeout and no ownership checks.
- The main frame function (35556 / 0x1405b1020, AE 36555 / 0x140643c00) holds it from just before `Begin` to just after
  `End` (Present), so the whole frame is inside it.
- Other SE holders: `Precipitation::RenderOcclusion` and `RenderMask` (they run inside the frame), `MemoryLevelManager::BeginMemoryFree`,
  BufferedScreenshot (35883-35885), Scaleform resource release (80301 / 0x140ece650), console `ReloadProjectedUVTextures`
  (22634), console/script functions (36016), Havok debug (17266), 15040/15041/15045, Main init 35548. AE also has
  `BSTaskPool_HandleTask` (0x14065880d) taking it **(AE-only call site; purpose not traced)**.
- Texture and geometry creation do **not** take it. They rely on the D3D11 device being free-threaded. DXVK's
  allocation pool race (render-graph.md "DXVK fork: allocation-pool locking") came from exactly this.

#### R6.4 Init-time buffers (75592, 75593) and the dynamic ring

| Function                  | SE ID / addr        | AE ID / addr        | What it creates                                                                                                                                                                                                                                                                                                                                                                                                    |
| ------------------------- | ------------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| CreateConstantBufferPools | 75592 / 0x140d720d0 | 77400 / 0x140e4d000 | Dynamic CBs (Usage DYNAMIC, Bind CONSTANT, CPU WRITE). 4 x 3840 B at 0x143027a08; size-bucketed pools 16..0x130 step 16 at 0x143027a38, 16..0x90 at 0x143027ad8, 16..0x1b0 at 0x143027b28, 16..0x130 at 0x143027c08 and 0x143027ca8, 16..0x270 at 0x143027d48; single CBs 0x10 at 0x143027a28, 0x240 at 0x143027e80, **0x2d0 at 0x143027e88 (per-frame CB, bound at VS/PS slot 12 by Begin)**, 0x10 at 0x143027e90 |
| CreateDynamicBuffers      | 75593 / 0x140d72350 | 77401 / 0x140e4d280 | 3 x 4 MiB DYNAMIC VB+IB (bind 3) ring at 0x143025f18, each with an EVENT query (0x143026168) and a "free" flag (0x143026164); a 0x6000 B static quad IB (indices 0,1,2,0,2,3 x 2048) at 0x143025f38; a 0x8000 B half-float VB at 0x143025f40                                                                                                                                                                       |
| MapDynamicBuffer (ring)   | 75484 / 0x140d6c8a0 | 77271 / 0x140e46d60 | Bump allocator in the current buffer (index 0x143025f30, offset 0x143025f34). On overflow: `End(query[i])`, next buffer; if its flag is clear, spin `GetData` (first with DONOTFLUSH) + `Sleep(1)` until the query is done. Then `Map(buffer, WRITE_NO_OVERWRITE)` (never DISCARD). Callers: `DrawParticleShaderTriShape`, `RenderPassImmediately_Standard`, 75491, 0x140d6ca10, 0x140d6d5d0                       |

InitD3D (75595 / 0x140d72810; inlined into `Renderer::Init` 77226 on AE) also calls `BSGraphics::Renderer::Init::InitD3D`
(SE 0x140d88720, AE 77649 / 0x140e65260: depth-stencil/blend/raster/sampler state objects; CS hooks this call at
SE +0x50 / AE Init +0x2BC), then 0x140d88c90, 0x140d891f0, 0x140d89230, 0x140d89480 (AE 0x140e657b0, 0x140e65d10,
0x140e65d50, 0x140e65fa0), then 75592 and 75593, then `IDXGIDevice1::SetMaximumFrameLatency(2)`.
`Renderer::Init` (75445/77226) then creates 64 OCCLUSION queries (0x143025f60) and 6 TIMESTAMP_DISJOINT queries
(0x14302ac78, stride 0x50) and creates `Data/ShaderCache`.

#### R6.5 BSGraphics::TriShape ownership

Layout (CommonLib `RE/N/NiSkinPartition.h`, confirmed by 75473):

| Offset | Field                                                            |
| ------ | ---------------------------------------------------------------- |
| 0x00   | `ID3D11Buffer* vertexBuffer`                                     |
| 0x08   | `ID3D11Buffer* indexBuffer`                                      |
| 0x10   | `VertexDesc` (8 bytes)                                           |
| 0x18   | `volatile uint32 refCount` (starts at 1)                         |
| 0x20   | `uint8* rawVertexData` (CPU copy, **kept** for the shape's life) |
| 0x28   | `uint16* rawIndexData` (CPU copy, kept)                          |

Every `BSGeometry` holds one in `rendererData` (BSGeometry +0x138 on SE, CommonLib runtime-data +0x18). Skin partitions hold
one per partition (`Partition::buffData`, +0x48).

Geometry calls go through a static facade object (SE 0x141e10a50, pointer in 0x1430136c0; AE 0x142035cd0, pointer in
0x143274040; vtable SE 0x14186bf80 / AE 0x141ac5610). Each slot is a thunk that loads `Renderer` and jumps to:

| Slot | SE ID / addr         | AE ID / addr                    | Role                                                                                                                                                                                                                                                                                                                               |
| ---- | -------------------- | ------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0x10 | 75473 / 0x140d6b8d0  | 77259 / 0x140e45d30             | `CreateTriShape(NiStream, vertexDesc, vertexCount, indexCount)`: allocates the 0x30 struct, reads the vertex/index data from the stream into the raw copies, `CreateBuffer` VB (DEFAULT, bind VERTEX, initial data = raw copy) and IB (DEFAULT, bind INDEX). The device is `Renderer + 0x48` (RendererData device), not the global |
| 0x18 | 75476 / 0x140d6be60  | 77262 / 0x140e462c0             | Create with an existing vertex source (FUN_140d4e060) and a new IB with initial data **(inferred: dynamic/skinned shapes)**                                                                                                                                                                                                        |
| 0x20 | 530710 / 0x140d6c310 | 0x140e46800 (no AE ID resolved) | IncRef **(inferred)**                                                                                                                                                                                                                                                                                                              |
| 0x28 | 75480 / 0x140d6c320  | 77267 / 0x140e46810             | `ReleaseTriShape`: atomic decrement at +0x18; at 0 releases IB then VB, frees both raw copies, frees the struct. BSTriShape's destructor calls it                                                                                                                                                                                  |
| 0x30 | 75489 / 0x140d6cab0  | 77276 / 0x140e47050             | draw (R1's range)                                                                                                                                                                                                                                                                                                                  |
| 0x38 | 75477 / 0x140d6bfe0  | 77263 / 0x140e464d0             | draw (R1's range; Bink uses it)                                                                                                                                                                                                                                                                                                    |
| 0x48 | 75481 / 0x140d6c3a0  | 77268 / 0x140e46890             | create from stream for geometry type 4 (BSTriShape LoadBinary branches on `this[0x150] == 4`)                                                                                                                                                                                                                                      |

Lifetime: creation runs wherever the NIF loads (BSResource model loader threads, or the main thread for synchronous loads).
Release runs on whichever thread drops the last geometry reference: cell unload on the main thread, failed loads on loader
threads. Nothing synchronizes release against the GPU; D3D11's deferred destruction covers it today. A tripwire buffer
must therefore accept `Release` from any thread and must not free GPU memory until the frames that used it retire.

#### R6.6 Textures

| Function                                  | SE ID / addr                             | AE ID / addr                             | Notes                                                                                                                    |
| ----------------------------------------- | ---------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| BSTextureDB load (EntryDB DBTraits vfunc) | 101532 / 0x141328370                     | 108531 / 0x141515810                     | Opens a `BSResourceNiBinaryStream` and calls 75509; stores the result at entry +0x48                                     |
| CreateTextureFromDDS                      | 75509 / 0x140d6dd70                      | 77301 / 0x140e484c0                      | Calls 75721 with the global device; sets texture +0x20 = 1                                                               |
| DDS loader (DDSTextureLoader-style)       | 75721 / 0x140d7dc50                      | 77533 / 0x140e59520                      | Parses the header, then 75724                                                                                            |
| CreateD3DResources                        | 75724 / 0x140d7dff0                      | 77538 / 0x140e598c0                      | `CreateTexture2D` (slot 5) with initial data, then `CreateShaderResourceView`. CreationEngineRaytracing detours this one |
| `BSGraphics::Texture` destructor          | 75527 / 0x140d6ec70                      | 77322 / 0x140e49990                      | from CERT prior art                                                                                                      |
| CreateRenderTarget / CreateDepthStencil   | 75467 / 0x140d6a870, 75469 / 0x140d6aca0 | 77253 / 0x140e44d90, 77255 / 0x140e45180 | used by `BSShaderRenderTargets::Create`                                                                                  |
| Renderer::Map/Unmap texture               | 75514 / 0x140d6e4c0, 75515 / 0x140d6e510 | not resolved                             | `context->Map(res, 0, type, 0, &mapped)` / `Unmap`, used by Bink                                                         |

The EntryDB load runs on BSResource's queued-load threads for streamed textures, and on the requesting thread for blocking
loads **(inferred: the vfunc is reached only through the EntryDB vtable, no direct callers)**. A one-run thread-ID log
on 75724 and 75473 would settle it.

#### R6.7 Bink

`FUN_1410a1210` (SE 87890; AE 90259 / 0x141195ca0), reached from 87887 (0x1410a0e40) and 87920 (vtable), with players
called from 0x1405af130 (Main), 0x140301360, 0x1409736a0 and 0x1410a3070:
`BinkWait` / `BinkDoFrame`, then `FUN_1410a2840` (Map the plane textures, `BinkCopyToBufferRect`, Unmap). Then
`SetRenderTarget(0, kFRAMEBUFFER)`, depth MAIN, ApplyState, `Begin(0)` unless one is already open (MoviePlayer vfunc
0x58), the quad draw `FUN_1410a2370` (its own VS/PS, `FUN_140d6bfe0` draws), then `End` (Present) unless mode 2.
Bink frames therefore Present on their own, outside the main frame, on the main thread (or on the loading thread through
`WaitForSequenceToEnd` **(inferred)**).

---

### R7. Swap chain and window

#### R7.1 Function table

| Function                                | SE ID / addr                 | AE ID / addr        | Notes                                                                                               |
| --------------------------------------- | ---------------------------- | ------------------- | --------------------------------------------------------------------------------------------------- |
| `Renderer::Init`                        | 75445 / 0x140d68dd0          | 77226 / 0x140e423e0 | Copies the window properties into RendererData, calls InitD3D (inlined on AE), queries, state reset |
| InitD3D wrapper                         | 75595 / 0x140d72810          | inlined in 77226    | CS's `RelocationID(75595, 77226) + (0x50, 0x2BC)` is the `Init::InitD3D` call inside it             |
| CreateDeviceAndSwapChain                | 75589 / 0x140d718d0          | 77396 / 0x140e4c830 | the only `D3D11CreateDeviceAndSwapChain` caller                                                     |
| CreateSwapChainViews(window)            | 0x140d71d20 (no ID resolved) | 77397 / 0x140e4cc50 | GetBuffer, RTV, SRV, sizes                                                                          |
| Main init that installs the RT callback | 35548 / 0x1405ae010          | 36547 / 0x140640120 | writes `0x14302c868 = BSShaderRenderTargets::Create` (SE 100458 / AE 107175)                        |
| `Renderer::CreateSwapChain`             | 75457 / 0x140d69ea0          | 77242 / 0x140e44360 | extra windows; no direct callers                                                                    |
| `Renderer::Begin`                       | 75460 / 0x140d6a0c0          | 77245 / 0x140e44590 |                                                                                                     |
| `Renderer::End`                         | 75461 / 0x140d6a2b0          | 77246 / 0x140e44770 | **the only engine Present**                                                                         |
| `Renderer::WindowSizeChanged`           | 75453 / 0x140d698a0          | 77238 / 0x140e43bc0 | ResizeBuffers                                                                                       |
| `Renderer::ResetWindow`                 | 75454 / 0x140d69b70          | 77239 / 0x140e44050 | fullscreen and ResizeTarget                                                                         |
| `Renderer::ResizeWindow`                | 75449 / 0x140d695b0          | 77234 / 0x140e433a0 | no direct callers                                                                                   |
| `Renderer::RequestWindowResize`         | 75450 / 0x140d69620          | 77235 / 0x140e43410 | from the WndProc                                                                                    |
| `Renderer::SetWindowPosition`           | 75448 / 0x140d69590          | 77233 / 0x140e43380 | from the WndProc                                                                                    |
| `Renderer::SetWindowActiveState`        | 75451 / 0x140d69650          | 77236 / 0x140e43440 | from the WndProc                                                                                    |
| `Renderer::KillWindow`                  | 75452 / 0x140d696b0          | 77237 / 0x140e434a0 | no direct callers                                                                                   |
| `Renderer::UpdateViewPort`              | 75455 / 0x140d69d00          | 77240 / 0x140e441c0 | from SetRenderTarget, StartAccumulating, cube faces, ImageSpace, UI                                 |
| `Renderer::Shutdown`                    | 75447 / 0x140d69140          | 77228 / 0x140e42e10 | from 35552 / 0x1405af560 (AE 36551 / 0x140641c60)                                                   |
| WindowProc                              | 35635 / 0x1405b8890          | 36649 / 0x14064c880 |                                                                                                     |
| Main frame (Begin..End)                 | 35556 / 0x1405b1020          | 36555 / 0x140643c00 | callers: 35565 (AE 36564), 0x140153f80, 0x14026ba80                                                 |

Globals: device SE 0x143025f08 / AE 0x143286a10 (`Renderer::GetDevice`); RendererData* SE 0x143025f00 / AE 0x143286a08;
current window* SE 0x143025f10 / AE 0x143286a18; immediate context SE 0x143027ea0 / AE 0x1432887b0; `IDXGIOutput*` of adapter
output 0, kept for the process: SE 0x143027e98 / AE 0x143288798.

#### R7.2 CreateDeviceAndSwapChain (75589 / 77396) in detail

1. `CreateDXGIFactory(IID_IDXGIFactory)`: **DXGI 1.0** (IID 7b7166ec-21c7-44ae-b21a-c9ae321ae369).
2. `factory->EnumAdapters(RendererData.uiAdapter (+0x00))` (slot 7). This is the INI adapter index, from
   `RendererInitOSData + 0x28`.
3. `adapter->EnumOutputs(0, &g_output)` (slot 7). Only output 0 is used.
4. Windowed (`RendererData + 0x20 == 0`): refresh = 0/1. Fullscreen: `output->GetDisplayModeList(R8G8B8A8_UNORM,
DXGI_ENUM_MODES_INTERLACED)` (slot 8), capped at 1000 modes. Among modes matching the State screen size
   (0x14302c8bc/0x14302c8c0), it picks the refresh rate closest to the desired one (`+0x04` / `+0x08`, the INI refresh
   rate over 1). A mode with Scaling != 0 sets `scaleMode (+0x14) = STRETCHED`. No match: falls back to windowed (`+0x20 = 0`).
5. `DXGI_SWAP_CHAIN_DESC`: width and height = screen size; refresh as chosen; Format **R8G8B8A8_UNORM (0x1c)**; scanline
   ordering `+0x18`; scaling `+0x14`; SampleDesc 1/0; BufferUsage **0x30** (SHADER_INPUT | RENDER_TARGET_OUTPUT);
   **BufferCount 2**; OutputWindow = `renderWindows[0].hWnd` (`+0x48`); Windowed = !fullscreen; **SwapEffect DISCARD (0)**;
   Flags **ALLOW_MODE_SWITCH (2)**.
6. `D3D11CreateDeviceAndSwapChain(adapter, UNKNOWN, NULL, flags = debugLayer ? DEBUG : 0, NULL, 0, SDK 7, &desc,
&renderWindows[0].swapChain (+0x60), &device (+0x38), &featureLevel, &context (+0x40))`. No feature-level list (the
   runtime default). CS's `hk_D3D11CreateDeviceAndSwapChainUpscaling` (src/Features/Upscaling.cpp, IAT patch plus
   call-site redirect) rewrites it: FLIP_DISCARD, BufferCount >= 2, non-sRGB or R10G10B10A2 for HDR, feature level 11_1, and
   routes to DXVK's export when DXVK is loaded.
7. Fullscreen: `swapChain->ResizeTarget(&desc.BufferDesc)` (slot 14).
8. Debug device with the INI flag: `ID3D11InfoQueue` SetBreakOnSeverity(CORRUPTION, ERROR, optionally WARNING) and a deny
   filter for one message ID (0x161).
9. Sets the globals (current window = window 0, device, context).
10. `factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER)` (3): **DXGI never handles
    Alt+Enter**; the engine does fullscreen itself.
11. Releases the adapter and the factory.

Then InitD3D: if `RendererInitOSData.unk2C != 0`, `CreateSwapChainViews(0)` and copy window 0's RTV/SRV into kFRAMEBUFFER.
Then the `0x14302c868` callback (`BSShaderRenderTargets::Create`, which creates **every** engine render target and depth
stencil), then the state objects, CB pools and ring, and `SetMaximumFrameLatency(2)`.

#### R7.3 Back-buffer objects the engine stores

- `RendererWindow` (0x50 bytes, `renderWindows[32]` at RendererData +0x48): hWnd +0x00, x/y +0x08/+0x0C, width/height
  +0x10/+0x14, **swapChain +0x18**, swapChainRenderTarget +0x20, +0x28 zeroed by CreateSwapChainViews, **RTV +0x30**,
  **SRV +0x38**, +0x48 is copied into kFRAMEBUFFER's UAV slot on resize (null for a swap chain buffer **(inferred)**).
- `CreateSwapChainViews(i)`: `swapChain->GetBuffer(0, IID_ID3D11Texture2D)` (slot 9), `CreateRenderTargetView(tex, NULL)`
  into +0x30, `CreateShaderResourceView(tex, NULL)` into +0x38, `tex->GetDesc`. Window 0 or 1 sets the State screen and
  render sizes (0x14302c8b4..c0); other windows only grow them. Calls `RenderTargetManager` 75637 (0x140d74b40) with
  {w, h, R8G8B8A8_UNORM}, then **releases the texture**. The engine keeps only the views; the views keep the buffer alive.
- `kFRAMEBUFFER` = `renderTargets[0]` (RendererData +0xA48): its texture pointer is never set here **(inferred: null)**; RTV
  (+0xA58) and SRV (+0xA60) are pointed at the current window's views at init, by **every `Renderer::Begin`**, and after a
  resize. The UAV (+0xA70) is set from window +0x48 after a resize.

#### R7.4 Begin / End (per frame)

`Begin(window)`:

1. `swapChain[0]->GetFullscreenState(&fs, NULL)` (slot 11).
2. If a resize was requested (`Renderer + 0x34`): calls the resize callback `Renderer + 0x08` if set, then
   `WindowSizeChanged(0)`, stores the requested size, then `ResetWindow`. Otherwise, if the app wants fullscreen, the swap
   chain is not fullscreen and our window is in the foreground: `ResetWindow` (re-enters fullscreen after Alt+Tab).
3. `0x14302c8e0 = 1` (in-frame flag), the TAA jitter update (CS hooks this call at +0xE5 / AE +0xE2; +0x133 on GOG),
   **`context->ClearState()`** (slot 110), `RendererShadowState` flags reset (0x140d73bd0).
4. `VSSetConstantBuffers(12, 1, &cb_0x2d0)` and `PSSetConstantBuffers(12, 1, &cb_0x2d0)`: the per-frame CB
   (0x143027e88). `SetPerFrameBuffers` (0x140d70290) fills it.
5. Current window = window `p0` if it has a swap chain, else 0. kFRAMEBUFFER RTV/SRV = its views.
6. `GetClientRect` (0 falls back to the screen size), then RenderTargetManager 75637 with the size and R8G8B8A8_UNORM.

`End`: `FUN_14133a7b0(1)`, an optional 60 FPS sleep (`SleepIfFasterThan60FPS` under 0x141e07128),
**`currentWindow->swapChain->Present(RendererData.presentInterval (+0x30), 0)`** (slot 8), timing, frame counter
0x14302c8dc, `0x14302c8e0 = 0`, `FUN_14133a750(1)`.

The engine never calls ResizeBuffers or SetFullscreenState from inside a frame. A resize happens at the next `Begin`.

#### R7.5 Resize, fullscreen, Alt+Tab, kill

- **WindowProc (35635 / 36649)**: `WM_ACTIVATE` (6) calls `SetWindowActiveState(false/true)`. `WM_WINDOWPOSCHANGED`
  calls `SetWindowPosition(0, x, y)` and `RequestWindowResize(cx, cy)` (it only sets the flag at +0x34 and the size
  +0x38/+0x3C, and only when windowed and the size differs). `WM_SYSCOMMAND` blocks SC_SCREENSAVE and SC_MONITORPOWER.
  `WM_NCACTIVATE` (0x86) and 0x105 have their own handling.
- **WindowSizeChanged(i)**: `GetWindowRect`, then `GetFullscreenState`. If the size changed (or `+0x2C` forces it):
  `ClearState` + `Flush`, release window i's RTV/SRV/+0x48 and kFRAMEBUFFER's views, then **release every render target
  (all 0x72 = 114 `RenderTargetData`: texture, RTV, SRV, copy texture, copy SRV) and every depth stencil (12
  `ZeroDepthStencilData`)**, `Flush`, **`ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, ALLOW_MODE_SWITCH)`** (slot 13),
  `CreateSwapChainViews(i)`, then **`BSShaderRenderTargets::Create` again through 0x14302c868**, kFRAMEBUFFER views,
  `UpdateViewPort`, store the new size. CS hooks `BSShaderRenderTargets::Create` (100458/107175), so its hooks run again
  on every resize.
- **ResetWindow(i)**: `GetClientRect`; `swapChain->GetDesc` (slot 12); if the buffer size differs from the client size it
  calls `ResizeTarget(client size, R8G8B8A8_UNORM, scaling, scanline)` (slot 14). `GetFullscreenState`; if the app wants
  fullscreen, the chain is not fullscreen and the window is in the foreground: **`SetFullscreenState(TRUE, NULL)`**
  (slot 10), then ResizeTarget. Then `ShowWindow(SW_SHOWNORMAL)` unless minimized, and for windowed
  `SetWindowPos(HWND_NOTOPMOST, NOSIZE|NOMOVE)`.
- **SetWindowActiveState(active)** (fullscreen only): deactivate calls **`SetFullscreenState(FALSE, NULL)`** and
  `ShowWindow(SW_MINIMIZE)`. Activate calls `ShowWindow(SW_RESTORE)`, and the next `Begin` re-enters fullscreen through
  `ResetWindow`.
- **KillWindow(i)**: `OMSetRenderTargets(0, NULL, NULL)`, release the window views and kFRAMEBUFFER's, reset the shadow
  state's SRV slots, ApplyState. For window 0 it also releases all render targets and depth stencils. Then
  `swapChain->Release()` and `DestroyWindow`.
- *_CreateSwapChain(hwnd_, setCurrent)**: `device->QI(IDXGIDevice)` -> `GetParent(IDXGIAdapter)` ->
  `GetParent(IDXGIFactory)`, then `factory->CreateSwapChain` (slot 10) into the first free `renderWindows[i]`, with the same
  desc as the main one (R8G8B8A8, usage 0x30, 2 buffers, ALLOW_MODE_SWITCH, Windowed = !fullscreen). Then
  `CreateSwapChainViews` if setCurrent, and `MakeWindowAssociation(hwnd, 3)`. No direct callers found (not used by vanilla
  flat **(inferred)**).
- **Shutdown**: ClearState, Flush, releases the ring buffers and queries, static IB/VB, state objects, then targets.

#### R7.6 Every IDXGI* call, by caller

| Interface method (slot)                               | Caller(s)                                                                  |
| ----------------------------------------------------- | -------------------------------------------------------------------------- |
| `IDXGIFactory::EnumAdapters` (7)                      | CreateDeviceAndSwapChain                                                   |
| `IDXGIFactory::MakeWindowAssociation` (8), flags 3    | CreateDeviceAndSwapChain, CreateSwapChain                                  |
| `IDXGIFactory::CreateSwapChain` (10)                  | CreateSwapChain                                                            |
| `IDXGIAdapter::EnumOutputs` (7)                       | CreateDeviceAndSwapChain                                                   |
| `IDXGIOutput::GetDisplayModeList` (8)                 | CreateDeviceAndSwapChain                                                   |
| `IDXGIDevice1::SetMaximumFrameLatency(2)` (12)        | InitD3D                                                                    |
| `IDXGISwapChain::Present(interval, 0)` (8)            | `Renderer::End` only (called by the main frame, the loading loop and Bink) |
| `IDXGISwapChain::GetBuffer(0)` (9)                    | CreateSwapChainViews                                                       |
| `IDXGISwapChain::SetFullscreenState` (10)             | ResetWindow (TRUE), SetWindowActiveState (FALSE)                           |
| `IDXGISwapChain::GetFullscreenState` (11)             | Begin, ResetWindow, WindowSizeChanged                                      |
| `IDXGISwapChain::GetDesc` (12)                        | ResetWindow                                                                |
| `IDXGISwapChain::ResizeBuffers(0,0,0,UNKNOWN,2)` (13) | WindowSizeChanged                                                          |
| `IDXGISwapChain::ResizeTarget` (14)                   | CreateDeviceAndSwapChain (fullscreen), ResetWindow                         |
| `IDXGISwapChain::Release`                             | KillWindow                                                                 |

CS adds: `IDXGISwapChain::Present` vtable detour (slot 8) installed after InitD3D (src/Hooks.cpp
`BSGraphics_Renderer_Init_InitD3D`), and HDRDisplay's Present hooks below it.

---

### R8. Engine D3D11 work around DCLF's epochs

Deferred clears: `SetRenderTarget` / `SetCurrentDepthStencilTarget` / `SetCurrentCubeMapRenderTarget` with mode
SRTM_CLEAR (0), CLEAR_DEPTH (1) or CLEAR_STENCIL (2) only record the request. **`Renderer::ApplyState` (75462 /
0x140d6a330, AE 77247 / 0x140e44850) and `SetDirtyStates` (75580/77386) do the clears**: `ClearRenderTargetView`
(slot 50, 0x190) per RT, `ClearDepthStencilView` (slot 53, 0x1a8), then `OMSetRenderTargets` (slot 33) and the
depth-stencil/raster state. So "where a target is cleared" is the next ApplyState or draw after the bind.

Helper IDs used below: SetClearColors SE 75463 / AE 77249; SetClearColorFromArray 75464 / 77250; SetRenderTarget 75646 /
77453; SetCurrentDepthStencilTarget 75647 / 77454; SetCurrentCubeMapRenderTarget 75648 / AE 77455 **(inferred, the next
ID)**; `Renderer::SubmitAccumulator` (DCLF's "accumulator render") 99789 / 0x1412c15c0, 106436 / 0x1414a90f0;
`StartAccumulating` 99790 / 0x1412c1600; `ClearDepthStencil(flags)` 75466 / 0x140d6a810, 77252 / 0x140e44d30.

Depth-stencil enum indices seen: 0 kMAIN, 4 kSHADOWMAPS, 5 kDECAL_OCCLUSION, 6 kCUBEMAP_REFLECTIONS,
7 kPOST_ZPREPASS_COPY. Render target 0x10/0x11 = kLOCAL_MAP / kLOCAL_MAP_SWAP, 7 = kMOTION_VECTOR.

#### R8.1 Table

| Site                                                                                         | SE ID / addr                                                                           | AE ID / addr                                                              | D3D11 work                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | DCLF / CS hook around it                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| -------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `BSShadowLight::RenderShadowmap(light, descriptor, count*, flags)`                           | 100820 / 0x141305610                                                                   | 107604 / 0x1414f0cf0                                                      | Releases the light's last-frame object array. A descriptor with target -1 gets target 4 (kSHADOWMAPS) and the lowest set bit of the free mask (AE 0x142035798, DCLF `kShadowFreeMask`), which is cleared. `SetClearColors(1,1,1,1)`. **Only if `descriptor.clearRenderTarget` (+0xE8)**: `SetCurrentDepthStencilTarget(target (+0x54), SRTM_CLEAR_DEPTH, slice (+0x58))`, `SetRenderTarget(0, none, RESTORE)`, `ApplyState(false)` (the clear and bind happen here). Then `SubmitAccumulator(camera (+0x40), accumulator (+0x48), flags \| 0x400)`, restore the clear colour, and build the shadow projection (State 0x140d7bbc0 / AE 0x140e56e30). A descriptor that does not clear draws into whatever target was bound last                                                                                                                                                                                             | DCLF does not hook RenderShadowmap. It takes `BSShaderAccumulator` vfunc 0x2A (`FinishAccumulatingPreResolveDepth`, src/Features/DrawcallLimitFix.cpp) and runs `CaptureShadowView` for render modes 0xD-0xF **after the native draws**, inside SubmitAccumulator, with the slice bound and cleared. `ShadowViews::PredictTargets` mirrors the slice assignment. Accumulate vfuncs (0x9) and culling are hooked in Features/Shadows. `EngineFixes/ShadowmapCascadeRasterizerFix` also touches this path |
| Directional cascades                                                                         | (BSShadowDirectionalLight::Render)                                                     |                                                                           | Each cascade into target 2 at its index, after the volumetric copy (target 3) when AE dword 0x142033498 == 2 (from DCLF ShadowViews.cpp; not re-traced)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | as above                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `BSCubeMapCamera::RenderFaces(cam, faceMask, silhouettes, clearRoots, noSky)`                | 100780 / 0x141302370 (vfunc 0x35 thunk 100778 / 0x141301f80)                           | 107564 / 0x1414ed920 (thunk 107562 / 0x1414ed4e0)                         | For each face bit: `SetCurrentCubeMapRenderTarget(kREFLECTIONS cube 0, SRTM_CLEAR, face)`, `UpdateViewPort(0, 0, true)`, `SetCurrentDepthStencilTarget(6 kCUBEMAP_REFLECTIONS, SRTM_CLEAR, 0)`, `ApplyState(false)` (clears both), depth mode 3, `BSCubeMapCamera::SetFace` (100779 / 107563, DCLF "Orient"). Accumulator +0x1A0 gets render mode 0, 0x19 (no sky) or 0x1B (silhouettes) and the camera position; raster/stencil mode 2; `SubmitAccumulator(cam, acc, 8)`; mode 1. After all faces: `SetCurrentCubeMapRenderTarget(-1)`. Optionally releases the roots (`clearRoots`)                                                                                                                                                                                                                                                                                                                                      | DCLF Features/Reflection: `write_vfunc<0x35, FaceRender>` on VTABLE_BSCubeMapCamera (wraps the whole call; sets the view's accumulators +0x1A0/+0x1A8), `Orient` on the SetFace call (AE +0xE0 / SE +0xE5), `AccumulatorRender` on the SubmitAccumulator call (AE +0x2D8 / SE +0x2D6), and TESWaterReflections::Update's call (AE 32174 +0xC1C / SE 31383 +0x8A0)                                                                                                                                       |
| `Main::RenderDepth(firstPerson, controlDriven)`                                              | 100421 / 0x1412e3520                                                                   | 107139 / 0x1414ccb30                                                      | Start: `SetCurrentDepthStencilTarget(kMAIN, SRTM_CLEAR)`, RT0-2 none (RESTORE), cube none. `StartAccumulating(worldCam, depthAcc 0x1432333f8, 0x21)`, then the world-drawn call (+0x18E / +0x1AA). The first-person block culls and draws into the same accumulator with the first-person depth range (State 0x140d7c3e0), `SubmitAccumulator(fpCam, depthAcc, 3)`. End: **`CopyResource(kPOST_ZPREPASS_COPY.texture, depthStencils[kMAIN].texture)`** (SE call at 0x1412e3a43, AE at 0x1414cd56f; src = `Renderer + 0x1FB8 + i*0x98`), then raster mode 1 and depth mode 3                                                                                                                                                                                                                                                                                                                                                | CS: `Main_RenderDepth` thunk at Main::Draw's call (SE 35560 +0x395, AE 36559 +0x395, 1.7.104 +0x3B3; Terrain Blending's site). `Main_RenderDepth_WorldDrawn` at +0x18E / +0x1AA runs `FrameDriver::DepthCommitted` (DCLF's depth epoch, reflection faces, first-person depth) and dirties the RT state. Features/FirstPerson hooks the first-person draw at AE 107139 +0x94E / SE 100421 +0x427. DCLF's world depth is in kMAIN before the copy, so the copy includes it                                |
| `BSShaderAccumulator::FinishAccumulating` (render mode 0, called by vfunc 0x2A's dispatcher) | 99938 / 0x1412cacd0 (dispatcher 99936 / 0x1412cac20, table 0x1431d1c40 by render mode) | 106583 / 0x1414b2d90 (dispatcher 106581 / 0x1414b2ce0, table 0x14332b120) | Draws batch groups through `RenderBatches` and `RenderPersistentPassList` (the CS/DCLF `RenderPassImmediately` sites). Depth mode 4 around the opaque groups when the accumulator's +0x5C is set and not in the loading screen. **If flags bit 7 and passes 0x5C000071..0x5C006071 exist: `CopyResource(kPOST_ZPREPASS_COPY, kMAIN depth)` again** (SE 0x1412cb07e, AE 0x1414b312d), for the soft and decal passes **(purpose inferred)**. If passes 0x5C00006D..70 exist: `ClearDepthStencil(STENCIL)` (75466), clear colours 0, RT0 = NORMAL_TAAMASK_SSRMASK(_SWAP), RT1 = kMOTION_VECTOR, RT2-4 none, draw them, restore the clear colour. If flags & 0x40: unbind depth and RT1-4, `ApplyState(true)`, then an ImageSpace compute effect (100428 / 0x1412e4470; AE 107146 / 0x1414ce240; ImageSpaceManager +0x1C0 with `Dispatch`) **(the effect's identity not traced)**, then RT0 = kMAIN, depth kMAIN, depth mode 1 | CS/DCLF: vfunc 0x2A thunk (DCLF: occlusion maps for mode 0x1C, shadow views for 0xD-0xF; nothing for mode 0). LLF/DCLF `RenderPassImmediately` thunks at 100871 +0xEE / 107667 +0xED and 100852 +0x29E / 107642 +0x28F; CS `RelocationID(99938, 106583)` call sites at +0x8E/+0x84 and +0x319/+0x308. DCLF Forward hooks the blended draws (kBlendedUnderWater / kBlendedAfterWater)                                                                                                                    |
| `Renderer::Begin`                                                                            | 75460 / 0x140d6a0c0                                                                    | 77245 / 0x140e44590                                                       | `ClearState` (slot 110), the per-frame CB at VS/PS slot 12, kFRAMEBUFFER = window views (R7.4)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | CS Upscaling/HDR: the jitter call at +0xE5 / +0xE2 (+0x133 GOG). DCLF frames start at `Main_Draw_Early` (35560 / 36559 +0xD3), not at Begin. **ClearState wipes every binding each frame, before any DCLF work**                                                                                                                                                                                                                                                                                        |
| Decal occlusion pass in the frame                                                            | 35563 / 0x1405b29f0                                                                    | 36562 / 0x1406458a0                                                       | Camera from a pass, `SetRenderTarget(0, none, RESTORE)`, depth 5 (kDECAL_OCCLUSION, CLEAR when +0x31), begin/end around `SubmitAccumulator(cam, acc, 0)` **(occlusion query inferred from 0x140d70150/0x140d701b0)**                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | none seen                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Precipitation occlusion                                                                      | `Precipitation::RenderMask` / `RenderOcclusion`                                        | AE 26175 / 0x1404076c0 and RenderMask                                     | Accumulator renders under `Renderer::Lock`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | CS: `Main_Precipitation_RenderOcclusion` at 35560/36559 +0x3A1 (+0x3BF on 1.7.104) when Skylighting is off                                                                                                                                                                                                                                                                                                                                                                                              |

Main frame order (SE 35556): Lock, Begin, decal occlusion (35563), deferred release flush, `SetPerFrameBuffers`,
`Main::Draw` work (35565 / AE 36564 starts and waits the ServingThread job lists), ImageSpace (0x1412979e0), UI::Render,
screenshots (0x1405c0610), End, Unlock.

---

### R9. Menu 3D and map

The full list of `Renderer::SubmitAccumulator` callers is the full list of engine scene renders. SE: RenderMask, 35563
(decal occlusion), `RenderFirstPersonView` x2, `RenderDepth`, `RenderWorld`, the local map x2 (16094), `UI3DSceneManager::Render`,
`RenderFaces`, `RenderShadowmap`. AE adds 26175 (precipitation). **No other menu path draws world geometry.** Every one of
them goes through a BSShaderAccumulator, so every pass registers through `BSBatchRenderer::RegisterPass` (vfunc 2), which
DCLF's `PassCapture::Hook` takes globally.

| Path                                                             | SE ID / addr                                                                       | AE ID / addr                                                                       | How it renders                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | DCLF                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `UI3DSceneManager::Render(mgr, scheme, camera, keepDepth)`       | 51855 / 0x1408d2390                                                                | 52727 / 0x140972590                                                                | Its own accumulators (+0x10 colour, +0x18 depth/pre). Depth kMAIN SRTM_CLEAR, `SetPerFrameBuffers`, culls the menu scene (0x1412b9db0), `StartAccumulating(cam, acc+0x18, flags \| 0x20)`, then clear colour 0; RT1 = kMOTION_VECTOR CLEAR; RT0 = kMAIN CLEAR; depth kMAIN CLEAR (RESTORE if keepDepth); `SubmitAccumulator(cam, acc+0x10, flags \| 0x200)`; then a **mini ImageSpace chain**: 0x1412ee4a0 (effect 0x55), HDR/tonemap setup (0x141298260/350, 0x1412e76f0, `ImageSpaceManager` 0x141297410), and effect 0x22 (copy) to the framebuffer. Ends with depth kMAIN and ApplyState | Not a main renderer, so `WithholdMain`/`WithholdOwn`/shadow `Withhold` never take it. **But `WaitsForBaseMips` applies to every batch renderer**: with texture streaming on (not full mode), a BSLightingShader pass whose diffuse or normal has no base window is withheld here too, so menu items can draw late. LinearLighting detours this function (src/Features/LinearLighting.cpp, `menuAddress = RelocationID(51855, 52727)`) |
| `Inventory3DManager::Render`                                     | 50882 / 0x140887750                                                                | 51755 / 0x140927560                                                                | Updates the item, then `UI3DSceneManager::Render(mgr, this+0x34, NULL, 0)`. Called from menus' PostDisplay (Inventory, Barter, Container, Gift, Magic, crafting, ...)                                                                                                                                                                                                                                                                                                                                                                                                                        | as above                                                                                                                                                                                                                                                                                                                                                                                                                              |
| MistMenu::PostDisplay (main menu background and loading screen)  | 51449 / 0x1408ad6f0                                                                | 52308 / 0x14094de10                                                                | `UI3DSceneManager::Render(mgr, 4, NULL, keep)`. LoadingMenu sets scheme 4, so the **loading-screen model renders here on the ServingThread**                                                                                                                                                                                                                                                                                                                                                                                                                                                 | as above                                                                                                                                                                                                                                                                                                                                                                                                                              |
| LockpickingMenu / StatsMenu / BookMenu / JournalMenu PostDisplay | 51072 / 0x140896530, 51639 / 0x1408c07f0, 50120 / 0x140858140, 52420 / 0x1408f4100 | 51951 / 0x140936ef0, 52511 / 0x140960c90, 51051 / 0x1408f1260, 53317 / 0x140994bc0 | `UI3DSceneManager::Render` (lockpick model, perk constellations, 3D book, journal book)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | as above                                                                                                                                                                                                                                                                                                                                                                                                                              |
| UI::Render (all menus)                                           | 79947 / 0x140ebf510                                                                | 82084 / 0x140fa4f00                                                                | `SetPerFrameBuffers`, RT1 = kMOTION_VECTOR, RT2 = mask (or none when RT0 is the framebuffer), depth kMAIN, ApplyState, Scaleform begin (0x140edb830), each menu's `PostDisplay` (vfunc 0x30) with `UpdateViewPort(screen)`, Scaleform end (0x140edb850), then effect 0x22 to the framebuffer if RT0 is not it. Scaleform uses its own D3D11 HAL with the device/context from `GetScaleformRendererParameters` (0x140d70400)                                                                                                                                                                  | CS `MenuManagerDrawInterfaceStart` detour on 79947/82084                                                                                                                                                                                                                                                                                                                                                                              |
| Local map render                                                 | 16094 / 0x1401f5b10                                                                | 16335 / 0x140242e00 (from 52967 / 0x14097bf20, LocalMapMenu)                       | `BSTreeManager` LOD toggle, clear colour 0, depth kMAIN SRTM_CLEAR, RT0 = **kLOCAL_MAP_SWAP (0x11) CLEAR**, `SubmitAccumulator(localMapCam (+0x302B8), acc (+0x302C8), 0)`. Then render mode 0x13 (the water and second pass), RT0 RESTORE, Submit again. Unbind depth, then ImageSpace effect **0x62 = BSImagespaceShaderLocalMap** (`Func1` SE 0x1412b6110 / AE 0x14149d470) from 0x11 into **kLOCAL_MAP (0x10)** with the fog-of-war parameters (+0x302D0)                                                                                                                                | Its accumulator is not a main renderer; DCLF does not claim it (only `WaitsForBaseMips` applies). The LOD-type `GetRenderPasses_LocalMap` variants (Grass 0x1414afb00, Water 0x1414c08e0 on AE) pick the passes                                                                                                                                                                                                                       |
| World map (MapMenu)                                              | (Main::RenderWorld with the map camera)                                            |                                                                                    | The normal main world render with WorldMap shader permutations **(inferred; no separate SubmitAccumulator caller exists)**                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | DCLF runs: GeometryPort writes the world-map rows for WorldMap passes (Core/Shading/GeometryPort.*). Skylighting's integration checks `isMapMenuOpen`                                                                                                                                                                                                                                                                                 |
| RaceSexMenu (RaceMenu 3D)                                        | (main world render)                                                                |                                                                                    | The player is drawn by the normal world render with the RaceSex camera **(inferred; no menu render path)**                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | DCLF's normal frame applies (the player's first-person/third-person roots as usual)                                                                                                                                                                                                                                                                                                                                                   |

Answer to "does DCLF claim menu geometry": **no**, except that the texture-streaming base-mips gate in
`PassCapture::Hook` withholds any lighting pass in any batch renderer while its base mips are missing. DCLF also stops
claiming entirely during loading screens (`FrameView::IsLoadingScreenUp()`).

---

### R12. AE 1.6.1170 coverage and 1.7.104

All AE addresses above came from the AE database and were checked in the AE program, except:

- `SetCurrentCubeMapRenderTarget` AE 77455: inferred as the next ID after 77454.
- IncRef slot 0x20, AE 0x140e46800: address checked, ID not in the database.
- `CreateSwapChainViews` SE 0x140d71d20 has no SE ID in the database (AE 77397 exists).
- Renderer Map/Unmap texture (SE 75514/75515): AE not resolved.
- AE's InitD3D is inlined into `Renderer::Init` (77226); the SE-only ID 75595 has no AE twin.

1.7.104 is not loaded: every ID here is **unverified on 1.7.104**. Repo hints: CS uses +0x3B3/+0x3BF for 1.7.104 and GOG
at Main::Draw's depth and precipitation calls, and +0x133 for the Begin jitter call; DCLF's `Offset::ae17` overrides exist
for some sites.

---

### Open questions

1. **Loading-screen threading.** The loading loop presents from the ServingThread under `Renderer::Lock`, and it runs
   Scaleform, UI3DSceneManager (BSShaderAccumulator, BSBatchRenderer, ImageSpace) and Present there. If ORG owns Present,
   either ORG submission and Present must be safe from that thread (serialized by the same lock), or the loop must be
   moved to the main thread, which the engine does not support (the main thread is blocked in the load). Confirm with a
   thread-ID log on `Renderer::End`.
2. Which threads run the BSTextureDB / BSModelDB loads (101532/108531 and 75473/77259)? Inferred to be BSResource loader
   threads plus synchronous main-thread loads. One logging run settles it.
3. Does Bink ever present on the ServingThread (`MoviePlayer::WaitForSequenceToEnd` at the loop's start)?
4. `WindowSizeChanged` destroys and recreates **every** engine render target and depth stencil through the
   `BSShaderRenderTargets::Create` callback. Tripwire handles in `RenderTargetData` must survive Release + recreate, and
   anything caching the old ID3D11 pointers (DCLF imports, CS features) must re-import.
5. Fullscreen is exclusive-mode DXGI (DISCARD, ALLOW_MODE_SWITCH, SetFullscreenState on Alt+Tab via WM_ACTIVATE). CS
   already forces FLIP_DISCARD. With BasicRHI owning a Vulkan swap chain, the engine's `SetFullscreenState`,
   `ResizeTarget`, `GetFullscreenState`, `GetDesc` and `ResizeBuffers` calls need a stub swap chain object that answers
   them consistently (borderless fullscreen recommended).
6. kFRAMEBUFFER has views but no texture pointer **(inferred)**. Confirm before a tripwire assumes `texture != null`
   (SaveRenderTargetToFile 75522 on kFRAMEBUFFER would show it).
7. The ImageSpace compute effect inside FinishAccumulating under flags & 0x40 (SE 100428 / AE 107146): identify it
   (R4's lane).
8. The second `CopyResource` into kPOST_ZPREPASS_COPY inside FinishAccumulating (flags bit 7, passes 0x5C000071..): which
   pass groups those IDs are, and whether DCLF-drawn depth that lands after RenderDepth is in it.
9. `Renderer + 0x08` (the resize callback Begin calls) and `Renderer + 0x2C` (force-resize flag): who sets them?
10. AE `BSTaskPool_HandleTask` takes `Renderer::Lock` (0x14065880d): what task, and on which thread?
