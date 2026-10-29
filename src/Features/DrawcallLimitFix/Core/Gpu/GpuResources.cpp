// GpuResources: the buffer registries' resolution and leases, each instance on its owner's thread (see the header).

#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"

#include <chrono>
#include <exception>
#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF
{
	namespace
	{
		// Shared by the instances: a generation names one import whichever instance made it.
		std::atomic<std::uint64_t> nextGeneration{ 1 };

		// The owner's writes to a counter only it writes: a load and a store, no read-modify-write.
		template <class T, class U>
		void Bump(std::atomic<T>& a_counter, U a_amount)
		{
			a_counter.store(a_counter.load(std::memory_order_relaxed) + static_cast<T>(a_amount), std::memory_order_relaxed);
		}
	}

	struct GpuResources::Batch
	{
		std::vector<winrt::com_ptr<ID3D11Buffer>> references;  // the owner's: moved into the leases once done
		std::vector<IUnknown*> objects;
		std::vector<DxvkOrgInteropResourceInfo> infos;              // pinned
		std::vector<DxvkOrgInteropTrackedResource> tracked;        // pinless
		std::vector<HRESULT> results;
		bool pinless = false;
		bool described = false;  // the batched export answered (else Acquire describes each one alone)
		double ms = 0.0;
		std::atomic<bool> claimed{ false };  // taken by the job or by the owner, whichever describes first
		std::atomic<bool> done{ false };     // released with the answers
		bool noted = false;                  // the owner counted it (NoteResolve)

		/** @brief The description, by whoever claimed it (the job, or the owner finding it unstarted). Touches only the batch. */
		void Describe()
		{
			const auto start = std::chrono::steady_clock::now();
			described = pinless ? RenderGraphRuntime::Get().TrackResources(objects, tracked, results) : RenderGraphRuntime::Get().DescribeResources(objects, infos, results);
			ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		const DxvkOrgInteropResourceInfo& Info(std::uint32_t a_index) const { return pinless ? tracked[a_index].info : infos[a_index]; }

		~Batch()
		{
			// The tracked imports no lease took (their reference still here) end with the batch, on whichever thread drops it last.
			if (!pinless || !described)
				return;
			std::vector<IUnknown*> untaken;
			for (std::size_t i = 0; i < references.size(); ++i)
				if (SUCCEEDED(results[i]) && references[i])
					untaken.push_back(references[i].get());
			RenderGraphRuntime::Get().UntrackResources(untaken);
		}
	};

	GpuResources::Held::~Held()
	{
		// The last lease went behind the GPU's use (the leases' holders retire them so): the import ends, then the storages it read.
		if (tracked) {
			IUnknown* object = reference.get();
			RenderGraphRuntime::Get().UntrackResources({ &object, 1 });
		}
		storages.clear();
	}

	GpuResources& GpuResources::Scene()
	{
		static GpuResources resources;
		return resources;
	}

	GpuResources& GpuResources::Frame()
	{
		static GpuResources resources;
		return resources;
	}

	bool GpuResources::Enabled() const
	{
		return RenderGraphRuntime::Get().IsActive();
	}

	std::optional<GpuResources::LeasedBuffer> GpuResources::Acquire(ID3D11Buffer* a_buffer)
	{
		if (!a_buffer)
			return std::nullopt;
		if (const auto it = entries.find(a_buffer); it != entries.end()) {
			if (auto owner = it->second.owner.lock())
				return LeasedBuffer{ it->second.buffer, it->second.generation, std::shared_ptr<const void>(std::move(owner)) };
			// Its last lease went (the removal is queued): a buffer at this address now is resolved as new.
			if (const auto c = byCookie.find(it->second.cookie); c != byCookie.end() && c->second == a_buffer)
				byCookie.erase(c);
			entries.erase(it);
			Bump(counters.cached, -1);
		}

		// Resolved by this pass's Prefetch (or the pass before's), under its reference: the description is still this buffer's.
		for (auto* map : { &prefetched, &prefetchedBefore }) {
			const auto it = map->find(a_buffer);
			if (it == map->end())
				continue;
			const auto batch = std::move(it->second.batch);
			const std::uint32_t i = it->second.index;
			map->erase(it);
			Finish(*batch);
			if (!batch->described)
				break;  // no batched export: described alone below
			auto& reference = batch->references[i];
			if (Diagnostics::Observe().claimsOrScene) {
				Bump(counters.prefetchTaken, 1);
				reference->AddRef();
				Bump(counters.prefetchReleased, reference->Release() == 1 ? 1u : 0u);
			}
			const auto& info = batch->Info(i);
			if (SUCCEEDED(batch->results[i]) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0) {
				// A description older than a relocation taken names storage nothing holds for it: described again below (the batch
				// ends its import).
				if (batch->pinless && Superseded(batch->tracked[i].cookie, batch->tracked[i].storageSerial)) {
					Bump(counters.prefetchSuperseded, 1);
					break;
				}
				return Insert(a_buffer, std::move(reference),
					Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address },
					batch->pinless ? &batch->tracked[i] : nullptr);
			}
			Bump(counters.rejected, 1);
			return std::nullopt;
		}

		const auto start = std::chrono::steady_clock::now();
		winrt::com_ptr<ID3D11Buffer> reference;
		reference.copy_from(a_buffer);
		auto& runtime = RenderGraphRuntime::Get();
		std::optional<LeasedBuffer> lease;
		auto insert = [&](const DxvkOrgInteropResourceInfo& a_info, const DxvkOrgInteropTrackedResource* a_tracked) {
			lease = Insert(a_buffer, std::move(reference),
				Buffer{ reinterpret_cast<std::uint64_t>(a_info.buffer.buffer), a_info.buffer.offset, a_info.buffer.size, a_info.buffer.address }, a_tracked);
		};
		if (runtime.PinlessImports()) {
			IUnknown* object = a_buffer;
			DxvkOrgInteropTrackedResource tracked{};
			HRESULT result = E_FAIL;
			const bool imported = runtime.TrackResources({ &object, 1 }, { &tracked, 1 }, { &result, 1 }) && SUCCEEDED(result);
			if (imported && tracked.info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && tracked.info.buffer.address != 0)
				insert(tracked.info, &tracked);
			else if (imported)
				runtime.UntrackResources({ &object, 1 });
		} else {
			DxvkOrgInteropResourceInfo info{};
			if (runtime.DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0)
				insert(info, nullptr);
		}
		if (!lease)
			Bump(counters.rejected, 1);
		NoteResolve(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), 1);
		return lease;
	}

	GpuResources::LeasedBuffer GpuResources::Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved,
		const DxvkOrgInteropTrackedResource* a_tracked)
	{
		Entry entry;
		entry.buffer = a_resolved;
		entry.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
		if (!entry.generation)
			std::terminate();
		// The lease holds the reference the description was taken under for as long as it lives, so the address cannot be reused;
		// its release queues the entry's removal (any thread).
		auto* held = new Held;
		held->reference = std::move(a_reference);
		held->tracked = a_tracked != nullptr;
		std::shared_ptr<Held> owner(held, [released = released, key = a_buffer](Held* a_held) {
			delete a_held;
			released->Push(key);
		});
		entry.owner = owner;
		if (a_tracked) {
			entry.cookie = a_tracked->cookie;
			entry.serial = a_tracked->storageSerial;
			byCookie[entry.cookie] = a_buffer;
		}
		LeasedBuffer lease{ entry.buffer, entry.generation, std::shared_ptr<const void>(std::move(owner)) };
		entries.insert_or_assign(a_buffer, std::move(entry));
		Bump(counters.cached, 1);
		return lease;
	}

	void GpuResources::PostRelocations(std::span<const ImportRelocations::Relocation> a_relocations)
	{
		std::scoped_lock lock(inboxMutex);
		inbox.insert(inbox.end(), a_relocations.begin(), a_relocations.end());
	}

	bool GpuResources::Superseded(std::uint64_t a_cookie, std::uint64_t a_serial) const
	{
		const auto it = moved.find(a_cookie);
		return it != moved.end() && it->second.serial > a_serial;
	}

	void GpuResources::TakeRelocations()
	{
		++passes;
		relocatedBuffers.clear();  // the last BeginFrame's, taken or not (Frame()'s holders never re-acquire)
		std::vector<ImportRelocations::Relocation> taken;
		{
			std::scoped_lock lock(inboxMutex);
			taken.swap(inbox);
		}
		for (auto& relocation : taken) {
			auto& seen = moved[relocation.cookie];
			seen.serial = std::max(seen.serial, relocation.serial);
			seen.pass = passes;
			const auto c = byCookie.find(relocation.cookie);
			const auto e = c != byCookie.end() ? entries.find(c->second) : entries.end();
			// Not this instance's, its last lease gone, or imported after the move: nothing reads the old storage here.
			if (e == entries.end() || e->second.cookie != relocation.cookie || e->second.serial >= relocation.serial) {
				Bump(counters.relocationsIgnored, 1);
				continue;
			}
			if (auto owner = e->second.owner.lock()) {
				std::scoped_lock lock(owner->mutex);
				owner->storages.push_back(std::move(relocation.storage));
			}
			relocatedBuffers.push_back(c->second);
			entries.erase(e);
			byCookie.erase(c);
			Bump(counters.cached, -1);
			Bump(counters.relocated, 1);
		}
		// A prefetched description is taken within two passes (BeginFrame's): the moves older than that name none.
		if (!moved.empty() && passes % 8 == 0)
			std::erase_if(moved, [&](const auto& a_moved) { return a_moved.second.pass + 3 < passes; });
	}

	void GpuResources::NoteResolve(double a_ms, std::uint64_t a_buffers)
	{
		Bump(counters.resolvedTotal, a_buffers);
		Bump(counters.resolveMsTotal, a_ms);
		if (a_ms > counters.resolveMsMax.load(std::memory_order_relaxed))
			counters.resolveMsMax.store(a_ms, std::memory_order_relaxed);
	}

	void GpuResources::Prefetch(std::span<ID3D11Buffer* const> a_buffers)
	{
		std::shared_ptr<Batch> batch;
		for (auto* buffer : a_buffers) {
			if (!buffer || prefetched.contains(buffer) || prefetchedBefore.contains(buffer))
				continue;
			if (const auto it = entries.find(buffer); it != entries.end() && !it->second.owner.expired())
				continue;
			if (!batch)
				batch = std::make_shared<Batch>();
			// Named once; described on the pool, under this reference (taken here, on the owner's thread).
			prefetched.emplace(buffer, Prefetched{ batch, static_cast<std::uint32_t>(batch->objects.size()) });
			batch->references.emplace_back().copy_from(buffer);
			batch->objects.push_back(buffer);
		}
		if (!batch)
			return;
		batch->pinless = RenderGraphRuntime::Get().PinlessImports();
		if (batch->pinless)
			batch->tracked.resize(batch->objects.size());
		else
			batch->infos.resize(batch->objects.size());
		batch->results.assign(batch->objects.size(), E_FAIL);
		Bump(counters.prefetchBatches, 1);
		Bump(counters.prefetched, batch->objects.size());
		// The job holds the batch, so its answers outlive whatever the owner drops meanwhile. A refused job is left to Finish.
		SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF buffer prefetch",
			[batch](const auto&) {
				ZoneScopedN("CS.DCLF.Scene.PrefetchBuffers.Describe");
				if (batch->claimed.exchange(true, std::memory_order_acq_rel))
					return;
				batch->Describe();
				batch->done.store(true, std::memory_order_release);
				batch->done.notify_all();
			});
	}

	void GpuResources::Finish(Batch& a_batch)
	{
		if (!a_batch.done.load(std::memory_order_acquire)) {
			const auto start = std::chrono::steady_clock::now();
			if (!a_batch.claimed.exchange(true, std::memory_order_acq_rel)) {
				// Not started (the pool is busy): described here, the job finding it claimed.
				a_batch.Describe();
				a_batch.done.store(true, std::memory_order_relaxed);
				Bump(counters.prefetchRunByOwner, 1);
			} else {
				// Running on a worker (synchronizing with DXVK's worker): waited for, as ParallelFor waits for a claimed chunk.
				ZoneScopedN("CS.DCLF.Scene.PrefetchBuffers.Wait");
				a_batch.done.wait(false, std::memory_order_acquire);
				Bump(counters.prefetchWaited, 1);
				Bump(counters.prefetchWaitMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
		}
		if (!std::exchange(a_batch.noted, true) && a_batch.described)
			NoteResolve(a_batch.ms, a_batch.objects.size());
	}

	void GpuResources::BeginFrame()
	{
		// The pass before's batches go (their references with them, on whichever thread drops a batch last); this pass's stay one more.
		prefetchedBefore = std::move(prefetched);
		prefetched.clear();
		released->Drain([this](ID3D11Buffer*&& a_key) {
			// A key resolved again since holds a live lease: that entry stays.
			if (const auto it = entries.find(a_key); it != entries.end() && it->second.owner.expired()) {
				if (const auto c = byCookie.find(it->second.cookie); c != byCookie.end() && c->second == a_key)
					byCookie.erase(c);
				entries.erase(it);
				Bump(counters.cached, -1);
			}
		});
		TakeRelocations();
	}

	GpuResources::Stats GpuResources::GetStats() const
	{
		Stats stats;
		stats.cached = counters.cached.load(std::memory_order_relaxed);
		stats.rejected = counters.rejected.load(std::memory_order_relaxed);
		stats.resolvedTotal = counters.resolvedTotal.load(std::memory_order_relaxed);
		stats.prefetchBatches = counters.prefetchBatches.load(std::memory_order_relaxed);
		stats.prefetched = counters.prefetched.load(std::memory_order_relaxed);
		stats.resolveMsTotal = counters.resolveMsTotal.load(std::memory_order_relaxed);
		stats.resolveMsMax = counters.resolveMsMax.load(std::memory_order_relaxed);
		stats.prefetchTaken = counters.prefetchTaken.load(std::memory_order_relaxed);
		stats.prefetchReleased = counters.prefetchReleased.load(std::memory_order_relaxed);
		stats.prefetchRunByOwner = counters.prefetchRunByOwner.load(std::memory_order_relaxed);
		stats.prefetchWaited = counters.prefetchWaited.load(std::memory_order_relaxed);
		stats.prefetchWaitMs = counters.prefetchWaitMs.load(std::memory_order_relaxed);
		stats.relocated = counters.relocated.load(std::memory_order_relaxed);
		stats.relocationsIgnored = counters.relocationsIgnored.load(std::memory_order_relaxed);
		stats.prefetchSuperseded = counters.prefetchSuperseded.load(std::memory_order_relaxed);
		return stats;
	}

	void GpuResources::Clear()
	{
		entries.clear();
		prefetched.clear();
		prefetchedBefore.clear();
		released->Discard();
		byCookie.clear();
		moved.clear();
		relocatedBuffers.clear();
		{
			std::scoped_lock lock(inboxMutex);
			inbox.clear();
		}
		for (auto* counter : { &counters.cached, &counters.rejected })
			counter->store(0, std::memory_order_relaxed);
		for (auto* counter : { &counters.resolvedTotal, &counters.prefetchBatches, &counters.prefetched, &counters.prefetchTaken, &counters.prefetchReleased,
				 &counters.prefetchRunByOwner, &counters.prefetchWaited, &counters.relocated, &counters.relocationsIgnored, &counters.prefetchSuperseded })
			counter->store(0, std::memory_order_relaxed);
		counters.resolveMsTotal.store(0.0, std::memory_order_relaxed);
		counters.prefetchWaitMs.store(0.0, std::memory_order_relaxed);
		counters.resolveMsMax.store(0.0, std::memory_order_relaxed);
	}
}
