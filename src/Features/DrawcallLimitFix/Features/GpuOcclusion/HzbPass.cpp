// GpuOcclusion's depth pyramid (Api/DepthPyramid.h): the HZB, its program and its downsample's counter, made with the main view's
// resources, and the pass rebuilding it from the main depth in the depth epoch.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Features/GpuOcclusion/GpuOcclusion.h"
#include "Features/DrawcallLimitFix/Features/GpuOcclusion/HzbLayout.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Features::GpuOcclusion
{
	// The producer's state, held with the main view's resources (Resources::hzbState): the downsample's program. Its finished-group
	// count is the pyramid's scratch (Resources::hzbScratch): zeroed by the first depth commit, then by each dispatch's last group.
	struct HzbState
	{
		std::shared_ptr<const ComputeProgram> program;
	};
}

namespace DCLF::Draws
{
	// In DCLF::Draws, where they were defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).
	struct HzbBindings
	{
		org::DeclaredViewToken depth;
		org::DeclaredViewToken counter;
		std::vector<org::DeclaredViewToken> hzbMips;
	};

	struct HzbFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		// One single-pass downsample per kHzbDispatchMips levels, the first from scene depth, each next from the previous one's
		// last level. Up to 4096 texels a side takes one.
		struct Dispatch
		{
			Features::GpuOcclusion::HzbConstants constants{};
			std::uint32_t groupsX = 0, groupsY = 0;
		};
		std::vector<Dispatch> dispatches;
	};

	/**
	 * @brief Builds the HZB in the ZPrepass segment after DCLF's depth draws, so it holds the native occluders plus DCLF's. On
	 * AE that is before the first-person model's depth and the rooms' stencil draws: fewer occluders, never more. The chain
	 * is one single-pass downsample (HzbCS.hlsl, FidelityFX SPD): per-level dispatches would need a full barrier each.
	 */
	class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
	{
	public:
		HzbPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const Features::GpuOcclusion::HzbState> a_state, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), state(std::move(a_state)), segment(a_segment) {}

		HzbBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			HzbBindings bindings{};
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			bindings.counter = a_builder.UnorderedAccess(resources->hzbScratch).View();
			bindings.hzbMips.reserve(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				bindings.hzbMips.push_back(a_builder.UnorderedAccess(resources->hzb, org::UavView{ UINT32_MAX, mip }).View());
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		HzbFrame Prepare(const HzbBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			using namespace Features::GpuOcclusion;
			HzbFrame prepared{};
			// Its epoch is the schedule's (the depth epoch's phase 2 block: the PassContext it was made with), where the depth is the
			// world's final depth.
			const auto now = segment;
			if (!resources->hzb || !state->program || !resources->hzbScratch)
				return prepared;
			// The depth commit zeroed the group counter (hzbScratchZeroed).
			const auto frame = CurrentFrame(a_preparation, now);
			if (!frame)
				return prepared;
			const std::uint32_t renderWidth = frame->width ? frame->width : resources->width;
			const std::uint32_t renderHeight = frame->height ? frame->height : resources->height;
			prepared.program = state->program;
			const std::uint32_t depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			const std::uint32_t counterIndex = CaptureViewIndex(a_preparation, a_bindings.counter);
			std::vector<std::uint32_t> levelIndices(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				levelIndices[mip] = CaptureViewIndex(a_preparation, a_bindings.hzbMips[mip]);
			const auto levelSize = [&](std::uint32_t a_mip, std::uint32_t a_axis) {
				return std::max(1u, (a_axis == 0 ? resources->hzbWidth : resources->hzbHeight) >> a_mip);
			};
			for (std::uint32_t level = 0; level < resources->hzbMips;) {
				HzbFrame::Dispatch dispatch{};
				auto& constants = dispatch.constants;
				constants.counterIndex = counterIndex;
				if (level == 0) {
					// Mip 0 is half the padded domain; only the rendered area holds depth (the image can exceed the viewport).
					constants.fromDepth = 1;
					constants.sourceIndex = depthIndex;
					constants.domainSize[0] = resources->hzbWidth * 2;
					constants.domainSize[1] = resources->hzbHeight * 2;
					constants.validSize[0] = std::min(renderWidth, constants.domainSize[0]);
					constants.validSize[1] = std::min(renderHeight, constants.domainSize[1]);
				} else {
					constants.fromDepth = 0;
					constants.sourceIndex = levelIndices[level - 1];
					constants.domainSize[0] = constants.validSize[0] = levelSize(level - 1, 0);
					constants.domainSize[1] = constants.validSize[1] = levelSize(level - 1, 1);
				}
				constants.targetSize[0] = levelSize(level, 0);
				constants.targetSize[1] = levelSize(level, 1);
				// A group reduces its tile through six levels; the last group continues past the sixth only when that level fits
				// one tile (a domain of at most kHzbTile tiles a side).
				const std::uint32_t domain = std::max(constants.domainSize[0], constants.domainSize[1]);
				const std::uint32_t reach = domain / kHzbTile <= kHzbTile ? kHzbDispatchMips : 6u;
				constants.mips = std::min(resources->hzbMips - level, reach);
				for (std::uint32_t i = 0; i < constants.mips; ++i)
					constants.targetIndices[i] = levelIndices[level + i];
				dispatch.groupsX = (constants.domainSize[0] + kHzbTile - 1) / kHzbTile;
				dispatch.groupsY = (constants.domainSize[1] + kHzbTile - 1) / kHzbTile;
				constants.workGroups = dispatch.groupsX * dispatch.groupsY;
				level += constants.mips;
				prepared.dispatches.push_back(dispatch);
			}
			return prepared;
		}

		static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || a_frame.dispatches.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			for (std::size_t i = 0; i < a_frame.dispatches.size(); ++i) {
				const auto& dispatch = a_frame.dispatches[i];
				if (i != 0) {
					// The previous dispatch's last level must be complete before this one reads it.
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
					commands.Barriers(batch);
				}
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::GpuOcclusion::kHzbConstantWords,
					reinterpret_cast<const std::uint32_t*>(&dispatch.constants));
				commands.Dispatch(dispatch.groupsX, dispatch.groupsY, 1);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const Features::GpuOcclusion::HzbState> state;
		RenderGraphRuntime::Segment segment;
	};
}

