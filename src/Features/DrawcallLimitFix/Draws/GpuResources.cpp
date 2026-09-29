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

		const auto start = std::chrono::steady_clock::now();
		DxvkOrgInteropResourceInfo info{};
		const bool stable = RenderGraphRuntime::Get().DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0;
		std::optional<LeasedBuffer> lease;
		if (stable) {
			Entry entry;
			entry.buffer.vkBuffer = reinterpret_cast<std::uint64_t>(info.buffer.buffer);
			entry.buffer.offset = info.buffer.offset;
			entry.buffer.size = info.buffer.size;
			entry.buffer.address = info.buffer.address;
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
			lease = LeasedBuffer{ entry.buffer, entry.generation, std::move(owner) };
			entries.emplace(a_buffer, std::move(entry));
			++stats.cached;
		} else {
			++stats.rejected;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++stats.resolvedThisFrame;
		++stats.resolvedTotal;
		stats.resolveMs += ms;
		stats.resolveMsTotal += ms;
		stats.resolveMsMax = std::max(stats.resolveMsMax, ms);
		return lease;
	}

	void GpuResources::BeginFrame()
	{
		stats.resolvedThisFrame = 0;
		stats.resolveMs = 0.0;
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
		stats = {};
	}
}
