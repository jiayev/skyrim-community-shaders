# Skyrim AE texture bookkeeping: from file load to the last D3D11 release

Findings from the Ghidra database of `SkyrimSE.exe` 1.6.1170 (AE), gathered on 2026-10-28 while chasing textures that stay
alive with Drawcall Limit Fix (DCLF) enabled. Addresses are AE runtime addresses with image base `0x140000000`.
Address Library IDs are AE IDs (SE IDs only where CommonLib gives them). Names such as `FUN_…`, `DAT_…` and
`_LAB_…` are Ghidra's; the meanings given are inferred from the decompilation and the call sites named next to them.
Where something is inference rather than read from the code, it says so.

Companion to [skyrim-engine-notes.md](skyrim-engine-notes.md) (see "The material database" there).

## Summary

```
BSShaderManager::GetTexture (105640)            material / effect / sky / water code asks for "textures\..."
  └ BSTextureDB demand (76004..76008, 443433)   BSResource::EntryDB<BSTextureDB::DBTraits>, entry per file
      └ DBTraits load (76013 -> 76018)          entry->data = NiSourceTexture::Create (70716)
          └ BSShaderResourceManager vfunc 0xd0 (0x141515810, 108531)   [EngineFixes replaces this slot]
              └ Renderer::LoadTexture (77301)                           Texture.refCount = 1
                  └ DDS loader (77533 -> 77539 -> 77538)                allocates BSGraphics::Texture (0x28),
                                                                         CreateTexture2D, CreateShaderResourceView
```

Ownership, from the D3D11 objects up:

| Object | Holds | Field | Count that frees it |
| --- | --- | --- | --- |
| `BSGraphics::Texture` (0x28, no vtable) | `ID3D11Texture2D*` creation ref, SRV creation ref, UAV | `+0x00`, `+0x10`, `+0x08` | `+0x20` (u32), set to 1 at load |
| `NiSourceTexture` (0x58) | the one `BSGraphics::Texture` reference | `+0x48` | NiRefObject `+0x08` |
| `BSResource::Entry` of the texture DB (0x30) | one `NiPointer<NiTexture>` | `+0x28` | released by the DB's release queue only when the NiSourceTexture's count is 1 |
| Materials, effect/sky/water properties, defaults, decals, … | `NiPointer<NiSourceTexture>` | see below | their own counts |

Release is immediate below the NiSourceTexture: the last `NiSourceTexture` reference runs its destructor, which calls
`BSShaderResourceManager` vfunc `0xe0` -> `Renderer::ReleaseTexture` (77322), which decrements `Texture+0x20` and, at
zero, `Release()`s the SRV, the texture and the UAV and frees the block. There is no renderer-side deferred-release
queue. The deferral is above it, in three main-thread queues:

1. **The texture DB's release queue** (32 buckets): one bucket every 7 frames from `Main::Update`. An entry is freed only
   when nothing but the DB holds its NiSourceTexture; otherwise it is requeued and checked again 32 steps (~224 frames)
   later. This is the "~217 frames" ring.
2. **The model DB's release queue** and the facegen DBs, on other slots of the same 7-frame round robin.
3. **Deferred NiObject releases**: the GarbageCollector array and the deferred-release list at `0x142029d68`, drained
   each frame by the `Job_Garbage` frame job (and by `Main::Update` while in menu mode), fully every 7 frames.

So a texture outlives its last user only as long as something holds the `NiSourceTexture` (or, rarely, an extra
`BSGraphics::Texture` reference). The DB will not drop it while any other reference exists, and it does not track
who holds it.

## 1. The load path

The call stack in the DXVK trace (`+0xe5a0d5 < +0xe596ba < EngineFixes.dll+0x35b53 < +0xd2f11e < +0xe1084e < +0xe0f947`)
maps onto these functions:

