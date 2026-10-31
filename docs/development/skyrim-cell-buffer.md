# Skyrim SE/AE cell buffers: how detached cells are kept and unloaded

Findings from the Ghidra databases of `SkyrimSE.exe` 1.6.1170 (AE) and 1.5.97 (SE), gathered on 2026-10-29 to design a
VRAM-driven eviction that unloads the oldest buffered cells one at a time instead of running the whole `pcb`. Addresses
are runtime addresses with image base `0x140000000`. Every function and global that Community Shaders would call or read
is given for both runtimes as `AE id / SE id`, the form `REL::RelocationID(se, ae)` takes (note the order there is SE
first). Names such as `FUN_…` and `DAT_…` are Ghidra's; the names used here are inferred from the decompilation and the
call sites. Where something is inference rather than read from the code, it says so.

Companion to [skyrim-texture-bookkeeping.md](skyrim-texture-bookkeeping.md), whose §4 describes `pcb` and whose §2/§3
describe the DB release queues this document relies on.

## Summary

```
TES (singleton 403450 / 516923)
  +0x078 GridCellArray* gridCells            uGridsToLoad x uGridsToLoad attached exterior grid
  +0x0c0 TESObjectCELL* interiorCell         the current interior (null outside)
  +0x0c8 TESObjectCELL** interiorBuffer      uInterior Cell Buffer slots (default 3, min 1)
  +0x0d0 TESObjectCELL** exteriorBuffer      uExterior Cell Buffer slots (min (g+2)*g+1 = 36 for g = 5)
```

- Both buffers are plain arrays of `TESObjectCELL*` allocated from Skyrim's `MemoryManager` at TES init. They are
  **most-recently-used lists by position**: index 0 is the newest, the highest non-null index is the oldest. There is no
  timestamp; a cell's "age" is its slot. (`ExtraDetachTime` is written on detach, but it drives cell reset, not the
  buffer.)
- The exterior buffer holds **every loaded exterior cell, attached ones included**: attached grid cells collect at the
  front (normally slots `0 .. g²-1`), detached cells behind them, newest to oldest.
- A cell enters the buffer when it is loaded (the cell loader task, on an IO thread) and moves within it on detach. When
  an insert overflows the array, the cell pushed off the end is **unloaded on the spot**, with no further checks.
- The engine already has a "drop one oldest cell" function: `TES::UnloadOldestBufferedCell` (AE `0x14019c5e0`,
  13296 / SE `0x140153220`, 13156). The allocator's out-of-memory callback (36093 / 35201) calls it once per failed
  allocation and runs the DB purges after it, and only falls back to `pcb` when nothing is left to evict.
- One cell is unloaded by `TESDataHandler::UnloadCell(cell)` (AE `0x1401b66d0`, 13721 / SE `0x14016bda0`, 13623). It
  drops the cell's 3D, references, land, navmesh/havok data and grass on the calling thread. Textures and models are
  freed later, through the DB release queues. To get VRAM back promptly, follow the eviction with the full purge 36813 /
  35842, which is what `pcb` does.
- Main thread only. The buffers are also written from IO threads, so pause IO around the eviction as `pcb` does.

## 1. The buffers

### Layout and capacity

| Field | AE | SE | Notes |
| --- | --- | --- | --- |
| `TES*` singleton | `0x1431872c8` (403450) | `0x142f26b20` (516923) | CommonLib `TES::GetSingleton` |
| `TES+0x78` `gridCells` | same | same | `GridCellArray`, `length` at `+0x10` |
| `TES+0xc0` `interiorCell` | same | same | |
| `TES+0xc8` `interiorBuffer` | same | same | `TESObjectCELL*[uInteriorCellBuffer]` |
| `TES+0xd0` `exteriorBuffer` | same | same | `TESObjectCELL*[uExteriorCellBuffer]` |
| `TES` worldspace | `+0x148` | `+0x140` | CommonLib `RUNTIME_DATA2::worldSpace` |
| `uInterior Cell Buffer:General` value (u32) | `0x141fd4750` (358814) | `0x141db1740` (500861) | setting object at `-8` (358813 / 500860), default 3 |
| `uExterior Cell Buffer:General` value (u32) | `0x141fd4768` (358817) | `0x141db1758` (500863) | setting object at `-8` (358816 / 500862), default 0 → computed |
| `uGridsToLoad:General` value (u32) | `0x141fd6eb0` (359676) | `0x141db3e30` (501245) | setting object at `-8` (359675 / 501244), default 5 |

