#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	namespace Draws
	{
		void PlanZBuckets(const MainSizing& a_resources, const Lookups& a_lookups, const SceneStore::Tables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_resources.zBucketCapacity.size());
			const auto& groups = a_indirect.zGroups;
			const auto& keys = a_tables.pipelines;
			a_out.map.assign(slots, kNoBucket);
			a_out.calls.clear();
			// Per group, its bucket.
			std::vector<std::uint32_t> groupBucket(a_indirect.zPipelines.size(), kNoBucket);
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (!a_resources.zBucketCapacity[p] || p >= a_lookups.pipelines.size() || p >= keys.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == keys[p]) || entry.setIndex >= groups.size())
					continue;
				const std::uint32_t group = groups[entry.setIndex];
				auto& bucket = groupBucket[group];
				if (bucket == kNoBucket) {
					bucket = a_out.Buckets();
					a_out.calls.push_back({ bucket, 0u, 0u, a_indirect.zPipelines[group] });
				}
				a_out.calls[bucket].capacity += a_resources.zBucketCapacity[p];
				a_out.map[p] = bucket;
			}
			const std::uint32_t buckets = a_out.Buckets();
			a_out.table.assign(std::size_t(buckets) * 4, 0u);
			std::uint32_t first = 0;
			for (std::uint32_t b = 0; b < buckets; ++b) {
				auto& call = a_out.calls[b];
				call.first = first;
				a_out.table[2 * b] = first;
				a_out.table[2 * b + 1] = call.capacity;
				// Phase 2's ranges are the same past the sequences' first draw range.
				a_out.table[2 * (buckets + b)] = first + a_resources.sequenceDraws;
				a_out.table[2 * (buckets + b) + 1] = call.capacity;
				first += call.capacity;
			}
		}

		std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets)
		{
			std::vector<LatchedCopy> out;
			const auto targets = a_resources.latchedTargets.load(std::memory_order_acquire);
			std::size_t used = 0;
			// As LatchedUploads: a target the passes did not declare is staged, not latched.
			auto add = [&](const void* a_key, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_key || !a_bytes || !targets || std::find(targets->begin(), targets->end(), a_key) == targets->end())
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			// In CommitMainPayload's order.
			const void* constants = a_resources.frameConstants.get();
			if (!a_depthOnly)
				for (const auto& frameBuffer : a_resources.frameBuffers)
					if (frameBuffer.copy)
						add(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(constants, sizeof(DrawBindings), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				add(constants, a_blocks.vs[slot], FrameSlotOffset(false, slot));
				add(constants, a_blocks.ps[slot], FrameSlotOffset(true, slot));
			}
			add(constants, sizeof(SceneStore::FrameCapture::lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			add(constants, sizeof(LodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
			add(constants, sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
			add(constants, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
			add(constants, sizeof(ExtrasFrame::projectedGlobals), kExtrasPixelFrameOffset);
			if (!a_depthOnly && a_resources.foliage)
				add(constants, 8 * sizeof(std::uint32_t), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame));
			if (const auto& scene = *a_resources.scene; a_depthOnly && scene.treeLodCull && scene.treeLodShapes) {
				add(scene.treeLodDraw.get(), sizeof(TreeLod::DrawRow), 0);
				add(scene.treeLodVisible->Key(), sizeof(TreeLod::VisibleHeader), 0);
			}
			add(a_resources.count.get(), a_depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t), 0);
			if (!a_depthOnly) {
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
			}
			if (a_depthOnly && a_resources.pool && a_buckets)
				for (const auto& counts : a_resources.zBucketCounts)
					if (counts)
						add(counts->Key(), std::size_t(a_buckets) * sizeof(std::uint32_t), 0);
			return out;
		}

		std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies)
		{
			return a_copies.empty() ? 0 : std::size_t(a_copies.back().latchOffset) + a_copies.back().bytes;
		}

		void ReserveLatchedBlock(std::shared_ptr<org::LatchBlock>& a_block, std::size_t a_bytes, std::uint32_t a_slots)
		{
			if (!a_bytes || (a_block && a_block->Stride() >= a_bytes))
				return;
			// Frames in flight keep the old one (LatchedList::latch).
			a_block = std::make_shared<org::LatchBlock>("cs.dclf.latched-copies", static_cast<std::uint32_t>(std::bit_ceil(std::max<std::size_t>(2 * a_bytes, 4096))), a_slots);
		}

		// a_revision: a revision's (the rows' addresses it names: a ready growth's), else the commit's (the current versions').
		MainShapeInputs MainShapeInputsOf(const Resources& a_resources, bool a_depthOnly, bool a_revision)
		{
			MainShapeInputs in;
			in.depthOnly = a_depthOnly;
			const MainSizing& sizing = a_revision ? Growths::Get().RevisionSizing<MainSizing>(a_resources) : a_resources;
			in.sequenceDraws = sizing.sequenceDraws;
			in.sequenceDecals = sizing.sequenceDecals;
			in.materialRows = a_revision ? a_resources.materialRows.RevisionAddress() : a_resources.materialRows.address;
			in.pipelineRows = a_revision ? a_resources.pipelineRows.RevisionAddress() : a_resources.pipelineRows.address;
			in.cullMode = ActiveToggles().cullMode;
			in.latch = a_resources.latch;
			in.latchLayout = a_resources.latchLayout;
			return in;
		}

		std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in)
		{
			auto frame = std::make_shared<PassFrame>();
			frame->sequenceDraws = a_in.sequenceDraws;
			frame->sequenceDecals = a_in.sequenceDecals;
			frame->materialRows = a_in.materialRows;
			frame->pipelineRows = a_in.pipelineRows;
			// The max counts: the sequence buffer's ranges, which hold every draw the scene can produce (ReserveMainSequences). Not the
			// counts of the tables the shape was made from: a publication a revision covers may hold newer tables (a decal group's
			// first decal, more draws), and its commit would find the shape short. They change with the buffer's growth alone, which
			// a revision adopts. The depth segment is given no decals (BuildDrawsCS).
			frame->drawCapacity = a_in.sequenceDraws;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				frame->decalCapacity[group] = a_in.depthOnly ? 0u : a_in.sequenceDecals;
			frame->width = a_in.viewport.width;
			frame->height = a_in.viewport.height;
			frame->minDepth = a_in.viewport.minDepth;
			frame->maxDepth = a_in.viewport.maxDepth;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->cullMode = a_in.cullMode;
			if (const auto pixel = SwitchValue(Switch::GBufferProbe); !pixel.empty()) {
				if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
					frame->probeX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
					frame->probeY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
					frame->probePixel = frame->probeX < frame->width && frame->probeY < frame->height;
				}
			}
			frame->latch = a_in.latch;
			frame->latchLayout = a_in.latchLayout;
			frame->zCalls = a_in.zCalls;
			frame->zPlan = a_in.zPlan;
			frame->latched = a_in.latched;
			return frame;
		}

		std::uint32_t MainShapeDifferences(const PassFrame& a, const PassFrame& b)
		{
			std::uint32_t out = 0;
			auto note = [&](MainShapeField a_field, bool a_differs) {
				if (a_differs)
					out |= 1u << a_field;
			};
			note(kShapeCapacity, a.drawCapacity != b.drawCapacity || a.decalCapacity != b.decalCapacity);
			note(kShapeSequences, a.sequenceDraws != b.sequenceDraws || a.sequenceDecals != b.sequenceDecals);
			note(kShapeRows, a.materialRows != b.materialRows || a.pipelineRows != b.pipelineRows);
			note(kShapeViewport, a.width != b.width || a.height != b.height || a.minDepth != b.minDepth || a.maxDepth != b.maxDepth);
			note(kShapeHeaps, !SameHandle(a.resourceHeap, b.resourceHeap) || !SameHandle(a.samplerHeap, b.samplerHeap));
			note(kShapePipelines, !SameIndirect(a.indirect, b.indirect));
			note(kShapeCull, a.cullMode != b.cullMode || a.probePixel != b.probePixel || a.probeX != b.probeX || a.probeY != b.probeY);
			note(kShapeLatch, a.latch != b.latch);
			note(kShapeZCalls, a.zCalls != b.zCalls);
			note(kShapeLatchedCopies, a.latched.copies != b.latched.copies);
			note(kShapeLatchedBlock, a.latched.latch != b.latched.latch);
			// Whatever SameShape compares that these do not name.
			if (!out && !a.SameShape(b))
				out |= 1u << kShapePipelines;
			return out;
		}

		void PlanReflectionBuckets(const MainSizing& a_main, std::span<const rhi::PipelineHandle> a_slotPipelines, ReflectionPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_main.zBucketCapacity.size());
			a_out.map.assign(slots, kNoBucket);
			a_out.buckets.clear();
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (p >= a_slotPipelines.size() || !a_slotPipelines[p].valid() || !a_main.zBucketCapacity[p])
					continue;
				const auto pipeline = a_slotPipelines[p];
				auto it = std::find_if(a_out.buckets.begin(), a_out.buckets.end(), [&](const auto& a_bucket) { return SameHandle(a_bucket.pipeline, pipeline); });
				if (it == a_out.buckets.end())
					it = a_out.buckets.insert(a_out.buckets.end(), ReflectionFrame::Bucket{ pipeline, 0, 0 });
				it->capacity += a_main.zBucketCapacity[p];
				a_out.map[p] = static_cast<std::uint32_t>(it - a_out.buckets.begin());
			}
			a_out.draws = 0;
			for (auto& bucket : a_out.buckets) {
				bucket.first = a_out.draws;
				a_out.draws += bucket.capacity;
			}
		}

		std::shared_ptr<ReflectionFrame> MakeReflectionShape(const ReflectionResources& a_resources, const ReflectionShapeInputs& a_in)
		{
			const auto& main = *a_resources.main;
			auto frame = std::make_shared<ReflectionFrame>();
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->latch = a_resources.latch;
			frame->facesOffset = a_resources.latchLayout.FaceOffset(0);
			frame->zeros = a_resources.zeros;
			frame->width = a_resources.width;
			frame->height = a_resources.height;
			frame->materialRows = a_in.materialRows;
			frame->pipelineRows = a_in.pipelineRows;
			frame->sequenceDraws = (a_in.sizing ? *a_in.sizing : static_cast<const ReflectionSizing&>(a_resources)).sequenceDraws;
			frame->buckets = a_in.buckets;
			frame->latchLayout = a_resources.latchLayout;
			frame->map = a_in.map;
			// The colour segment's frame constants, VS and PS b12 the face's (its camera): the frame lighting (PS b13) is the main
			// pass's, its sun a frame old (dclf-lod.md, "The constants").
			const auto base = FramePushWords(main.frameConstantsAddress);
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				auto& push = frame->push[f];
				push = base;
				const std::uint64_t block = a_resources.faceBlocksAddress + std::uint64_t(f) * kReflectionFaceBlockBytes;
				for (const bool pixel : { false, true })
					if (const std::uint32_t word = FramePushWord(pixel, kPerFrameVertexRegister); word != ~0u) {
						push[word] = static_cast<std::uint32_t>(block);
						push[word + 1] = static_cast<std::uint32_t>(block >> 32);
					}
			}
			if (a_in.tree.valid()) {
				frame->tree = a_in.tree;
				frame->treeSignature = a_in.treeSignature;
				frame->treeGroups = static_cast<std::uint32_t>((std::uint64_t(a_in.treeShapes) * TreeLod::kMaxGroupInstances + kTreeLodCullGroup - 1) / kTreeLodCullGroup);
			}
			return frame;
		}
	}

	void IndirectDraws::MakeRevisionShapes()
	{
		ZoneScopedN("CS.DCLF.MakeRevisionShapes");
		auto* host = RenderGraphRuntime::Get().Host();
		if (failed || !impl->resources || !impl->resources->scene || !host)
			return;
		auto& store = SceneStore::Get();
		const auto& tables = store.GetSceneTables();
		auto& r = *impl->resources;
		// The capacities for the tables as the scene work left them: a growth here is one the frame's epochs would make.
		impl->ReserveSceneTables(tables);
		impl->ReserveMainSequences(tables);
		impl->ReserveIndexPool(tables, store.GetTablesGeneration());
		// The colour commit's sun cascades and local shadow light volumes, a shadow view each (the last Rebuild's candidates): the latch
		// a revision names holds them from its join on, not from a colour commit's growth (which a frame without claims never runs).
		{
			const std::uint32_t views = ShadowViews::Get().Candidates();
			impl->ReserveMainLatch(r, std::max(views, r.latchLayout.cascades), std::max(views, r.latchLayout.shadowVolumes), r.latchLayout.buckets);
		}
		// The growths the graph finished (G2): what the revision made now names, its shapes' addresses included. One still pending
		// keeps the revision from being sealed (AssembleRevision).
		impl->revisions.growthPending = Growths::Get().Settle();
		const auto indirect = GetIndirectState();
		// The heaps every recording binds: the graph's own, as built.
		decltype(PassFrame::resourceHeap) resourceHeap{}, samplerHeap{};
		if (auto* descriptors = host->Descriptors()) {
			resourceHeap = descriptors->GetSRVDescriptorHeap().GetHandle();
			samplerHeap = descriptors->GetSamplerDescriptorHeap().GetHandle();
		}
		auto& parity = impl->shapeParity;
		const std::uint32_t frameNumber = store.GetFrame();
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			auto& revisions = parity.revisions[shape];
			auto& frames = parity.revisionFrames[shape];
			revisions[1] = std::move(revisions[0]);
			frames[1] = frames[0];
			revisions[0] = nullptr;
			frames[0] = ~0u;
			// A segment no commit has captured for yet: no viewport, no frame blocks.
			if (!parity.known[shape] || !indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			auto in = MainShapeInputsOf(r, depthOnly, true);
			in.viewport = parity.viewport[shape];
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			ZBucketPlan plan;
			if (depthOnly && r.pool)
				PlanZBuckets(Growths::Get().RevisionSizing<MainSizing>(r), store.GetLookups(), tables, indirect, plan);
			in.zCalls = plan.calls;
			in.zPlan = std::make_shared<const ZBucketPlan>(std::move(plan));
			in.latched.copies = MainLatchedLayout(r, depthOnly, parity.blockSizes[shape], static_cast<std::uint32_t>(in.zCalls.size()));
			ReserveLatchedBlock(r.latchedBlocks[shape], LatchedBytes(in.latched.copies), host->FrameSlots());
			if (!in.latched.copies.empty())
				in.latched.latch = r.latchedBlocks[shape];
			revisions[0] = MakeMainShape(in);
			frames[0] = frameNumber;
		}
		// The shadow and occlusion epochs': a shape for each view layout they drew recently, from the last shadow build's payload
		// (a revision's own, once the payload is a revision's).
		auto& sp = impl->shadowParity;
		for (std::size_t kind = 0; kind < 2; ++kind) {
			auto& revisions = sp.revisions[kind];
			auto& frames = sp.revisionFrames[kind];
			revisions[1] = std::move(revisions[0]);
			frames[1] = frames[0];
			revisions[0].clear();
			frames[0] = ~0u;
		}
		const auto shadowIndirect = GetShadowIndirectState();
		// The occlusion maps' layout as last captured: one the revisions make a shape for (OcclusionReady asks for it).
		if (RevisionClaims()) {
			auto layouts = impl->PredictedOcclusion();
			auto& recent = impl->recentOcclusionLayouts;
			if (!layouts.empty()) {
				std::erase(recent, layouts);
				recent.insert(recent.begin(), std::move(layouts));
				if (recent.size() > RecentShapes<ShadowFrame>::kShapes)
					recent.resize(RecentShapes<ShadowFrame>::kShapes);
			}
		}
		if (auto shadow = impl->shadow; (sp.known || !impl->shadowPlacements.empty() || !impl->recentOcclusionLayouts.empty()) && shadow && shadowIndirect.valid) {
			const auto& payload = impl->CommittedShadow();
			// The slots the layouts name hold every draw the scene can produce, as the epochs reserve them.
			std::uint32_t slots = 0;
			for (const auto* recent : { &shadow->recentShapes, &shadow->recentOcclusionShapes })
				for (const auto& shape : recent->shapes)
					if (shape)
						for (const auto& view : shape->views)
							slots = std::max(slots, view.slot + 1);
			for (const auto& view : impl->shadowPlacements)
				slots = std::max(slots, view.slot + 1);
			for (const auto& layouts : impl->recentOcclusionLayouts)
				for (const auto& view : layouts)
					slots = std::max(slots, view.slot + 1);
			impl->ReserveShadowSequences(tables, slots, 0);
			// The latch for what the frame's epochs can name (as the shadow epoch reserves it, over every mode): its views, key slots,
			// the states registered and the sun's processes, a view's each at most. A growth at the epoch instead would leave the
			// revision's latch behind.
			{
				const auto& lookups = store.GetLookups();
				std::size_t keys = lookups.shadowSlotKeys.size() + tables.shadowKeysUsed.size();
				for (const auto& used : tables.occlusionKeysUsed)
					keys += used.size();
				const auto views = static_cast<std::uint32_t>(impl->shadowPlacements.size());
				impl->ReserveShadowLatch(views, static_cast<std::uint32_t>(keys), DrawPipelines::Get().ShadowRasterStateCount(),
					std::max(views, shadow->latchLayout.sunProcesses));
			}
			const auto bounds = ShadowBoundsOf(impl->drawBound, store.GetLookups());
			for (std::size_t kind = 0; kind < 2; ++kind) {
				const bool occlusion = kind == 1;
				// The layouts the epoch may draw: with scene revisions the shadow views' placements (all of them: DecideShadowCoverage) and
				// the occlusion maps' last, else those its commits drew lately.
				std::vector<std::vector<ShadowViewLayout>> sources;
				if (RevisionClaims()) {
					if (occlusion)
						sources = impl->recentOcclusionLayouts;
					else if (!impl->shadowPlacements.empty())
						sources.push_back(impl->shadowPlacements);
				}
				else
					for (const auto& shape : (occlusion ? shadow->recentOcclusionShapes : shadow->recentShapes).shapes)
						if (shape)
							sources.push_back(LayoutOf(*shape));
				std::vector<std::vector<ShadowViewLayout>> seen;
				for (auto& layouts : sources) {
					const ShadowSizing& sizing = Growths::Get().RevisionSizing<ShadowSizing>(*shadow);
					if (std::find(seen.begin(), seen.end(), layouts) != seen.end() ||
						std::any_of(layouts.begin(), layouts.end(), [&](const auto& a_view) { return a_view.slot >= shadow->sequences.size() || a_view.slot >= sizing.viewSlots; }))
						continue;
					ShadowShapeInputs in;
					in.resourceHeap = resourceHeap;
					in.samplerHeap = samplerHeap;
					in.indirect = shadowIndirect;
					for (const auto& layout : layouts)
						in.rows.push_back(impl->ShadowRowBuckets(layout.rasterState, store.GetLookups(), shadowIndirect));
					in.views = layouts;
					in.payload = &payload;
					in.bounds = &bounds;
					in.sizing = &sizing;
					in.materialRows = shadow->materialRows.RevisionAddress();
					in.viewBlocks = shadow->viewBlocks.RevisionAddress();
					in.previous = occlusion ? shadow->occlusionPublished : shadow->published;
					if (!occlusion) {
						in.latched.copies = ShadowLatchedLayout(*shadow);
						ReserveLatchedBlock(shadow->latchedBlock, LatchedBytes(in.latched.copies), host->FrameSlots());
						if (!in.latched.copies.empty())
							in.latched.latch = shadow->latchedBlock;
					}
					sp.revisions[kind][0].push_back(MakeShadowShape(*shadow, in));
					seen.push_back(std::move(layouts));
				}
				sp.revisionFrames[kind][0] = frameNumber;
			}
		}
		// The reflection's, from the same resources (its faces draw from the main epochs' inputs).
		auto& reflection = impl->reflection;
		auto& rp = impl->reflectionParity;
		rp.revisions[1] = std::move(rp.revisions[0]);
		rp.revisionFrames[1] = rp.revisionFrames[0];
		rp.revisions[0] = nullptr;
		rp.revisionFrames[0] = ~0u;
		// The faces' buffers for the main sizing's newest change, at the join it is asked for: the two changes are adopted with the
		// same revision (none is sealed while either is pending), so a commit's plan from the main sizing always fits them.
		if (reflection.resources) {
			ReflectionPlan latest;
			PlanReflectionBuckets(Growths::Get().LatestSizing<MainSizing>(r), reflection.slotPipelines, latest);
			impl->ReserveReflection(static_cast<std::uint32_t>(latest.map.size()), static_cast<std::uint32_t>(latest.buckets.size()), latest.draws);
		}
		// Without a commit's: what a reflection shape is made from is the faces' resources and the main sizing (the heaps the graph's).
		if ((rp.known || RevisionClaims()) && reflection.resources && indirect.valid) {
			ReflectionPlan plan;
			PlanReflectionBuckets(Growths::Get().RevisionSizing<MainSizing>(r), reflection.slotPipelines, plan);
			auto& scene = *impl->scene;
			const auto treeLod = scene.treeLodPipelines.load(std::memory_order_acquire);
			ReflectionShapeInputs in;
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			in.buckets = std::move(plan.buckets);
			in.map = std::make_shared<const std::vector<std::uint32_t>>(std::move(plan.map));
			in.materialRows = r.materialRows.RevisionAddress();
			in.pipelineRows = r.pipelineRows.RevisionAddress();
			// The revision's sizings: the faces' lists hold the scene's tree slots when the two it names agree.
			const ReflectionSizing& sizing = Growths::Get().RevisionSizing<ReflectionSizing>(*reflection.resources);
			const SceneSizing& sceneSizing = Growths::Get().RevisionSizing<SceneSizing>(scene);
			in.sizing = &sizing;
			if (reflection.treePipeline.valid() && treeLod && scene.treeLodCull && sizing.treeShapeCapacity == sceneSizing.treeLodShapeCapacity) {
				in.tree = reflection.treePipeline;
				in.treeSignature = treeLod->drawSignature;
				in.treeShapes = sceneSizing.treeLodShapeCapacity;
			}
			rp.revisions[0] = MakeReflectionShape(*reflection.resources, in);
			rp.revisionFrames[0] = frameNumber;
		}
		// The revision of these shapes (R3c b): assembled, its changed epochs recorded for it.
		impl->AssembleRevision(frameNumber);
	}

	void IndirectDraws::Impl::NoteShadowParity(bool a_occlusion, const ShadowFrame& a_frame, const std::vector<LatchedCopy>& a_layout, std::uint32_t a_frameNumber)
	{
		auto& p = shadowParity;
		p.known = true;
		if (!a_occlusion && a_frame.latched.copies != a_layout)
			++p.layoutMisses;
		const std::size_t kind = a_occlusion ? 1 : 0;
		const auto layouts = LayoutOf(a_frame);
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[kind][lag];
			const ShadowFrame* revision = nullptr;
			for (std::size_t i = 0; i < 2 && !revision; ++i)
				if (p.revisionFrames[kind][i] + lag == a_frameNumber)
					for (const auto& shape : p.revisions[kind][i])
						if (LayoutOf(*shape) == layouts)
							revision = shape.get();
			if (!revision) {
				++counts.missing;
				continue;
			}
			++counts.compared;
			if (revision->SameShape(a_frame)) {
				++counts.same;
				continue;
			}
			std::uint32_t differences = 0;
			auto note = [&](ShadowParity::Field a_field, bool a_differs) {
				if (a_differs)
					differences |= 1u << a_field;
			};
			for (std::size_t v = 0; v < a_frame.views.size(); ++v) {
				const auto& a = a_frame.views[v];
				const auto& b = revision->views[v];
				note(ShadowParity::kCapacity, a.capacity != b.capacity);
				note(ShadowParity::kBuckets, a.buckets != b.buckets);
				note(ShadowParity::kPush, a.push != b.push);
				note(ShadowParity::kRows, a.materialRows != b.materialRows || a.sequenceDraws != b.sequenceDraws);
			}
			note(ShadowParity::kLatch, a_frame.latch != revision->latch || a_frame.zeros != revision->zeros || a_frame.viewBlocksOffset != revision->viewBlocksOffset);
			note(ShadowParity::kLatched, !(a_frame.latched == revision->latched));
			note(ShadowParity::kHeaps, !SameHandle(a_frame.resourceHeap, revision->resourceHeap) || !SameHandle(a_frame.samplerHeap, revision->samplerHeap));
			note(ShadowParity::kPipelines, a_frame.indirect.valid != revision->indirect.valid || !SameHandle(a_frame.indirect.layout, revision->indirect.layout) ||
											   a_frame.indirect.version != revision->indirect.version);
			std::string fields;
			for (std::uint32_t f = 0; f < ShadowParity::kFields; ++f)
				if ((differences >> f) & 1) {
					++counts.differ[f];
					fields += fmt::format("{}{}", fields.empty() ? "" : ", ", ShadowParity::kFieldNames[f]);
				}
			if (p.logged < 24) {
				++p.logged;
				logger::info("[DCLF] shape parity: frame {} {} commit ({} views) against the revision {} frame{} before: {} differ", a_frameNumber, a_occlusion ? "occlusion" : "shadow",
					a_frame.views.size(), lag ? "made a" : "made the same", lag ? "" : "'s join", fields.empty() ? "(none named)" : fields);
			}
		}
	}

	void IndirectDraws::Impl::NoteReflectionParity(const ReflectionFrame& a_frame, std::uint32_t a_frameNumber)
	{
		auto& p = reflectionParity;
		p.known = true;
		p.resourceHeap = a_frame.resourceHeap;
		p.samplerHeap = a_frame.samplerHeap;
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[lag];
			std::size_t at = 2;
			for (std::size_t i = 0; i < 2 && at == 2; ++i)
				if (p.revisions[i] && p.revisionFrames[i] + lag == a_frameNumber)
					at = i;
			if (at == 2) {
				++counts.missing;
				continue;
			}
			++counts.compared;
			const auto& revision = *p.revisions[at];
			if (revision.SameShape(a_frame)) {
				++counts.same;
				continue;
			}
			++counts.differ[kShapePipelines];
			if (p.logged < 12) {
				++p.logged;
				logger::info("[DCLF] shape parity: frame {} reflection commit against the revision of frame {} differs: latch {}, buckets {} vs {}, pipelines {}, tree {}, push {}, sequences {} vs {}",
					a_frameNumber, p.revisionFrames[at], revision.latch == a_frame.latch ? "same" : "differs", a_frame.buckets.size(), revision.buckets.size(),
					SameIndirect(revision.indirect, a_frame.indirect) ? "same" : "differ", SameHandle(revision.tree, a_frame.tree) && revision.treeGroups == a_frame.treeGroups ? "same" : "differs",
					revision.push == a_frame.push ? "same" : "differs", a_frame.sequenceDraws, revision.sequenceDraws);
			}
		}
	}

	void IndirectDraws::Impl::NoteShapeParity(std::size_t a_shape, const PassFrame& a_frame, const std::vector<LatchedCopy>& a_layout, std::uint32_t a_frameNumber)
	{
		auto& p = shapeParity;
		if (a_frame.latched.copies != a_layout)
			++p.layoutMisses[a_shape];
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[a_shape][lag];
			// lag 0: the revision made at this frame's join; 1: the one made the frame before.
			std::size_t at = 2;
			for (std::size_t i = 0; i < 2 && at == 2; ++i)
				if (p.revisions[a_shape][i] && p.revisionFrames[a_shape][i] + lag == a_frameNumber)
					at = i;
			if (at == 2) {
				++counts.missing;
				continue;
			}
			++counts.compared;
			const auto differences = MainShapeDifferences(*p.revisions[a_shape][at], a_frame);
			if (!differences) {
				++counts.same;
				continue;
			}
			std::string fields;
			for (std::uint32_t f = 0; f < kMainShapeFields; ++f)
				if ((differences >> f) & 1) {
					++counts.differ[f];
					fields += fmt::format("{}{}", fields.empty() ? "" : ", ", kMainShapeFieldNames[f]);
				}
			if (p.logged < 24) {
				++p.logged;
				const auto& revision = *p.revisions[a_shape][at];
				logger::info("[DCLF] shape parity: frame {} {} commit against the revision of frame {}: {} differ (capacity {} vs {}, sequences {} vs {}, {} vs {} z calls, {} vs {} latched copies)",
					a_frameNumber, a_shape == kDepthShape ? "Z-prepass" : "colour", p.revisionFrames[a_shape][at], fields, a_frame.drawCapacity, revision.drawCapacity,
					a_frame.sequenceDraws, revision.sequenceDraws, a_frame.zCalls.size(), revision.zCalls.size(), a_frame.latched.copies.size(), revision.latched.copies.size());
			}
		}
	}
}

#endif
