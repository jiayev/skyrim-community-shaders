// The main opaque pass: DCLF's draws in the Z-prepass (depth, both phases) and the main colour pass, and tree LOD's draw.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Passes/PassesInternal.h"

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
			auto* stats = a_prepared.stats;
			const std::uint32_t statsSlot = a_recording.FrameSlot();
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
			// Begins a draw pass identically on the command list and on a preprocess state list.
			auto beginDrawPass = [&](rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin) {
				a_list.BeginPass(a_begin);
				a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				a_list.BindLayout(frame.indirect.layout);
				a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			};
			// The frame slot's preprocess state list; null without explicit preprocessing.
			auto preprocessState = [&]() -> rhi::CommandList* {
				if (!a_prepared.preprocess)
					return nullptr;
				auto& state = a_prepared.preprocess->Begin(statsSlot);
				state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				return &state;
			};
			if (zPrepass) {
				// Plain indexed indirect draws (DCLF_PULLED), one per bucket over its range of the phase's sequences (phase 2's past
				// sequenceDraws), with push words kZDrawPush* and the bucket's count word. A plain draw cannot bind its own vertex
				// buffer or push data, so the vertex stage fetches through the sequence its instance names (Lighting.hlsl).
				(void)count;
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
					rhi::DepthAttachment terrainDepth = depth;
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
				return;
			}

			// The main pass tests depth EQUAL against the Z-prepass and writes none.
			// Opaque decal depth goes first: the engine draws its opaque decal group with depth writes and bias, so a host fragment
			// under an opaque decal texel fails the colour EQUAL test (the decal overwrites every target there). The depth variants
			// test LESS_EQUAL with the bias and alpha-test, so a transparent texel keeps the host's depth. Blended decals write no
			// depth: their host must still be shaded.
			// nvperf ranges around the colour pass's parts, when a capture selects them.
			void* const nvCommands = NvPerfBridge::Active() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
			auto subRange = [&](const char* a_name) { return nvCommands && NvPerfBridge::PushPassRange(nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (nvCommands) NvPerfBridge::PopPassRange(nvCommands, a_pushed); };
			// The colour passes, every target loaded.
			rhi::PassBeginInfo mainBegin = begin;
			mainBegin.colors = { colors.data(), a_prepared.targetCount };
			mainBegin.debugName = "DCLF main opaque";
			rhi::PassBeginInfo decalBegin = begin;
			decalBegin.colors = { colors.data(), a_prepared.targetCount };
			decalBegin.debugName = "DCLF decals";
			rhi::PassBeginInfo decalDepthBegin = begin;  // depth only
			decalDepthBegin.debugName = "DCLF decal depth";
			const auto colourSignature = frame.indirect.signatures[kColorVariant];
			auto decalArguments = [&](std::uint32_t a_group) {
				return (2 * std::uint64_t(frame.sequenceDraws) + std::uint64_t(a_group) * frame.sequenceDecals) * sizeof(DrawSequence);
			};
			auto decalCount = [](std::uint32_t a_group) { return std::uint64_t(DecalCountWord(a_group)) * sizeof(std::uint32_t); };
			// The terrain groups' ranges (kTerrainGroups), within phase 2's, and their count words.
			auto terrainArguments = [&](std::uint32_t a_group) {
				return (std::uint64_t(frame.sequenceDraws) + (a_group == 0 ? 0u : frame.terrainCapacity[0])) * sizeof(DrawSequence);
			};
			auto terrainCount = [](std::uint32_t a_group) { return std::uint64_t(kCountTerrainGroupWord + a_group) * sizeof(std::uint32_t); };
			const bool anyTerrain = frame.terrainCapacity[0] || frame.terrainCapacity[1];
			const bool anyDecals = frame.decalCapacity[0] || frame.decalCapacity[1] || frame.decalCapacity[2] || anyTerrain;
			// The second pass's calls in order: opaque decals and multi-index layers, Terrain Blending's terrain and the meshes it
			// redraws (its replay follows the engine's opaque groups), then blended decals.
			auto secondPass = [&](auto&& a_call) {
				for (const std::uint32_t group : kDecalDrawOrder) {
					if (group == 1)
						for (std::uint32_t terrain = 0; terrain < kTerrainGroups; ++terrain)
							if (frame.terrainCapacity[terrain])
								a_call(terrainArguments(terrain), terrainCount(terrain), frame.terrainCapacity[terrain],
									terrain == 0 ? "cs.dclf.colour.terrain" : "cs.dclf.colour.terrain-no-blend");
					if (frame.decalCapacity[group])
						a_call(decalArguments(group), decalCount(group), frame.decalCapacity[group],
							group == 0 ? "cs.dclf.colour.decals-opaque" : group == 2 ? "cs.dclf.colour.decals-layers" : "cs.dclf.colour.decals-blended");
				}
			};

			// Every call is preprocessed before the first pass: generated inside the pass by NVIDIA's driver, each colour call idles
			// the GPU for a fixed time in these eight-target passes. The state list must match the execution below exactly (same
			// pass setup and heaps); sequences and counts are final (the culling wrote them).
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
					secondPass([&](std::uint64_t a_arguments, std::uint64_t a_count, std::uint32_t a_capacity, const char*) {
						commands.PreprocessIndirect(state, colourSignature, sequences, a_arguments, count, a_count, a_capacity);
					});
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
			if (anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				beginDrawPass(commands, decalBegin);
				secondPass([&](std::uint64_t a_arguments, std::uint64_t a_count, std::uint32_t a_capacity, const char* a_name) {
					const bool groupPart = subRange(a_name);
					commands.ExecuteIndirect(colourSignature, sequences, a_arguments, count, a_count, a_capacity);
					endSubRange(groupPart);
				});
				commands.EndPass();
				endSubRange(decalPart);
			}
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

#endif