Sizing (TES init `FUN_1401999e0` 13279; the exterior realloc `FUN_1401a2aa0` 13384 does the same):

- `uInteriorCellBuffer` is forced to at least 1.
- `uExteriorCellBuffer` is raised to at least `(uGridsToLoad + 2) * uGridsToLoad + 1` (36 for 5 grids, 64 for 7). The
  static initializer (`FUN_14000c560`) starts from that value before the INI is read. So the exterior buffer always has
  room for the g² attached grid plus a ring of `2g + 1` detached cells.
- Both arrays are `MemoryManager::Allocate(8 * n)` and zero-filled. Code everywhere reads the size from the INI globals
  (and `FUN_1401a2ba0` 13385 / `FUN_1401a2bb0` 13386 return them), so read the globals rather than caching them.

`TESObjectCELL` fields the buffer code reads (AE / SE where they differ):

| Offset | Field | Use |
| --- | --- | --- |
| `+0x10` | `formFlags` | bit 5 `kDeleted` (UnloadCell does nothing), **bit 21 `kStillLoading`** (skipped by every purge) |
| `+0x40` | `cellFlags` | bit 0 `kIsInteriorCell` |
| `+0x44` | `cellState` (u8) | see below |
| `+0x46` | `cellDetached` | set to 1 on detach, 0 on unload |
| `+0x120` / `+0x118` | `spinLock` (owner, count) | **`count` at `+0x124` / `+0x11c` non-zero = in use**, tested by `FUN_1402c3810` (19145) / `FUN_14026e5f0` (18669) |
| `+0x128` / `+0x120` | `worldSpace` | |
| `+0x130` / `+0x128` | `loadedData` | `LOADED_CELL_DATA*` (0x180), freed on unload |

`cellState` values, from the writes seen here; the names follow CommonLibF4's enum (Skyrim has no `kAttachQueued`) and
are an inference: `0` not loaded, `1` unloading (set by UnloadCell), `4` loaded and detached (set at the end of
detach), `5` detaching, `6` attaching, `7` attached (CommonLib `kAttached`). The purges skip `6..7` as "attached".

### Order and movement

| Function | AE | SE | Behaviour |
| --- | --- | --- | --- |
| `TES::AddCellToBuffer(cell)` | `0x1401a61d0` (13434) | `0x14015c380` (13280) | interior → 13435; exterior → 13367, then insert at slot 0, or at slot `g²` when slot `g²-1` is occupied and the cell is not attaching/attached. Everything from there is shifted down one until the cell's old slot (move to front) or the end. **The cell shifted off the end is passed to `UnloadCell` with no state or lock check.** |
| exterior insert (no interior branch) | `0x1401a64c0` (13438) | `0x14015c5c0` (13284) | the exterior half of 13434 |
| `TES::AddInteriorToBuffer(cell)` | `0x1401a62a0` (13435) | `0x14015c3a0` (13281) | already present: move to slot 0. Else shift everything down, unloading the old `[n-1]`, then `[0] = cell` |
| `TES::SortExteriorBuffer()` | `0x1401a1800` (13367) | `0x140157d50` (13217) | when the queued grid centre (`TES+0xb8/0xbc`) differs from the current one (`+0xb0/+0xb4`), moves cells within `g/2` of the queued centre forward in the `g²..` region. **Stops at the first null slot** |
| `TES::DemoteDetachedExterior(cell)` | `0x1401a1950` (13368) | – (not mapped) | on detach: moves the cell back past the run of attached (state 7) cells that follow it |
| `TES::RemoveExteriorFromBuffer(cell, unload)` | `0x1401a19e0` (13369) | `0x140157f30` (13219) | clears the cell from the grid (`GridCellArray::ClearItemForCell` 15968 / 15733), optionally `UnloadCell`, then compacts the array |
| `TES::RemoveInteriorFromBuffer(cell)` | `0x1401a1ad0` (13370) | `0x140158010` (13220) | `UnloadCell`, then compacts. **If the cell is not in the buffer it still clears the last slot without unloading it**, so only call it with a cell you found there |
| `TES::IsInInteriorBuffer` / `IsInExteriorBuffer` | 13433 / 13437 | not mapped | linear search |
| `TES::UnloadAllInteriors(keepCurrent)` | `0x1401a63d0` (13436) | `0x14015c4d0` (13282) | same per-cell checks as `pcb` |
| `TES::UnloadAllExteriors(keepAttached, worldspace)` | `0x1401a6570` (13439) | `0x14015c670` (13285) | same, optionally only one worldspace; frees the worldspace's persistent data when the buffer ends up empty |

