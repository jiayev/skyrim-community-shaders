// The shadow (and sky occlusion) views' passes and their graph extension: culling, the views' draws, and their latched copies.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Passes/IndexPoolPass.h"

namespace DCLF::Draws
{
	/** @brief The revision's shadow (or, a_sky, occlusion) shape being prepared; null without one. */
	std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const org::PassPrepareContext& a_preparation, bool a_sky)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation))
			return a_sky ? shapes->occlusion : shapes->shadow;
		return nullptr;
	}

	struct ShadowBuildBindings
	{
		std::vector<org::DeclaredViewToken> sequences, count, bucketCounts;  // per view slot
		org::DeclaredViewToken visibility, poolFirsts;
		org::DeclaredViewToken fadeRoots;
	};

	struct ShadowBuildPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		struct Dispatch
		{
			BuildDrawsConstants constants{};
			std::uint32_t latchOffset = 0;  // the view's BuildDrawsLatch within the slot's region
		};
		std::vector<Dispatch> dispatches;  // one per captured view
	};

	/** @brief The shadow views' culling: BuildDrawsCS in its single phase, frustum only, one dispatch per view. */
	class ShadowBuildDrawsPass final : public org::TypedRenderGraphPass<ShadowBuildDrawsPass, ShadowBuildPrepared, ShadowBuildBindings>
	{
	public:
		ShadowBuildDrawsPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
			resources(std::move(a_resources)), sky(a_sky) {}

		ShadowBuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ShadowBuildBindings bindings{};
			// The scene list, the geometry table and the shadow rows are the installed payload's (the latch names them).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.UnorderedAccess(*resources->sequences[s]).View());
				bindings.count.push_back(a_builder.UnorderedAccess(resources->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(*resources->bucketCounts[s]).View());
			}
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			// Fade roots' rows: casters under stood-in roots follow FadeStateCS's state, occluders their roots' OnVisible
			// (kCullFadeOnVisible). The states are last frame's, undeclared as in the depth segment.
			if (resources->scene->fadeRoots)
				bindings.fadeRoots = a_builder.ShaderResource(*resources->scene->fadeRoots).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(FadeRows() ? 1u : 0u);
			a_out.push_back(NamedSerial(a_preparation, resources->pool->firsts));
		}

		ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowBuildPrepared prepared{};
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			if (!frame || frame->views.empty() || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			const auto visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			const auto poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			const bool fadeRows = FadeRows();
			const auto fadeRootsIndex = fadeRows ? CaptureViewIndex(a_preparation, a_bindings.fadeRoots) : 0u;
			for (const auto& view : frame->views) {
				if (view.slot >= a_bindings.sequences.size() || view.modeIndex >= kShadowModeCount)
					continue;
				ShadowBuildPrepared::Dispatch dispatch{};
				dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				auto& constants = dispatch.constants;
				constants.latchIndex = frame->latch->SrvIndex();
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences[view.slot]);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count[view.slot]);
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[view.slot]);
				constants.poolFirstsIndex = poolFirstsIndex;
				constants.materialRowStride = sizeof(ShadowMaterialRow);  // no pipeline rows: their stride stays 0
				// Bucketed draws are bounded by their buckets; this bounds the rest.
				constants.phaseTwoBase = view.sequenceDraws;
				// The single phase: frustum only, no engine-visibility gate.
				constants.phaseBits = 0;
				// Every dispatch writes the visibility words; nothing reads them here.
				constants.visibilityIndex = visibilityIndex;
				constants.fadeRootsIndex = fadeRootsIndex;
				prepared.dispatches.push_back(dispatch);
			}
			return prepared;
		}

		static void Record(const ShadowBuildBindings&, const ShadowBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.latch || a_prepared.dispatches.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
			for (const auto& dispatch : a_prepared.dispatches)
				RecordLatchedDispatch(dispatch.constants, *a_prepared.latch, a_prepared.signature, dispatch.latchOffset, a_recording);
		}

	private:
		// The fade roots' rows mirror the tables (the depth commit uploads them).
		bool FadeRows() const { return resources->scene->fadeRoots && resources->scene->fadeRootCount; }

		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> sequences, bucketCounts;  // per view slot
		org::ResourceBindingToken pool;  // the index pool (IndexPool)
		org::ResourceBindingToken constants, viewBlocks;
	};

	struct ShadowPrepared
	{
		std::shared_ptr<const ShadowFrame> frame;
		bool sky = false;
		struct View
		{
			std::uint32_t index = 0;  // into frame->views
			org::PreparedDescriptorReference depthView{};
		};
		std::vector<View> views;
	};

	/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport. */
	class ShadowViewPass final : public org::TypedRenderGraphPass<ShadowViewPass, ShadowPrepared, ShadowPassBindings>
	{
	public:
		ShadowViewPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
			resources(std::move(a_resources)), sky(a_sky) {}

		ShadowPassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			ShadowPassBindings bindings{};
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i] && IsOcclusionTarget(i) == sky) {
					bindings.depthViews[i].reserve(resources->depthLayers[i]);
					for (std::uint32_t slice = 0; slice < resources->depthLayers[i]; ++slice)
						bindings.depthViews[i].push_back(a_builder.DepthReadWrite(resources->depth[i], org::DsvView{ UINT32_MAX, 0, slice }).View());
				}
			}
			// A slot's sequences are both arguments and read by address (DCLF_PULLED).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.IndirectArguments(*resources->sequences[s]).Resource());
				a_builder.ShaderResource(*resources->sequences[s], noViews);
				bindings.bucketCounts.push_back(a_builder.IndirectArguments(*resources->bucketCounts[s]).Resource());
			}
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(*resources->viewBlocks.buffer, noViews).Resource();
			// Face positions, read by address after the commit's uploads.
			a_builder.ShaderResource(*resources->scene->facePositions, noViews);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(NamedSerial(a_preparation, resources->pool->indices));
		}

		ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowPrepared prepared{};
			auto frame = CurrentShadowFrame(a_preparation, sky);
			if (!frame || frame->views.empty() || !frame->indirect.valid)
				return prepared;
			for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				if (!view.capacity || view.slot >= a_bindings.sequences.size() || view.target >= kShadowDepthTargets || !resources->depth[view.target] ||
					IsOcclusionTarget(view.target) != sky)
					continue;
				// The declared slices, not the live target's: a target grown since has slices these bindings cannot name.
				if (view.slice >= resources->depthLayers[view.target] || view.slice >= a_bindings.depthViews[view.target].size())
					continue;
				prepared.views.push_back({ i, a_preparation.Capture(a_bindings.depthViews[view.target][view.slice]) });
			}
			if (!prepared.views.empty())
				prepared.frame = std::move(frame);
			prepared.sky = sky;
			return prepared;
		}

		static void Record(const ShadowPassBindings& a_bindings, const ShadowPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			std::vector<rhi::DepthAttachment> depths(a_prepared.views.size());
			std::vector<rhi::PassBeginInfo> begins(a_prepared.views.size());
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& prepared = a_prepared.views[i];
				const auto& view = frame.views[prepared.index];
				auto& begin = begins[i];
				begin.x = view.x;
				begin.y = view.y;
				begin.width = view.width;
				begin.height = view.height;
				begin.minDepth = view.minDepth;
				begin.maxDepth = view.maxDepth;
				// The engine's casters are already in the slice: loaded, added to, stored.
				auto& depth = depths[i];
				depth.dsv = a_recording.Resolve(prepared.depthView);
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.depth = &depth;
				begin.debugName = a_prepared.sky ? "DCLF occlusion map" : "DCLF shadow view";
			}
			// Per view: a plain indexed indirect draw per bucket over its range of the slot's sequences, counted by its count word.
			// A plain draw cannot bind its own vertex buffer, so the vertex stage fetches through the sequence (Utility.hlsl, DCLF_PULLED).
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };  // the whole buffer
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& view = frame.views[a_prepared.views[i].index];
				commands.BeginPass(begins[i]);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.layout);
				commands.SetIndexBuffer(pool);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, view.push.data());
				const auto sequences = a_recording.Resolve(a_bindings.sequences[view.slot]).GetHandle();
				const auto counts = a_recording.Resolve(a_bindings.bucketCounts[view.slot]).GetHandle();
				const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
				for (std::uint32_t b = 0; b < view.buckets.size(); ++b) {
					const auto& bucket = view.buckets[b];
					if (!bucket.capacity || bucket.pipeline >= frame.indirect.pipelines.size())
						continue;
					const std::uint64_t first = std::uint64_t(bucket.first) * sizeof(DrawSequence);
					std::uint32_t words[kDrawPushWords]{};
					split(words + kShadowDrawPushSequences, sequencesAddress + first);
					// The layout's rows word: 0, as the Z-prepass's pulled draws (each draw's row is its sequence's; the rows are the
					// installed payload's, which no recording names).
					split(words + kShadowDrawPushMaterialRows, 0);
					split(words + kShadowDrawPushVertexLayout, frame.indirect.vertexLayouts[bucket.pipeline]);
					commands.BindPipeline(frame.indirect.pipelines[bucket.pipeline]);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.indirect.drawSignature, sequences, first + kSequenceDrawOffset, counts, std::uint64_t(b) * sizeof(std::uint32_t),
						bucket.capacity);
				}
				commands.EndPass();
			}
		}

	private:
		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.view-blocks"), resources->viewBlocks.buffer);
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.bucket-counts{}", s)), resources->bucketCounts[s]);
			}
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i])
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::ShadowView);
			AddPass(a_out, PassKind::Copy, "cs.dclf.shadow.latched-copies", ShadowLatchedCopies(false), epoch);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [shadow = resources](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentShadowFrame(a_preparation, false);
					return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
				})))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			AddPass(a_out, PassKind::Compute, "cs.dclf.shadow.build-draws", std::make_shared<ShadowBuildDrawsPass>(resources, false), epoch);
			AddPass(a_out, PassKind::Render, "cs.dclf.shadow.view", std::make_shared<ShadowViewPass>(resources, false), epoch);
			// Skylighting's occlusion map: the same passes in its own epoch after RenderMask.
			const auto skyEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::SkyOcclusion);
			AddPass(a_out, PassKind::Copy, "cs.dclf.sky.latched-copies", ShadowLatchedCopies(true), skyEpoch);
			AddPass(a_out, PassKind::Compute, "cs.dclf.sky.build-draws", std::make_shared<ShadowBuildDrawsPass>(resources, true), skyEpoch);
			AddPass(a_out, PassKind::Render, "cs.dclf.sky.view", std::make_shared<ShadowViewPass>(resources, true), skyEpoch);
		}

	private:
		/** @brief The views' latched copies: blocks, counters, and the commit's values in the constants. */
		std::shared_ptr<org::RenderPass> ShadowLatchedCopies(bool a_sky) const
		{
			auto targets = [shadow = resources] {
				std::vector<LatchedTarget> out{ { nullptr, shadow->viewBlocks.buffer }, { shadow->constants, nullptr } };
				for (std::size_t s = 0; s < shadow->count.size(); ++s) {
					out.push_back({ shadow->count[s], nullptr });
					out.push_back({ nullptr, shadow->bucketCounts[s] });
				}
				return out;
			};
			return MakeLatchedCopiesPass(std::move(targets), [a_sky](const org::PassPrepareContext& a_preparation) {
				const auto frame = CurrentShadowFrame(a_preparation, a_sky);
				return frame ? LatchedSource{ frame->generation, frame, frame->latched } : LatchedSource{};
			});
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources)
	{
		return std::make_unique<ShadowExtension>(std::move(a_resources));
	}
}

#endif
