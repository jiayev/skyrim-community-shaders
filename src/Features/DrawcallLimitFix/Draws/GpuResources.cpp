#include "GpuResources.h"

#include <chrono>
#include <exception>

#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF
{
	GpuResources& GpuResources::Get()
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
			--stats.cached;
		}

		// Resolved by this frame's Prefetch.
		if (const auto it = prefetched.find(a_buffer); it != prefetched.end()) {
			std::optional<LeasedBuffer> lease;
			if (it->second)
				lease = Insert(a_buffer, *it->second);
			else
				++stats.rejected;
			prefetched.erase(it);
			return lease;
		}

		const auto start = std::chrono::steady_clock::now();
		DxvkOrgInteropResourceInfo info{};
		const bool stable = RenderGraphRuntime::Get().DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0;
		std::optional<LeasedBuffer> lease;
		if (stable)
			lease = Insert(a_buffer, Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address });
		else
			++stats.rejected;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++stats.resolvedThisFrame;
		++stats.resolvedTotal;
		stats.resolveMs += ms;
		stats.resolveMsTotal += ms;
		stats.resolveMsMax = std::max(stats.resolveMsMax, ms);
		return lease;
	}

	GpuResources::LeasedBuffer GpuResources::Insert(ID3D11Buffer* a_buffer, const Buffer& a_resolved)
	{
		Entry entry;
		entry.buffer = a_resolved;
		entry.generation = nextGeneration++;
		if (!entry.generation)
			std::terminate();
		// The lease holds a reference for as long as it lives, so the address cannot be reused; its release queues
		// the entry's removal.
		winrt::com_ptr<ID3D11Buffer> reference;
		reference.copy_from(a_buffer);
		std::shared_ptr<const void> owner(new winrt::com_ptr<ID3D11Buffer>(std::move(reference)),
			[released = released, key = a_buffer](const winrt::com_ptr<ID3D11Buffer>* a_reference) {
				delete a_reference;
				const std::lock_guard lock(released->mutex);
				released->keys.push_back(key);
			});
		entry.owner = owner;
		LeasedBuffer lease{ entry.buffer, entry.generation, std::move(owner) };
		entries.emplace(a_buffer, std::move(entry));
		++stats.cached;
		return lease;
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
			prefetched.emplace(buffer, std::nullopt);  // named once; filled below
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
				prefetched[keys[i]] = Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address };
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++stats.prefetchBatches;
		stats.prefetched += keys.size();
		stats.resolvedThisFrame += static_cast<std::uint32_t>(keys.size());
		stats.resolvedTotal += keys.size();
		stats.resolveMs += ms;
		stats.resolveMsTotal += ms;
		stats.resolveMsMax = std::max(stats.resolveMsMax, ms);
	}

	void GpuResources::BeginFrame()
	{
		stats.resolvedThisFrame = 0;
		stats.resolveMs = 0.0;
		prefetched.clear();
		std::vector<ID3D11Buffer*> keys;
		{
			const std::lock_guard lock(released->mutex);
			keys.swap(released->keys);
		}
		for (auto* key : keys) {
			// A key resolved again since holds a live lease: that entry stays.
			if (const auto it = entries.find(key); it != entries.end() && it->second.owner.expired()) {
				entries.erase(it);
				--stats.cached;
			}
		}
	}

	void GpuResources::Clear()
	{
		entries.clear();
		prefetched.clear();
		stats = {};
	}
}