Who moves cells:

- **Load**: `CellLoaderTask` (vtable data `0x1417a4ec0`), whose work function `FUN_14029bca0` (18529) loads the cell and
  then calls 13434. Inference from its state machine (it is queued on `IOManager+0xe0` by `FUN_14029bc00` 18528) is
  that it runs on an IO thread. `ExteriorCellLoader::QueueCellLoad` (`0x14029c530`) and the other loaders also call
  13434. **So the buffer arrays are written from IO threads. Nothing locks them.** That is why `pcb` pauses IO.
- **Detach**: `TESObjectCELL::Detach(cell, removeHavok)` (`FUN_1402c15e0`, 19130 / `0x14026c5d0`, 18655). It runs
  only for cells in state 6/7: it sets state 5 and waits for `cell+0x34` (the grass-create lock count) to reach 0. It
  then calls 13368 for an exterior or 13435 for an interior, and unloads the 3D/havok if asked. It ends with state 4,
  `cellDetached = 1`, and writes the detach time (`kDetachTime` change flag, `ExtraDetachTime` via `FUN_140164290`).
- **Overflow**: 13434 / 13435 unload the cell that falls off the end. With the minimum exterior size that is the oldest
  detached cell outside the `2g+1` ring.

So the oldest buffered cell is the highest non-null index. For the exterior buffer that means at or above `g²`
(`g = gridCells->length`, or `uGridsToLoad`), skipping attached ones.

## 2. Unloading one cell

### `TESDataHandler::UnloadCell` (AE `0x1401b66d0`, 13721 / SE `0x14016bda0`, 13623)

`void UnloadCell(TESDataHandler* dh, TESObjectCELL* cell)`, `dh` from 400269 / 514141. It takes only the cell and does
not touch the buffers: callers clear the slot themselves.

1. Returns if `cell` is null or `kDeleted`. Sets the TLS "context" word `+0x768` to `0x6a` and the IO-side flag
   `0x143275dec` (inference: "unloading in progress"). If a save is in progress (`BGSSaveLoadGame+0x340` bit 1), it
   also sets TLS `+0x670` bit 0 for the duration.
2. `TESObjectCELL::Detach(cell, true)` (19130). This is a no-op unless the cell is attached or attaching.
3. `ProcessLists` removal of the cell's actors (`FUN_140773c00` 41413 and its sibling); `cellState = 1`.
4. `UnloadCellMopp`; for an exterior that is not flagged at `formFlags` bit 13, unload its `TESObjectLAND`
   (`FUN_1402a8a80` 18749).
5. `FUN_1402bb690` (19056): under the cell lock, releases `loadedData->cell3D` (`FUN_1402c51f0` 19170: clears the
   portal graph if it is the current one, drops the `NiPointer<NiNode>`) and every reference's 3D (`FUN_14017a6e0`).
6. Frees the cell's arrays (`+0xd8`, the havok water objects at `+0xf0` with `hkReferencedObject::RemoveReference`,
   `+0x108`). `FUN_1402bfae0` (19113) cleans up references: temporary references go to `GarbageCollector::Add` (36459),
   others are reverted/saved. It then flushes middle-low processes and, unless `TESDataHandler+0xda5` is set, runs the
   `ProcessLists` cell unload (`FUN_1402bac30` 19046).
7. `FUN_1402b87a0` (18981): removes the cell's havok world, frees `LOADED_CELL_DATA` (`FUN_1402c64a0` 19192), clears
   the queued-ref counters and ends at `cellState = 0`, `cellDetached = 0`.
8. `autoWaterLoaded = 0`, `BGSGrassManager::RemoveCellGrass`; for an exterior, the worldspace drops the cell
   (`FUN_140306cd0` 20461).

Preconditions (what every engine caller checks before calling it on a buffered cell, e.g. 13604):

- the cell is not attaching/attached (`cellState` not 6 or 7);
- not `kStillLoading` (`formFlags & 0x200000`): an IO thread is still building it;
- not in use: spin-lock count `cell+0x124` (AE) / `+0x11c` (SE) is 0 (19145 / 18669);
- for the interior buffer, not `TES->interiorCell`.

