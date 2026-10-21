#pragma once

// The draw passes' recording helpers: attachments, the frame buffers' declaration, the preprocessed indirect draw, a bucket's plain draws,
// and the frame slots' addresses a draw pass pushes; and the common passes a feature's epoch is made of: the latched copies, the latched
// BuildDraws dispatch, the shadow epoch's index pool copies. Self-contained (a feature's pass includes it: Features/Forward's,
// Features/MainView's, Features/Shadows/Engine's); PassHelpers.h includes it.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Draws
{
	struct ShadowResources;

	// Records one BuildDrawsCS dispatch whose values and group count are the slot's latch at a_latchOffset.
	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording);

	/**
	 * @brief What a latched copies pass copies, as of the shape its preparation is for: the shape's generation, the shape (held), its lists;
	 * and in an epoch hosting others' (Api::EpochDef::hostedBy: the depth epoch, the reflection's and first person's depth), each hosted
	 * shape's, one entry per hosted view in the host's order: its own when the preparation names it (its recordings with that view's
	 * passes' work), else none (generation 0, no lists), copied after the epoch's own.
	 */
	struct LatchedSource
	{
		struct Hosted
		{
			std::uint64_t generation = 0;
			std::shared_ptr<const void> shape;
			std::span<const LatchedList> lists;
		};
		std::uint64_t generation = 0;
		std::shared_ptr<const void> shape;
		std::span<const LatchedList> lists;
		std::vector<Hosted> hosted;
	};
	/**
	 * @brief An epoch's first pass: its shape's latched copies (LatchedList) from their blocks to where its passes read them,
	 * recorded with the epoch's ticket on ORG's host thread (its commit records no copy). It declares a_targets() as copy
	 * destinations, published to a_declared when given (what a commit may latch: LatchedUploads); a copy of another target is skipped.
	 * An epoch hosting others' copies the hosted shapes' lists too (one pass: a hosted epoch has no latched copies of its own).
	 */
	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared = nullptr);

	/**
	 * @brief The shadow epoch's index pool copies (Passes/Shadow.cpp: IndexPoolPass, Core's, as the depth epoch's), dispatched from the
	 * revision's shadow shape's latch.
	 */
	std::shared_ptr<org::RenderPass> MakeShadowIndexPoolPass(const std::shared_ptr<ShadowResources>& a_resources);

	/** @brief A pass's frame buffers (t16 and up) and Light Limit Fix's lights (t35-t37) declared as read, through no view. */
	void DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, bool a_lights = true);

	/**
	 * @brief One device-generated call of a draw pass: its signature (a state class's: each call draws one class's sub-range of a range),
	 * where its sequences start in the arguments, its count word and its max count.
	 */
	struct IndirectCall
	{
		rhi::CommandSignatureHandle signature{};
		std::uint64_t argumentsOffset = 0;
		rhi::ResourceHandle counts{};
		std::uint64_t countOffset = 0;
		std::uint32_t capacity = 0;
		const char* range = nullptr;  // the nvperf range around its execution, when a capture selects it (none: none)
		const char* preprocessRange = nullptr;  // and around its preprocess (none: none)
	};
	/**
	 * @brief One explicitly preprocessed draw pass (the forward ranges'): the preprocess of every call on the frame slot's state list, set
	 * up as the execution is (a_begin's attachments, the layout, the topology, the frame push data, the heaps), then the pass executing
	 * them in order. Nothing without a call.
	 */
	void RecordPreprocessedDraws(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::ResourceHandle a_arguments,
		std::span<const IndirectCall> a_calls, const char* a_preprocessRange = nullptr);
	/** @brief RecordPreprocessedDraws of one call. */
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

	/**
	 * @brief One bucket's plain indexed indirect draws (DCLF_PULLED): its range of the sequences from slot a_first, its pipeline, its
	 * push words (kZDrawPush*: the range's first sequence, a_offsetDepth) and its count word a_bucket of a_counts. The vertex stage
	 * fetches each draw's vertices and rows through the sequence its instance names, as a plain draw binds no vertex buffer or push
	 * data of its own.
	 */
	inline void RecordBucketDraw(rhi::CommandList& a_commands, rhi::CommandSignatureHandle a_signature, rhi::ResourceHandle a_sequences, std::uint64_t a_sequencesAddress,
		std::uint64_t a_first, rhi::PipelineHandle a_pipeline, rhi::ResourceHandle a_counts, std::uint32_t a_bucket, std::uint32_t a_capacity, bool a_offsetDepth = false)
	{
		const std::uint64_t first = a_first * sizeof(DrawSequence);
		std::uint32_t words[kDrawPushWords]{};
		words[kZDrawPushSequences] = static_cast<std::uint32_t>(a_sequencesAddress + first);
		words[kZDrawPushSequences + 1] = static_cast<std::uint32_t>((a_sequencesAddress + first) >> 32);
		words[kZDrawPushOffsetDepth] = a_offsetDepth ? 1u : 0u;
		a_commands.BindPipeline(a_pipeline);
		a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		a_commands.ExecuteIndirect(a_signature, a_sequences, first + kSequenceDrawOffset, a_counts, std::uint64_t(a_bucket) * sizeof(std::uint32_t), a_capacity);
	}

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
