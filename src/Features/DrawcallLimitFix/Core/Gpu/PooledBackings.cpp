// PooledBackings (see the header): DCLF's allocate and retire hooks for its backing pools.
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/Runtime/ResourceCleanupQueue.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <stdexcept>
#include <string>

namespace DCLF::PooledBackings
{
	std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return 0;
		auto device = host->GetDesc().device;
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
}