The grid is protected by the state check: attached cells are state 7. 13369 additionally clears grid slots that point
at the cell. Persistent references survive because they live in the worldspace's persistent cell, which is not in the
buffer; it is released only when the exterior buffer ends up empty (`FUN_1403070f0` 20463 / `0x1402b27a0` 20029, then
`TES worldspace = null`). Quest-held cells: nothing in these paths checks quest aliases. A held reference is a
persistent reference, so it is not part of the cell being unloaded (inference). The engine applies no other per-cell
test.

### Threading and IO

- Main thread only. The out-of-memory path does its eviction only when `GetCurrentThreadId() == Main+0x28` (`Main*`
  403449 / 516943), and the console `pcb` and all other callers run on the main thread.
- **Pause IO.** `pcb` brackets the work with `MemoryLevelManager::BeginMemoryFree` / `EndMemoryFree`. CS should too,
  because the cell loader tasks write the same arrays from IO threads (§1).

| Function | AE | SE |
| --- | --- | --- |
| `MemoryLevelManager` object | `0x1420f69e0` (400323), a function-local static (guard `0x1420f69f4`, ctor `FUN_140651f20` 36811); zero-initialised storage is fine for `BeginMemoryFree` | `0x141ebeb00` (514175), getter `FUN_14014f560` (13110) |
| `BeginMemoryFree(mlm, char state[2], bool lockRenderer, bool pauseIO)` | `0x140652390` (36815) | `0x1405bf640` (35844) |
| `EndMemoryFree(mlm, char state[2])` | `0x140652410` (36816) | `0x1405bf6c0` (35845) |
| `IOManager::Pause(io)` / `Resume(io)` (what Begin/End call) | `0x140e01cb0` (75712) / `0x140e01cf0` (75713) | `0x140d2d520` (73976) / `0x140d2d560` (73977) |
| `IOManager**` | `0x143275df0` (411121) | `0x143015478` (524542) |

