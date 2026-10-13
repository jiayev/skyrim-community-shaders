#pragma once

// What the passes and the graph extensions share: recording helpers, registration, the latched copies pass, attachments.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief Folds the camera translation into the view-projection, so that a bound's absolute world
	 * position can be projected without carrying the camera separately.
	 *
	 * The draws are camera-relative (the engine's posAdjust), and a bound is absolute: the camera is subtracted
	 * here, one matrix column, instead of from every bound, which is exact and keeps the camera out of a
	 * push-constant block at Vulkan's guaranteed 128 bytes.
	 *
	 * Row-major, as the shader's `mul(M, v)` reads it: M'(p,1) = M(p-e,1) needs only column 3 changed,
	 * by subtracting M's upper-left 3x3 applied to the camera.
	 */
	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16]);

	// Records one BuildDrawsCS dispatch whose values and group count are the slot's latch at a_latchOffset.
	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording);

	/** @brief HzbCS.hlsl's constants: one single-pass downsample of the chain. */
	struct HzbConstants
	{
		std::uint32_t sourceIndex;
		std::uint32_t fromDepth;
		std::uint32_t validSize[2];   // the source texels holding depth
		std::uint32_t domainSize[2];  // the source's power-of-two extent; past validSize it reads the far plane
		std::uint32_t targetSize[2];  // the first written level's
		std::uint32_t mips;
		std::uint32_t workGroups;
		std::uint32_t counterIndex;
		std::uint32_t padding;
		std::uint32_t targetIndices[kHzbDispatchMips];
	};
	static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);

	// MainPayload::objectState values besides the Skip reasons.
	constexpr std::uint8_t kObjectStateDrawable = 0xF0;
	constexpr std::uint8_t kObjectStateDecal = 0xF1;
	constexpr std::uint8_t kObjectStateAbsent = 0xFF;

	// A DXVK image the graph uses in place: imported without ownership, with simultaneous access (DXVK
	// keeps the images it hands out in GENERAL and uses them between the graph's commands).
	std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
		const char* a_name);

	/**
	 * @brief The index pool every plain indexed draw binds (IndexPool): the scene's (SceneBuffers::pool), made with its
	 * copy program by whichever extension sets up first. Null, logged, when the program cannot be created.
	 */
	std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small);

	/**
	 * @brief Brings the index pool up to the tables (ShadowEpochs.cpp): whichever commit draws from it first in a frame gives the
	 * ranges out and writes their copies' dispatch at a_poolOffset of its latch slot; a later commit finds nothing to copy.
	 */
	void UpdateIndexPool(IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
		std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads);

	// The graph extensions that add DCLF's passes (Passes.cpp).
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources);
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources);

	/**
	 * @brief Registers a graph resource under a_id. A versioned buffer is not registered: its passes declare it (its resolver), and a
	 * registration would hold the version registered for as long as the graph, where a version should go when nothing uses it. Nothing
	 * looks these identifiers up, but the registration is not only a name: the graph holds the resource strongly and tracks it (an
	 * unregistered one gets a weak anonymous handle, pinned a frame at a time) and tags its memory with the identifier.
	 */
	template <class T>
	void Register(org::RenderGraph& a_graph, org::ResourceIdentifier a_id, const std::shared_ptr<T>& a_resource)
	{
		if constexpr (!std::is_same_v<T, org::VersionedBuffer>)
			if (a_resource)
				a_graph.RegisterResource(std::move(a_id), a_resource);
	}
	/** @brief The scene tables, which every extension registers (the same identifiers: a second registration is an update). */
	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene);

	/** @brief A pass of an extension: a copy or compute pass on the graphics queue unless a_queue says otherwise, a render pass. */
	enum class PassKind : std::uint8_t
	{
		Copy,
		Compute,
		Render,
	};
	enum class PassQueue : std::uint8_t
	{
		Default,  // the graphics queue for a copy or compute pass, none asked for a render pass
		None,
		Compute,
	};
	template <class Epoch>
	void AddPass(std::vector<org::RenderGraph::ExternalPassDesc>& a_out, PassKind a_kind, std::string a_name, std::shared_ptr<org::RenderPass> a_pass, Epoch a_epoch,
		PassQueue a_queue = PassQueue::Default)
	{
		using Desc = org::RenderGraph::ExternalPassDesc;
		auto desc = a_kind == PassKind::Copy ? Desc::Copy(std::move(a_name), std::move(a_pass)) :
		            a_kind == PassKind::Compute ? Desc::Compute(std::move(a_name), std::move(a_pass)) :
		                                          Desc::Render(std::move(a_name), std::move(a_pass));
		if (a_queue == PassQueue::Compute)
			desc.PreferQueue(org::QueueKind::Compute);
		else if (a_queue == PassQueue::Default && a_kind != PassKind::Render)
			desc.PreferQueue(org::QueueKind::Graphics);
		a_out.push_back(std::move(desc.Epoch(a_epoch)));
	}

	/** @brief What a latched copies pass copies, as of the shape its preparation is for: the shape's generation, the shape (held), its lists. */
	struct LatchedSource
	{
		std::uint64_t generation = 0;
		std::shared_ptr<const void> shape;
		std::span<const LatchedList> lists;
	};
	/**
	 * @brief An epoch's first pass: its shape's latched copies (LatchedList) from their blocks to where its passes read them,
	 * recorded with the epoch's ticket on ORG's host thread (its commit records no copy). It declares a_targets() as copy
	 * destinations, published to a_declared when given (what a commit may latch: LatchedUploads); a copy of another target is skipped.
	 */
	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared = nullptr);

	/**
	 * @brief A forward range's sort, back to front (ForwardSortCS.hlsl), as of the shape its preparation is for: the staging half's first
	 * slot and capacity (each half's), and where its count is. No program: nothing to sort.
	 */
	struct ForwardSortSource
	{
		std::uint64_t generation = 0;
		std::shared_ptr<const ComputeProgram> program;
		std::uint32_t stagingBase = 0, capacity = 0, countOffset = 0;
		bool countAtOffset = false;  // the count is the word at countOffset, not the main count buffer's forward word
	};
	/** @brief The sort of a forward range of a_sequences (the main one, a view's), its count read from a_counts. */
	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source);

	/** @brief A pass's frame buffers (t16 and up) and Light Limit Fix's lights (t35-t37) declared as read, through no view. */
	void DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, bool a_lights = true);

	/**
	 * @brief One explicitly preprocessed draw pass (the forward ranges'): the preprocess on the frame slot's state list, set up as the
	 * execution is (a_begin's attachments, the layout, the topology, the frame push data, the heaps), then the execution.
	 */
	void RecordPreprocessedDraw(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::CommandSignatureHandle a_signature,
		rhi::ResourceHandle a_arguments, std::uint64_t a_argumentsOffset, rhi::ResourceHandle a_counts, std::uint64_t a_countOffset, std::uint32_t a_capacity);

	/** @brief The rows' tables' addresses and strides into BuildDraws' constants (BuildDrawsCS: RowsOf). */
	inline void SetRows(BuildDrawsConstants& a_constants, std::uint64_t a_materialRows, std::uint64_t a_pipelineRows)
	{
		a_constants.materialRowsAddressLo = static_cast<std::uint32_t>(a_materialRows);
		a_constants.materialRowsAddressHi = static_cast<std::uint32_t>(a_materialRows >> 32);
		a_constants.materialRowStride = kMaterialRowBytes;
		a_constants.pipelineRowsAddressLo = static_cast<std::uint32_t>(a_pipelineRows);
		a_constants.pipelineRowsAddressHi = static_cast<std::uint32_t>(a_pipelineRows >> 32);
		a_constants.pipelineRowStride = kPipelineRowBytes;
	}
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
	/**
	 * @brief A bucket map into a latch slot at a_offset: a_map, then kNoBucket for the pipeline slots past it up to a_slots (gained since
	 * the map was made: none of their draws is the frame's).
	 */
	inline void WriteBucketMap(const org::LatchBlock& a_latch, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, std::span<const std::uint32_t> a_map,
		std::uint32_t a_slots)
	{
		if (!a_map.empty())
			LatchWrite(a_latch, a_what, a_slot, a_offset, std::as_bytes(a_map));
		if (a_slots <= a_map.size())
			return;
		static thread_local std::vector<std::uint32_t> none;
		none.assign(a_slots - a_map.size(), kNoBucket);
		LatchWrite(a_latch, a_what, a_slot, a_offset + static_cast<std::uint32_t>(a_map.size() * sizeof(std::uint32_t)), std::as_bytes(std::span(none)));
	}
	/** @brief One compute dispatch of a_program with its push constants. */
	template <class Constants>
	void RecordDispatch(rhi::CommandList& a_commands, const ComputeProgram& a_program, const Constants& a_constants, std::uint32_t a_groups)
	{
		a_commands.BindLayout(a_program.layout->GetHandle());
		a_commands.BindPipeline(a_program.pipeline->GetHandle());
		a_commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(sizeof(Constants) / 4), reinterpret_cast<const std::uint32_t*>(&a_constants));
		a_commands.Dispatch(a_groups, 1, 1);
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
