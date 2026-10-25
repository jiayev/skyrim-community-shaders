// The CPU-driven pipeline's pre-work (PreWork.h): each site's passes with CPU-mode work prepared on the render thread from the frame's
// committed shapes, and recorded on DXVK's CS thread into DXVK's command buffer, between the barriers ORG would have put around and
// between them. And the DirectPass's resolution of a versioned buffer through the adopted set (Core/Extensions/PassContext.h).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/PreWork.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"

#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <array>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	std::shared_ptr<org::Buffer> DirectPass::Version(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const
	{
		if (!a_buffer || !adopted.Names(*a_buffer))
			return nullptr;
		const auto& version = adopted.Of(*a_buffer).version;
		return version ? version->buffer : nullptr;
	}

	std::uint64_t DirectPass::Units(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const
	{
		return Draws::NamedUnits(adopted, a_buffer);
	}

	std::uint32_t DirectPass::Srv(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const
	{
		return Srv(Version(a_buffer));
	}

	std::uint32_t DirectPass::Uav(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const
	{
		return Uav(Version(a_buffer));
	}

	std::uint32_t DirectPass::Srv(const std::shared_ptr<org::Buffer>& a_buffer)
	{
		return a_buffer && a_buffer->HasSRV() ? a_buffer->GetSRVInfo(0).slot.index : 0u;
	}

	std::uint32_t DirectPass::Uav(const std::shared_ptr<org::Buffer>& a_buffer)
	{
		return a_buffer && a_buffer->HasUAVShaderVisible() ? a_buffer->GetUAVShaderVisibleInfo(0).slot.index : 0u;
	}
}

namespace DCLF::Direct
{
	namespace
	{
		// Each site's trigger (Direct.h: Site), the one its epochs are submitted at in the GPU path: its pre-work is theirs (the hosted
		// epochs' passes with their host's).
		constexpr std::array<std::string_view, static_cast<std::size_t>(Site::kCount)> kSiteTriggers{ "shadow-maps.after", "depth.committed", "opaque.after", "blended.draw",
			"fp.colour-render" };

		constexpr auto Access(std::initializer_list<rhi::ResourceAccessType> a_types)
		{
			std::uint64_t bits = 0;
			for (const auto type : a_types)
				bits |= static_cast<std::uint64_t>(type);
			return static_cast<rhi::ResourceAccessType>(bits);
		}
		constexpr auto Sync(std::initializer_list<rhi::ResourceSyncState> a_states)
		{
			std::uint32_t bits = 0;
			for (const auto state : a_states)
				bits |= static_cast<std::uint32_t>(state);
			return static_cast<rhi::ResourceSyncState>(bits);
		}

		using A = rhi::ResourceAccessType;
		using S = rhi::ResourceSyncState;
		// What the pre-work's passes do: copies (the latched copies) and dispatches, reading and writing buffers (and the local shadow
		// masks' images, DXVK's, imported in GENERAL: memory barriers order them, as ORG's for those imports do, with no layout change).
		constexpr auto kWorkSync = Sync({ S::Copy, S::ComputeShading });
		constexpr auto kWorkWrites = Access({ A::CopyDest, A::UnorderedAccess });
		// (No indirect-argument reads: its dispatches are direct, and its copy and compute stages cannot take that access.)
		constexpr auto kWorkAccess = Access({ A::CopyDest, A::CopySource, A::UnorderedAccess, A::ShaderResource, A::ConstantBuffer });
		// Before it: everything before in DXVK's buffer and the frames before (the draws reading last frame's wind buffer, the index pool,
		// the families' arguments; DXVK's own writes of the images and buffers it reads) against its copies and dispatches.
		constexpr rhi::GlobalBarrier kEntry{ S::All, kWorkSync,
			Access({ A::RenderTarget, A::UnorderedAccess, A::DepthReadWrite, A::CopyDest, A::RaytracingAccelerationStructureWrite }), kWorkAccess };
		// Between two passes: one's copies and dispatches against the next's.
		constexpr rhi::GlobalBarrier kBetween{ kWorkSync, kWorkSync, kWorkWrites, kWorkAccess };
		// After it: its writes against everything the site and DXVK do next: the draws' index, vertex, pixel and indirect-argument
		// reads, their copies, and any compute.
		constexpr rhi::GlobalBarrier kExit{ kWorkSync, S::All, kWorkWrites,
			Access({ A::IndexBuffer, A::VertexBuffer, A::ConstantBuffer, A::ShaderResource, A::IndirectArgument, A::UnorderedAccess, A::CopySource, A::CopyDest }) };

		void Barrier(rhi::CommandList& a_list, const rhi::GlobalBarrier& a_barrier)
		{
			const rhi::BarrierBatch batch{ {}, {}, { &a_barrier, 1 } };
			a_list.Barriers(batch);
		}

		/** @brief The heaps the site's shape names (the frame's resource and sampler heaps): what every pass's descriptors index. */
		void HeapsOf(Site a_site, const Draws::RevisionShapes& a_shapes, rhi::DescriptorHeapHandle& a_resources, rhi::DescriptorHeapHandle& a_samplers)
		{
			auto take = [&](const auto& a_shape) {
				if (a_shape && !a_resources.valid()) {
					a_resources = a_shape->resourceHeap;
					a_samplers = a_shape->samplerHeap;
				}
			};
			switch (a_site) {
			case Site::Shadow:
				take(a_shapes.shadow);
				break;
			case Site::Depth:
				take(a_shapes.main[Draws::kDepthShape]);
				break;
			case Site::Colour:
			case Site::Forward:
				take(a_shapes.main[Draws::kColourShape]);
				break;
			default:
				break;
			}
			for (const auto& view : a_shapes.views)
				take(view);
		}
	}

	std::shared_ptr<const PreWork> PreparePreWork(Site a_site, const PreWorkInputs& a_inputs)
	{
		if (!a_inputs.adopted || !a_inputs.shapes || a_site >= Site::kCount)
			return nullptr;
		const auto trigger = kSiteTriggers[static_cast<std::size_t>(a_site)];
		Api::DirectWork work;
		std::vector<std::shared_ptr<const void>> viewHolds;
		for (std::uint32_t e = 0, epochs = Draws::EpochCount(); e < epochs; ++e) {
			const auto& traits = Draws::EpochTraitsOf(e);
			// A hosted epoch's passes are its host's (the host's list names them), prepared with it.
			if (traits.hostedBy != Draws::kNoEpoch || Draws::TriggerName(Draws::EpochTrigger(e)) != trigger)
				continue;
			Api::DirectPass base{ .segment = traits.segment, .shapes = *a_inputs.shapes, .adopted = *a_inputs.adopted, .frameSlot = a_inputs.frameSlot };
			// A View epoch (fp.colour, sharing fp.depth's shape) is made from its view's resources, which its committed shape names.
			std::shared_ptr<Draws::ViewResources> view;
			switch (traits.shape) {
			case Draws::ShapeKind::Shadow:
				if (!a_inputs.shadow)
					continue;
				base.shadow = &a_inputs.shadow;
				break;
			case Draws::ShapeKind::View:
				if (!a_inputs.main || !a_inputs.shapes->views[traits.index])
					continue;
				view = a_inputs.main->views[traits.index].lock();
				if (!view)
					continue;
				viewHolds.push_back(view);
				base.main = &a_inputs.main;
				base.view = &view;
				break;
			default:
				if (!a_inputs.main)
					continue;
				base.main = &a_inputs.main;
				break;
			}
			Draws::PrepareDirectPasses(e, base, work);
		}
		if (work.records.empty())
			return nullptr;
		auto prepared = std::make_shared<PreWork>();
		prepared->site = a_site;
		prepared->records = std::move(work.records);
		prepared->passEnds = std::move(work.passEnds);
		prepared->holds = std::move(viewHolds);
		if (a_inputs.main)
			prepared->holds.push_back(a_inputs.main);
		if (a_inputs.shadow)
			prepared->holds.push_back(a_inputs.shadow);
		HeapsOf(a_site, *a_inputs.shapes, prepared->resourceHeap, prepared->samplerHeap);
		return prepared;
	}

	void RecordPreWork(const PreWork& a_work, rhi::CommandList& a_list)
	{
		if (a_work.records.empty())
			return;
		// The passes' descriptors index the frame's heaps, which DXVK's buffer has not bound (the site binds them again for its draws).
		if (a_work.resourceHeap.valid())
			a_list.SetDescriptorHeaps(a_work.resourceHeap, a_work.samplerHeap);
		Barrier(a_list, kEntry);
		std::size_t pass = 0;
		for (std::size_t r = 0; r < a_work.records.size(); ++r) {
			a_work.records[r](a_list);
			// A barrier after each pass's last record but the last pass's (the exit barrier is that one's).
			if (pass < a_work.passEnds.size() && r + 1 == a_work.passEnds[pass]) {
				++pass;
				if (r + 1 < a_work.records.size())
					Barrier(a_list, kBetween);
			}
		}
		Barrier(a_list, kExit);
	}
}
