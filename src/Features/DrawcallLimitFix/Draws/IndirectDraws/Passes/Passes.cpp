// DCLF's render graph passes and the extensions that add them (main opaque, forward, shadow views, occlusion, HZB, tree LOD and
// wind, fade states, visibility events). Declared at graph build; prepared and recorded on ORG's host thread from the epoch
// revision's shape (RevisionShapes), never from the frame's live state.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

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

	struct TreeLodCullBindings
	{
		org::DeclaredViewToken shapes, instances, draw, visible, hzb;
	};

	struct TreeLodCullPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeLodCullConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree LOD's cull (TreeLodCullCS.hlsl) in the depth segment's two phases, like BuildDraws: phase 1 tests every instance
	 * against the frustum and last frame's HZB; phase 2 retests phase 1's occluded ones against the HZB rebuilt this frame. Both
	 * append to the list the passes draw.
	 */
	class TreeLodCullPass final : public org::TypedRenderGraphPass<TreeLodCullPass, TreeLodCullPrepared, TreeLodCullBindings>
	{
	public:
		TreeLodCullPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_phase) :
			resources(std::move(a_resources)), phase(a_phase) {}

		TreeLodCullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->scene;
			TreeLodCullBindings bindings{};
			bindings.shapes = a_builder.ShaderResource(*scene.treeLodShapes).View();
			bindings.instances = a_builder.ShaderResource(*scene.treeLodInstances).View();
			bindings.draw = a_builder.ShaderResource(scene.treeLodDraw).View();
			bindings.visible = a_builder.UnorderedAccess(*scene.treeLodVisible).View();
			// Which HZB a phase sees depends on its place around the HZB build (GatherStructuralPasses).
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			return bindings;
		}

		// The recording depends on the tables' versions (a growth changes views and dispatch) and the depth segment's shape.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			const auto& scene = *resources->scene;
			for (const auto* table : { &scene.treeLodShapes, &scene.treeLodInstances, &scene.treeLodVisible })
				a_out.push_back(NamedSerial(a_preparation, *table));
			a_out.push_back(frame ? frame->generation : 0);
		}

		TreeLodCullPrepared Prepare(const TreeLodCullBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TreeLodCullPrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			// The shape slots every table the cull reads holds (the revision's versions).
			const std::uint64_t shapes = std::min({ NamedUnitsOf(a_preparation, scene.treeLodShapes), NamedUnitsOf(a_preparation, scene.treeLodInstances),
				NamedUnitsOf(a_preparation, scene.treeLodVisible) });
			if (!scene.treeLodCull || !shapes || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.treeLodCull;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
			constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
			constants.drawIndex = CaptureViewIndex(a_preparation, a_bindings.draw);
			constants.visibleIndex = CaptureViewIndex(a_preparation, a_bindings.visible);
			constants.latchIndex = frame->latch->SrvIndex();
			constants.phase = phase;
			const std::uint64_t records = shapes * TreeLod::kMaxGroupInstances;
			constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader) + records * sizeof(std::uint32_t));
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// Dispatched for full capacity; the shader stops at the real counts.
			prepared.groups = static_cast<std::uint32_t>((records + kTreeLodCullGroup - 1) / kTreeLodCullGroup);
			return prepared;
		}

		static void Record(const TreeLodCullBindings&, const TreeLodCullPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::uint32_t phase = 1;
	};

	struct TreeLodReadbackBindings
	{
		org::ResourceBindingToken visible;
	};

	struct TreeLodReadbackPrepared
	{
		std::shared_ptr<SceneBuffers::TreeLodCounts> counts;
	};

	/** @brief Copies tree LOD's list header to the frame slot's host buffer after phase 2, first summing what the slot held. */
	class TreeLodReadbackPass final : public org::TypedRenderGraphPass<TreeLodReadbackPass, TreeLodReadbackPrepared, TreeLodReadbackBindings>
	{
	public:
		explicit TreeLodReadbackPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeLodReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(*scene->treeLodVisible) };
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(NamedSerial(a_preparation, scene->treeLodVisible));
		}

		TreeLodReadbackPrepared Prepare(const TreeLodReadbackBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			return { scene->treeLodCounts };
		}

		static void Record(const TreeLodReadbackBindings& a_bindings, const TreeLodReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.counts)
				return;
			auto& counts = *a_frame.counts;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= counts.readback.size())
				return;
			if (counts.filled[slot]) {
				auto resource = counts.readback[slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					TreeLod::VisibleHeader header;
					std::memcpy(&header, mapped, sizeof(header));
					counts.frames.fetch_add(1, std::memory_order_relaxed);
					counts.phaseOne.fetch_add(header.phaseOne[1], std::memory_order_relaxed);
					counts.retests.fetch_add(header.retests, std::memory_order_relaxed);
					counts.phaseTwo.fetch_add(header.phaseTwo[1], std::memory_order_relaxed);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(counts.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.visible).GetHandle(), 0,
				sizeof(TreeLod::VisibleHeader));
			counts.filled[slot] = 1;
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

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
			// Read by device address; declared so the graph orders them after their uploads.
			a_builder.ShaderResource(*resources->materialRows.buffer, noViews);
			a_builder.ShaderResource(*resources->pipelineRows.buffer, noViews);
			a_builder.ShaderResource(*resources->scene->objects, noViews);
			a_builder.ShaderResource(*resources->scene->extras, noViews);
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
				const std::uint64_t poolAddress = device.GetBufferDeviceAddress({ a_recording.Resolve(a_bindings.pool).GetHandle(), 0 });
				auto drawCall = [&](const PassFrame::ZCall& a_call, bool a_offsetDepth) {
					RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesAddress, phaseBase + a_call.first, a_call.pipeline, counts, a_call.bucket,
						a_call.capacity, a_offsetDepth, a_prepared.phaseTwo ? 2u : 1u, poolAddress);
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

	/*
	 * The forward epoch's passes (drawcall-limit-fix.md). The colour BuildDraws appends visible forward draws (kObjectForward) with
	 * their view depth to the staging half; ForwardSortPass (colour epoch) sorts them back to front into the sorted half;
	 * ForwardDrawPass draws it in the forward epoch (the engine's blended draw, FUN_1414b39d0, after the deferred composite) on the
	 * colour pass's attachments; forward pipelines write the main target alone.
	 */
	namespace
	{
		struct ForwardSortBindings
		{
			org::DeclaredViewToken sequences, count;
		};

		struct ForwardSortFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			ForwardSortConstants constants{};
			std::uint32_t groups = 0;
		};

		class ForwardSortPass final : public org::TypedRenderGraphPass<ForwardSortPass, ForwardSortFrame, ForwardSortBindings>
		{
		public:
			ForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source) :
				sequences(std::move(a_sequences)), counts(std::move(a_counts)), source(std::move(a_source)) {}

			ForwardSortBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(*sequences).View(),
					counts.versioned ? a_builder.ShaderResource(*counts.versioned).View() : a_builder.ShaderResource(counts.buffer).View() };
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(source(a_preparation).generation); }

			ForwardSortFrame Prepare(const ForwardSortBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ForwardSortFrame prepared{};
				const auto range = source(a_preparation);
				if (!range.program || !range.capacity)
					return prepared;
				prepared.program = range.program;
				auto& constants = prepared.constants;
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
				constants.stagingBase = range.stagingBase;
				constants.capacity = range.capacity;
				constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
				constants.countOffset = range.countOffset;
				constants.options = range.countAtOffset ? ForwardSortConstants::kCountOffset : 0u;
				// Full capacity; threads past the count return.
				prepared.groups = (range.capacity + kForwardSortGroup - 1) / kForwardSortGroup;
				return prepared;
			}

			static void Record(const ForwardSortBindings&, const ForwardSortFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (a_frame.program && a_frame.groups)
					RecordDispatch(a_recording.Commands(), *a_frame.program, a_frame.constants, a_frame.groups);
			}

		private:
			Versioned sequences;
			LatchedTarget counts;
			std::function<ForwardSortSource(const org::PassPrepareContext&)> source;
		};
	}

	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source)
	{
		return std::make_shared<ForwardSortPass>(std::move(a_sequences), std::move(a_counts), std::move(a_source));
	}

	class ForwardDrawPass final : public org::TypedRenderGraphPass<ForwardDrawPass, PreparedDraws, PassBindings>
	{
	public:
		explicit ForwardDrawPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			// As MainOpaquePass: the forward pipelines are the main set's.
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.count = a_builder.IndirectArguments(resources->count);
			a_builder.ShaderResource(*resources->materialRows.buffer, noViews);
			a_builder.ShaderResource(*resources->pipelineRows.buffer, noViews);
			a_builder.ShaderResource(*resources->scene->objects, noViews);
			a_builder.ShaderResource(*resources->scene->extras, noViews);
			a_builder.VertexBuffer(*resources->scene->facePositions);
			DeclareFrameBuffers(a_builder, *resources);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			// The forward epoch is prepared for the colour shape (Revision/Assemble.cpp).
			auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			if (!frame || !frame->forwardCapacity || !frame->indirect.valid || !resources->forwardSort || !resources->preprocessForward)
				return prepared;
			prepared.preprocess = resources->preprocessForward.get();
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
			if (!a_prepared.frame || !a_prepared.preprocess)
				return;
			const auto& frame = *a_prepared.frame;
			// Every attachment loaded: the draws blend onto the composited frame; G-buffer targets are masked off.
			auto colors = LoadedColours(a_recording, a_prepared.targetViews, a_bindings.targets, a_prepared.targetCount);
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { colors.data(), a_prepared.targetCount };
			begin.debugName = "DCLF forward";
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			// The sorted half, counted by the forward count word.
			const std::uint64_t arguments = (ForwardStagingSlot(frame) + frame.forwardCapacity) * sizeof(DrawSequence);
			const std::uint64_t countOffset = std::uint64_t(kCountForwardWord) * sizeof(std::uint32_t);
			RecordPreprocessedDraw(a_recording, *a_prepared.preprocess, begin, frame.resourceHeap, frame.samplerHeap, frame.indirect.layout, a_prepared.framePushWords,
				frame.indirect.signatures[kColorVariant], sequences, arguments, count, countOffset, frame.forwardCapacity);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	/**
	 * The refraction pass (drawcall-limit-fix.md): the refraction range's draws, drawn in the forward epoch into kREFRACTION_NORMALS
	 * with one plain indexed indirect draw (DCLF_REFRACTION), testing the main depth. The target is first cleared to FUN_1414b4860's
	 * colour (0.5, 0.5, 0, 0: no offset, no mask), as the engine does before its group 5; ExecuteForward then marks the normals
	 * begun so the engine's later draws add to DCLF's.
	 */
	struct RefractionBindings
	{
		org::DeclaredViewToken target, depth;
		org::ResourceBindingToken sequences, count, pool;
	};

	struct RefractionPrepared
	{
		std::shared_ptr<const PassFrame> frame;
		org::PreparedDescriptorReference targetView{}, depthView{};
		std::uint64_t frameConstants = 0;
	};

	class RefractionDrawPass final : public org::TypedRenderGraphPass<RefractionDrawPass, RefractionPrepared, RefractionBindings>
	{
	public:
		explicit RefractionDrawPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		RefractionBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			RefractionBindings bindings{};
			bindings.target = a_builder.RenderTarget(resources->refractionNormals).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			// The sequences are both arguments and read by address (DCLF_PULLED).
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			a_builder.ShaderResource(*resources->sequences, noViews);
			bindings.count = a_builder.IndirectArguments(resources->count);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			a_builder.ShaderResource(*resources->materialRows.buffer, noViews);
			a_builder.ShaderResource(*resources->pipelineRows.buffer, noViews);
			a_builder.ShaderResource(*resources->scene->objects, noViews);
			a_builder.ShaderResource(*resources->scene->extras, noViews);
			// Face positions, read by address.
			a_builder.ShaderResource(*resources->scene->facePositions, noViews);
			DeclareFrameBuffers(a_builder, *resources, false);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->indices) : 0);
		}

		RefractionPrepared Prepare(const RefractionBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			RefractionPrepared prepared{};
			// Prepared for the colour shape, as ForwardDrawPass.
			auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			if (!frame || !frame->refractionCapacity || !frame->indirect.valid || !frame->indirect.refraction.valid() || !frame->indirect.refractionDrawSignature.valid() ||
				frame->indirect.refractionFormat != resources->refractionFormat || !resources->pool)
				return prepared;
			prepared.frame = std::move(frame);
			prepared.targetView = a_preparation.Capture(a_bindings.target);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			prepared.frameConstants = resources->frameConstantsAddress;
			return prepared;
		}

		static void Record(const RefractionBindings& a_bindings, const RefractionPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame || !a_prepared.frameConstants)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			rhi::ColorAttachment colour{};
			colour.rtv = a_recording.Resolve(a_prepared.targetView);
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.rgba[0] = colour.clear.rgba[1] = 0.5f;  // FUN_1414b4860's clear colour (0x141ad285c, 0.5), as RENDER_NORMAL_CLEAR writes
			colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
			colour.resource = a_recording.Resolve(a_bindings.target.Resource()).GetHandle();
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { &colour, 1 };
			begin.debugName = "DCLF refraction normals";
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			// Blocks in the shadow views' layout (kShadowPush*): the colour epoch's frame record and slots (main camera), and the zeroed
			// shared light block as the zero block.
			const std::uint64_t constants = a_prepared.frameConstants;
			std::uint32_t pass[kShadowPushWords]{};
			split(pass + kShadowPushFrameRecord, constants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			split(pass + kShadowPushViewBlock, constants + FrameSlotOffset(false, 0));
			split(pass + kShadowPushPerFrame, constants + FrameSlotOffset(false, kPerFrameVertexRegister));
			split(pass + kShadowPushZeros, constants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
			split(pass + kShadowPushSharedData, constants + FrameSlotOffset(false, kSharedDataRegister));
			split(pass + kShadowPushFeatureData, constants + FrameSlotOffset(false, kFeatureDataRegister));
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			const std::uint64_t first = RefractionSlot(frame) * sizeof(DrawSequence);
			// Call words: the range's first sequence, and the frame's LodFadeFrame (after FrameLighting in the lighting slot) in the
			// material rows' words, which the refraction stage reads instead.
			std::uint32_t words[kDrawPushWords]{};
			split(words + kShadowDrawPushSequences, device.GetBufferDeviceAddress({ sequences, 0 }) + first);
			split(words + kShadowDrawPushMaterialRows, constants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
			commands.BeginPass(begin);
			commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commands.BindLayout(frame.indirect.refractionLayout);
			commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, pass);
			commands.BindPipeline(frame.indirect.refraction);
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			commands.ExecuteIndirect(frame.indirect.refractionDrawSignature, sequences, first + kSequenceDrawOffset, count,
				std::uint64_t(kCountRefractionWord) * sizeof(std::uint32_t), frame.refractionCapacity);
			commands.EndPass();
		}

	private:
		std::shared_ptr<Resources> resources;
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
		org::DeclaredViewToken inputs, inputsDepth, geometries, sequences, count, hzb, visibility, frustum;
		org::DeclaredViewToken sortCounts, sortStaging, sortRanks;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken bucketCounts, poolFirsts;  // the depth segment's buckets (Resources::zBucketCounts) and the pool's firsts
		org::DeclaredViewToken portalPrograms, rootPrograms;  // the main camera's first phase: its portal programs, each root's
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
			bindings.inputs = a_builder.ShaderResource(*resources->inputs).View();
			if (resources->inputsDepth)
				bindings.inputsDepth = a_builder.ShaderResource(*resources->inputsDepth).View();
			bindings.geometries = a_builder.ShaderResource(*resources->scene->geometries).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			if (resources->frustum)
				bindings.frustum = a_builder.UnorderedAccess(*resources->frustum).View();
			// Phase 1 tests a member against its root's portal program (kPortal*).
			if (resources->frustum && resources->scene->portalPrograms && resources->scene->fadeRootPrograms) {
				bindings.portalPrograms = a_builder.ShaderResource(*resources->scene->portalPrograms).View();
				bindings.rootPrograms = a_builder.ShaderResource(*resources->scene->fadeRootPrograms).View();
			}
			// Every phase reads the fade roots' static rows (phase 1's fade verdicts, LOD skin partitions). The states FadeStateCS
			// published last frame are undeclared: nothing this frame writes them.
			if (resources->scene->fadeRoots)
				bindings.fadeRoots = a_builder.ShaderResource(*resources->scene->fadeRoots).View();
			// Phase 1 sees last frame's HZB, phase 2 the one rebuilt this frame: same resource, ordered by their place around the build.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			// The depth phases write into the pipeline slots' buckets, indexed from the pool, for the Z-prepass's plain draws.
			if (segment == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				bindings.bucketCounts = a_builder.UnorderedAccess(*resources->zBucketCounts[fixedPhase == 2 ? 1 : 0]).View();
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			}
			// The colour segment's refraction draws are pooled too.
			if (segment == RenderGraphRuntime::Segment::MainOpaque && resources->pool)
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			if (Sorts()) {
				bindings.sortCounts = a_builder.UnorderedAccess(resources->sort->counts).View();
				bindings.sortStaging = a_builder.UnorderedAccess(*resources->sort->staging).View();
				bindings.sortRanks = a_builder.UnorderedAccess(*resources->sort->ranks).View();
			}
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(fixedPhase);
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->firsts) : 0);
		}

		BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			BuildDrawsFrame prepared{};
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			if (!frame || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			const std::uint32_t phase = Phase(now);
			// Phase 2 (retesting phase 1's rejects against the rebuilt HZB) belongs to the depth segment only.
			if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			// The depth phases read their own inputs (Resources::inputsDepth).
			const bool depthInputs = (phase == 1 || phase == 2) && resources->inputsDepth;
			constants.inputsIndex = CaptureViewIndex(a_preparation, depthInputs ? a_bindings.inputsDepth : a_bindings.inputs);
			constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			SetRows(constants, frame->materialRows, frame->pipelineRows);
			constants.phaseBits = (phase & 0xFu) << 4;
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			// Frustum stamps: only depth phase 1 tests every candidate's frustum.
			if (resources->frustum && phase == 1)
				constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			// A member whose root's portal program culls its bound is out of view.
			if (resources->frustum && phase == 1 && a_bindings.portalPrograms.layout && resources->scene->fadeRoots && resources->scene->fadeRootCount) {
				constants.portalProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.portalPrograms);
				constants.rootProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.rootPrograms);
			}
			if (resources->scene->fadeRoots && resources->scene->fadeRootCount) {
				constants.fadeRootsIndex = CaptureViewIndex(a_preparation, a_bindings.fadeRoots);
			}
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			if (now == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts);
				constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			}
			if (Sorts()) {
				constants.sortCountsIndex = CaptureViewIndex(a_preparation, a_bindings.sortCounts);
				constants.sortStagingIndex = CaptureViewIndex(a_preparation, a_bindings.sortStaging);
				constants.sortRanksIndex = CaptureViewIndex(a_preparation, a_bindings.sortRanks);
			}
			constants.phaseTwoBase = frame->sequenceDraws;
			constants.decalBase = 2 * frame->sequenceDraws;
			constants.decalStride = frame->sequenceDecals;
			// The forward staging half: colour segment only.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->forwardCapacity) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.forwardCapacity = frame->forwardCapacity;
			}
			// The refraction range follows the forward halves (forwardBase + 2 * forwardCapacity), so forwardBase is set even
			// without forward capacity.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->refractionCapacity && resources->pool) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.refractionCapacity = frame->refractionCapacity;
				constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			}
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
		// Only the colour segment sorts (by pipeline, for its device-generated draw); depth draws are bucketed.
		bool Sorts() const { return resources->sort && segment != RenderGraphRuntime::Segment::ZPrepass; }

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

	/** @brief The sort's scatter, after BuildDraws and the scan, over the draws' full range (threads past the count return). */
	class SortSequencesPass final : public org::TypedRenderGraphPass<SortSequencesPass, SortSequencesFrame, SortSequencesBindings>
	{
	public:
		SortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		SortSequencesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			SortSequencesBindings bindings{};
			bindings.staging = a_builder.ShaderResource(*resources->sort->staging).View();
			bindings.ranks = a_builder.ShaderResource(*resources->sort->ranks).View();
			bindings.offsets = a_builder.ShaderResource(resources->sort->offsets).View();
			bindings.count = a_builder.ShaderResource(resources->count).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		SortSequencesFrame Prepare(const SortSequencesBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			SortSequencesFrame prepared{};
			if (!BuildsDraws(a_preparation, *resources, segment))
				return prepared;
			prepared.program = resources->sort->scatter;
			auto& constants = prepared.constants;
			constants.stagingIndex = CaptureViewIndex(a_preparation, a_bindings.staging);
			constants.ranksIndex = CaptureViewIndex(a_preparation, a_bindings.ranks);
			constants.offsetsIndex = CaptureViewIndex(a_preparation, a_bindings.offsets);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
			const auto frame = CurrentFrame(a_preparation, segment);
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
		org::DeclaredViewToken trees, clocks, frame;
		std::array<org::DeclaredViewToken, 2> wind;
	};

	struct TreeWindFramePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeWindConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree wind (TreeWindCS.hlsl) on the compute queue in the Z-prepass epoch: advances each tree's clock and writes its
	 * TreeParams and WindTimers into this frame's wind buffer (by parity), which next frame's draws read. Nothing this frame
	 * reads it, so it overlaps the raster work; the host orders it against adjacent frames, and readers need not declare it.
	 */
	class TreeWindPass final : public org::TypedRenderGraphPass<TreeWindPass, TreeWindFramePrepared, TreeWindBindings>
	{
	public:
		explicit TreeWindPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeWindBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			TreeWindBindings bindings{};
			bindings.trees = a_builder.ShaderResource(*scene->trees).View();
			bindings.frame = a_builder.ShaderResource(scene->treeFrameBuffer).View();
			bindings.clocks = a_builder.UnorderedAccess(*scene->treeClocks).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.wind[h] = a_builder.UnorderedAccess(*scene->treeWindRows[h]).View();
			return bindings;
		}

		// The recording depends on the buffers' versions; count and frame inputs are in the uploaded frame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			for (const auto* buffer : { &scene->trees, &scene->treeClocks, &scene->treeWindRows[0], &scene->treeWindRows[1] })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
		}

		TreeWindFramePrepared Prepare(const TreeWindBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TreeWindFramePrepared prepared{};
			if (!scene->treeWind || !scene->treeFrameBuffer)
				return prepared;
			prepared.program = scene->treeWind;
			auto& constants = prepared.constants;
			constants.treesIndex = CaptureViewIndex(a_preparation, a_bindings.trees);
			constants.clocksIndex = CaptureViewIndex(a_preparation, a_bindings.clocks);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.windIndices[h] = CaptureViewIndex(a_preparation, a_bindings.wind[h]);
			// Full capacity (the nodeless entry, then the tree slots); the shader stops at the count.
			const std::uint64_t trees = std::min({ NamedUnitsOf(a_preparation, scene->trees), NamedUnitsOf(a_preparation, scene->treeClocks),
				NamedUnitsOf(a_preparation, scene->treeWindRows[0]), NamedUnitsOf(a_preparation, scene->treeWindRows[1]) });
			prepared.groups = static_cast<std::uint32_t>((trees + 1 + kTreeWindGroup - 1) / kTreeWindGroup);
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
	};

	struct FadeStateBindings
	{
		org::DeclaredViewToken roots, states, frame, log, programs, rootPrograms, animated, events, reported;
		std::array<org::DeclaredViewToken, 2> published;
	};

	struct FadeStatePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FadeStateConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/** @brief Fade roots' state (FadeStateCS.hlsl): each root's main-camera OnVisible, once a frame, before the depth culling. */
	class FadeStatePass final : public org::TypedRenderGraphPass<FadeStatePass, FadeStatePrepared, FadeStateBindings>
	{
	public:
		explicit FadeStatePass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			const auto& scene = *resources->scene;
			FadeStateBindings bindings{};
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(*scene.fadeStates).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.published[h] = a_builder.UnorderedAccess(*scene.fadeStatesOut[h]).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.programs = a_builder.ShaderResource(*scene.portalPrograms).View();
			bindings.rootPrograms = a_builder.ShaderResource(*scene.fadeRootPrograms).View();
			bindings.animated = a_builder.ShaderResource(*scene.fadeAnimated).View();
			bindings.log = a_builder.UnorderedAccess(scene.fadeLog).View();
			if (scene.fadeEvents) {
				bindings.events = a_builder.UnorderedAccess(*scene.fadeEvents).View();
				bindings.reported = a_builder.UnorderedAccess(*scene.fadeReported).View();
			}
			return bindings;
		}

		// The recording depends on the buffers' versions and the depth shape; counts are in the uploaded FadeFrame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			const auto& scene = *resources->scene;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1], &scene.portalPrograms, &scene.fadeRootPrograms,
					 &scene.fadeAnimated, &scene.fadeEvents, &scene.fadeReported })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
			a_out.push_back(frame ? frame->generation : 0);
		}

		FadeStatePrepared Prepare(const FadeStateBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			// The root slots every buffer the pass reads or writes by root holds (the revision's versions: one unit group).
			std::uint64_t roots = ~0ull;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1], &scene.fadeRootPrograms, &scene.fadeAnimated })
				roots = std::min(roots, NamedUnitsOf(a_preparation, *buffer));
			if (scene.fadeReported)
				roots = std::min(roots, NamedUnitsOf(a_preparation, scene.fadeReported));
			if (!scene.fadeState || !roots || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.fadeState;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.programsIndex = CaptureViewIndex(a_preparation, a_bindings.programs);
			constants.logIndex = CaptureViewIndex(a_preparation, a_bindings.log);
			constants.rootProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.rootPrograms);
			constants.animatedIndex = CaptureViewIndex(a_preparation, a_bindings.animated);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.outIndices[h] = CaptureViewIndex(a_preparation, a_bindings.published[h]);
			// Only with a staging set that holds the whole bound list.
			if (const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.fadeEvents));
				scene.fadeEvents && scene.fadeWriteBack && scene.fadeWriteBack->SetFor(events)) {
				constants.eventsIndex = CaptureViewIndex(a_preparation, a_bindings.events);
				constants.reportedIndex = CaptureViewIndex(a_preparation, a_bindings.reported);
				constants.eventCapacity = events;
			}
			constants.latchIndex = frame->latch->SrvIndex();
			// Full capacity; the shader stops at the count.
			prepared.groups = static_cast<std::uint32_t>((roots + kFadeStateGroup - 1) / kFadeStateGroup);
			return prepared;
		}

		static void Record(const FadeStateBindings&, const FadeStatePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot.
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

	struct EventReadbackBindings
	{
		org::ResourceBindingToken events;
	};
	struct EventReadbackPrepared
	{
		std::shared_ptr<HostEventRing> ring;              // holds the staging set
		const HostEventRing::StagingSet* set = nullptr;  // the bound list's (its version the preparation's revision names)
	};

	/**
	 * @brief Copies an event list (fade write-back, visibility events) whole into the frame slot's host buffer: a fixed copy, so
	 * the recording depends on the slot alone. The depth commit harvests it when the slot comes round again.
	 */
	class EventReadbackPass final : public org::TypedRenderGraphPass<EventReadbackPass, EventReadbackPrepared, EventReadbackBindings>
	{
	public:
		EventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring) :
			scene(std::move(a_scene)), events(std::move(a_events)), ring(std::move(a_ring)) {}

		EventReadbackBindings Declare(org::PassBuilder& a_builder) { return { a_builder.CopySource(*events) }; }

		void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(NamedSerial(a_preparation, events)); }

		EventReadbackPrepared Prepare(const EventReadbackBindings&, const org::PassPrepareContext& a_preparation) const
		{
			return { ring, ring->SetFor(static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, events))) };
		}

		static void Record(const EventReadbackBindings& a_bindings, const EventReadbackPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			const std::uint32_t frameSlot = a_recording.FrameSlot();
			if (!a_prepared.set || frameSlot >= a_prepared.set->buffers.size())
				return;
			// The header and the list's full capacity.
			a_recording.Commands().CopyBufferRegion(a_prepared.set->buffers[frameSlot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.events).GetHandle(), 0,
				a_prepared.ring->bytesFor(a_prepared.set->events));
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
		Versioned events;
		std::shared_ptr<HostEventRing> ring;
	};

	struct VisibilityEventsBindings
	{
		org::DeclaredViewToken roots, states, frame, frustum, rootReported, objectReported, events;
	};

	struct VisibilityEventsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		VisibilityEventsConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The main camera's visibility as change events (VisibilityEventsCS.hlsl) at the depth segment's end, from the roots'
	 * InView and the objects' frustum stamps. The normal path writes the stood-in roots' kAccumulated from them.
	 */
	class VisibilityEventsPass final : public org::TypedRenderGraphPass<VisibilityEventsPass, VisibilityEventsPrepared, VisibilityEventsBindings>
	{
	public:
		explicit VisibilityEventsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		VisibilityEventsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->scene;
			VisibilityEventsBindings bindings{};
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.ShaderResource(*scene.fadeStates).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.frustum = a_builder.ShaderResource(*resources->frustum).View();
			bindings.rootReported = a_builder.UnorderedAccess(*scene.visibilityRootReported).View();
			bindings.objectReported = a_builder.UnorderedAccess(*scene.visibilityObjectReported).View();
			bindings.events = a_builder.UnorderedAccess(*scene.visibilityEvents).View();
			return bindings;
		}

		// The buffers' versions and the depth shape; frame, counts and camera are in the uploaded fade frame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			const auto& main = *resources;
			const auto& scene = *main.scene;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &main.frustum, &scene.visibilityRootReported, &scene.visibilityObjectReported,
					 &scene.visibilityEvents })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
			a_out.push_back(frame ? frame->generation : 0);
		}

		VisibilityEventsPrepared Prepare(const VisibilityEventsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			VisibilityEventsPrepared prepared{};
			const auto& scene = *resources->scene;
			const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.visibilityEvents));
			// Only with a staging set that holds the whole list, so the copy misses nothing.
			if (!scene.visibilityEventsProgram || !scene.visibilityReadback || !events || !scene.visibilityReadback->SetFor(events))
				return prepared;
			prepared.program = scene.visibilityEventsProgram;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			constants.rootReportedIndex = CaptureViewIndex(a_preparation, a_bindings.rootReported);
			constants.objectReportedIndex = CaptureViewIndex(a_preparation, a_bindings.objectReported);
			constants.eventsIndex = CaptureViewIndex(a_preparation, a_bindings.events);
			constants.eventCapacity = events;
			// Every root slot, then every object slot while the segment builds (without depth phase 1 no stamp is this frame's): what the
			// revision's versions of the buffers read and written by slot hold.
			constants.rootSlots = static_cast<std::uint32_t>(std::min({ NamedUnitsOf(a_preparation, scene.fadeRoots), NamedUnitsOf(a_preparation, scene.fadeStates),
				NamedUnitsOf(a_preparation, scene.visibilityRootReported) }));
			constants.objectSlots = BuildsDraws(a_preparation, *resources, RenderGraphRuntime::Segment::ZPrepass) ?
			                            static_cast<std::uint32_t>(std::min(NamedUnitsOf(a_preparation, resources->frustum), NamedUnitsOf(a_preparation, scene.visibilityObjectReported))) :
			                            0u;
			prepared.groups = (constants.rootSlots + constants.objectSlots + kVisibilityEventsGroup - 1) / kVisibilityEventsGroup;
			return prepared;
		}

		static void Record(const VisibilityEventsBindings&, const VisibilityEventsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kVisibilityEventsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

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
			HzbConstants constants{};
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
		HzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		HzbBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			HzbBindings bindings{};
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			bindings.counter = a_builder.UnorderedAccess(resources->hzbCounter).View();
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
			HzbFrame prepared{};
			// Only in the Z-prepass segment, where the depth is the world's final depth.
			const auto now = segment;
			if (now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			if (!resources->hzb || !resources->hzbProgram || !resources->hzbCounter)
				return prepared;
			// The depth commit zeroed the group counter (hzbCounterZeroed).
			const auto frame = CurrentFrame(a_preparation, now);
			if (!frame)
				return prepared;
			const std::uint32_t renderWidth = frame->width ? frame->width : resources->width;
			const std::uint32_t renderHeight = frame->height ? frame->height : resources->height;
			prepared.program = resources->hzbProgram;
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
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			for (std::size_t i = 0; i < a_frame.dispatches.size(); ++i) {
				const auto& dispatch = a_frame.dispatches[i];
				if (i != 0) {
					// The previous dispatch's last level must be complete before this one reads it.
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
					commands.Barriers(batch);
				}
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&dispatch.constants));
				commands.Dispatch(dispatch.groupsX, dispatch.groupsY, 1);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

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
		// ORG builds a DSV per slice for arrays, so a shadow view attaches its own slice.
		a_desc.isArray = a_image.arrayLayers > 1;
		a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
		a_desc.initialLayout = rhi::ResourceLayout::Common;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
		if (result)
			result->SetName(a_name);
		return result;
	}

	/** @brief The revision's shadow (or, a_sky, occlusion) shape being prepared; null without one. */
	std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const org::PassPrepareContext& a_preparation, bool a_sky)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation))
			return a_sky ? shapes->occlusion : shapes->shadow;
		return nullptr;
	}

	struct ShadowBuildBindings
	{
		org::DeclaredViewToken inputs;
		std::vector<org::DeclaredViewToken> sequences, count, bucketCounts;  // per view slot
		org::DeclaredViewToken geometries, visibility, poolFirsts;
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
			bindings.inputs = a_builder.ShaderResource(*resources->inputs).View();
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.UnorderedAccess(*resources->sequences[s]).View());
				bindings.count.push_back(a_builder.UnorderedAccess(resources->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(*resources->bucketCounts[s]).View());
			}
			bindings.geometries = a_builder.ShaderResource(*resources->scene->geometries).View();
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
			const auto geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
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
				constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs);
				constants.geometriesIndex = geometriesIndex;
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences[view.slot]);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count[view.slot]);
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[view.slot]);
				constants.poolFirstsIndex = poolFirstsIndex;
				constants.materialRowsAddressLo = static_cast<std::uint32_t>(view.materialRows);
				constants.materialRowsAddressHi = static_cast<std::uint32_t>(view.materialRows >> 32);
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

	struct IndexPoolBindings
	{
		org::DeclaredViewToken copies, indices;
	};

	struct IndexPoolPrepared
	{
		std::shared_ptr<const IndexPool> pool;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t poolOffset = 0;  // ShadowLatchLayout::PoolOffset
		IndexPoolConstants constants{};
	};

	/** @brief Copies this frame's new index pool ranges (IndexPoolCS.hlsl) before draws read the pool; dispatched from the latch. */
	class IndexPoolPass final : public org::TypedRenderGraphPass<IndexPoolPass, IndexPoolPrepared, IndexPoolBindings>
	{
	public:
		// The latch holding the pool's dispatch, from the shape the epoch runs (shadow or depth: whichever commit gave out ranges).
		struct LatchOf
		{
			std::uint64_t generation = 0;
			std::shared_ptr<const org::LatchBlock> latch;
			std::uint32_t poolOffset = 0;
		};
		IndexPoolPass(std::shared_ptr<IndexPool> a_pool, std::function<LatchOf(const org::PassPrepareContext&)> a_latch) :
			pool(std::move(a_pool)), latchOf(std::move(a_latch)) {}

		IndexPoolBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			IndexPoolBindings bindings{};
			bindings.copies = a_builder.ShaderResource(*pool->copies).View();
			bindings.indices = a_builder.UnorderedAccess(*pool->indices).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto latch = latchOf(a_preparation);
			a_out.push_back(latch.generation);
			a_out.push_back(latch.poolOffset);
			a_out.push_back(NamedSerial(a_preparation, pool->copies));
			a_out.push_back(NamedSerial(a_preparation, pool->indices));
		}

		IndexPoolPrepared Prepare(const IndexPoolBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			IndexPoolPrepared prepared{};
			const auto latch = latchOf(a_preparation);
			if (!latch.latch)
				return prepared;
			prepared.pool = pool;
			prepared.latch = latch.latch;
			prepared.poolOffset = latch.poolOffset;
			prepared.constants.latchIndex = latch.latch->SrvIndex();
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
		std::shared_ptr<IndexPool> pool;
		std::function<LatchOf(const org::PassPrepareContext&)> latchOf;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> sequences, bucketCounts;  // per view slot
		org::ResourceBindingToken pool;  // the index pool (IndexPool)
		org::ResourceBindingToken materialRows, constants, viewBlocks, objects, extras;
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
			a_builder.ShaderResource(*resources->materialRows.buffer, noViews);
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(*resources->viewBlocks.buffer, noViews).Resource();
			a_builder.ShaderResource(*resources->scene->objects, noViews);
			a_builder.ShaderResource(*resources->scene->extras, noViews);
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

	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene)
	{
		Register(a_graph, org::ResourceIdentifier("cs.dclf.objects"), a_scene.objects);
		if (a_scene.pool) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.index-pool"), a_scene.pool->indices);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-firsts"), a_scene.pool->firsts);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-copies"), a_scene.pool->copies);
		}
		Register(a_graph, org::ResourceIdentifier("cs.dclf.extras"), a_scene.extras);
		Register(a_graph, org::ResourceIdentifier("cs.dclf.geometries"), a_scene.geometries);
		Register(a_graph, org::ResourceIdentifier("cs.dclf.face-positions"), a_scene.facePositions);
		if (a_scene.trees) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.trees"), a_scene.trees);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-clocks"), a_scene.treeClocks);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-wind0"), a_scene.treeWindRows[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-wind1"), a_scene.treeWindRows[1]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-frame"), a_scene.treeFrameBuffer);
		}
		if (a_scene.fadeRoots) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-roots"), a_scene.fadeRoots);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states"), a_scene.fadeStates);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states-out0"), a_scene.fadeStatesOut[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states-out1"), a_scene.fadeStatesOut[1]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-frame"), a_scene.fadeFrameBuffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.portal-programs"), a_scene.portalPrograms);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-root-programs"), a_scene.fadeRootPrograms);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-log"), a_scene.fadeLog);
			if (a_scene.fadeEvents) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-events"), a_scene.fadeEvents);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-reported"), a_scene.fadeReported);
			}
			if (a_scene.visibilityEvents) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-events"), a_scene.visibilityEvents);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-root-reported"), a_scene.visibilityRootReported);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-object-reported"), a_scene.visibilityObjectReported);
			}
		}
		if (a_scene.treeLodCull) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-shapes"), a_scene.treeLodShapes);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-instances"), a_scene.treeLodInstances);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-meshes"), a_scene.treeLodMeshes);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-draw"), a_scene.treeLodDraw);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-visible"), a_scene.treeLodVisible);
		}
	}

	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.material-rows"), resources->materialRows.buffer);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.draw-inputs"), resources->inputs);
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

	namespace
	{
		struct LatchedCopiesBindings
		{
			std::vector<std::pair<const void*, org::ResourceBindingToken>> targets;  // LatchedTarget::Key
		};

		class LatchedCopiesPass final : public org::TypedRenderGraphPass<LatchedCopiesPass, LatchedSource, LatchedCopiesBindings>
		{
		public:
			LatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets, std::function<LatchedSource(const org::PassPrepareContext&)> a_source,
				std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared) :
				targets(std::move(a_targets)), source(std::move(a_source)), declared(a_declared) {}

			LatchedCopiesBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				LatchedCopiesBindings bindings{};
				auto keys = std::make_shared<std::vector<const void*>>();
				for (const auto& target : targets()) {
					bindings.targets.emplace_back(target.Key(), target.versioned ? a_builder.CopyDestination(*target.versioned).Resource() : a_builder.CopyDestination(target.buffer));
					keys->push_back(target.Key());
				}
				if (declared)
					declared->store(std::move(keys), std::memory_order_release);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(source(a_preparation).generation); }

			LatchedSource Prepare(const LatchedCopiesBindings&, const org::PassPrepareContext& a_preparation) const { return source(a_preparation); }

			static void Record(const LatchedCopiesBindings& a_bindings, const LatchedSource& a_prepared, org::PassRecordContext& a_recording)
			{
				auto& commands = a_recording.Commands();
				for (const auto& list : a_prepared.lists) {
					if (!list.latch)
						continue;
					const auto latch = list.latch->Resource()->GetAPIResource().GetHandle();
					const std::uint64_t region = list.fixed ? 0 : list.latch->Offset(a_recording.FrameSlot());
					for (const auto& copy : list.copies) {
						const auto target = std::find_if(a_bindings.targets.begin(), a_bindings.targets.end(), [&](const auto& a_target) { return a_target.first == copy.target; });
						if (target != a_bindings.targets.end())
							commands.CopyBufferRegion(a_recording.Resolve(target->second).GetHandle(), copy.dstOffset, latch, region + copy.latchOffset, copy.bytes);
					}
				}
			}

		private:
			std::function<std::vector<LatchedTarget>()> targets;
			std::function<LatchedSource(const org::PassPrepareContext&)> source;
			std::atomic<std::shared_ptr<const std::vector<const void*>>>* declared = nullptr;
		};
	}

	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared)
	{
		return std::make_shared<LatchedCopiesPass>(std::move(a_targets), std::move(a_source), a_declared);
	}

	/** @brief The buffers a main commit may latch into (LatchedUploads); its latched copies declare them. */
	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources)
	{
		std::vector<LatchedTarget> out;
		for (const auto& buffer : { a_resources.frameConstants, a_resources.count })
			if (buffer)
				out.push_back({ buffer, nullptr });
		for (const auto& counts : a_resources.zBucketCounts)
			if (counts)
				out.push_back({ nullptr, counts });
		for (const auto& frameBuffer : a_resources.frameBuffers)
			if (frameBuffer.copy)
				out.push_back({ frameBuffer.copy, nullptr });
		// Tree LOD's draw row and list header; not the trees' or fades' frame rows, which the compute queue reads.
		if (const auto& scene = *a_resources.scene; scene.treeLodCull) {
			out.push_back({ scene.treeLodDraw, nullptr });
			out.push_back({ nullptr, scene.treeLodVisible });
		}
		return out;
	}

	void DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, bool a_lights)
	{
		const std::span<const org::SrvView> noViews{};
		for (const auto& frameBuffer : a_resources.frameBuffers)
			a_builder.ShaderResource(frameBuffer.copy, noViews);
		if (a_lights && a_resources.lightLimitFix)
			for (const char* id : { "cs.llf.lights", "cs.llf.light-index-list", "cs.llf.light-grid" })
				a_builder.ShaderResource(org::ResourceIdentifier(id), noViews);
	}

	void RecordPreprocessedDraw(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::CommandSignatureHandle a_signature,
		rhi::ResourceHandle a_arguments, std::uint64_t a_argumentsOffset, rhi::ResourceHandle a_counts, std::uint64_t a_countOffset, std::uint32_t a_capacity)
	{
		auto& commands = a_recording.Commands();
		auto beginDrawPass = [&](rhi::CommandList& a_list) {
			a_list.SetDescriptorHeaps(a_resourceHeap, a_samplerHeap);
			a_list.BeginPass(a_begin);
			a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			a_list.BindLayout(a_layout);
			a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_push.data());
		};
		// Preprocess before the pass, with the execution's exact state (as MainOpaquePass).
		auto& state = a_preprocess.Begin(a_recording.FrameSlot());
		beginDrawPass(state);
		commands.PreprocessIndirect(state, a_signature, a_arguments, a_argumentsOffset, a_counts, a_countOffset, a_capacity);
		state.EndPass();
		state.End();
		beginDrawPass(commands);
		commands.ExecuteIndirect(a_signature, a_arguments, a_argumentsOffset, a_counts, a_countOffset, a_capacity);
		commands.EndPass();
	}

	class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.material-rows"), resources->materialRows.buffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pipeline-rows"), resources->pipelineRows.buffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-inputs"), resources->inputs);
			if (resources->inputsDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-inputs-depth"), resources->inputsDepth);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
			// Read by address; readers declare it so the graph orders them after the latched copies.
			Register(a_graph, org::ResourceIdentifier("cs.dclf.frame-constants"), resources->frameConstants);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts"), resources->zBucketCounts[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts2"), resources->zBucketCounts[1]);
			if (const auto& foliage = resources->foliage) {
				for (std::uint32_t h = 0; h < 2; ++h) {
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-ids{}", h)), foliage->ids[h]);
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-colours{}", h)), foliage->colours[h]);
				}
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-results"), foliage->results);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-owners"), foliage->owners);
			}
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
			if (resources->frustum)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.frustum"), resources->frustum);
			for (const auto& frameBuffer : resources->frameBuffers)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
			if (resources->nativeDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			if (resources->terrainDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.terrain-blending-depth"), resources->terrainDepth);
			if (resources->refractionNormals)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.refraction-normals"), resources->refractionNormals);
			if (resources->hzb) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb-counter"), resources->hzbCounter);
			}
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
			// Each epoch's first pass copies the commit's latched values. It publishes its declared targets (Resources::latchedTargets),
			// so a commit latches only what the pass will copy.
			for (const auto segment : { depthSegment, colourSegment })
				AddPass(a_out, PassKind::Copy, segment == depthSegment ? "cs.dclf.z.latched-copies" : "cs.dclf.latched-copies",
					MakeLatchedCopiesPass([resources = resources] { return MainLatchedTargets(*resources); },
						[segment](const org::PassPrepareContext& a_preparation) {
							const auto frame = CurrentFrame(a_preparation, segment);
							return frame ? LatchedSource{ frame->generation, frame, std::span(&frame->latched, 1) } : LatchedSource{};
						},
						&resources->latchedTargets),
					RenderGraphRuntime::EpochOf(segment));
			// A pass's segment is fixed, so each segment has its own instances.
			// The sort by pipeline (DrawSort): the scan runs in every execution (so the counts it clears are zero even without a
			// build), the scatter only after a build.
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
			// Tree wind for the next frame, beside this epoch's raster work.
			if (resources->scene->treeWind && resources->scene->trees)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.tree-wind", std::make_shared<TreeWindPass>(resources->scene), depth, PassQueue::Compute);
			// Fade roots' state for the next frame's culling, likewise.
			if (resources->scene->fadeState && resources->scene->fadeRoots)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.fade-state", std::make_shared<FadeStatePass>(resources), depth, PassQueue::Compute);
			// Its write-back events, to the host.
			if (resources->scene->fadeState && resources->scene->fadeRoots && resources->scene->fadeEvents && resources->scene->fadeWriteBack)
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.fade-events", std::make_shared<EventReadbackPass>(resources->scene, resources->scene->fadeEvents, resources->scene->fadeWriteBack), depth, PassQueue::None);
			AddPass(a_out, PassKind::Compute, "cs.dclf.z.build-draws", std::make_shared<BuildDrawsPass>(resources, depthSegment), depth);
			// The pool copies the depth commit gave out (when the shadow epoch has not), before the draws read them.
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [main = resources](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
					return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, frame ? frame->latchLayout.PoolOffset() : 0u };
				})))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			// Tree LOD's phase 1 list, before the depth draw.
			if (resources->scene->treeLodCull)
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.tree-lod-cull", std::make_shared<TreeLodCullPass>(resources, 1), depth);
			AddPass(a_out, PassKind::Render, "cs.dclf.z.depth", std::make_shared<MainOpaquePass>(resources, depthSegment), depth);
			AddPass(a_out, PassKind::Compute, "cs.dclf.build-draws", std::make_shared<BuildDrawsPass>(resources, colourSegment), colour);
			addSort("cs.dclf.sort-scan", "cs.dclf.sort-scatter", colourSegment, colour);
			// The forward sort after the colour build; its draw in the forward epoch (IndirectDraws::ExecuteForward).
			if (resources->forwardSort) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.forward-sort",
					MakeForwardSortPass(resources->sequences, { resources->count, nullptr }, [main = resources](const org::PassPrepareContext& a_preparation) {
						const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
						if (!frame || !BuildsDraws(a_preparation, *main, RenderGraphRuntime::Segment::MainOpaque))
							return ForwardSortSource{ frame ? frame->generation : 0 };
						return ForwardSortSource{ frame->generation, main->forwardSort, static_cast<std::uint32_t>(ForwardStagingSlot(*frame)), frame->forwardCapacity };
					}),
					colour);
				AddPass(a_out, PassKind::Render, "cs.dclf.forward", std::make_shared<ForwardDrawPass>(resources), RenderGraphRuntime::EpochOf(Segment::Forward));
			}
			// Refraction normals, in the forward epoch after the blended draws.
			if (resources->refractionNormals && resources->nativeDepth && resources->pool)
				AddPass(a_out, PassKind::Render, "cs.dclf.refraction", std::make_shared<RefractionDrawPass>(resources), RenderGraphRuntime::EpochOf(Segment::Forward));
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kBeforeColour, resources, colour, a_out });
			AddPass(a_out, PassKind::Render, "cs.dclf.main-opaque", std::make_shared<MainOpaquePass>(resources, colourSegment), colour);
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kAfterColour, resources, colour, a_out });
			// The two-phase tail, in the depth segment, in this order: build-draws (phase 1, last frame's HZB), depth draw, hzb
			// (rebuilt from it), build-draws-phase2 and tree-lod-cull-phase2 (phase 1's rejects retested), depth-phase2 (the
			// rescues). Phase 1's frame-old HZB misses what just came out from behind an occluder; phase 2 takes those back.
			if (resources->hzb) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.hzb", std::make_shared<HzbPass>(resources, depthSegment), depth);
				AddPass(a_out, PassKind::Compute, "cs.dclf.build-draws-phase2", std::make_shared<BuildDrawsPass>(resources, depthSegment, 2), depth);
				if (resources->scene->treeLodCull)
					AddPass(a_out, PassKind::Compute, "cs.dclf.tree-lod-cull-phase2", std::make_shared<TreeLodCullPass>(resources, 2), depth);
				AddPass(a_out, PassKind::Render, "cs.dclf.depth-phase2", std::make_shared<MainOpaquePass>(resources, depthSegment, true), depth);
			}
			if (resources->scene->treeLodCull && resources->scene->treeLodCounts)
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.tree-lod-counts", std::make_shared<TreeLodReadbackPass>(resources->scene), depth, PassQueue::None);
			// Visibility changes once both phases' verdicts are final, and their readback copy.
			const auto& sceneBuffers = *resources->scene;
			if (sceneBuffers.visibilityEventsProgram && sceneBuffers.visibilityReadback && sceneBuffers.fadeState && sceneBuffers.fadeRoots && resources->frustum) {
				AddPass(a_out, PassKind::Compute, "cs.dclf.z.visibility-events", std::make_shared<VisibilityEventsPass>(resources), depth);
				AddPass(a_out, PassKind::Copy, "cs.dclf.z.visibility-readback", std::make_shared<EventReadbackPass>(resources->scene, resources->scene->visibilityEvents, resources->scene->visibilityReadback), depth, PassQueue::None);
			}
			Diagnostics::Notify(Diagnostics::MainGraphPasses{ Diagnostics::MainGraphPasses::kAfterDepth, resources, depth, a_out });
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