`pcb` passes `lockRenderer = false, pauseIO = true`. `BeginMemoryFree` pauses only if `mlm+0x11` (set by
`CheckMemoryLevel` 35841) is clear and a `MemoryManager` field (`0x1435f1644` AE) is 0 (inference: not already inside the
allocator's out-of-memory handling). The pause is a counter (`IOManager+0xa4`). On the first pause it waits for the
busy IO threads, processing queued work on the calling thread in a `Sleep(1)` loop, and then parks every IO thread with
`WaitForSingleObject(INFINITE)` (`FUN_140dfe0c0` 75643). **That blocks the main thread until in-flight IO tasks
finish.** Expect a stall of a frame or more when streaming is busy (inference; not measured).

### Is one cell outside `pcb` safe?

Yes. That is what the engine does under memory pressure. `TES::UnloadOldestBufferedCell` (AE `0x14019c5e0`, 13296 /
SE `0x140153220`, 13156), `bool (TES*)`:

- **Exterior player** (`interiorCell == null`): first the oldest interior. Scanning `interiorBuffer` from the end, the
  first non-null, non-`kStillLoading` slot is nulled and unloaded. Without one, it calls 13367, then scans
  `exteriorBuffer` from the end down to `g²`. The first non-null, non-`kStillLoading` entry is unloaded, **unless it is
  attaching/attached, in which case the function returns false** without looking further.
- **Interior player**: the oldest non-attached exterior in `g²..`, else the oldest interior that is not `interiorCell`,
  else the first non-attached cell in the `0..g²-1` region (it then calls `gridCells->DetachAll`, vfunc 1).
- It nulls the slot (no compaction) and calls `UnloadCell`. If the exterior buffer is then empty it releases the
  worldspace (20463, `TES worldspace = null`). It returns true if it unloaded a cell.
- **It does not check the cell lock (19145)**, unlike `pcb`'s walk, and it does not pause IO itself.

The out-of-memory callback (`FUN_1406289b0`, 36093 / `FUN_14059b220`, 35201) case 5, on the main thread:

```
TLS+0x670 &= ~1;  GC flag TLS+0x678 = 0  (FUN_14063c8e0 36472 / 35505)
if (!TES::UnloadOldestBufferedCell(tes)) TES::PurgeBufferedCells(tes);   else next stage = 5 again
FUN_14019c0d0(tes, 0)   (13290 / 13150: one texture-DB bucket if TES+0x13c / SE +0x132 is set)
GC flag = 1; restore the save-load thread flag (19378)
case 4: GarbageCollector full drain (36470 / 35503, force 1)
case 3: deferred NiObject list full drain (70546 / 69182), model DB both buckets, other DBs, facegen,
        texture DB all 32 buckets (76030 / 74298), havok GC, MemoryManager compaction (68082/68083 / 66837/66838)
```

Because a successful eviction sets the next stage back to 5, each further failed allocation evicts one more cell, then
purges. That is the fine-grained `pcb` the engine already uses. It runs without pausing IO (inference: under memory
exhaustion the IO threads are stalled in the same allocator).

Minimum safe sequence for CS (main thread, outside loading and saving):

1. `BeginMemoryFree(mlm, state, false, true)`.
2. For each victim: pick it yourself (below) and call `TES::RemoveExteriorFromBuffer(tes, cell, true)` (13369 / 13219)
   or `TES::RemoveInteriorFromBuffer(tes, cell)` (13370 / 13220). These unload the cell and compact the array, so
   13367 keeps working (it stops at holes). `slot = nullptr; UnloadCell(dh, cell)` also works but leaves holes, as `pcb`
   and 13296 do.
3. The purge (§3).
4. `EndMemoryFree(mlm, state)`.

Victim selection that mirrors the engine's checks and adds the lock test 13296 lacks:

- interior buffer, index `n-1 .. 0`: non-null, `!= interiorCell`, not `kStillLoading`, lock count 0;
- exterior buffer, index `n-1 .. 0` (§6: not just down to `g²`): non-null, `cellState` 4, not in the grid, not `kStillLoading`, lock count 0.
  Exclude 5 (detaching) as well; the engine's purges let it through, but it is mid-transition.
- Take the other space first (interiors while outside, exteriors while inside), as 13296 does.
- Do not empty the exterior buffer while outside. Its front holds the attached grid, which these checks skip anyway.

`TES::UnloadOldestBufferedCell` (13296 / 13156) inside the IO pause is the simplest correct option if the missing lock
check is acceptable. Run it in a loop until it returns false or N cells are gone.

## 3. Textures and models after an eviction

Unloading a cell drops the engine's references on the calling thread: the cell's `cell3D`, each reference's 3D, and
through them the scene graph's materials. Several holders are deferred:

- temporary references go to the `GarbageCollector` (drained per frame by `Job_Garbage`, fully every 7 frames);
- NiNodes whose count is below 3 can be parked on the deferred-release list `0x142029d68` (`UnloadCellMopp` and
  `UpdateCellNode` call `FUN_140d27a30` 70544);
- the models' **DB entries** stay loaded until the model DB's release queue (round robin case 2, every 7 frames) sees
  them unreferenced. Only then does the material and texture count fall to the texture DB's 1. The texture DB frees it
  on its bucket's next visit, up to 32 × 7 = 224 frames later (texture doc §2).

So the GPU objects are **not** freed by `UnloadCell` itself; without help they go within a few hundred frames. To free
them now, run the full purge once after the batch:

| Function | AE | SE | Notes |
| --- | --- | --- | --- |
| `MemoryLevelManager::FreeAll` (full purge) | `0x140651fd0` (36813) | `0x1405bf400` (35842) | `pcb` passes the MLM in `rcx`; the body ignores it. Order: GC full drain, `FUN_140ba8b10(1)`, light flush (106336), deferred list full drain (70546), model/facegen managers, the other DBs, LipSynch DB, **model DB both buckets, then texture DB all 32 buckets**. So a model freed in the first walk frees its textures in the second |
| texture DB, one bucket | `0x140e110f0` (76029) | `0x140d3abb0` (74297) | what the round robin and the TES attach paths use |
| texture DB, all 32 buckets | `0x140e11110` (76030) | `0x140d3abd0` (74298) | what 36093 uses; walk the model DB first or it frees little |

- Thread: main thread. Everything here is designed to run there (`Main::Update`, the console, the main-thread-only
  branch of 36093). The release path is synchronous below the DB: `NiSourceTexture` destructor → `ReleaseTexture` →
  `ID3D11*::Release` on this thread (texture doc §3). DXVK then frees the memory once the GPU has retired the frames
  that used it, so VRAM drops a frame or two after the call.
- Running it inside the IO pause, as `pcb` does, keeps IO threads from taking handles to entries mid-walk. 36093 runs
  it without the pause.
- Cost: it walks every DB queue and drains every deferred list, and each freed object is destroyed inline. A `pcb` is
  a visible hitch. Expect the same order of cost for each 36813 call, plus the IO pause's wait (inference; not
  measured). Batch: evict N cells, then purge once, and rate-limit to one batch every few seconds.
