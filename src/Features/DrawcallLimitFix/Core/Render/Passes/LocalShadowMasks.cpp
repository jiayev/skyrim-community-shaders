// The local shadow lights' masks for DCLF's receivers (LocalShadowMaskCS.hlsl; dclf-architecture.md, "Local shadow masks"): one dispatch
// over kSHADOW_MASK at the colour segment's start, from the main depth, the shadow maps and the colour commit's mask records.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

#include <Resources/GloballyIndexedResource.h>

#include <algorithm>
#include <cmath>

namespace DCLF::Draws
{
	struct LocalShadowMaskBindings
	{
		org::DeclaredViewToken mask, depth;
		std::array<org::DeclaredViewToken, 2> maps;
	};

	struct LocalShadowMaskFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		LocalShadowMaskConstants constants{};
		std::uint32_t groupsX = 0, groupsY = 0;
	};

	/**
	 * @brief Resolves each local shadow light's channel of kSHADOW_MASK for every pixel, after the colour commit latched the records
	 * (the latched copies: the frame constants) and before the colour draws, which read the mask (MainOpaquePass declares it). Channels
	 * without a record keep what the engine drew. The records are the commit's (the latch), so a recording reused across frames resolves
	 * each frame's lights.
	 */
	class LocalShadowMaskPass final : public org::TypedRenderGraphPass<LocalShadowMaskPass, LocalShadowMaskFrame, LocalShadowMaskBindings>
	{
	public:
		LocalShadowMaskPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		LocalShadowMaskBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			LocalShadowMaskBindings bindings{};
			bindings.mask = a_builder.UnorderedAccess(resources->shadowMask).View();
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			// Every slice: an array's default view is one slice's (a record's slice past it loads 0, which reads as shadowed).
			for (std::uint32_t i = 0; i < 2; ++i) {
				const org::SrvView maps{ resources->shadow->depthLayers[i] > 1 ? static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) : UINT32_MAX };
				bindings.maps[i] = a_builder.ShaderResource(resources->shadow->depth[i], maps).View();
			}
			// The frame constants (PS b12, read by address) as the latched copies left them.
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			a_out.push_back(frame ? frame->generation : 0);
		}

		LocalShadowMaskFrame Prepare(const LocalShadowMaskBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			LocalShadowMaskFrame prepared{};
			const auto frame = CurrentFrame(a_preparation, segment);
			if (!frame || !frame->latch || !resources->localShadowMasks || !resources->frameConstantsAddress)
				return prepared;
			prepared.program = resources->localShadowMasks;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			constants.maskIndex = CaptureViewIndex(a_preparation, a_bindings.mask);
			constants.depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			for (std::uint32_t i = 0; i < 2; ++i)
				constants.shadowMapsIndex[i] = CaptureViewIndex(a_preparation, a_bindings.maps[i]);
			const std::uint64_t frameBlock = resources->frameConstantsAddress + FrameSlotOffset(true, 12);
			constants.frameBlock[0] = static_cast<std::uint32_t>(frameBlock);
			constants.frameBlock[1] = static_cast<std::uint32_t>(frameBlock >> 32);
			constants.width = resources->shadowMaskWidth;
			constants.height = resources->shadowMaskHeight;
			// The rendered area alone: under dynamic resolution (an upscaler) the frame covers the mask's top-left ratio of each axis,
			// as the shader's clip position takes it (FrameBuffer c44: 1 / ratio).
			float widthRatio = 1.0f, heightRatio = 1.0f;
			if (const auto* state = RE::BSGraphics::State::GetSingleton()) {
				const auto& data = state->GetRuntimeData();
				widthRatio = std::clamp(data.dynamicResolutionWidthRatio, 0.0f, 1.0f);
				heightRatio = std::clamp(data.dynamicResolutionHeightRatio, 0.0f, 1.0f);
			}
			const auto width = std::min(constants.width, static_cast<std::uint32_t>(std::ceil(constants.width * widthRatio)));
			const auto height = std::min(constants.height, static_cast<std::uint32_t>(std::ceil(constants.height * heightRatio)));
			prepared.groupsX = (width + 7) / 8;
			prepared.groupsY = (height + 7) / 8;
			return prepared;
		}

		static void Record(const LocalShadowMaskBindings&, const LocalShadowMaskFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.groupsX || !a_frame.groupsY)
				return;
			auto constants = a_frame.constants;
			constants.latchRegion = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kLocalShadowMaskConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groupsX, a_frame.groupsY, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	std::shared_ptr<org::RenderPass> MakeLocalShadowMaskPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment)
	{
		const auto& shadow = a_resources->shadow;
		if (!a_resources->shadowMask || !a_resources->localShadowMasks || !shadow || !shadow->depth[0] || !shadow->depth[1])
			return nullptr;
		return std::make_shared<LocalShadowMaskPass>(std::move(a_resources), a_segment);
	}
}
