// The main opaque pass: DCLF's draws in the Z-prepass (depth, both phases) and the main colour pass, and tree LOD's draw.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	/**
	 * @brief Records tree LOD's draw (dclf-lod.md): push data is the draw row's address and whether this is phase 2's depth draw;
	 * arguments come from the list's header (TreeLod::VisibleHeader).
	 */
	void RecordTreeLodDraw(rhi::CommandList& a_commands, const TreeLodPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, std::uint64_t a_draw, rhi::ResourceHandle a_visible)
	{
		std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_draw), static_cast<std::uint32_t>(a_draw >> 32), a_phaseTwo ? 1u : 0u };
		const std::uint64_t arguments = a_colour ? offsetof(TreeLod::VisibleHeader, colour) :
		                                a_phaseTwo ? offsetof(TreeLod::VisibleHeader, phaseTwo) : offsetof(TreeLod::VisibleHeader, phaseOne);
		a_commands.BindPipeline(a_colour ? a_pipelines.colour : a_pipelines.depth);
		a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		a_commands.ExecuteIndirect(a_pipelines.drawSignature, a_visible, arguments, {}, 0, 1);
	}

	namespace
	{
		/** @brief What MainOpaquePass::Record's stages share: the frame, the resolved sequences and count, the depth pass setup, the stats slot. */
		struct MainRecording
		{
			const PassFrame& frame;
			rhi::CommandList& commands;
			rhi::ResourceHandle sequences;
			rhi::ResourceHandle count;
			const rhi::PassBeginInfo& begin;  // depth attached, no colours
			const rhi::DepthAttachment& depth;
			PassStats* stats;
			std::uint32_t statsSlot;
		};

		/**
		 * @brief The Z-prepass's draws: plain indexed indirect draws (DCLF_PULLED), one per bucket over its range of the phase's
		 * sequences (phase 2's past sequenceDraws), with push words kZDrawPush* and the bucket's count word. A plain draw cannot bind
		 * its own vertex buffer or push data, so the vertex stage fetches through the sequence its instance names (Lighting.hlsl).
		 */
		void RecordDepthDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			const auto& begin = a_pass.begin;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
			if (stats)
				stats->Start(commands, statsSlot, depthKind);
			commands.BeginPass(begin);
			commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commands.BindLayout(frame.indirect.zLayout);
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
			if (stats)
				stats->Begin(commands, statsSlot, depthKind);
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			const auto counts = a_recording.Resolve(a_bindings.bucketCounts).GetHandle();
			const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
			const std::uint64_t phaseBase = a_prepared.phaseTwo ? std::uint64_t(frame.sequenceDraws) : 0;
			auto drawCall = [&](const PassFrame::ZCall& a_call, bool a_offsetDepth) {
				RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesAddress, phaseBase + a_call.first, a_call.pipeline, counts, a_call.bucket,
					a_call.capacity, a_offsetDepth);
			};
			// A terrain group 1 twin (near blocks) draws into the main depth with Terrain Blending's offset, as its depth hook
			// does (TerrainBlending::TerrainShaderHacks, OFFSET_DEPTH).
			bool terrainNear = false;
			for (const auto& call : frame.zCalls) {
				drawCall(call, call.terrainNear);
				terrainNear = terrainNear || call.terrainNear;
			}
			// Tree LOD's depth: one instanced draw of the phase's culled records.
			if (a_prepared.treeLod)
				RecordTreeLodDraw(commands, *a_prepared.treeLod, false, a_prepared.phaseTwo, a_prepared.treeLodDraw, a_recording.Resolve(a_bindings.treeLodVisible).GetHandle());
			if (stats)
				stats->End(commands, statsSlot, depthKind);
			commands.EndPass();
			// The twins again without the offset into Terrain Blending's terrain depth (its first draw of a near block), which
			// TerrainBlending::BlendPrepassDepths reads after this epoch.
			if (terrainNear && a_prepared.terrainDepth) {
				rhi::DepthAttachment terrainDepth = a_pass.depth;
				terrainDepth.dsv = a_recording.Resolve(a_prepared.terrainDepthView);
				rhi::PassBeginInfo terrainBegin = begin;
				terrainBegin.depth = &terrainDepth;
				terrainBegin.debugName = "DCLF terrain depth";
				commands.BeginPass(terrainBegin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				for (const auto& call : frame.zCalls)
					if (call.terrainNear)
						drawCall(call, false);
				commands.EndPass();
			}
			if (stats)
				stats->Resolve(commands, statsSlot, depthKind);
		}

		/** @brief Begins a draw pass identically on the command list and on a preprocess state list. */
		void BeginDrawPass(rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin, const PassFrame& a_frame, const PreparedDraws& a_prepared)
		{
			a_list.BeginPass(a_begin);
			a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			a_list.BindLayout(a_frame.indirect.layout);
			a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
		}

		/** @brief The colour segment's passes (every target loaded; the decal depth pass depth only) and its draw ranges. */
		struct ColourPasses
		{
			const PassFrame& frame;
			rhi::PassBeginInfo mainBegin;
			rhi::PassBeginInfo decalBegin;
			rhi::PassBeginInfo decalDepthBegin;
			rhi::CommandSignatureHandle colourSignature;
			bool anyDecals = false;

			std::uint64_t DecalArguments(std::uint32_t a_group) const
			{
				return (2 * std::uint64_t(frame.sequenceDraws) + std::uint64_t(a_group) * frame.sequenceDecals) * sizeof(DrawSequence);
			}
			static std::uint64_t DecalCount(std::uint32_t a_group) { return std::uint64_t(DecalCountWord(a_group)) * sizeof(std::uint32_t); }
			// The terrain groups' ranges (kTerrainGroups), within phase 2's, and their count words.
			std::uint64_t TerrainArguments(std::uint32_t a_group) const
			{
				return (std::uint64_t(frame.sequenceDraws) + (a_group == 0 ? 0u : frame.terrainCapacity[0])) * sizeof(DrawSequence);
			}
			static std::uint64_t TerrainCount(std::uint32_t a_group) { return std::uint64_t(kCountTerrainGroupWord + a_group) * sizeof(std::uint32_t); }
			// The second pass's calls in order: opaque decals and multi-index layers, Terrain Blending's terrain and the meshes it
			// redraws (its replay follows the engine's opaque groups), then blended decals.
			template <class Call>
			void SecondPass(Call&& a_call) const
			{
				for (const std::uint32_t group : kDecalDrawOrder) {
					if (group == 1)
						for (std::uint32_t terrain = 0; terrain < kTerrainGroups; ++terrain)
							if (frame.terrainCapacity[terrain])
								a_call(TerrainArguments(terrain), TerrainCount(terrain), frame.terrainCapacity[terrain],
									terrain == 0 ? "cs.dclf.colour.terrain" : "cs.dclf.colour.terrain-no-blend");
					if (frame.decalCapacity[group])
						a_call(DecalArguments(group), DecalCount(group), frame.decalCapacity[group],
							group == 0 ? "cs.dclf.colour.decals-opaque" : group == 2 ? "cs.dclf.colour.decals-layers" : "cs.dclf.colour.decals-blended");
				}
			}
		};

		/**
		 * @brief Every colour call preprocessed before the first pass: generated inside the pass by NVIDIA's driver, each colour call
		 * idles the GPU for a fixed time in these eight-target passes. The state list must match the execution exactly (same pass
		 * setup and heaps); sequences and counts are final (the culling wrote them). Nothing without explicit preprocessing.
		 */
		void RecordColourPreprocess(const PreparedDraws& a_prepared, const MainRecording& a_pass, const ColourPasses& a_passes)
		{
			if (!a_prepared.preprocess)
				return;
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			const auto count = a_pass.count;
			// The frame slot's preprocess state list.
			auto& state = a_prepared.preprocess->Begin(a_pass.statsSlot);
			state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			if (frame.decalCapacity[0]) {
				BeginDrawPass(state, a_passes.decalDepthBegin, frame, a_prepared);
				commands.PreprocessIndirect(state, frame.indirect.signatures[kDepthVariant], sequences, a_passes.DecalArguments(0), count, ColourPasses::DecalCount(0), frame.decalCapacity[0]);
				state.EndPass();
			}
			BeginDrawPass(state, a_passes.mainBegin, frame, a_prepared);
			if (frame.drawCapacity)
				commands.PreprocessIndirect(state, a_passes.colourSignature, sequences, 0, count, 0, frame.drawCapacity);
			state.EndPass();
			if (a_passes.anyDecals) {
				BeginDrawPass(state, a_passes.decalBegin, frame, a_prepared);
				a_passes.SecondPass([&](std::uint64_t a_arguments, std::uint64_t a_count, std::uint32_t a_capacity, const char*) {
					commands.PreprocessIndirect(state, a_passes.colourSignature, sequences, a_arguments, count, a_count, a_capacity);
				});
				state.EndPass();
			}
			state.End();
		}

		/** @brief The colour passes' execution: decal depth, the main draws and tree LOD's colour, then the second pass; nvperf ranges around the parts. */
		void RecordColourExecution(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			const ColourPasses& a_passes, void* const a_nvCommands)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			const auto count = a_pass.count;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			auto subRange = [&](const char* a_name) { return a_nvCommands && NvPerfBridge::PushPassRange(a_nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (a_nvCommands) NvPerfBridge::PopPassRange(a_nvCommands, a_pushed); };
			if (frame.decalCapacity[0]) {
				const bool part = subRange("cs.dclf.colour.decal-depth");
				BeginDrawPass(commands, a_passes.decalDepthBegin, frame, a_prepared);
				commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, a_passes.DecalArguments(0), count, ColourPasses::DecalCount(0), frame.decalCapacity[0]);
				commands.EndPass();
				endSubRange(part);
			}
			if (stats)
				stats->Start(commands, statsSlot, PassStats::kColour);
			const bool mainPart = subRange("cs.dclf.colour.main");
			BeginDrawPass(commands, a_passes.mainBegin, frame, a_prepared);
			if (stats)
				stats->Begin(commands, statsSlot, PassStats::kColour);
			if (frame.drawCapacity) {
				const bool drawPart = subRange("cs.dclf.colour.main-draws");
				commands.ExecuteIndirect(a_passes.colourSignature, sequences, 0, count, 0, frame.drawCapacity);
				endSubRange(drawPart);
			}
			// Tree LOD's colour tests EQUAL against its Z-prepass draw, under the Z-prepass layout (same frame push data).
			if (a_prepared.treeLod) {
				const bool treePart = subRange("cs.dclf.colour.tree-lod");
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				RecordTreeLodDraw(commands, *a_prepared.treeLod, true, false, a_prepared.treeLodDraw, a_recording.Resolve(a_bindings.treeLodVisible).GetHandle());
				endSubRange(treePart);
			}
			if (stats)
				stats->End(commands, statsSlot, PassStats::kColour);
			commands.EndPass();
			endSubRange(mainPart);
			if (stats)
				stats->Resolve(commands, statsSlot, PassStats::kColour);

			// The second pass: decals after every opaque draw in the engine's order, each group from its own range and count word,
			// LESS_EQUAL with the engine's decal bias; only the layers write depth here (opaque decal depth is already in). Before the
			// blended group, Terrain Blending's terrain (blend mode 1, LESS_EQUAL with depth writes, mask t55 in the frame record) and
			// its no-blend meshes (EQUAL), as that feature replays them (Deferred::Hooks::Main_RenderWorld_BlendedDecals).
			if (a_passes.anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				BeginDrawPass(commands, a_passes.decalBegin, frame, a_prepared);
				a_passes.SecondPass([&](std::uint64_t a_arguments, std::uint64_t a_count, std::uint32_t a_capacity, const char* a_name) {
					const bool groupPart = subRange(a_name);
					commands.ExecuteIndirect(a_passes.colourSignature, sequences, a_arguments, count, a_count, a_capacity);
					endSubRange(groupPart);
				});
				commands.EndPass();
				endSubRange(decalPart);
			}
		}

		/**
		 * @brief The colour segment's draws. The main pass tests depth EQUAL against the Z-prepass and writes none.
		 * Opaque decal depth goes first: the engine draws its opaque decal group with depth writes and bias, so a host fragment
		 * under an opaque decal texel fails the colour EQUAL test (the decal overwrites every target there). The depth variants
		 * test LESS_EQUAL with the bias and alpha-test, so a transparent texel keeps the host's depth. Blended decals write no
		 * depth: their host must still be shaded.
		 */
		void RecordColourDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			std::array<rhi::ColorAttachment, kColorTargets>& a_colors)
		{
			const auto& frame = a_pass.frame;
			// nvperf ranges around the colour pass's parts, when a capture selects them.
			void* const nvCommands = NvPerfBridge::Active() ? rhi::vulkan::get_cmd_list(a_pass.commands) : nullptr;
			// The colour passes, every target loaded.
			ColourPasses passes{ .frame = frame, .mainBegin = a_pass.begin, .decalBegin = a_pass.begin, .decalDepthBegin = a_pass.begin,
				.colourSignature = frame.indirect.signatures[kColorVariant] };
			passes.mainBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.mainBegin.debugName = "DCLF main opaque";
			passes.decalBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.decalBegin.debugName = "DCLF decals";
			passes.decalDepthBegin.debugName = "DCLF decal depth";  // depth only
			const bool anyTerrain = frame.terrainCapacity[0] || frame.terrainCapacity[1];
			passes.anyDecals = frame.decalCapacity[0] || frame.decalCapacity[1] || frame.decalCapacity[2] || anyTerrain;

			RecordColourPreprocess(a_prepared, a_pass, passes);
			RecordColourExecution(a_bindings, a_prepared, a_recording, a_pass, passes, nvCommands);
		}
	}

	class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
	{
	public:
		// phaseTwo: the second depth draw, of what the rebuilt HZB brought back (the sequence buffer's reserved part).
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
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.count = a_builder.IndirectArguments(resources->count);
			if (segment == RenderGraphRuntime::Segment::ZPrepass) {
				// The plain draws (DCLF_PULLED) read sequences and face positions by address and index the pool.
				a_builder.ShaderResource(*resources->sequences, noViews);
				a_builder.ShaderResource(*resources->scene->facePositions, noViews);
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				bindings.bucketCounts = a_builder.IndirectArguments(*resources->zBucketCounts[phaseTwo ? 1 : 0]).Resource();
				// Terrain Blending's terrain depth, drawn by the near terrain blocks' twin buckets.
				if (resources->terrainDepth)
					bindings.terrainDepth = a_builder.DepthReadWrite(resources->terrainDepth).View();
			} else {
				// The face draws' second vertex stream.
				a_builder.VertexBuffer(*resources->scene->facePositions);
				// The foliage record (Policy::RecordFoliagePixels): alpha-tested draws write per-pixel ids by address.
				if (const auto& foliage = resources->foliage) {
					const std::span<const org::UavView> noUavs{};
					for (std::uint32_t h = 0; h < 2; ++h) {
						a_builder.UnorderedAccess(foliage->ids[h], noUavs);
						a_builder.UnorderedAccess(foliage->colours[h], noUavs);
					}
				}
			}
			DeclareFrameBuffers(a_builder, *resources);
			a_builder.ShaderResource(resources->frameConstants, noViews);  // the frame record's slots, by address
			// Tree LOD's arguments and the tables its vertex stage reads by address.
			if (const auto& scene = *resources->scene; scene.treeLodCull) {
				bindings.treeLodVisible = a_builder.IndirectArguments(*scene.treeLodVisible).Resource();
				for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
					a_builder.ShaderResource(*table, noViews);
				a_builder.ShaderResource(scene.treeLodDraw, noViews);
			}
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(phaseTwo ? 1 : 0);
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->indices) : 0);
			// Tree LOD's pipelines (built late, rebuilt on a target change).
			a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->scene->treeLodPipelines.load(std::memory_order_acquire).get()));
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			const auto now = segment;
			auto frame = CurrentFrame(a_preparation, now);
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
			if (resources->scene->treeLodCull) {
				prepared.treeLod = resources->scene->treeLodPipelines.load(std::memory_order_acquire);
				prepared.treeLodDraw = resources->scene->treeLodDrawAddress;
			}
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
				prepared.targetViews[i] = a_preparation.Capture(a_bindings.targets[i]);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			if (prepared.zPrepass && resources->terrainDepth) {
				prepared.terrainDepthView = a_preparation.Capture(a_bindings.terrainDepth);
				prepared.terrainDepth = true;
			}
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			auto colors = LoadedColours(a_recording, a_prepared.targetViews, a_bindings.targets, a_prepared.targetCount);
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

			// The Z-prepass adds DCLF's objects to the native depth, inside the native depth pass, so everything after (native
			// depth-tested draws, the sky, depth readers) sees them.
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			begin.depth = &depth;
			begin.debugName = "DCLF depth";
			const MainRecording pass{ frame, commands, sequences, count, begin, depth, a_prepared.stats, a_recording.FrameSlot() };
			if (zPrepass) {
				RecordDepthDraws(a_bindings, a_prepared, a_recording, pass);
				return;
			}
			RecordColourDraws(a_bindings, a_prepared, a_recording, pass, colors);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool phaseTwo = false;
	};

	std::shared_ptr<org::RenderPass> MakeMainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_phaseTwo)
	{
		return std::make_shared<MainOpaquePass>(std::move(a_resources), a_segment, a_phaseTwo);
	}
}