- After one 36813, something released later (a reference handed to a task, a GC entry added during the purge) waits for
  the normal ring. A second, cheaper pass a few frames later (model DB round robin + 76030) catches it.
- `pcb` also compacts the MemoryManager (`FUN_140cc3550`, 68083 / 66838). That returns heap memory, not VRAM; it is
  optional.

## 4. Engine memory-pressure paths

- **Allocator out-of-memory** (`MemoryManager` retry loops `FUN_140cc3370` 68076 and `FUN_140cc3cb0` 68106): when a
  pool allocation fails, it calls 36093 / 35201 with an escalating stage (TLS `+0x6b4/+0x6b8`) and retries until it
  succeeds. Stage 0: one texture bucket (13290, if `TES+0x13c` is set). Stage 2: facegen. Stages 3 and 4: the purges
  above. **Stage 5: evict the oldest buffered cell (13296), or `pcb` when none is left, then stages 4 and 3.** Stage 7:
  compaction and `Sleep(1)` for other threads. It is triggered only by system-memory allocation failure in Skyrim's own
  heap, **never by VRAM**: D3D11 allocations do not go through it.
- **Large interior**: `TES::SetCurrentCell` (13316 / 13171) and 13317 run `pcb` when `bPreemptivelyUnloadCells:General`
  (setting `0x141fd4730`, 358810, default 0) is set and the cell being entered has more than `iLargeIntRefCount:General`
  (`0x141fd4c58`, 358965, default 1000) references (`references` capacity minus free count).
- `pcb` is also run by load-game and new-game paths (`FUN_140648cd0` 36601, `FUN_14064a680` 36625), after
  `BGSTerrainManager::UnloadAll(true, …)`.
- `TES+0x13c` (AE) / `+0x132` (SE) gates the single texture-bucket walks on cell attach and detach (13290 / 13150).
  CommonLib calls it `allowUnusedPurge` and puts it at `0x13C` for SE as well. The SE code reads `+0x132`, so the
  CommonLib SE offset looks wrong (inference). Likewise 13604 and 13295 set a byte at AE `+0x134` / SE `+0x12a`, which
  CommonLib's AE layout calls `gridBufferDistanceX`. That byte's meaning was not traced.

## 5. Terrain LOD and the grid

