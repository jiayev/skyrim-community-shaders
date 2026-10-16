#pragma once

// The draw passes' recording helpers: attachments, the frame buffers' declaration, the preprocessed indirect draw, and the frame slots'
// addresses a draw pass pushes. Self-contained (a feature's draw pass includes it: Features/Forward's); PassHelpers.h includes it.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF::Draws
{
	/** @brief A pass's frame buffers (t16 and up) and Light Limit Fix's lights (t35-t37) declared as read, through no view. */
	void DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, bool a_lights = true);

	/**
	 * @brief One explicitly preprocessed draw pass (the forward ranges'): the preprocess on the frame slot's state list, set up as the
	 * execution is (a_begin's attachments, the layout, the topology, the frame push data, the heaps), then the execution.
	 */
	void RecordPreprocessedDraw(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::CommandSignatureHandle a_signature,
		rhi::ResourceHandle a_arguments, std::uint64_t a_argumentsOffset, rhi::ResourceHandle a_counts, std::uint64_t a_countOffset, std::uint32_t a_capacity);

	// The frame slots' layout (Resources/FrameSlots.h) is IndirectDraws' implementation: a draw pass outside it pushes their addresses
	// through these (Passes/Forward.cpp).
	/** @brief FramePushWords(a_frameConstants): the frame push words of the main frame constants at a_frameConstants. */
	std::array<std::uint32_t, kFramePushWords> MainFramePushWords(std::uint64_t a_frameConstants);
	/**
	 * @brief The frame push block in the shadow views' layout (kShadowPush*) for the main camera: the colour epoch's frame record and
	 * slots at a_frameConstants, and the zeroed shared light block as the zero block.
	 */
	void MainShadowLayoutPush(std::uint64_t a_frameConstants, std::uint32_t (&a_out)[kShadowPushWords]);
	/** @brief The address of the frame's LodFadeFrame (after FrameLighting in the lighting slot) in the main frame constants at a_frameConstants. */
	std::uint64_t LodFadeFrameAddress(std::uint64_t a_frameConstants);

	/** @brief A pass's colour attachments, loaded and stored: a_count of a_views, each with its resource (a_tokens). */
	template <std::size_t N>
	std::array<rhi::ColorAttachment, N> LoadedColours(org::PassRecordContext& a_recording, const std::array<org::PreparedDescriptorReference, N>& a_views,
		const std::array<org::DeclaredViewToken, N>& a_tokens, std::uint32_t a_count)
	{
		std::array<rhi::ColorAttachment, N> out{};
		for (std::uint32_t i = 0; i < a_count; ++i) {
			out[i].rtv = a_recording.Resolve(a_views[i]);
			out[i].loadOp = rhi::LoadOp::Load;
			out[i].storeOp = rhi::StoreOp::Store;
			out[i].resource = a_recording.Resolve(a_tokens[i].Resource()).GetHandle();
		}
		return out;
	}
	/** @brief A pass's depth attachment through a_view, depth and stencil loaded and stored. */
	inline rhi::DepthAttachment LoadedDepth(org::PassRecordContext& a_recording, const org::PreparedDescriptorReference& a_view)
	{
		rhi::DepthAttachment depth{};
		depth.dsv = a_recording.Resolve(a_view);
		depth.depthLoad = rhi::LoadOp::Load;
		depth.depthStore = rhi::StoreOp::Store;
		depth.stencilLoad = rhi::LoadOp::Load;
		depth.stencilStore = rhi::StoreOp::Store;
		return depth;
	}
}
