# DXVK: unlocked allocation-pool access on the import and sparse paths

Found 2026-10-03 while hunting DCLF parity bugs. Fixed in our DXVK fork (`extern/dxvk`, `src/dxvk/dxvk_memory.cpp`). The
same code is in upstream DXVK master as of 2026-10-03 (the last change to `dxvk_memory.cpp` there is from 2026-08-25).

## The crash

An access violation in `DxvkResourceAllocationPool::alloc` (`dxvk_memory.h`, `m_next = list->next`), reading address
`0x1`. It happened during an engine `ID3D11Device::CreateBuffer` on Skyrim's loading thread:

```
dxvk::DxvkResourceAllocationPool::alloc            dxvk_memory.h:746
dxvk::DxvkMemoryAllocator::createAllocation        dxvk_memory.cpp:1551
dxvk::DxvkMemoryAllocator::allocateMemory          dxvk_memory.cpp:909
dxvk::DxvkMemoryAllocator::createBufferResource
dxvk::DxvkBuffer::DxvkBuffer
dxvk::D3D11Buffer::D3D11Buffer
dxvk::D3D11Device::CreateBuffer
SkyrimSE.exe+0E46498 (the engine's buffer creation, loading a skinned mesh)
```

It occurred once in about ten 60-second camera-traversal runs with the DCLF parity checks on.

## The cause

`DxvkResourceAllocationPool` is a free list of `DxvkResourceAllocation` storage with no synchronization of its own.
`alloc()` pops the head and `recycle()` pushes onto it. Its contract is that the owning `DxvkMemoryAllocator` only
touches it under `m_mutex`, and almost every path does:

- `createAllocation(type, pool, ...)` and `createAllocation(type, memory, ...)` run inside `allocateMemory` /
  `allocateDedicatedMemory`, which hold `m_mutex`;
- `freeAllocation` and `freeCachedAllocationsLocked` hold `m_mutex` when they call `m_allocationPool.free`.

Three paths call `m_allocationPool.create` without the lock:

| Function | Reached from |
| --- | --- |
| `importBufferResource` | `DxvkDevice::importBuffer`: D3D11on12 wrapped buffers, and our ORG interop (`dxvkCreateBufferFromVkBuffer`) |
| `importImageResource` | `DxvkDevice::importImage`: swapchain images in `Presenter::createSwapChain`, D3D11on12 wrapped textures |
| `createAllocation(DxvkSparsePageTable*, ...)` | `createBufferResource` for every sparse buffer, and `createImageResource` for a sparse image without metadata pages |

When one of these pops the list while another thread pops or pushes under the lock, both threads can take the same
storage. Alternatively, a node can be lost or relinked to storage that is now a live allocation. The value `0x1` in the
crash fits this: a live allocation's first word is its reference count. If that allocation's storage is still linked as
the tail of the free list, its count of 1 is later read as the `next` pointer.

## How we triggered it

Community Shaders' render graph wraps OpenRenderGraph buffers as D3D11 buffers
(`RenderGraphRuntime::WrapBuffer` → `dxvkCreateBufferFromVkBuffer` → `importBuffer`). It does so on the render thread,
when DCLF's buffers grow and on every parity-check readback (the fade log and tree-wind buffers). Skyrim's loading thread
creates buffers at the same time, through the locked path. The parity runs import often enough to hit the window about
once a minute of traversal.

## Can upstream DXVK trigger it?

Yes. D3D11 devices are free-threaded, so any of the three paths can run while another thread creates or releases a
resource:

1. **Tiled resources.** `CreateBuffer` with `D3D11_RESOURCE_MISC_TILED` always takes the unlocked sparse path. So does
   `CreateTexture2D` with `D3D11_RESOURCE_MISC_TILED`, when the image needs no metadata pages (the common case). An app
   that creates tiled resources on one thread while another thread creates or releases ordinary resources can corrupt
   the pool. This is the easiest upstream reproduction:
   - thread A loops `CreateBuffer(D3D11_RESOURCE_MISC_TILED)` / `Release`;
   - thread B loops ordinary `CreateBuffer` / `Release`.

   Releases matter too, because `freeAllocation` pushes onto the same list.
2. **Swapchain recreation.** `D3D11SwapChain::PresentImage` → `Presenter::acquireNextImage` → `recreateSwapChain` →
   `createSwapChain` imports each swapchain image. This runs on the presenting thread, which holds only the immediate
   context's lock, not the allocator's. A recreation (window resize, fullscreen toggle, `VK_ERROR_OUT_OF_DATE_KHR`,
   a changed sync interval or latency mode) that coincides with resource creation on a loading thread races. It is rare
   but plausible in games that stream assets on worker threads.
3. **D3D11on12.** `D3D11on12Device::CreateWrappedResource` imports, on whatever thread calls it.

D3D9 is mostly not exposed. A `D3DCREATE_MULTITHREADED` device serializes its calls under the device lock, and without
the flag concurrent calls are already undefined. D3D9 does not use sparse resources.

The window is small (a few instructions between the load of `m_next` and the store), so on upstream it would show up as
rare, unreproducible heap corruption or crashes in the allocator rather than a deterministic failure.

## The fix

The three paths take `m_mutex` around the pool access. The rest of each import (filling in the allocation,
`getBufferDeviceAddress`) stays outside the lock:

```cpp
// importBufferResource and importImageResource
Rc<DxvkResourceAllocation> allocation;
{ std::lock_guard<dxvk::mutex> lock(m_mutex);
  allocation = m_allocationPool.create(this, nullptr); }

// createAllocation(DxvkSparsePageTable*, const DxvkAllocationInfo&): its callers hold no lock
std::lock_guard<dxvk::mutex> lock(m_mutex);
auto allocation = m_allocationPool.create(this, nullptr);
```

None of these callers already holds `m_mutex` (a `dxvk::mutex`, not recursive). `createBufferResource` and
`createImageResource` take it only inside `allocateMemory`, and the imports are called from `DxvkBuffer` / `DxvkImage`
construction.

An alternative is to make the pool itself thread-safe, with a lock-free list or its own mutex. That would put the
contract in the shared component instead of in every caller. But every other user already holds `m_mutex`, so taking it
on the three missing paths is the smallest change and the easiest one to upstream.

## Verification

- With the fix, three 60-second traversal runs with every DCLF parity check on had no crash (trav28 to trav30).
  Before it, about one run in ten crashed. Given that rate, these runs agree with the fix but don't prove it on their
  own.
- The stress test in item 1 above would show the upstream problem directly; it has not been written yet.