| Return address | Function (AE) | ID | What it is |
| --- | --- | --- | --- |
| `0x140e0f947` | `FUN_140e0f840` | 76013 | `BSTextureDB::DBTraits` load callback: takes the entry's stream (`entry+0x20`), calls 76018 with `&entry->data` (`entry+0x28`) |
| `0x140e1084e` | `FUN_140e10790` | 76018 | DBTraits create: `NiSourceTexture::Create(stream, name, …)` and stores the result into `entry+0x28` (`NiPointer` assignment) |
| `0x140d2f11e` | `FUN_140d2ef80` | 70716 | `NiSourceTexture::Create` from a stream: allocates 0x58, ctor 70720, copies format prefs to `+0x10`, stores the stream at `+0x40` (stream refcount `+0x10` in steps of `0x1000`), name at `+0x20`, a `.ddx` flag (bit 5 of `+0x50`), then calls `[0x143274040]->vfunc 0xd0` |
| `EngineFixes.dll+0x35b53` | `Fixes::TextureLoadCrash::detail::BSShaderResourceManager_LoadTexture+0x103` | – | EngineFixes' replacement for that vfunc (below) |
| `0x140e596ba` | `FUN_140e59520` | 77533 | DDS loader from a `BSResourceNiBinaryStream` (DirectXTK `CreateDDSTextureFromMemory`-like): reads the file, checks `DDS `, header size `0x7c`, the `DX10` header, then 77539 |
| `0x140e5a0d5` | `FUN_140e59c00` | 77539 | validates dimensions/format/mips (`<= 0x4000`, arrays `<= 0x800`), builds the subresource data, calls 77538; if that fails with `E_INVALIDARG` (`0x80070057`) and the file has mips, it asks the device's feature level (vtable `+0x128`) for a maximum size and retries with the larger mips dropped |
| `0x140e59a44` | `FUN_140e598c0` | 77538 (`+0x184`) | allocates the `BSGraphics::Texture` and calls `ID3D11Device::CreateTexture2D` (vtable `+0x28`, the call at `0x140e59a41`) into `Texture+0x00` |
| `0x140e59b97` | `FUN_140e598c0` | 77538 (`+0x2d7`) | `ID3D11Device::CreateShaderResourceView` (vtable `+0x38`, the call at `0x140e59b94`) into `Texture+0x10` |

`FUN_140e598c0` also handles 1D (`+0x20`) and 3D (`+0x30`) textures, and cube maps (the `bVar`/`param_9` path sets
`TEXTURECUBE` view dimensions). If the SRV fails it releases the texture through `+0x10` but leaks the 0x28 block.

### `BSShaderResourceManager` and `Renderer` texture functions

`_LAB_143274040` (ID 410530) holds the renderer resource manager. Its vtable at `0x141ac5610` (ID 255472, CommonLib
`VTABLE_BSShaderResourceManager`) has RTTI `BSShaderResourceManager` (an `IRendererResourceManager`). The texture
slots:

| Slot | Address | Forwards to | Meaning |
| --- | --- | --- | --- |
| `0xd0` | `0x141515810` (108531) | `Renderer::LoadTexture` `FUN_140e484c0` (77301) | `LoadTexture(NiSourceTexture*)`: wraps `tex+0x40` in a `BSResourceNiBinaryStream`, calls 77533 with the device (`[0x143286a10]`, ID 411348), sets `Texture+0x20 = 1`, stores the result at `NiSourceTexture+0x48` |
| `0xd8` | `0x141515880` | `0x140e49980` (77321) | `AddRefTexture`: `lock inc dword [Texture+0x20]` |
| `0xe0` | `0x1415158a0` | `FUN_140e49990` (77322) | `ReleaseTexture` (below) |
| `0xe8` | `0x1415158c0` | `FUN_140e49a00` (77323) | `GetTextureDimensions(NiSourceTexture*)`: reads them from the SRV's resource each time (`GetResource`, `GetDesc`), not from the Texture block |

No code reaches slot `0xd8` through `[0x143274040]` (every vfunc call after a load of that global was classified; none
uses `0xd8`), so extra `BSGraphics::Texture` references are rare in the engine itself.

`ReleaseTexture` (`FUN_140e49990`, 77322):

```
if (InterlockedDecrement(&tex->refCount /* +0x20 */) == 0) {
    if (tex->srv     /* +0x10 */) tex->srv->Release();
    if (tex->texture /* +0x00 */) tex->texture->Release();
    if (tex->uav     /* +0x08 */) tex->uav->Release();
    free(tex, 0x28);
}
```

Direct callers besides slot `0xe0`: the Bink video player (`FUN_141195f30` 90260, `FUN_1411965e0` 90275,
`FUN_141196dc0` 90276) and `FUN_1404355b0` (27041, a texture list guarded by the spin lock at `0x14313fa68`, next to
`ImageSpaceManager`; not traced further).

### The `BSGraphics::Texture` block (the owner of the D3D11 references)