- `pcb`'s cell step (`FUN_1401adce0`, 13604 / `FUN_140163bb0`, 13502) ends with `BGSTerrainManager::UnloadAll(mgr,
  false, false)` (AE `0x14050ed60`, 31812 / SE `0x1404b3190`, 31026; `mgr` from `0x14315b6a8`, 402262 /
  `0x142f23cb8`, 516111), **only when `interiorCell` is set**. It unloads the worldspace's distant LOD (terrain, objects,
  trees: their LOD DBs are flushed through the same calls 36813 makes) and empties the LOD tree node. Outside, that
  would unload the visible distant world, so CS must not call it there. Inside, it can return a large share of VRAM
  (the LOD textures) and is cheap to rebuild on exit: call it once per eviction batch while `interiorCell != null`. The
  first argument (`true` on load game) also resets more terrain state; keep it `false`.
- Grid vs buffer: the attached grid (`gridCells`, g × g, `GridCellArray::cells` at `+0x18`) is a view onto cells that
  are also in the exterior buffer, normally its first g² slots, all in state 7. In an interior, 13604 and 13295 first
  call the worldspace cleanup and `gridCells->DetachAll` (vfunc 1) before unloading exteriors. CS should not do this;
  it should only take cells that are in state 4 and not in the grid. Checking `cellState` is enough. For extra safety,
  also skip any cell found in `gridCells->cells`, which is what 13369 clears.

## 6. In an interior the exterior grid stays attached

Observed on 2026-10-29 (Nordic Souls, AE): load, `coc Whiterun`, `coc RiverwoodSleepingGiantInn`, no `pcb`. In the inn
the exterior buffer held **no** unloadable cell. The Whiterun grid (g² cells) was still in `gridCells`, in state 7,
so every check in §2 rejected it, and VRAM stayed at the Whiterun level (13.8 GB of a 14.5 GB budget) for as long as
the player stayed inside. The engine keeps the grid attached across an interior visit (inference: so leaving through
the same door is instant). Only `pcb`, the out-of-memory path and a large-interior preemptive purge let it go.

`pcb`'s cell step (13604 / 13502) does this first, only while `TES->interiorCell` is set:

| Step | AE | SE | What it does |
| --- | --- | --- | --- |
| `TESWorldSpace` 3D unload `(ws)`, when `TES` worldspace (AE `+0x148` / SE `+0x140`) is set | `0x140309430` (20515) | `0x1402b49f0` (20072) | if the worldspace's portal graph (`ws+0x130`) has a count of 1, clears it from `ShadowSceneNode` and drops it; removes and drops the scene nodes at `ws+0x148` / `+0x150` (through `BSShaderManager::State` vfunc `0x1c0`); walks the map at `ws+0x138` and releases each entry's 3D (persistent objects, inference) |
| `gridCells->DetachAll()` (`GridArray` vfunc 1) | `0x140221380` | via the vtable | for every grid slot `GridCellArray::Detach(x, y)` (vfunc 5, `0x140221a40`): `TESObjectCELL::Detach(cell, true)` (19130), then `ClearItem` (vfunc 6). **The grid ends empty** and its cells end in state 4, still in the exterior buffer |

`GridCellArray` vtable: AE `0x14178b160` (188592) / SE 232660 (CommonLib `VTABLE_GridCellArray`).

After `DetachAll` the former grid cells stay in the front slots of the exterior buffer (`TES::DemoteDetachedExterior`,
13368, only moves a detaching cell past the attached cells behind it). A victim scan that starts at slot `g²` misses
them, so scan the whole buffer and rely on the state and grid checks.

Leaving the interior after this behaves as after `pcb`: the exterior grid is loaded again on the way out.

## Recommended sequence

```
// main thread only (e.g. an SKSE task), not during a load/save or a cell transition
mlm = AE 400323 (0x1420f69e0) / SE 13110();                char st[2];
BeginMemoryFree(mlm, st, false, true);                      // 36815 / 35844 — pauses IO, may stall
tes = *TES (403450 / 516923); g = tes->gridCells->length;
if tes->interiorCell && no buffered exterior is unloadable && the grid holds cells:   // §6
    if tes->worldSpace: TESWorldSpace 3D unload(ws)         // 20515 / 20072
    tes->gridCells->DetachAll()                             // vfunc 1; the grid's cells become unloadable
for k in 0..N-1:
    cell = oldest candidate  (interiors first when outside, exteriors first when inside;
                              exterior slots n-1..0, state 4, not in the grid, !kStillLoading, lock==0;
                              interior slots n-1..0, != interiorCell, !kStillLoading, lock==0)
    if !cell: break
    exterior: RemoveExteriorFromBuffer(tes, cell, true)    // 13369 / 13219
    interior: RemoveInteriorFromBuffer(tes, cell)          // 13370 / 13220
    // alternative per step: if (!UnloadOldestBufferedCell(tes)) break;   13296 / 13156
if tes->interiorCell && terrainMgr: BGSTerrainManager::UnloadAll(mgr, false, false)  // 31812 / 31026, optional
FreeAll(mlm);                                               // 36813 / 35842: model DB then texture DB, all buckets
EndMemoryFree(mlm, st);                                     // 36816 / 35845
```

## Open risks

- The IO-thread writers of the buffers are inferred from `CellLoaderTask`'s state machine, not from a captured thread
  ID. If the work function in fact runs on the main thread, the IO pause is only a precaution. It still matches `pcb`.
- 13296 does not check the cell lock. 13370 clears the last interior slot when the cell is missing. 13434's overflow
  eviction unloads without checks. Use the explicit selection above to avoid all three.
- Cells evicted while the player is about to re-enter them are reloaded from disk, a streaming hitch. The engine
  accepts this in `pcb` and under memory pressure. Prefer cells far from the queued grid centre (13367 already sorts
  near cells forward).
- `ExtraDetachTime`-based respawn is unaffected: it is written on detach, not on unload. A cell unloaded early is
  reset no sooner than it would be otherwise (inference).
- The cost of 36813 and of the IO pause has not been measured. Rate-limit until it has.
- The single-texture-bucket gate (`TES+0x13c` / `+0x132`) and the `+0x134` / `+0x12a` byte are not covered by
  CommonLib's TES layout; do not rely on them by name.
