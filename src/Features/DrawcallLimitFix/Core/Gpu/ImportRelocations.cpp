// ImportRelocations: DXVK's moves of the pinless imports, taken on the render thread and routed to their importers (see the header).

#include "Features/DrawcallLimitFix/Core/Gpu/ImportRelocations.h"

#include <array>
#include <vector>

#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF::ImportRelocations
{
	namespace
	{
		std::atomic<std::uint64_t> imagesTaken{ 0 }, buffersTaken{ 0 };
		std::atomic<std::int64_t> storagesHeld{ 0 };

		// The old storage, given back to DXVK by its last holder (any thread).
		std::shared_ptr<const void> Hold(void* a_storage)
		{
			storagesHeld.fetch_add(1, std::memory_order_relaxed);
			return std::shared_ptr<const void>(a_storage, [](const void* a_held) {
				void* storage = const_cast<void*>(a_held);
				RenderGraphRuntime::Get().ReleaseStorage({ &storage, 1 });
				storagesHeld.fetch_sub(1, std::memory_order_relaxed);
			});
		}
	}

	void Poll()
	{
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.PinlessImports())
			return;
		ZoneScopedN("CS.DCLF.Imports.PollRelocations");
		std::array<DxvkOrgInteropRelocation, 256> taken{};
		std::vector<Relocation> images, buffers;
		for (;;) {
			const std::uint32_t count = runtime.PollRelocations(taken);
			for (std::uint32_t i = 0; i < count; ++i) {
				Relocation relocation{ taken[i].cookie, taken[i].storageSerial, Hold(taken[i].storage) };
				(taken[i].kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE ? images : buffers).push_back(std::move(relocation));
			}
			if (count < taken.size())
				break;
		}
		imagesTaken.fetch_add(images.size(), std::memory_order_relaxed);
		buffersTaken.fetch_add(buffers.size(), std::memory_order_relaxed);
		if (!images.empty())
			GpuTextures::Get().NoteRelocations(images);
		// Each instance imports on its own (a buffer both use is two imports): each takes what names its own.
		if (!buffers.empty()) {
			GpuResources::Scene().PostRelocations(buffers);
			GpuResources::Frame().PostRelocations(buffers);
		}
	}

	Stats GetStats()
	{
		return { imagesTaken.load(std::memory_order_relaxed), buffersTaken.load(std::memory_order_relaxed), storagesHeld.load(std::memory_order_relaxed) };
	}
}