namespace DCLF::Features::GpuOcclusion
{
	namespace
	{
		/**
		 * @brief The HZB: mip 0 is half the depth's next power of two, so a mip is chosen from a screen extent by log2 alone. Padded
		 * texels are filled with the far plane, which suppresses culling rather than causing it.
		 */
		std::shared_ptr<const void> Setup(const Api::DepthPyramidSetup& a_setup, Api::DepthPyramid& a_out)
		{
			auto nextPowerOfTwo = [](std::uint32_t a_value) {
				std::uint32_t result = 1;
				while (result < a_value)
					result <<= 1;
				return result;
			};
			a_out.width = std::max(1u, nextPowerOfTwo(a_setup.width) / 2);
			a_out.height = std::max(1u, nextPowerOfTwo(a_setup.height) / 2);
			a_out.mips = 1;
			for (std::uint32_t size = std::max(a_out.width, a_out.height); size > 1; size >>= 1)
				++a_out.mips;

			org::TextureDescription hzbDesc{};
			for (std::uint32_t mip = 0; mip < a_out.mips; ++mip)
				hzbDesc.imageDimensions.push_back({ std::max(1u, a_out.width >> mip), std::max(1u, a_out.height >> mip), 0, 0 });
			hzbDesc.format = rhi::Format::R32_Float;
			hzbDesc.channels = 1;
			hzbDesc.hasUAV = true;
			hzbDesc.uavFormat = hzbDesc.format;
			hzbDesc.hasSRV = true;
			hzbDesc.srvFormat = hzbDesc.format;
			a_out.texture = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
			a_out.texture->SetName("cs.dclf.hzb");
			auto state = std::make_shared<HzbState>();
			a_out.scratch = Draws::CreateWords(1, true, "cs.dclf.hzb-counter");

			state->program = ComputeProgram::Load(a_setup.device, { .source = kHzbShader, .constantWords = kHzbConstantWords });
			if (!state->program) {
				logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
				a_out.texture.reset();
			}
			return state;
		}

		// Only while the setup gave a texture (the depth epoch's phase 2 block).
		std::shared_ptr<org::RenderPass> MakePass(const Api::PassContext& a_context)
		{
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::HzbPass>(a_context.Main(), Extensions::DepthPyramidState<HzbState>(a_context), a_context.segment);
		}
	}

	void RegisterPyramid(Api::InstallContext& a_context)
	{
		a_context.RegisterDepthPyramid({ .resource = "cs.dclf.hzb", .scratchResource = "cs.dclf.hzb-counter", .setup = &Setup });
		a_context.RegisterPass({ .key = "gpu-occlusion.hzb", .passName = "cs.dclf.hzb", .make = &MakePass });
	}
}
