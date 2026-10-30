// PooledBackings (see the header): DCLF's allocate and retire hooks for its backing pools.
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Render/Runtime/ResourceCleanupQueue.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <cassert>
#include <stdexcept>
#include <string>

namespace DCLF::PooledBackings
{
	std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return 0;
		auto device = host->Device();
		return device.GetBufferDeviceAddress({ const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), 0 });
	}

	org::services::BackingAllocation Allocate(const org::services::BackingRequest& a_request)
	{
		org::Resource::ScopedECSRegistrationSuppression suppressECS;
		std::shared_ptr<org::Buffer> buffer;
		if (a_request.like) {
			buffer = a_request.like->UnmaterializedLike();
			if (a_request.stride)
				buffer->ResizeStructured(static_cast<std::uint32_t>(a_request.capacityClass));
			else
				buffer->ResizeBytes(a_request.capacityClass);
		} else if (a_request.stride) {
			if (a_request.capacityClass > UINT32_MAX)
				throw std::length_error(fmt::format("[DCLF] {}: {} rows are past a structured buffer's elements", a_request.name, a_request.capacityClass));
			buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(a_request.capacityClass), a_request.stride,
				a_request.flags.unorderedAccess);
		} else {
			buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, a_request.capacityClass, a_request.flags.unorderedAccess);
		}
		if (!a_request.name.empty())
			buffer->SetName(std::string(a_request.name).c_str());
		buffer->Materialize();
		org::services::BackingAllocation allocation;
		allocation.address = a_request.flags.deviceAddress ? DeviceAddressOf(*buffer) : 0;
		assert(allocation.address % 16 == 0);  // a whole buffer; row and block offsets keep 16 (DrawPipelines.h)
		allocation.srvIndex = buffer->HasSRV() ? buffer->GetSRVInfo(0).slot.index : 0;
		allocation.buffer = std::move(buffer);
		return allocation;
	}

	void Retire(std::shared_ptr<org::Buffer> a_buffer)
	{
		if (!a_buffer)
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		auto cleanup = host ? host->ResourceCleanup() : nullptr;
		if (!cleanup)
			return;  // no graph host (process exit): nothing left to keep off a frame
		// The node's last reference drops here: the queue destroys the buffer on its cleanup lane.
		(void)cleanup->Make<std::shared_ptr<org::Buffer>>(std::move(a_buffer));
	}

	org::services::BackingPoolHooks Hooks()
	{
		org::services::BackingPoolHooks hooks;
		hooks.allocate = [](const org::services::BackingRequest& a_request) { return Allocate(a_request); };
		hooks.retire = [](std::shared_ptr<org::Buffer> a_buffer) { Retire(std::move(a_buffer)); };
		return hooks;
	}

	std::uint32_t PerFrameSpares()
	{
		// The newest frame's backing is still bound when the next is acquired: the other frames in flight retire in turn (measured: one
		// fewer makes a backing again every few frames; this many makes none once warm).
		return RenderGraphRuntime::kGameFramesInFlight - 1;
	}

	std::uint32_t SceneCopies()
	{
		static const std::uint32_t copies = [] {
			const auto& value = SwitchValue(Switch::SceneCopies);
			const auto asked = value.empty() ? 3u : static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
			return std::clamp(asked, 2u, RenderGraphRuntime::kGameFramesInFlight);
		}();
		return copies;
	}

	const org::services::ShrinkPolicy& Shrink()
	{
		static const org::services::ShrinkPolicy policy = [] {
			const auto& value = SwitchValue(Switch::Shrink);
			if (value == "off" || value == "0")
				return org::services::ShrinkPolicy::Never();
			if (value == "eager")
				return org::services::ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(0), 1);
			return org::services::ShrinkPolicy::WhenBelow(25, std::chrono::milliseconds(5000), 2);
		}();
		return policy;
	}
}
