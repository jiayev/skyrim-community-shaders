#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
	{
	public:
		// phaseTwo: the second depth draw of the two-phase culling, which draws only what the rebuilt HZB
		// brought back, from the reserved part of the sequence buffer.
		MainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_phaseTwo = false) :
			resources(std::move(a_resources)), segment(a_segment), phaseTwo(a_phaseTwo) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			bindings.sequences = a_builder.IndirectArguments(resources->sequences);
			bindings.count = a_builder.IndirectArguments(resources->count);
			// Read through device addresses; declared so the graph orders them after their uploads.
			bindings.materialRows = a_builder.ShaderResource(resources->materialRows.buffer, noViews).Resource();
			bindings.pipelineRows = a_builder.ShaderResource(resources->pipelineRows.buffer, noViews).Resource();
			bindings.objects = a_builder.ShaderResource(resources->scene->objects, noViews).Resource();
			bindings.bones = a_builder.ShaderResource(resources->scene->bones, noViews).Resource();
			// Read by the input assembler (the face draws' second stream), after the commit's uploads into it.
			a_builder.VertexBuffer(resources->scene->facePositions);
			for (const auto& frameBuffer : resources->frameBuffers)
				bindings.frameBuffers.push_back(a_builder.ShaderResource(frameBuffer.copy, noViews).Resource());
			if (resources->lightLimitFix) {
				bindings.lights = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.lights"), noViews).Resource();
				bindings.lightIndexList = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"), noViews).Resource();
				bindings.lightGrid = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-grid"), noViews).Resource();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(phaseTwo ? 1 : 0);
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			const auto now = segment;
			auto frame = CurrentFrame(*resources, now);
			if (!frame || (!frame->drawCapacity && !frame->decalCapacity[0] && !frame->decalCapacity[1] && !frame->decalCapacity[2]) || !frame->indirect.valid)
				return prepared;
			// The rescue draw belongs to the depth segment only.
			if (phaseTwo && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.phaseTwo = phaseTwo;
			prepared.zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
			prepared.stats = resources->passStats.get();
			if (!prepared.zPrepass)
				prepared.preprocess = resources->preprocessMain.get();
			prepared.framePushWords = FramePushWords(resources->frameConstantsAddress);
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
				prepared.targetViews[i] = a_preparation.Capture(a_bindings.targets[i]);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			std::array<rhi::ColorAttachment, kColorTargets> colors{};
			for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i) {
				colors[i].rtv = a_recording.Resolve(a_prepared.targetViews[i]);
				colors[i].loadOp = rhi::LoadOp::Load;
				colors[i].storeOp = rhi::StoreOp::Store;
				colors[i].resource = a_recording.Resolve(a_bindings.targets[i].Resource()).GetHandle();
			}
			const bool zPrepass = a_prepared.zPrepass;
			if (a_prepared.phaseTwo && !zPrepass)
				return;
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;

			// DCLF's Z-prepass: depth only, like the native one. It adds DCLF's objects to the depth the native
			// passes already wrote, so they occlude and are occluded correctly. It runs in its own segment,
			// inside the native depth pass, so that the rest of the frame - the native draws that test depth,
			// the sky and everything that reads the depth buffer afterwards - sees DCLF's objects.
			rhi::DepthAttachment depth{};
			depth.dsv = a_recording.Resolve(a_prepared.depthView);
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			begin.depth = &depth;
			begin.debugName = "DCLF depth";
			auto* stats = a_prepared.stats;
			const std::uint32_t statsSlot = a_recording.FrameSlot();
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
			// A pass of DCLF's draws: its attachments, the layout, the topology and the frame push data - on the command list,
			// and on a preprocess state list, identically.
			auto beginDrawPass = [&](rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin) {
				a_list.BeginPass(a_begin);
				a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				a_list.BindLayout(frame.indirect.layout);
				a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			};
			// The frame slot's preprocess state list, with this pass's heaps; null without explicit preprocessing.
			auto preprocessState = [&]() -> rhi::CommandList* {
				if (!a_prepared.preprocess)
					return nullptr;
				auto& state = a_prepared.preprocess->Begin(statsSlot);
				state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				return &state;
			};
			if (zPrepass) {
				// Phase 2 draws only the rescues, from the reserved half of the sequence buffer and
				// its own counter word. Its argument offset has to be a constant the CPU knows, which
				// is why the two phases have fixed ranges instead of sharing one.
				const std::uint64_t argumentOffset = a_prepared.phaseTwo ? std::uint64_t(frame.sequenceDraws) * sizeof(DrawSequence) : 0;
				const std::uint64_t countOffset = a_prepared.phaseTwo ? kCountDrawnPhaseTwoBytes : 0;
				if (stats)
					stats->Start(commands, statsSlot, depthKind);
				beginDrawPass(commands, begin);
				if (stats)
					stats->Begin(commands, statsSlot, depthKind);
				commands.ExecuteIndirect(frame.indirect.depthPassSignature, sequences, argumentOffset, count, countOffset, frame.drawCapacity);
				if (stats)
					stats->End(commands, statsSlot, depthKind);
				commands.EndPass();
				if (stats)
					stats->Resolve(commands, statsSlot, depthKind);
				return;
			}

			// The main pass: depth test EQUAL against the Z-prepass's (its pipelines do not write depth; the
			// attachment stays in the layout the pass declared).

			// The opaque decals' depth, before any colour: the engine's main pass
			// draws its opaque decal group with depth writes and its bias, so this is where the frame's depth has them, and
			// with it every host fragment under an opaque decal texel fails the colour pass's EQUAL test and is not shaded -
			// the decal overwrites every target there (blending off, full masks). The group's depth variants test LESS_EQUAL
			// with the decal's bias and run the alpha test, so a transparent texel leaves the host's depth. The blended
			// group has none: it writes no depth natively, and it blends over its host, which must still be shaded.
			// nvperf ranges around the colour pass's parts (NvPerfBridge::PushPassRange), when a capture selects them.
			void* const nvCommands = NvPerfBridge::Active() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
			auto subRange = [&](const char* a_name) { return nvCommands && NvPerfBridge::PushPassRange(nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (nvCommands) NvPerfBridge::PopPassRange(nvCommands, a_pushed); };
			// The colour passes: the main one with the targets as the frame loads them, the decals' with all of them loaded; each
			// with the layout, the topology and the frame push data.
			auto decalColors = colors;
			for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i)
				decalColors[i].loadOp = rhi::LoadOp::Load;
			rhi::PassBeginInfo mainBegin = begin;
			mainBegin.colors = { colors.data(), a_prepared.targetCount };
			mainBegin.debugName = "DCLF main opaque";
			rhi::PassBeginInfo decalBegin = begin;
			decalBegin.colors = { decalColors.data(), a_prepared.targetCount };
			decalBegin.debugName = "DCLF decals";
			rhi::PassBeginInfo decalDepthBegin = begin;  // depth only
			decalDepthBegin.debugName = "DCLF decal depth";
			const auto colourSignature = frame.indirect.signatures[kColorVariant];
			auto decalArguments = [&](std::uint32_t a_group) {
				return (2 * std::uint64_t(frame.sequenceDraws) + std::uint64_t(a_group) * frame.sequenceDecals) * sizeof(DrawSequence);
			};
			auto decalCount = [](std::uint32_t a_group) { return std::uint64_t(DecalCountWord(a_group)) * sizeof(std::uint32_t); };
			const bool anyDecals = frame.decalCapacity[0] || frame.decalCapacity[1] || frame.decalCapacity[2];

			// Every call below is preprocessed here, before the first pass: generated inside the pass
			// instead, by NVIDIA's driver, each colour call cost a fixed ~170 us of idle GPU in these eight-target passes. The
			// preprocess is generated for the state list's state - each call's pass begun and set up exactly as it is below,
			// with the same heaps - which the execution must match; the sequences and counts are final before this pass (the
			// culling wrote them).
			if (auto* preprocessList = preprocessState()) {
				auto& state = *preprocessList;
				if (frame.decalCapacity[0]) {
					beginDrawPass(state, decalDepthBegin);
					commands.PreprocessIndirect(state, frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
					state.EndPass();
				}
				beginDrawPass(state, mainBegin);
				if (frame.drawCapacity)
					commands.PreprocessIndirect(state, colourSignature, sequences, 0, count, 0, frame.drawCapacity);
				state.EndPass();
				if (anyDecals) {
					beginDrawPass(state, decalBegin);
					for (const std::uint32_t group : kDecalDrawOrder)
						if (frame.decalCapacity[group])
							commands.PreprocessIndirect(state, colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
					state.EndPass();
				}
				state.End();
			}

			if (frame.decalCapacity[0]) {
				const bool part = subRange("cs.dclf.colour.decal-depth");
				beginDrawPass(commands, decalDepthBegin);
				commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
				commands.EndPass();
				endSubRange(part);
			}
			if (stats)
				stats->Start(commands, statsSlot, PassStats::kColour);
			const bool mainPart = subRange("cs.dclf.colour.main");
			beginDrawPass(commands, mainBegin);
			if (stats)
				stats->Begin(commands, statsSlot, PassStats::kColour);
			if (frame.drawCapacity) {
				const bool drawPart = subRange("cs.dclf.colour.main-draws");
				commands.ExecuteIndirect(colourSignature, sequences, 0, count, 0, frame.drawCapacity);
				endSubRange(drawPart);
			}
			if (stats)
				stats->End(commands, statsSlot, PassStats::kColour);
			commands.EndPass();
			endSubRange(mainPart);
			if (stats)
				stats->Resolve(commands, statsSlot, PassStats::kColour);

			// The second pass: decals, after every opaque draw, in the engine's order - its opaque decal
			// group, the multi-index layers, then its blended group, each from its own fixed-slot range and its own count
			// word. The pipelines test depth LESS_EQUAL with the engine's decal bias; only the layers write depth here (the
			// opaque group's is already there, from the decal depth pass). Same attachments, all loaded.
			if (anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				beginDrawPass(commands, decalBegin);
				for (const std::uint32_t group : kDecalDrawOrder) {
					if (!frame.decalCapacity[group])
						continue;
					const bool groupPart = subRange(group == 0 ? "cs.dclf.colour.decals-opaque" : group == 2 ? "cs.dclf.colour.decals-layers" : "cs.dclf.colour.decals-blended");
					commands.ExecuteIndirect(colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
					endSubRange(groupPart);
				}
				commands.EndPass();
				endSubRange(decalPart);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool phaseTwo = false;
	};

	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16])
	{
		std::memcpy(a_out, a_viewProj.data(), sizeof(a_out));
		for (std::uint32_t row = 0; row < 4; ++row) {
			a_out[row * 4 + 3] = a_viewProj[row * 4 + 3] -
			                     (a_viewProj[row * 4 + 0] * a_eye.x + a_viewProj[row * 4 + 1] * a_eye.y +
								     a_viewProj[row * 4 + 2] * a_eye.z);
		}
	}

	struct BuildDrawsBindings
	{
		org::DeclaredViewToken inputs, inputsDepth, geometries, objects, sequences, count, hzb, visibility, frustum;
		org::DeclaredViewToken sortCounts, sortStaging, sortRanks;
		org::DeclaredViewToken fadeRoots, fadeStates;
	};

	struct BuildDrawsFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		BuildDrawsConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
	};

	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording)
	{
		auto& commands = a_recording.Commands();
		const std::uint64_t offset = a_latch.Offset(a_recording.FrameSlot()) + a_latchOffset;
		a_constants.latchOffset = static_cast<std::uint32_t>(offset);
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_constants));
		commands.ExecuteIndirect(a_signature, a_latch.Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
	}

	// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
	class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
	{
	public:
		BuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase = 0) :
			resources(std::move(a_resources)), segment(a_segment), fixedPhase(a_phase) {}

		BuildDrawsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			BuildDrawsBindings bindings{};
			bindings.inputs = a_builder.ShaderResource(resources->inputs).View();
			if (resources->inputsDepth)
				bindings.inputsDepth = a_builder.ShaderResource(resources->inputsDepth).View();
			bindings.geometries = a_builder.ShaderResource(resources->scene->geometries).View();
			bindings.objects = a_builder.ShaderResource(resources->scene->objects).View();
			bindings.sequences = a_builder.UnorderedAccess(resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			bindings.visibility = a_builder.UnorderedAccess(resources->visibility).View();
			if (resources->frustum)
				bindings.frustum = a_builder.UnorderedAccess(resources->frustum).View();
			// The depth segment's first phase reads the fade roots' state (FadeStateCS, just before it).
			if (segment == RenderGraphRuntime::Segment::ZPrepass && fixedPhase != 2 && resources->scene->fadeRoots) {
				bindings.fadeRoots = a_builder.ShaderResource(resources->scene->fadeRoots).View();
				bindings.fadeStates = a_builder.ShaderResource(resources->scene->fadeStates).View();
			}
			// Phase 1 sees the HZB the previous frame left, phase 2 the one just rebuilt from this
			// frame's depth. Both read the same resource; what differs is where they sit relative to
			// the build, which is why the ordering below is the whole design.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			if (Sorts()) {
				bindings.sortCounts = a_builder.UnorderedAccess(resources->sort->counts).View();
				bindings.sortStaging = a_builder.UnorderedAccess(resources->sort->staging).View();
				bindings.sortRanks = a_builder.UnorderedAccess(resources->sort->ranks).View();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(fixedPhase);
		}

		BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			BuildDrawsFrame prepared{};
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			if (!frame || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			const std::uint32_t phase = Phase(now);
			// The second phase belongs to the depth segment only: it is what re-tests phase 1's rejects
			// against the HZB that has just been rebuilt from this frame's depth.
			if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			// The depth segment's phases read its own inputs (Resources::inputsDepth), the colour segment the colour inputs.
			const bool depthInputs = (phase == 1 || phase == 2) && resources->inputsDepth;
			constants.inputsIndex = CaptureViewIndex(a_preparation, depthInputs ? a_bindings.inputsDepth : a_bindings.inputs);
			constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			constants.objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			// The rows both segments' draws name (MainRows), at their tables' addresses as the epoch sized them.
			constants.materialRowsAddressLo = static_cast<std::uint32_t>(frame->materialRows);
			constants.materialRowsAddressHi = static_cast<std::uint32_t>(frame->materialRows >> 32);
			constants.materialRowStride = kMaterialRowBytes;
			constants.pipelineRowsAddressLo = static_cast<std::uint32_t>(frame->pipelineRows);
			constants.pipelineRowsAddressHi = static_cast<std::uint32_t>(frame->pipelineRows >> 32);
			constants.pipelineRowStride = kPipelineRowBytes;
			constants.phaseBits = (phase & 0xFu) << 4;
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			// The frustum stamps: the depth segment's first phase alone tests every candidate's frustum.
			if (resources->frustum && phase == 1)
				constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			if (phase == 1 && resources->scene->fadeRoots && resources->scene->fadeRootCount) {
				constants.fadeRootsIndex = CaptureViewIndex(a_preparation, a_bindings.fadeRoots);
				constants.fadeStatesIndex = CaptureViewIndex(a_preparation, a_bindings.fadeStates);
			}
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			if (Sorts()) {
				constants.sortCountsIndex = CaptureViewIndex(a_preparation, a_bindings.sortCounts);
				constants.sortStagingIndex = CaptureViewIndex(a_preparation, a_bindings.sortStaging);
				constants.sortRanksIndex = CaptureViewIndex(a_preparation, a_bindings.sortRanks);
			}
			// The sequence buffer's ranges as the epoch sized them.
			constants.phaseTwoBase = frame->sequenceDraws;
			constants.decalBase = 2 * frame->sequenceDraws;
			constants.decalStride = frame->sequenceDecals;
			return prepared;
		}

		static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			RecordLatchedDispatch(a_frame.constants, *a_frame.latch, a_frame.signature, 0, a_recording);
		}

		std::uint32_t Phase(RenderGraphRuntime::Segment a_segment) const
		{
			if (fixedPhase)
				return fixedPhase;
			return a_segment == RenderGraphRuntime::Segment::ZPrepass ? 1u : 3u;
		}

	private:
		// Phase 2 appends its few rescues into its own range, unsorted; the other builds are followed by the sort's passes.
		bool Sorts() const { return resources->sort && fixedPhase != 2; }

		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		std::uint32_t fixedPhase = 0;
	};

	struct SortSequencesBindings
	{
		org::DeclaredViewToken staging, ranks, offsets, count, sequences;
	};

	struct SortSequencesFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		SortSequencesConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The sort's scatter (DrawSort), after its segment's BuildDraws and the scan. It covers every slot BuildDraws can
	 * append (the draws' range as the epoch sized it), each thread past the count returning.
	 */
	class SortSequencesPass final : public org::TypedRenderGraphPass<SortSequencesPass, SortSequencesFrame, SortSequencesBindings>
	{
	public:
		SortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		SortSequencesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			SortSequencesBindings bindings{};
			bindings.staging = a_builder.ShaderResource(resources->sort->staging).View();
			bindings.ranks = a_builder.ShaderResource(resources->sort->ranks).View();
			bindings.offsets = a_builder.ShaderResource(resources->sort->offsets).View();
			bindings.count = a_builder.ShaderResource(resources->count).View();
			bindings.sequences = a_builder.UnorderedAccess(resources->sequences).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		SortSequencesFrame Prepare(const SortSequencesBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			SortSequencesFrame prepared{};
			if (!BuildsDraws(*resources, segment))
				return prepared;
			prepared.program = resources->sort->scatter;
			auto& constants = prepared.constants;
			constants.stagingIndex = CaptureViewIndex(a_preparation, a_bindings.staging);
			constants.ranksIndex = CaptureViewIndex(a_preparation, a_bindings.ranks);
			constants.offsetsIndex = CaptureViewIndex(a_preparation, a_bindings.offsets);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
			const auto frame = CurrentFrame(*resources, segment);
			constants.drawLimit = frame ? frame->sequenceDraws : 0;
			prepared.groups = (constants.drawLimit + kSortSequencesGroup - 1) / kSortSequencesGroup;
			return prepared;
		}

		static void Record(const SortSequencesBindings&, const SortSequencesFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kSortSequencesConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	struct TreeWindBindings
	{
		org::DeclaredViewToken trees, clocks, list, records, frame;
	};

	struct TreeWindFramePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeWindConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree wind (TreeWindCS.hlsl): mode 0 brings the tree clocks to this frame (once a frame, whichever epoch runs it
	 * first), mode 1 writes the members' TreeParams and WindTimers into their records, after the epoch's uploads and ahead
	 * of the draws that read them. Both run in every epoch that draws or uploads the scene's records.
	 */
	class TreeWindPass final : public org::TypedRenderGraphPass<TreeWindPass, TreeWindFramePrepared, TreeWindBindings>
	{
	public:
		TreeWindPass(std::shared_ptr<SceneBuffers> a_scene, std::uint32_t a_mode) :
			scene(std::move(a_scene)), mode(a_mode) {}

		TreeWindBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			TreeWindBindings bindings{};
			bindings.trees = a_builder.ShaderResource(scene->trees).View();
			bindings.frame = a_builder.ShaderResource(scene->treeFrameBuffer).View();
			if (mode == 0) {
				bindings.clocks = a_builder.UnorderedAccess(scene->treeClocks).View();
			} else {
				bindings.clocks = a_builder.ShaderResource(scene->treeClocks).View();
				bindings.list = a_builder.ShaderResource(scene->treeObjects).View();
				bindings.records = a_builder.UnorderedAccess(scene->objects).View();
			}
			return bindings;
		}

		// What the recording depends on: the buffers' layout (a growth gives them new views). The counts and the frame's inputs
		// are the frame row, which every commit uploads.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(scene->layout.load(std::memory_order_acquire));
			a_out.push_back(mode);
		}

		TreeWindFramePrepared Prepare(const TreeWindBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			TreeWindFramePrepared prepared{};
			// Every slot the buffers hold: the shader stops at the frame row's count.
			const std::uint32_t count = mode == 0 ? scene->treeCapacity : scene->treeObjectCapacity;
			if (!scene->treeWind || !count || !scene->treeFrameBuffer)
				return prepared;
			prepared.program = scene->treeWind;
			auto& constants = prepared.constants;
			constants.mode = mode;
			constants.treesIndex = CaptureViewIndex(a_preparation, a_bindings.trees);
			constants.clocksIndex = CaptureViewIndex(a_preparation, a_bindings.clocks);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			if (mode == 1) {
				constants.listIndex = CaptureViewIndex(a_preparation, a_bindings.list);
				constants.recordsIndex = CaptureViewIndex(a_preparation, a_bindings.records);
			}
			constants.treeWord = static_cast<std::uint32_t>(offsetof(BindlessObject, tree) / 16);
			prepared.groups = (count + kTreeWindGroup - 1) / kTreeWindGroup;
			return prepared;
		}

		static void Record(const TreeWindBindings&, const TreeWindFramePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeWindConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
		std::uint32_t mode = 0;
	};

	/** @brief The two tree wind passes, first in an epoch's list (after its uploads, ahead of everything that reads the records). */
	void AddTreeWindPasses(const std::shared_ptr<SceneBuffers>& a_scene, const char* a_prefix, std::uint32_t a_epoch, std::vector<org::RenderGraph::ExternalPassDesc>& a_out)
	{
		if (!a_scene || !a_scene->treeWind || !a_scene->trees)
			return;
		for (std::uint32_t mode = 0; mode < 2; ++mode)
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(fmt::format("{}.tree-wind-{}", a_prefix, mode ? "records" : "clocks"),
				std::static_pointer_cast<org::RenderPass>(std::make_shared<TreeWindPass>(a_scene, mode)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(a_epoch));
	}

	struct FadeStateBindings
	{
		org::DeclaredViewToken roots, states, frame, objects, log, visibility;
	};

	struct FadeStatePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FadeStateConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The fade roots' state (FadeStateCS.hlsl): each root's OnVisible for the main camera whenever its bound is in the
	 * depth segment's frustum, once a frame, ahead of that segment's culling.
	 */
	class FadeStatePass final : public org::TypedRenderGraphPass<FadeStatePass, FadeStatePrepared, FadeStateBindings>
	{
	public:
		explicit FadeStatePass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->scene;
			FadeStateBindings bindings{};
			bindings.roots = a_builder.ShaderResource(scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(scene.fadeStates).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.visibility = a_builder.ShaderResource(scene.fadeVisibility).View();
			bindings.objects = a_builder.ShaderResource(scene.objects).View();
			bindings.log = a_builder.UnorderedAccess(scene.fadeLog).View();
			return bindings;
		}

		// What the recording depends on: the buffers' layout and the depth segment's shape (its latch). The root count, the frame
		// and the log are the frame row (Records.h, FadeFrame), which the depth commit uploads.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			a_out.push_back(resources->scene->layout.load(std::memory_order_acquire));
			a_out.push_back(frame ? frame->generation : 0);
		}

		FadeStatePrepared Prepare(const FadeStateBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			if (!scene.fadeState || !scene.fadeRootCapacity || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.fadeState;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			constants.objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			constants.logIndex = CaptureViewIndex(a_preparation, a_bindings.log);
			constants.latchIndex = frame->latch->SrvIndex();
			// Every slot the buffers hold: the shader stops at the frame row's count.
			prepared.groups = (scene.fadeRootCapacity + kFadeStateGroup - 1) / kFadeStateGroup;
			return prepared;
		}

		static void Record(const FadeStateBindings&, const FadeStatePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot (RecordLatchedDispatch's offset 0).
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kFadeStateConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	struct HzbBindings
	{
		org::DeclaredViewToken depth;
		std::vector<org::DeclaredViewToken> hzbMips;
	};

	struct HzbFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		// One dispatch per mip, each reading the level above (mip 0 reads the scene depth).
		struct Level
		{
			HzbConstants constants{};
			std::uint32_t groupsX = 0, groupsY = 0;
		};
		std::vector<Level> levels;
	};

	/**
	 * @brief Builds the hierarchical depth buffer after the world's depth draws.
	 *
	 * It runs in the ZPrepass segment, after DCLF's own depth draws, so the HZB describes the depth the
	 * frame actually has: the native occluders the engine drew (terrain and everything ineligible) plus
	 * the objects DCLF drew itself. On AE that is inside the depth pass, before the first-person model's
	 * depth and the rooms' stencil draws, which it therefore leaves out: fewer occluders, never more.
	 *
	 * The whole mip chain is one pass with a full memory barrier between the dispatches, rather than one
	 * pass per level. Levels of a single texture are not separate resources to the graph, so a per-level
	 * pass would declare the same resource as both its input and its output and the ordering would not
	 * mean what it reads as.
	 */
	class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
	{
	public:
		HzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		HzbBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			HzbBindings bindings{};
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			bindings.hzbMips.reserve(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				bindings.hzbMips.push_back(a_builder.UnorderedAccess(resources->hzb, org::UavView{ UINT32_MAX, mip }).View());
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		HzbFrame Prepare(const HzbBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			HzbFrame prepared{};
			// Only in the Z-prepass segment: anywhere else the depth is not the world's final depth.
			const auto now = segment;
			if (now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			if (!resources->hzb || !resources->hzbProgram)
				return prepared;
			const auto frame = CurrentFrame(*resources, now);
			const std::uint32_t renderWidth = frame && frame->width ? frame->width : resources->width;
			const std::uint32_t renderHeight = frame && frame->height ? frame->height : resources->height;
			prepared.program = resources->hzbProgram;
			const std::uint32_t depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip) {
				HzbFrame::Level level{};
				level.constants.targetIndex = CaptureViewIndex(a_preparation, a_bindings.hzbMips[mip]);
				level.constants.targetSize[0] = std::max(1u, resources->hzbWidth >> mip);
				level.constants.targetSize[1] = std::max(1u, resources->hzbHeight >> mip);
				if (mip == 0) {
					level.constants.fromDepth = 1;
					level.constants.sourceIndex = depthIndex;
					// The rendered area, not the texture: the depth image can be larger than the viewport
					// the draws use, and everything past that viewport is untouched. Bounding the read
					// here makes those texels answer the far plane, which suppresses culling.
					level.constants.sourceSize[0] = renderWidth;
					level.constants.sourceSize[1] = renderHeight;
				} else {
					level.constants.fromDepth = 0;
					level.constants.sourceIndex = prepared.levels.back().constants.targetIndex;
					level.constants.sourceSize[0] = prepared.levels.back().constants.targetSize[0];
					level.constants.sourceSize[1] = prepared.levels.back().constants.targetSize[1];
				}
				level.groupsX = (level.constants.targetSize[0] + 7) / 8;
				level.groupsY = (level.constants.targetSize[1] + 7) / 8;
				prepared.levels.push_back(level);
			}
			return prepared;
		}

		static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || a_frame.levels.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			for (std::size_t i = 0; i < a_frame.levels.size(); ++i) {
				const auto& level = a_frame.levels[i];
				if (i != 0) {
					// The level below must be complete before this one reads it.
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
					commands.Barriers(batch);
				}
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&level.constants));
				commands.Dispatch(level.groupsX, level.groupsY, 1);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	struct ProbeBindings
	{
		std::array<org::ResourceBindingToken, kColorTargets> sources;
		org::ResourceBindingToken depth;
		org::ResourceBindingToken destination;
	};

	struct ProbeFrame
	{
		std::uint32_t count = 0;
		std::uint32_t base = 0;        // first slot this pass writes
		std::uint32_t depthSlot = ~0u;  // where this pass puts the depth texel, if it samples it
		std::uint32_t x = 0, y = 0;
	};

	// Copies one texel of each main-pass target into the probe buffer. Two of these are declared, one
	// either side of the opaque pass, so the same texel is sampled before and after DCLF's colour draws
	// within a single frame.
	class ProbePass final : public org::TypedRenderGraphPass<ProbePass, ProbeFrame, ProbeBindings>
	{
	public:
		ProbePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_after) :
			resources(std::move(a_resources)), segment(a_segment), after(a_after) {}

		ProbeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ProbeBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.sources[i] = a_builder.CopySource(resources->native[i]);
			bindings.depth = a_builder.CopySource(resources->nativeDepth);
			bindings.destination = a_builder.CopyDestination(resources->probe);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources,
				now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling ? RenderGraphRuntime::Segment::ZPrepass : now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const
		{
			ProbeFrame prepared{};
			const auto now = segment;
			const bool zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
			const bool gapProbe = now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling;
			if (now != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after) && !gapProbe)
				return prepared;
			const auto frame = CurrentFrame(*resources, gapProbe ? RenderGraphRuntime::Segment::ZPrepass : now);
			if (!frame || !frame->probePixel)
				return prepared;
			prepared.x = frame->probeX;
			prepared.y = frame->probeY;
			if (zPrepass) {
				prepared.depthSlot = kProbeDepthAfterPrepass;  // what the Z-prepass left in the buffer
				return prepared;
			}
			if (gapProbe) {
				prepared.depthSlot = now == RenderGraphRuntime::Segment::SkyOcclusion ? kProbeDepthAfterSky : kProbeDepthAfterLightCulling;
				return prepared;
			}
			prepared.count = resources->targetCount;
			prepared.base = after ? kColorTargets : 0;
			// What the colour pass tests against.
			prepared.depthSlot = after ? kProbeDepthAfterColour : kProbeDepthBeforeColour;
			return prepared;
		}

		static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.count && a_frame.depthSlot == ~0u)
				return;
			auto& commands = a_recording.Commands();
			const auto buffer = a_recording.Resolve(a_bindings.destination).GetHandle();
			if (a_frame.depthSlot != ~0u) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.depth).GetHandle();
				copy.buffer = buffer;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.footprint.offset = std::uint64_t(a_frame.depthSlot) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
			for (std::uint32_t i = 0; i < a_frame.count; ++i) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
				copy.buffer = buffer;
				copy.mip = 0;
				copy.arraySlice = 0;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.z = 0;
				copy.footprint.offset = std::uint64_t(a_frame.base + i) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool after = false;
	};

	bool SetParityEnabled()
	{
		return SwitchEnabled(Switch::SetParity);
	}

	bool BuildParityEnabled()
	{
		return SwitchEnabled(Switch::BuildParity);
	}

	std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
		const char* a_name)
	{
		rhi::vulkan::ImportedImageDesc import{};
		import.image = a_image.image;
		import.createInfo.flags = a_image.flags;
		import.createInfo.imageType = a_image.type;
		import.createInfo.format = a_image.format;
		import.createInfo.extent = a_image.extent;
		import.createInfo.mipLevels = a_image.mipLevels;
		import.createInfo.arrayLayers = a_image.arrayLayers;
		import.createInfo.samples = a_image.samples;
		import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		import.createInfo.usage = a_image.usage;
		import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		import.currentLayout = a_image.layout;
		import.simultaneousAccess = true;
		import.debugName = a_name;
		rhi::ResourcePtr resource;
		if (a_image.layout != VK_IMAGE_LAYOUT_GENERAL || rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
			return nullptr;
		a_desc.imageDimensions.clear();
		a_desc.imageDimensions.push_back({ a_image.extent.width, a_image.extent.height, 0, 0 });
		// An array image publishes a view per slice (ORG builds one DSV per slice for arrays), which is
		// how a shadow view's epoch attaches the one slice the engine gave that view.
		a_desc.isArray = a_image.arrayLayers > 1;
		a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
		a_desc.initialLayout = rhi::ResourceLayout::Common;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
		if (result)
			result->SetName(a_name);
		return result;
	}

	/** @brief The frame shape a shadow epoch's passes draw: the shadow views', or (a_sky) the occlusion views'. */
	std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const ShadowResources& a_resources, bool a_sky)
	{
		return (a_sky ? a_resources.occlusionFrame : a_resources.frame).load(std::memory_order_acquire);
	}

	struct ShadowBuildBindings
	{
		std::array<org::DeclaredViewToken, kShadowModeCount> inputs;
		std::vector<org::DeclaredViewToken> sequences, count, bucketCounts;  // per view slot
		org::DeclaredViewToken geometries, objects, visibility, poolFirsts;
		org::DeclaredViewToken fadeRoots, fadeStates;
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
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				bindings.inputs[m] = a_builder.ShaderResource(resources->inputs[m]).View();
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.UnorderedAccess(resources->sequences[s]).View());
				bindings.count.push_back(a_builder.UnorderedAccess(resources->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(resources->bucketCounts[s]).View());
			}
			bindings.geometries = a_builder.ShaderResource(resources->scene->geometries).View();
			bindings.objects = a_builder.ShaderResource(resources->scene->objects).View();
			bindings.visibility = a_builder.UnorderedAccess(resources->visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(resources->pool->firsts).View();
			// The shadow views' casters under stood-in roots follow FadeStateCS's state (the occlusion views' do not).
			if (!sky && resources->scene->fadeRoots) {
				bindings.fadeRoots = a_builder.ShaderResource(resources->scene->fadeRoots).View();
				bindings.fadeStates = a_builder.ShaderResource(resources->scene->fadeStates).View();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(FadeRows() ? 1u : 0u);
			a_out.push_back(resources->pool->layout);
		}

		ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ShadowBuildPrepared prepared{};
			const auto frame = CurrentShadowFrame(*resources, sky);
			if (!frame || frame->views.empty() || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			const auto geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			const auto objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			const auto visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			const auto poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			const bool fadeRows = FadeRows();
			const auto fadeRootsIndex = fadeRows ? CaptureViewIndex(a_preparation, a_bindings.fadeRoots) : 0u;
			const auto fadeStatesIndex = fadeRows ? CaptureViewIndex(a_preparation, a_bindings.fadeStates) : 0u;
			for (const auto& view : frame->views) {
				if (view.slot >= a_bindings.sequences.size() || view.modeIndex >= kShadowModeCount)
					continue;
				ShadowBuildPrepared::Dispatch dispatch{};
				dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				auto& constants = dispatch.constants;
				constants.latchIndex = frame->latch->SrvIndex();
				constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs[view.modeIndex]);
				constants.geometriesIndex = geometriesIndex;
				constants.objectsIndex = objectsIndex;
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences[view.slot]);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count[view.slot]);
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[view.slot]);
				constants.poolFirstsIndex = poolFirstsIndex;
				// A draw's words name its material row (the table every view reads).
				constants.materialRowsAddressLo = static_cast<std::uint32_t>(view.materialRows);
				constants.materialRowsAddressHi = static_cast<std::uint32_t>(view.materialRows >> 32);
				constants.materialRowStride = sizeof(ShadowMaterialRow);  // no pipeline rows: their stride stays 0
				// The bucket path's draws are bounded by their buckets' capacities (the latch's bucket table); this bounds the other.
				constants.phaseTwoBase = view.sequenceDraws;
				// The single phase (the latch holds the frustum-only mode, with no engine-visibility gate).
				constants.phaseBits = 0;
				// The visibility words are written per object by every dispatch; nothing reads them here.
				constants.visibilityIndex = visibilityIndex;
				constants.fadeRootsIndex = fadeRootsIndex;
				constants.fadeStatesIndex = fadeStatesIndex;
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
		// The fade roots' rows hold the tables' (the depth commit's upload), so a root slot names its row.
		bool FadeRows() const { return !sky && resources->scene->fadeRoots && resources->scene->fadeRootCount; }

		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	struct IndexPoolBindings
	{
		org::DeclaredViewToken copies, indices;
	};

	struct IndexPoolPrepared
	{
		std::shared_ptr<const ShadowIndexPool> pool;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t poolOffset = 0;  // ShadowLatchLayout::PoolOffset
		IndexPoolConstants constants{};
	};

	/**
	 * @brief The index pool's copies (ShadowIndexPool, IndexPoolCS.hlsl): the ranges the shadow commit gave out this frame,
	 * copied from their index buffers before the shadow epoch's views draw from the pool. Its dispatch is in the latch.
	 */
	class IndexPoolPass final : public org::TypedRenderGraphPass<IndexPoolPass, IndexPoolPrepared, IndexPoolBindings>
	{
	public:
		explicit IndexPoolPass(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		IndexPoolBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			IndexPoolBindings bindings{};
			bindings.copies = a_builder.ShaderResource(resources->pool->copies).View();
			bindings.indices = a_builder.UnorderedAccess(resources->pool->indices).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, false);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->pool->layout);
		}

		IndexPoolPrepared Prepare(const IndexPoolBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			IndexPoolPrepared prepared{};
			const auto frame = CurrentShadowFrame(*resources, false);
			if (!frame || !frame->latch)
				return prepared;
			prepared.pool = resources->pool;
			prepared.latch = frame->latch;
			prepared.poolOffset = resources->latchLayout.PoolOffset();
			prepared.constants.latchIndex = frame->latch->SrvIndex();
			prepared.constants.copiesIndex = CaptureViewIndex(a_preparation, a_bindings.copies);
			prepared.constants.poolIndex = CaptureViewIndex(a_preparation, a_bindings.indices);
			return prepared;
		}

		static void Record(const IndexPoolBindings&, const IndexPoolPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.pool || !a_prepared.latch)
				return;
			auto constants = a_prepared.constants;
			const auto& layout = a_prepared.latch;
			const std::uint64_t offset = layout->Offset(a_recording.FrameSlot()) + a_prepared.poolOffset;
			constants.latchOffset = static_cast<std::uint32_t>(offset);
			auto& commands = a_recording.Commands();
			const auto& program = *a_prepared.pool->program;
			commands.BindLayout(program.layout->GetHandle());
			commands.BindPipeline(program.pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kIndexPoolConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.ExecuteIndirect(a_prepared.pool->dispatchSignature->GetHandle(), layout->Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> sequences, bucketCounts;  // per view slot
		org::ResourceBindingToken pool;  // the index pool (ShadowIndexPool)
		org::ResourceBindingToken materialRows, constants, viewBlocks, objects, bones;
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

	/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport, in one pass. */
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
			// The shadow views draw into the shadow maps, the occlusion epoch into the occlusion maps alone.
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i] && IsOcclusionTarget(i) == sky) {
					bindings.depthViews[i].reserve(resources->depthLayers[i]);
					for (std::uint32_t slice = 0; slice < resources->depthLayers[i]; ++slice)
						bindings.depthViews[i].push_back(a_builder.DepthReadWrite(resources->depth[i], org::DsvView{ UINT32_MAX, 0, slice }).View());
				}
			}
			// A slot's sequences are both the draws' arguments and what their vertex stage reads by device address (DCLF_PULLED).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.IndirectArguments(resources->sequences[s]));
				a_builder.ShaderResource(resources->sequences[s], noViews);
				bindings.bucketCounts.push_back(a_builder.IndirectArguments(resources->bucketCounts[s]));
			}
			bindings.materialRows = a_builder.ShaderResource(resources->materialRows.buffer, noViews).Resource();
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(resources->viewBlocks.buffer, noViews).Resource();
			bindings.objects = a_builder.ShaderResource(resources->scene->objects, noViews).Resource();
			bindings.bones = a_builder.ShaderResource(resources->scene->bones, noViews).Resource();
			// The face positions (a dynamic shape's second stream), and the geometries' vertices and indices, are read by the
			// vertex stage through their addresses: the face positions after the commit's uploads into them.
			a_builder.ShaderResource(resources->scene->facePositions, noViews);
			bindings.pool = a_builder.IndexBuffer(resources->pool->indices);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->pool->layout);
		}

		ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ShadowPrepared prepared{};
			auto frame = CurrentShadowFrame(*resources, sky);
			if (!frame || frame->views.empty() || !frame->indirect.valid)
				return prepared;
			for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				if (!view.capacity || view.slot >= a_bindings.sequences.size() || view.target >= kShadowDepthTargets || !resources->depth[view.target] ||
					IsOcclusionTarget(view.target) != sky)
					continue;
				if (view.slice >= resources->depthLayers[view.target])
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
			// A view's pass: its slice and viewport, loaded, added to, stored.
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
				// The slice the engine drew its own casters into: loaded, added to, stored.
				auto& depth = depths[i];
				depth.dsv = a_recording.Resolve(prepared.depthView);
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.depth = &depth;
				begin.debugName = a_prepared.sky ? "DCLF occlusion map" : "DCLF shadow view";
			}
			// A view: its pass (its slice, the layout, the topology, its push data: its blocks, and the index pool), then a plain
			// indexed indirect draw per bucket (ShadowBucket), the depth-only class's first: the bucket's pipeline, its push words
			// (kShadowDrawPushSequences and after), and its range of the slot's sequences, as many as its count word holds. The
			// vertex stage fetches each draw's vertices through the sequence its instance names (Utility.hlsl, DCLF_PULLED), since
			// a plain draw cannot bind a vertex buffer of its own.
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
					split(words + kShadowDrawPushMaterialRows, view.materialRows);
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

	/** @brief The scene tables, which both extensions register (the same identifiers: the second registration is an update). */
	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene)
	{
		a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.objects"), a_scene.objects);
		a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.bones"), a_scene.bones);
		a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.geometries"), a_scene.geometries);
		a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.face-positions"), a_scene.facePositions);
		if (a_scene.trees) {
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.trees"), a_scene.trees);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.tree-clocks"), a_scene.treeClocks);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.tree-objects"), a_scene.treeObjects);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.tree-frame"), a_scene.treeFrameBuffer);
		}
		if (a_scene.fadeRoots) {
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.fade-roots"), a_scene.fadeRoots);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.fade-states"), a_scene.fadeStates);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.fade-frame"), a_scene.fadeFrameBuffer);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.fade-visibility"), a_scene.fadeVisibility);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.fade-log"), a_scene.fadeLog);
		}
	}

	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.material-rows"), resources->materialRows.buffer);
			RegisterSceneBuffers(a_graph, *resources->scene);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-inputs{}", m)), resources->inputs[m]);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.view-blocks"), resources->viewBlocks.buffer);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.index-pool"), resources->pool->indices);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.pool-firsts"), resources->pool->firsts);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.pool-copies"), resources->pool->copies);
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.bucket-counts{}", s)), resources->bucketCounts[s]);
			}
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i])
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::ShadowView);
			AddTreeWindPasses(resources->scene, "cs.dclf.shadow", epoch, a_out);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, false)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.shadow.view",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, false)))
					.Epoch(epoch));
			// Skylighting's occlusion map: the same passes over its one view, in its own epoch after RenderMask.
			const auto skyEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::SkyOcclusion);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.sky.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, true)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(skyEpoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.sky.view",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, true)))
					.Epoch(skyEpoch));
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.material-rows"), resources->materialRows.buffer);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.pipeline-rows"), resources->pipelineRows.buffer);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-inputs"), resources->inputs);
			if (resources->inputsDepth)
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-inputs-depth"), resources->inputsDepth);
			RegisterSceneBuffers(a_graph, *resources->scene);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
			if (resources->frustum)
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.frustum"), resources->frustum);
			for (const auto& frameBuffer : resources->frameBuffers)
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
			if (resources->nativeDepth)
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			if (resources->hzb)
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
			if (resources->sort)
				resources->sort->Register(a_graph);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			using Segment = RenderGraphRuntime::Segment;
			const auto colour = RenderGraphRuntime::EpochOf(Segment::MainOpaque);
			const auto depth = RenderGraphRuntime::EpochOf(Segment::ZPrepass);
			const auto colourSegment = Segment::MainOpaque;
			const auto depthSegment = Segment::ZPrepass;
			// A pass instance runs only in its own epoch and its segment is fixed here, so the Z-prepass has its own
			// build-draws and draw pass.
			// The sort by pipeline after a build (SortDraws): the counts' prefix sum, then the scatter. The scan runs in every
			// execution, so the counts it clears are zero whether or not a build ran; the scatter only after a build.
			const auto addSort = [&](const char* a_scan, const char* a_scatter, RenderGraphRuntime::Segment a_segment, auto a_epoch) {
				if (!resources->sort)
					return;
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scan, resources->sort->ScanPass())
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scatter,
					std::static_pointer_cast<org::RenderPass>(std::make_shared<SortSequencesPass>(resources, a_segment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
			};
			AddTreeWindPasses(resources->scene, "cs.dclf.z", depth, a_out);
			// The fade roots' state, before the depth segment's culling reads it.
			if (resources->scene->fadeState && resources->scene->fadeRoots)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.fade-state",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<FadeStatePass>(resources)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			addSort("cs.dclf.z.sort-scan", "cs.dclf.z.sort-scatter", depthSegment, depth);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.z.depth",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment)))
					.Epoch(depth));
			AddTreeWindPasses(resources->scene, "cs.dclf", colour, a_out);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, colourSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(colour));
			addSort("cs.dclf.sort-scan", "cs.dclf.sort-scatter", colourSegment, colour);
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, false)))
						.Epoch(colour));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.main-opaque",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, colourSegment)))
					.Epoch(colour));
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, true)))
						.Epoch(colour));
			// The two-phase tail, all of it inside the depth segment and all of it in this order:
			//
			//   build-draws (phase 1, against the HZB the previous frame left)
			//   main-opaque (the phase-1 depth draw)
			//   hzb         (rebuilt from the depth that draw has just finished)
			//   build-draws-phase2 (phase 1's rejects, re-tested against the rebuilt HZB)
			//   depth-phase2       (the rescues, so their depth is in the frame too)
			//
			// Phase 1 tests against a depth buffer that is a frame old, which is what makes it cheap and
			// also what makes it wrong at the edges: anything that has just come out from behind an
			// occluder was hidden in that buffer. Phase 2 exists to take those back, after a rebuild
			// that can see them.
			if (resources->hzb) {
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.hzb",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<HzbPass>(resources, depthSegment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws-phase2",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment, 2)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.depth-phase2",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment, true)))
						.Epoch(depth));
			}
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.probe-after",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, depthSegment, true)))
						.Epoch(depth));
			if (resources->probe) {
				for (const auto segment : { Segment::SkyOcclusion, Segment::LightCulling })
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy(segment == Segment::SkyOcclusion ? "cs.dclf.sky.probe-depth" : "cs.dclf.light.probe-depth",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, segment, false)))
							.Epoch(RenderGraphRuntime::EpochOf(segment)));
			}
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources)
	{
		return std::make_unique<MainOpaqueExtension>(std::move(a_resources));
	}

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources)
	{
		return std::make_unique<ShadowExtension>(std::move(a_resources));
	}
}

#endif