Allocated by `FUN_140e598c0` with `NiMemoryAllocate(0x28)` (`0x14011bd00`, ID 10854), i.e. Skyrim's own
`MemoryManager` pools, not a CRT/process heap. Layout (matches CommonLib's `RE::BSGraphics::Texture`):

| Offset | Field | Value for a DDS loaded by the texture DB |
| --- | --- | --- |
| `+0x00` | `ID3D11Resource*` (here `ID3D11Texture2D*`) | creation reference |
| `+0x08` | UAV | `null` |
| `+0x10` | `ID3D11ShaderResourceView*` | creation reference |
| `+0x18` | height/width/mips/format (CommonLib) | **0**: `FUN_140e598c0` zeroes it and the load path never fills it |
| `+0x20` | `refCount` (u32) | 1 after `LoadTexture`; written as a 4-byte zero then 1 |
| `+0x24` | pad | not initialized (allocator garbage) |

So a world texture's block reads `{texture, 0, srv, 0, 1 | garbage<<32}`. A scan that expects dimensions at `+0x18`, or a
clean `+0x24`, will miss nearly all of them; that may explain the ~117 matches. Match on `+0x08 == 0`, `+0x18 == 0`,
`(u32)+0x20 >= 1` and the two D3D pointers instead, or better, reach them from the NiTexture list (below).

### EngineFixes' hook (`EngineFixes.dll+0x35b53`)

Symbolized with `EngineFixes.pdb` (`D:\Modlists\NordicSouls\mods\SSE Engine Fixes (skse64 plugin)`):
`Fixes::TextureLoadCrash::detail::BSShaderResourceManager_LoadTexture` replaces slot `0xd0`. It builds the same
`BSResourceNiBinaryStream`, calls `BSGraphics_Renderer_LoadTextureFromStream` (= 77533) with the device from REL::ID
411348, and on success writes `Texture+0x20 = 1` and `NiSourceTexture+0x48 = texture` exactly like vanilla
(`+0x49c..+0x4af`). On failure it clears `+0x48`, counts the failure (`TotalLoadFails`), and logs the file name (stream
vfunc `0x50`). It keeps no reference of its own.

### The NiTexture list

Every `NiTexture` is on a global doubly linked list, inserted by the NiSourceTexture constructor (`FUN_140d2f200`,
70720) and removed by the NiTexture destructor (`FUN_140d2f350`, 70722), under the critical section at `0x143274180`
(ID 410548):

- head `0x143274100` (ID 410544), tail `0x143274108` (ID 410545)
- `NiTexture+0x30` prev, `+0x38` next (CommonLib `NiTexture::prev/next`)

For a dump this is the complete inventory of live textures: walk from the head via `+0x38`, read the name at `+0x20`
(BSFixedString), the count at `+0x08`, `+0x48 -> BSGraphics::Texture`, and `+0x48 -> +0x10` for the SRV the DXVK trace
reports.

### `NiSourceTexture` (0x58)

| Offset | Field |
| --- | --- |
| `+0x00` | vtable `0x1419adbb8` (ID 237218) |
| `+0x08` | NiRefObject count |
| `+0x10` | `FormatPrefs` (`6, 3, 2` from the ctor) |
| `+0x20` | name (`BSFixedString`, the path under `textures\`) |
| `+0x28` | `0xffffff` from the ctor |
| `+0x30` / `+0x38` | NiTexture list prev / next |
| `+0x40` | `BSResource::Stream*` (refcount at `stream+0x10` in units of `0x1000`) |
| `+0x48` | `BSGraphics::Texture*` |
| `+0x50` | flags (bit 3 from `Create`'s argument, bit 5 `.ddx`; ctor sets `& 0xcc | 0xc`) |

Its vfunc 1 (`DeleteThis`, `0x140d27520`, 70538) calls the deleting destructor (`0x140d2f3f0`, 70723) directly: no
deferral. The destructor calls slot `0xe0` on `+0x48`, drops the stream, unlinks from the list.

Other creators: `FUN_140d2eca0` (70714) makes a NiSourceTexture from a file name without the DB (caller
`FUN_140e111b0`); `FUN_140d2ede0` (70715) is the stream variant 70716 wraps.

## 2. The texture database

`BSResource::EntryDB<BSTextureDB::DBTraits>` is a static object at `0x14327e160` (ID 411165), pointed to by
`0x1430fd330` (ID 400665). It is built by `FUN_140e10970` (76021, also loads `NullStream` and pre-demands
`textures\sky`), vtable `0x1419cfc48` (ID 238240). It keys entries by `BSResource::ID` (file hash, extension,
directory hash) built from `textures\<path>`.

### Layout

Offsets from the EntryDB object; "Base" is `EntryDBBase` at `+0xd0`, the `this` most of the functions take.
This fixes CommonLib's `EntryDBBase` placeholders (`unk00[0x40]` is the DBTraits part, then the members below).

| EntryDB | Base | Field | Evidence |
| --- | --- | --- | --- |
| `+0x000` | | `IEntryDB` vtable | 76021 |
| `+0x008` | | `postFlushNotifyQueue` (8 × 0x18 + step at `+0xc8`) | `FUN_140d08f50` 69809 |
| `+0x110` | `+0x40` | hash table: `Entry**` (open addressing, probe `i = i*5+1`, tombstone `0x142012fa8`) | 76012, 36818 |
| `+0x118` | `+0x48` | capacity (0x40 at init) | |
| `+0x11c` / `+0x120` | `+0x4c` / `+0x50` | counters (a removal increments the first, decrements the second: free and live, inferred) | 36818 |
| `+0x124` | `+0x54` | lookup hit counter | 76012 |
| `+0x12c` | `+0x5c` | reader/writer lock (bit 31 writer) | 76012, 36818 |
| `+0x130` | `+0x60` | load queue: 8 × `{u32 lock, Entry* head, Entry** tail}`, step at `+0x1f0` | 76068 |
| `+0x1f8` | `+0x128` | **release queue: 32 × `{u32 lock, Entry* head, Entry** tail}`**, step at `+0x4f8` (`0x14327e658`, ID 411179) | 76079, 76080 |
| `+0x500` | `+0x430` | root `BSResource::Location*` | 76021, 443433 |
| `+0x508` | `+0x438` | pool of 0x400 `CArgs` (0x10 each: args, name `BSFixedString`) | 76014, 443433 |
| `+0x4508` | `+0x4438` | free ring of 0x400 `CArgs*`, index at `+0x6508` | |

Absolute: hash table pointer `0x14327e270` (ID 411172), capacity `0x14327e278`, release buckets
`0x14327e358 + i*0x18` (ID 411178).

### Entry (`BSResource::Entry<NiPointer<NiTexture>, CArgs>`, 0x30)

Matches CommonLib's `BSResource::Entry` (`BSResourceEntry.h`). Freed with size 0x30 by 36818.

| Offset | Field |
| --- | --- |
| `+0x00` | `ID` (3 × u32) |
| `+0x0c` | `ctrl` (u32, below) |
| `+0x10` | `userData`: the `CArgs*` from the pool while a load is pending, else flags |
| `+0x18` | `next` (queue link) |
| `+0x20` | `BSResource::Stream*` (handed to the NiSourceTexture on load) |
| `+0x28` | `NiPointer<NiTexture>`: the DB's reference |

`ctrl` (from 76012, 443433, 76014, `FUN_1402483a0` 16417, 36818; the state names are inferred):

| Bits | Meaning |
| --- | --- |
| 0–26 (`0x07ffffff`) | handle count: one per outstanding lookup/handle |
| 27 (`0x08000000`) | a variant flag (`0x38000000` is written instead of `0x30000000` for one load kind) |
| 28–30 | state: `1`/`2` queued for load, `3` loaded and handled, **`4` loaded with no handles (in the release queue)**, `5` load failed or cancelled, `7` being destroyed |
| 31 | load in progress |

- A lookup (`FUN_140e0f6c0`, 76012) finds the entry under the read lock and increments `ctrl` (a handle).
- Releasing a handle (`FUN_1402483a0`, 16417) decrements; going from `0x30000001` it writes `0x40000000` and pushes
  the entry onto release bucket `(step + 31) & 31` (the one just processed, so the next visit is 32 steps away).
- A synchronous demand (`FUN_140e0e500` 76005, used by `BSShaderManager::GetTexture` when `demand` is set; non-demand
  is `FUN_140e10f60` 76026) takes a handle, loads if needed (443433 `FUN_140e0f3d0`, which itself calls 76018), copies
  `entry+0x28` into the caller's `NiPointer`, and releases the handle. So after an ordinary GetTexture the entry is
  already at `0x40000000` and in the release queue: **the DB's own handle count says nothing about users; users are only
  visible through the NiSourceTexture's NiRefObject count.**
- A load that ends with no handles (76014, `FUN_140e0f9b0`) also queues the entry for release.

### The release check and the release

`FUN_140e10930` (76020), `DBTraits::CanRelease(entry, &entry->data)`:

```
if (TLS[+0x9e5] /* force flag */ || entry->data == null || entry->data->refCount /* NiTexture+0x08 */ == 1)
    return true;    // only the DB holds it
return false;
```

`FUN_1406524c0` (36818), the per-entry release step, under the DB lock (`Base+0x5c` swapped to `0x80000000`):

1. If `CanRelease` and `ctrl` CAS `0x40000000 -> 0x70000000` succeed, remove the entry from the hash table.
2. Then `FUN_140e108b0` (76019, `DBTraits::Release`): clear `entry+0x28` and drop that NiPointer (-> the NiSourceTexture
   destructor -> `ReleaseTexture` -> D3D `Release`s, all inline on this thread), drop the stream, free the entry.
3. Otherwise (still referenced, or a handle was taken meanwhile) requeue it with `FUN_140652450` (36817) into bucket
   `(step + 31) & 31`.

`TLS+0x9e5` is set only by `FUN_140e11110` (76030) around a full pass, from its argument; its one caller
(`FUN_1406289b0`) passes 0, so in practice the check is always the refcount test.

### Who walks the release queue

| Caller | ID | When | Buckets |
| --- | --- | --- | --- |
| `FUN_140646c40`, case 3 | 36567 | **`Main::Update` (36564, call at `+0x7f6`) every frame runs one of 7 jobs round robin** (counter `0x143187558`, ID 403534); case 3 is `FUN_140e110f0(0)` (76029) -> `FUN_140e12f20(db, 0)` (76080) -> `FUN_140e12e10` (76079) | 1 |
| TES cell attach/detach paths: `FUN_14019b0b0` 13288, `FUN_14019d210` 13315, `FUN_14019d490` 13316, `FUN_14019dc90` 13317, `FUN_14019c0d0` 13290, `FUN_1401a59a0` 13428 | | only when the TES byte `+0x13c` is set (meaning not traced) | 1 each |
| `FUN_140977f70` 52883 | | after a Scaleform external texture is cleared | 1 |
| `FUN_140651fd0` 36813 | | `pcb` and other full purges (§4) | 32 |
| `FUN_1406289b0` 36093 via `FUN_140e11110` 76030 | | a `MemoryManager` callback (from `FUN_140cc3370` 68076 / `FUN_140cc3cb0` 68106; inferred: low-memory purge) | 32 |
| IEntryDB vfunc 2 (`0x140e13060`, 76081 -> 76079) | | no direct caller; reached only through the vtable, callers not identified | 1 |

So in normal play the ring advances one bucket every 7 frames, all on the main thread inside `Main::Update` (the call at
`0x140646696`). An unreferenced entry waits 32 steps × 7 frames = 224 frames (the ~217 observed is within a step of that, and the
TES paths above add steps); a still-referenced entry is re-checked every 32 steps
for as long as something holds its NiSourceTexture. The queue is not drained by Present, the render thread
or an IO thread.

The IO side: `IOManager` (`0x143275df0`, ID 411121) keeps the registered DBs at `+0xc0` (count `+0xd0`,
`FUN_140e01d30` 75714, registered by `FUN_14062b8f0` 36156). Its vfunc `0xa8` (`0x140e01dc0`, 75716) runs
`IEntryDB::Flush` (`FUN_140d08f50`, 69809) on each: that processes **load** work (vfunc `0x18`) and the
post-flush-notify queue, not the release queue.

### What holds a DB entry's texture

The DB holds exactly one reference (`entry+0x28`). Everything else that keeps the entry from being released holds the
`NiSourceTexture` itself:

- **Materials** (shared through the material database, [skyrim-engine-notes.md](skyrim-engine-notes.md) "The material
  database"; acquire `FUN_1414f7790` 107719, release `FUN_1414f7a40` 107720, count `+0x08`, the map does not hold a
  reference and a material is deleted as soon as its count reaches zero). Texture fields (CommonLib):
  `BSLightingShaderMaterialBase` `+0x48` diffuse, `+0x58` normal, `+0x60` rim/soft, `+0x68` specular/back;
  subclasses `+0xa0..+0xb0` (envmap, eye, facegen tint/detail/subsurface, glow, parallax height, multilayer, LOD
  landscape) and landscape `+0xa8` diffuse[5], `+0xd0` normal[5], `+0xf8`, `+0x100`; `BSEffectShaderMaterial`
  `+0x58` source, `+0x60` greyscale; `BSWaterShaderMaterial` `+0x38..+0x58`.
- **Properties** that hold textures outside a material: `BSSkyShaderProperty` `+0x98`/`+0xa0` (raw pointers),
  `BSParticleShaderProperty` `+0x140`/`+0x148`, `BSBloodSplatterShaderProperty` `+0x88`, `ShaderReferenceEffect`
  `+0xd0..+0xf0`.
- **Globals**: `BSShaderManager::State` default textures (`+0x60..+0xa0` on AE), the projected-UV textures
  (`0x14328cc28/30/38`), the error textures `0x14328cca0` / `0x14328ccb8` (IDs 411500 / 411503, handed out by
  GetTexture on failure, so a broken path shares one texture), `TES+0x160` blood decal, the tree LOD atlas
  (the global DCLF reads as `kTreeLodAtlas`).
- `BSShaderTextureSet` / `BGSTextureSet` hold paths, not textures. `BGSTextureSet` can hold DB **handles**
  (`FUN_140327470` 21365 queues each of its 8 paths into a `BSTextureDB::QueuedHandles`, vtable ID 238246, 0x88 bytes,
  refcounted at `+0x08`); while such a handle is alive the entry stays in state 3 and never enters the release queue.
  These are created for model/facegen preloads (`TESModel` 1401cbe99, `FUN_14042c720`, …) and are transient as far as
  the callers show.

## 3. The release chain

From a user to the GPU object, with the thread and the queue at each step:

1. The user (material, property, global) drops its `NiPointer<NiSourceTexture>`. For 3D this usually happens when the
   geometry is destroyed, which is often deferred (step 1a). A material is destroyed when its material-database
   count reaches zero (immediate, in `FUN_1414f7a40`, called by `BSShaderProperty::SetMaterial` and the property
   destructors).
   1a. Deferred NiObject releases:
   - `GarbageCollector` (`FUN_14063c120` 36469 drains its `NiPointer` array at `GC+0x10 -> +0x80`, count `+0x90`):
     per frame from `Job_Garbage` (`0x1406d2ec0`, a frame job, 39077) via `FUN_14063bf10`, from `Main::Update` while
     in menu mode, from the 7-frame round robin (case 4), and fully from 36813 (`FUN_14063c1f0` 36470).
   - The deferred-release list `0x142029d68` (ID 388583; data `0x142029d70`, count `0x142029d7a`, lock
     `0x143272eb0`): `FUN_140d27a30` (70544) moves a `NiPointer` there instead of releasing it when its count is
     below 3 (callers: cell unload, water removal, `BSTaskPool_HandleTask`, `BSTempNodeManager`, `UpdateCellNode`,
     …). Drained under a time budget by `FUN_140d27bf0` (70545) from `Job_Garbage` and from `Main::Update` in menu
     mode; fully by `FUN_140d27d30` (70546) on round robin case 5 and in 36813.
   - Model DB entries: case 2 of the round robin walks the `BSModelDB` release queue (`0x1420f6330`, EntryDB at
     `0x143275e30`, 2 buckets) and facegen (`BSFaceGenManager` cleanup `FUN_14042b420` 26826, the facegen DBs at
     `0x1431394d8`/`0x1431394e0`); case 1 the other DBs (`0x1430fc680` LipSynchAnimDB, `0x143146ee0`,
     `0x14314d198`, `0x143155448`, `0x14315b948`). A model entry holds the loaded NIF, so its geometry's materials.
2. When the NiSourceTexture's count reaches 1 (the DB's), the next visit of its bucket (≤ 224 frames, §2) releases the
   DB reference. If the texture was never in the DB (70714 or a direct creation), step 3 happens as soon as the last
   user lets go.
3. `NiSourceTexture` count 0 -> `DeleteThis` -> destructor (`0x140d2f3f0`, 70723 / 70721) ->
   `[0x143274040]->vfunc 0xe0` -> `ReleaseTexture` (77322) -> `Texture+0x20` to 0 -> `SRV->Release()`,
   `Texture2D->Release()`, `UAV->Release()`, free. Same thread, same call.

Nothing on the render thread, Present or the DXVK side is needed for the engine to drop its references. The queue whose
walk could matter is §2's, and it is driven from `Main::Update`'s round robin, not from rendering; DCLF does not
replace `Main::Update`. If DCLF keeps textures alive, the likelier cause is a reference held somewhere in step 1 (a
material, a property, a geometry kept in a DCLF structure, or an object whose deferred release never runs), which shows
up as `NiSourceTexture+0x08 > 1` on the surviving textures' DB entries.

## 4. `pcb`

The console command calls `TES::PurgeBufferedCells` (`0x14019c990`, 13299):

1. `FUN_140652390` pauses IO (`IOManager` pause through `FUN_140e01cb0`).
2. `FUN_1401adce0` (13604) unloads the buffered exterior and interior cells that are not attached (via
   `TESDataHandler`, `FUN_1401b66d0`) and calls `BGSTerrainManager::UnloadAll(false, false)`.
3. `FUN_140651fd0` (36813), the full purge:
   - `GarbageCollector` full drain (`FUN_14063c1f0(gc, 0)`), `FUN_140ba8b10(1)` (`0x14315c950`), `FUN_14031b2f0`,
     `FUN_1414a1d20(…, 0)` (106336; walks a light array and removes each light, inferred to be a ShadowSceneNode light flush), the deferred NiObject list
     (`FUN_140d27d30(0)`), the model and facegen managers (`FUN_140192280(…, 1)`, `FUN_14042b420`), the other DBs.
   - The LipSynchAnimDB and `BSModelDB` release queues (both buckets each). Released models release their geometry,
     materials and therefore NiSourceTexture references here.
   - **The texture DB release queue, all 32 buckets once**, with `FUN_1406524c0` (36818) per entry: every entry in
     state 4 whose NiSourceTexture count is now 1 is freed; every other one is requeued.
4. `FUN_140652410` resumes IO; `FUN_140cc3550` compacts the MemoryManager.

`pcb` does not force anything: it frees only what nobody else holds, and only in one pass, so a texture whose last
holder is released later (e.g. by the next frame's `Job_Garbage`) waits for the normal ring.

## 5. Other holders and caches

- **FaceGen**: `BSFaceGenManager` (`0x1420f6a20`, ID 400331) keeps a model map (`BSFaceGenModelMap`, entries with a
  `NiPointer<BSFaceGenModel>` and a timestamp) cleaned by `FUN_14042b420` on round robin case 2 and in 36813. NPC face
  tints are normal DB textures referenced by the facegen materials (`BSLightingShaderMaterialFacegen` `+0xa0..+0xb0`).
  The player's/runtime tint render targets were not traced.
- **Texture streaming / mip loader**: none. There is no streamer class in the RTTI, and each texture is created once,
  fully, by the DDS loader. The only deferred-load structure is the DB's load queue (8 buckets) processed through
  `IEntryDB::Flush` on the IO side.
- **Scaleform** (`BSScaleformExternalTexture`, `NiPointer<NiTexture>`) and **Bink** (77322's direct callers) hold
  their own textures.
- **Scene info** (`BGSSceneInfo::TextureUse`, `NiTMap<NiTexture*, …>`) and `OutputTextureUseMap` are debug texture-use
  maps keyed by `NiTexture*`, not traced.

## CommonLib correspondence

| Engine object | CommonLib type | Notes |
| --- | --- | --- |
| `BSGraphics::Texture` | `RE::BSGraphics::Texture` (`N/NiSourceTexture.h`) | layout right; `+0x18` is 0 for DB loads |
| `NiSourceTexture` | `RE::NiSourceTexture` | `resourceStream +0x40`, `rendererTexture +0x48` |
| NiTexture list | `RE::NiTexture::prev/next` | head/tail globals IDs 410544 / 410545 not in CommonLib |
| texture DB entry | `RE::BSResource::Entry<NiPointer<NiTexture>, CArgs>` | layout right |
| texture DB | `RE::BSResource::EntryDB<T>` / `EntryDBBase` | base at `+0xd0`; cache `+0x40`, load queue `+0x60` (8), release queue `+0x128` (32), root `+0x430` |
| bucket queues | `RE::BSResource::EntryBucketQueue` / `EntryQueue` | `{lock, head, tail}` 0x18 |
| `IOManager` | `RE::IOManager` (ID 411121) | slot 21 (`0xa8`) = DB flush |
| material refcount | `RE::BSShaderMaterial` `+0x08` | release only through 107720 |

## Not a texture-system object: the struct seen in the dump

The vtable-less blocks with `+0x00` an SRV pointer, `+0x08` `0x0000020000000200`, `+0x10` packed fields, `+0x18` a
hash-like qword and `+0x20` / `+0x28` pointers to similar blocks do not match anything on the engine's texture path:

- `BSGraphics::Texture` has the texture at `+0x00` and the SRV at `+0x10`, zero at `+0x18`, and no links.
- DB entries start with a 12-byte ID and link only through `+0x18`.
- The engine never stores a width/height pair next to an SRV for DB textures (77323 asks the resource each time), and
  the engine's resource names are narrow `BSFixedString`s, whereas `0x005c007300740001` reads as UTF-16 `…ts\`.

Inference: the structure belongs to another module (a plugin, the D3D11 layer or an injector) that caches SRVs with
their dimensions in a hashed, linked container. Comparing its `+0x00` pointers with the SRVs reached through the
NiTexture list (`+0x48 -> +0x10`) will tell whether it refers to game textures; it does not own the game's creation
references, which stay in the `BSGraphics::Texture` blocks above.

## Dump checklist

For each surviving SRV from the DXVK trace:

1. Walk the NiTexture list (`[0x143274100]`, `+0x38`) to the `NiSourceTexture` whose `+0x48 -> +0x10` is that SRV. Note
   its name (`+0x20`) and count (`+0x08`).
2. Find its DB entry: walk the table at `[0x14327e270]` (capacity `[0x14327e278]`, skip null and `0x142012fa8`) for
   `entry+0x28 == texture`. `ctrl` (`+0x0c`) `0x40000000` means the DB is waiting for the other references to go;
   `0x3…` plus a count means DB handles are still held.
3. `count - 1` (when in the DB) is the number of other holders. Search the dump for the NiSourceTexture pointer to find
   them: materials (fields above), properties, globals, or DCLF's own structures.
4. If the count is 1 and the entry is in state 4 but still alive, look at the release step (`0x14327e658`) and the
   bucket that holds it: the round robin (`0x143187558`) or `Main::Update` is not reaching it.
5. If no NiSourceTexture leads to the SRV, the `BSGraphics::Texture` has an extra reference (`+0x20 > 0` with no
   owner): look for slot `0xd8` callers outside the engine.

## The model database's release queue (and why a refcount-1 prototype can stay)

`BSResource::EntryDB<BSModelDB::DBTraits>` is at `0x143275e30` (pointer `0x1420f6330`, ID 400273); its `EntryDBBase` is at
`+0xd0`, laid out like the texture DB's (hash table `Base+0x40`, capacity `+0x48`, lock `+0x5c`), except that the
release queue at `Base+0x128` (`EntryDB+0x1f8`) has **2 buckets** with the step at `EntryDB+0x228`.

**Entry** (0x30, freed with size 0x30 by 13261): `+0x00` ID, `+0x0c` ctrl (bits 0–26 handle count, 28–30 state, 4 =
loaded with no handles / in the release queue, 7 = being destroyed, bit 31 busy), `+0x18` queue link, `+0x20` stream,
`+0x28` `NiPointer` to the model's root node (the prototype).

**Release check** `FUN_140e036e0` (75775): releasable if the thread-local force byte `TLS+0x9e4` is set, or `+0x28` is
null, or the root's NiRefObject count (`root+0x08`) is exactly 1. `TLS+0x9e4` is set only by the model DB's shutdown
`FUN_140e03910` (75779).

**Release step** `FUN_140198890` (13261), the model counterpart of 36818: under the lock, if the check passes and ctrl
CASes `0x40000000 -> 0x70000000`, the entry leaves the hash table and is freed (`FUN_140e036b0` 75774 drops the root).
**Otherwise it is requeued**, unconditionally, with `FUN_140198180` (13253) into bucket `(bucket + 1) & 1`. That covers
both a held root (count > 1) and a ctrl that is not exactly `0x40000000` (handles taken meanwhile).

Walkers: round robin case 2 (`FUN_140646c40` 36567, every 7 frames) and 36813 (`pcb`) and 36093 each take both buckets
in one call (the step decrements, `(step - 1) & 1`), so a requeued entry is seen again in the same call's second
bucket and then on every later visit. **The hypothesis is refuted**: a model entry that fails the check is not
dropped from the queue; it is re-checked every 7 frames, more often than a texture entry (32 buckets, every 224 frames;
otherwise the two match: same check on `+0x08 == 1`, same requeue on failure).

**Putting an entry in the queue**: only the handle release (inlined in many callers; standalone in `FUN_1401957e0`
13154, model counterpart of 16417) pushes it, on `0x30000001 -> 0x40000000`, into bucket `(step + 1) & 1`; a release
from `0x4000000N` just decrements (the entry is already queued). A lookup increments the count without changing the
state, so a state-4 entry stays queued while handles come and go. A load that ends with no handles queues it too.
Nothing found force-releases or re-adds entries with zero handles: `pcb` (13604 unloads cells, which releases their
handles; 36813 walks the queue once with the normal check) and the memory-pressure purge (36093) use the same check;
cell attach/detach only releases handles.

**So a prototype with count 1 that survives thousands of frames and a `pcb` is one whose entry is not at exactly
`0x40000000`**: its ctrl (`entry+0x0c`) is either `0x3000000N` (N handles held, not in the queue) or `0x4000000N` (in the
queue but a handle held, failing the CAS each visit), or its queue link was lost. Handles are held by
`BSQueuedResourceCollection<EntryDB<BSModelDB>>` objects (vtables for sizes 1, 2 and 42; handle array at `+0x60`,
inline or heap, count `+0x80`, stride 0x10; `Func2` 11401 waits on them), `TESModelDB::TESQueuedHandles`,
`QueuedHead` and the other inlined callers listed by the model DB's xrefs. Reading ctrl on the surviving entries tells
which case applies.

**Other DBs**: each checks only its own entry's data. The facegen DB step `FUN_14042d0e0` (via `FUN_14042b420` 26826)
follows the same pattern (check `FUN_1404208f0`, CAS `0x40000000`, requeue `FUN_140427ad0` into `bucket + 1`). A
prototype's count does not enter their checks; it blocks them only indirectly, by keeping its materials and so its
NiSourceTextures alive, which fails the texture DB's check.
