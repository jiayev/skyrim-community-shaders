#include "GpuResources.h"

#include <chrono>
#include <exception>

#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/RenderGraphRuntime.h"

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
				return LeasedBuffer{ it->second.buffer, it->second.generation, std::move(owner) };
			// Its last lease went (the removal is queued): a buffer at this address now is resolved as new.
			entries.erase(it);
			Bump(counters.cached, -1);
		}

		// Resolved by this pass's Prefetch, under its reference: the description is still this buffer's.
		if (const auto it = prefetched.find(a_buffer); it != prefetched.end()) {
			std::optional<LeasedBuffer> lease;
			auto& entry = it->second;
			if (SwitchEnabled(Switch::SetParity) || SwitchEnabled(Switch::PersistentParity)) {
				Bump(counters.prefetchTaken, 1);
				entry.reference->AddRef();
				Bump(counters.prefetchReleased, entry.reference->Release() == 1 ? 1u : 0u);
			}
			if (entry.buffer)
				lease = Insert(a_buffer, std::move(entry.reference), *entry.buffer);
			else
				Bump(counters.rejected, 1);
			prefetched.erase(it);
			return lease;
		}

		const auto start = std::chrono::steady_clock::now();
		winrt::com_ptr<ID3D11Buffer> reference;
		reference.copy_from(a_buffer);
		DxvkOrgInteropResourceInfo info{};
		const bool stable = RenderGraphRuntime::Get().DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0;
		std::optional<LeasedBuffer> lease;
		if (stable)
			lease = Insert(a_buffer, std::move(reference), Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address });
		else
			Bump(counters.rejected, 1);
		NoteResolve(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), 1);
		return lease;
	}

	GpuResources::LeasedBuffer GpuResources::Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved)
	{
		Entry entry;
		entry.buffer = a_resolved;
		entry.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
		if (!entry.generation)
			std::terminate();
		// The lease holds the reference the description was taken under for as long as it lives, so the address cannot be reused;
		// its release queues the entry's removal (any thread).
		std::shared_ptr<const void> owner(new winrt::com_ptr<ID3D11Buffer>(std::move(a_reference)),
			[released = released, key = a_buffer](const winrt::com_ptr<ID3D11Buffer>* a_reference) {
				delete a_reference;
				released->Push(key);
			});
		entry.owner = owner;
		LeasedBuffer lease{ entry.buffer, entry.generation, std::move(owner) };
		entries.emplace(a_buffer, std::move(entry));
		Bump(counters.cached, 1);
		return lease;
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
		std::vector<IUnknown*> wanted;
		std::vector<ID3D11Buffer*> keys;
		for (auto* buffer : a_buffers) {
			if (!buffer || prefetched.contains(buffer))
				continue;
			if (const auto it = entries.find(buffer); it != entries.end() && !it->second.owner.expired())
				continue;
			// Named once; described below, under this reference.
			Prefetched entry;
			entry.reference.copy_from(buffer);
			prefetched.emplace(buffer, std::move(entry));
			keys.push_back(buffer);
			wanted.push_back(buffer);
		}
		if (keys.empty())
			return;
		const auto start = std::chrono::steady_clock::now();
		std::vector<DxvkOrgInteropResourceInfo> infos(keys.size());
		std::vector<HRESULT> results(keys.size(), E_FAIL);
		if (!RenderGraphRuntime::Get().DescribeResources(wanted, infos, results)) {
			// No batched export: Acquire resolves each as before.
			for (auto* key : keys)
				prefetched.erase(key);
			return;
		}
		for (std::size_t i = 0; i < keys.size(); ++i) {
			const auto& info = infos[i];
			if (SUCCEEDED(results[i]) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0)
				prefetched[keys[i]].buffer = Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address };
		}
		Bump(counters.prefetchBatches, 1);
		Bump(counters.prefetched, keys.size());
		NoteResolve(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), keys.size());
	}

	void GpuResources::BeginFrame()
	{
		prefetched.clear();
		released->Drain([this](ID3D11Buffer*&& a_key) {
			// A key resolved again since holds a live lease: that entry stays.
			if (const auto it = entries.find(a_key); it != entries.end() && it->second.owner.expired()) {
				entries.erase(it);
				Bump(counters.cached, -1);
			}
		});
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
		return stats;
	}

	void GpuResources::Clear()
	{
		entries.clear();
		prefetched.clear();
		released->Discard();
		for (auto* counter : { &counters.cached, &counters.rejected })
			counter->store(0, std::memory_order_relaxed);
		for (auto* counter : { &counters.resolvedTotal, &counters.prefetchBatches, &counters.prefetched, &counters.prefetchTaken, &counters.prefetchReleased })
			counter->store(0, std::memory_order_relaxed);
		counters.resolveMsTotal.store(0.0, std::memory_order_relaxed);
		counters.resolveMsMax.store(0.0, std::memory_order_relaxed);
	}
}
