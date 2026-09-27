#include "GpuResources.h"

#include <chrono>

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

	std::optional<GpuResources::Buffer> GpuResources::Resolve(ID3D11Buffer* a_buffer)
	{
		if (!a_buffer)
			return std::nullopt;

		auto [it, inserted] = entries.try_emplace(a_buffer);
		auto& entry = it->second;
		entry.lastUsed = frame;
		if (!inserted)
			return entry.stable ? std::optional{ entry.buffer } : std::nullopt;

		// First use: hold a reference for as long as the entry lives, so the address cannot be reused.
		const auto start = std::chrono::steady_clock::now();
		entry.reference.copy_from(a_buffer);
		DxvkOrgInteropResourceInfo info{};
		entry.stable = RenderGraphRuntime::Get().DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0;
		if (entry.stable) {
			entry.buffer.vkBuffer = reinterpret_cast<std::uint64_t>(info.buffer.buffer);
			entry.buffer.offset = info.buffer.offset;
			entry.buffer.size = info.buffer.size;
			entry.buffer.address = info.buffer.address;
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
		return entry.stable ? std::optional{ entry.buffer } : std::nullopt;
	}

	bool GpuResources::Touch(ID3D11Buffer* a_buffer)
	{
		auto it = entries.find(a_buffer);
		if (it == entries.end() || !it->second.stable)
			return false;
		it->second.lastUsed = frame;
		return true;
	}

	void GpuResources::BeginFrame(std::uint32_t a_frame)
	{
		frame = a_frame;
		stats.resolvedThisFrame = 0;
		stats.resolveMs = 0.0;
		// A slow sweep is enough: entries only pin memory the game released.
		if ((a_frame % 64) != 0)
			return;
		for (auto it = entries.begin(); it != entries.end();) {
			if (a_frame - it->second.lastUsed > kEvictFrames) {
				(it->second.stable ? stats.cached : stats.rejected)--;
				it = entries.erase(it);
			} else {
				++it;
			}
		}
	}

	void GpuResources::Clear()
	{
		entries.clear();
		stats = {};
	}
}
