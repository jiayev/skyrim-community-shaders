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
			// Frames in flight keep the old one (LatchedList::latch). Made off the host's world: the revision code's thread may make it.
			a_block = org::LatchBlock::Create("cs.dclf.latched-copies", static_cast<std::uint32_t>(std::bit_ceil(std::max<std::size_t>(2 * a_bytes, 4096))), a_slots);
		}

		// a_revision: a revision's (the rows' addresses it names: a ready growth's), else the commit's (the current versions').
		MainShapeInputs MainShapeInputsOf(const Resources& a_resources, bool a_depthOnly, std::uint32_t a_cullMode, bool a_revision)
		{
			MainShapeInputs in;
			in.depthOnly = a_depthOnly;
			const MainSizing& sizing = a_revision ? Growths::Get().RevisionSizing<MainSizing>(a_resources) : a_resources;
			in.sequenceDraws = sizing.sequenceDraws;
			in.sequenceDecals = sizing.sequenceDecals;
			in.materialRows = a_revision ? a_resources.materialRows.RevisionAddress() : a_resources.materialRows.address;
			in.pipelineRows = a_revision ? a_resources.pipelineRows.RevisionAddress() : a_resources.pipelineRows.address;
			in.cullMode = a_cullMode;
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
			if (const std::string_view pixel = a_in.probe; !pixel.empty()) {
				if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
					frame->probeX = static_cast<std::uint32_t>(std::strtoul(std::string(pixel.substr(0, sep)).c_str(), nullptr, 10));
					frame->probeY = static_cast<std::uint32_t>(std::strtoul(std::string(pixel.substr(sep + 1)).c_str(), nullptr, 10));
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
			auto frame = std::make_shared<ReflectionFrame>();
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->latch = a_in.latch;
			frame->facesOffset = a_in.latchLayout.FaceOffset(0);
			frame->zeros = a_in.zeros;
			frame->width = a_in.width;
			frame->height = a_in.height;
			frame->materialRows = a_in.materialRows;
			frame->pipelineRows = a_in.pipelineRows;
			frame->sequenceDraws = (a_in.sizing ? *a_in.sizing : static_cast<const ReflectionSizing&>(a_resources)).sequenceDraws;
			frame->buckets = a_in.buckets;
			frame->latchLayout = a_in.latchLayout;
			frame->map = a_in.map;
			// The colour segment's frame constants, VS and PS b12 the face's (its camera): the frame lighting (PS b13) is the main
			// pass's, its sun a frame old (dclf-lod.md, "The constants").
			const auto base = FramePushWords(a_in.frameConstantsAddress);
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

	void IndirectDraws::Impl::EnsureRevisionLatches()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& latches = producer.latches;
		const auto& inputs = producer.inputs;
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		// The main latch at the first sizes Setup's resources had (it is no resource of theirs: one block for every main resources made).
		if (!latches.main && inputs.main) {
			latches.mainLayout = {};
			latches.mainLayout.cascades = smallTables ? 1u : kInitialSunCascades;
			latches.mainLayout.shadowVolumes = smallTables ? 1u : kInitialShadowVolumes;
			latches.main = org::LatchBlock::Create("cs.dclf.latch", latches.mainLayout.Bytes(), host->FrameSlots());
		}
		// The shadow latch at the layout the shadow resources' first slots were made for (SetupShadow).
		if (!latches.shadow && inputs.shadow) {
			latches.shadowLayout = InitialShadowLatchLayout();
			latches.shadow = org::LatchBlock::Create("cs.dclf.shadow.latch", latches.shadowLayout.Bytes(), host->FrameSlots());
		}
		if (!latches.reflection && inputs.reflection) {
			latches.reflectionLayout = { 64, 4 };
			latches.reflection = org::LatchBlock::Create("cs.dclf.reflection.latch", latches.reflectionLayout.Bytes(), host->FrameSlots());
		}
	}

	void IndirectDraws::Impl::MakeRevision(const SceneStore::RevisionRequest& a_request, std::uint32_t a_frame)
	{
		ZoneScopedN("CS.DCLF.MakeRevisionShapes");
		// The frame's inputs as last posted (RevisionInputs): with the request, all the revision reads.
		auto& made = producer.made;
		revisionInputsSlot.TakeInto(producer.inputs);
		committedRowsSlot.TakeInto(producer.committedRows);
		producer.frame = a_frame;
		// The uploader a growth's fills go through, as posted.
		Growths::Get().uploads = producer.inputs.uploads;
		// What the frame adopted since the last make (its records, and the stamp): first, so nothing below names an adopted change as
		// pending or reads an owner's sizing the frame wrote.
		Growths::Get().Prune();
		const auto& inputs = producer.inputs;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!inputs.claims || !inputs.main || !inputs.main->scene || !host || !a_request.tables)
			return;
		const auto& tables = *a_request.tables;
		// T6b3a: the revision is its publication's alone - its tables, its lookups and the catalog they were resolved from (named
		// explicitly: GetIndirectState, GetShadowIndirectState of it, for the generations its inputs hold) - never the frame's installed
		// lookups and catalog. Made from those, a revision's shapes followed which publication the frame happened to have installed: a
		// frame that kept its publication (a revision late) paired the next revision's tables with older lookups, which changed its
		// bucket plans and rows again, which made it late too.
		static const Lookups kNoLookups;
		const Lookups& lookups = a_request.lookups ? *a_request.lookups : kNoLookups;
		const PipelineCatalog* catalog = a_request.catalog.get();
		auto& r = *inputs.main;
		EnsureRevisionLatches();
		auto& latches = producer.latches;
		// The capacities for the tables as the publication holds them: a growth here is one the frame's epochs would make.
		if (inputs.scene)
			ReserveSceneTables(*inputs.scene, tables);
		ReserveMainSequences(r, tables, a_request.tablesGeneration);
		if (inputs.scene)
			ReserveIndexPool(*inputs.scene, tables, a_request.tablesGeneration);
		// The shadow material rows the last shadow build wanted (the frame's epochs reserved them before T6b3b).
		if (inputs.shadow)
			ReserveShadowRows(*inputs.shadow, inputs.shadowRowsWanted);
		// The colour commit's sun cascades and local shadow light volumes, a shadow view each (the last Rebuild's candidates): the latch
		// a revision names holds them from its join on, not from a colour commit's growth (which a frame without claims never runs).
		{
			const std::uint32_t views = inputs.shadowCandidates;
			// And the Z-prepass's bucket map for every pipeline slot the tables have: a publication a revision made now covers holds no more
			// (its commit is this join's or older), and a commit past the map draws nothing for the slots it lacks.
			const auto& layout = latches.mainLayout;
			ReserveMainLatch(std::max(views, layout.cascades), std::max(views, layout.shadowVolumes), std::max(layout.buckets, a_request.pipelines));
		}
		// The growths the graph finished (G2): what the revision made now names, its shapes' addresses included. One still pending
		// keeps the revision from being sealed (AssembleRevision).
		revisions.growthPending = Growths::Get().Settle();
		const auto indirect = catalog ? GetIndirectState(*catalog, inputs.targetsGeneration) : IndirectState{};
		// The heaps every recording binds: the graph's own, as built (posted at the build point).
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		const std::uint32_t frameNumber = a_frame;
		const auto shadowIndirect = catalog ? GetShadowIndirectState(*catalog, inputs.shadowFormat) : ShadowIndirectState{};
		// The occlusion maps' layout as last captured: one the revisions make a shape for (OcclusionReady asks for it).
		{
			auto layouts = inputs.occlusionLayouts;
			auto& recent = producer.recentOcclusionLayouts;
			if (!layouts.empty() && (recent.empty() || recent.front() != layouts)) {
				std::erase(recent, layouts);
				recent.insert(recent.begin(), std::move(layouts));
				if (recent.size() > Impl::kRecentOcclusionLayouts)
					recent.resize(Impl::kRecentOcclusionLayouts);
			}
		}
		// The shadow and reflection buffers for what the revisions' shapes name, reserved at every join (a growth asked for here is one
		// a revision adopts).
		const auto shadowState = inputs.shadow;
		if (shadowState && shadowIndirect.valid) {
			// The slots the layouts name hold every draw the scene can produce, as the epochs reserve them.
			std::uint32_t slots = 0;
			for (const auto& view : inputs.shadowPlacements)
				slots = std::max(slots, view.slot + 1);
			for (const auto& layouts : producer.recentOcclusionLayouts)
				for (const auto& view : layouts)
					slots = std::max(slots, view.slot + 1);
			ReserveShadowSequences(*shadowState, tables, a_request.tablesGeneration, slots, 0);
			// The latch for what the frame's epochs can name (over every mode): its views, key slots, the states registered and the
			// sun's processes, a view's each at most. The key slots of the publication's lookups (T6b2c step 5: the scene lane resolved
			// the slots of these tables' keys already; append-only per instance), and the keys its casters and occluders use.
			const std::size_t slotKeys = std::max(lookups.shadowSlotKeys.size(), a_request.shadowSlotKeys);
			const std::size_t keys = slotKeys + a_request.shadowKeys;
			const auto views = static_cast<std::uint32_t>(inputs.shadowPlacements.size());
			ReserveShadowLatch(*shadowState, views, static_cast<std::uint32_t>(keys), inputs.shadowRasterStates, std::max(views, latches.shadowLayout.sunProcesses));
		}
		// The faces' forward pipelines and tree LOD's, as the request's catalog has them for its tables (PrepareReflection's rule).
		std::vector<rhi::PipelineHandle> slotPipelines;
		rhi::PipelineHandle treePipeline{};
		const bool faces = inputs.reflectionTargets.colour != DXGI_FORMAT_UNKNOWN && inputs.toggles.reflections && inputs.claims;
		ReflectionPipelinesOf(catalog, tables, inputs.reflectionTargets, faces, inputs.toggles.lodTrees, slotPipelines, treePipeline);
		TreeLodPipelines treeLodPipelines;
		const bool treeLod = TreeLodPipelinesOf(catalog, inputs.targetsGeneration, treeLodPipelines);
		const auto faceState = inputs.reflection;
		// The faces' buffers for the main sizing's newest change, at the join it is asked for: the two changes are adopted with the
		// same revision (none is sealed while either is pending), so a commit's plan from the main sizing always fits them.
		if (faceState) {
			ReflectionPlan latest;
			PlanReflectionBuckets(Growths::Get().LatestSizing<MainSizing>(r), slotPipelines, latest);
			// The faces' map for every pipeline slot the tables have, as the Z-prepass's (above).
			ReserveReflection(*faceState, inputs.scene.get(), static_cast<std::uint32_t>(std::max<std::size_t>(latest.map.size(), a_request.pipelines)),
				static_cast<std::uint32_t>(latest.buckets.size()), latest.draws);
		}
		// The shapes are made again only when what they are made from moved (RevisionShapesKey): otherwise the revision sealed at this
		// join keeps the last ones. CS_DCLF_REVISION_PARITY makes them around its frames whatever the key, and flags a change the key
		// missed (AssembleRevision: <- SHAPE KEY).
		if (producer.drawBound.shadowVersion != shadowBoundChecked) {
			shadowBoundChecked = producer.drawBound.shadowVersion;
			if (!ShadowShapesHold(ShadowBoundsOf(producer.drawBound, lookups)))
				++shadowBoundOutgrown;
		}
		const auto key = RevisionShapesKey(indirect, shadowIndirect, lookups, a_request.pipelines, slotPipelines, treePipeline, treeLod ? &treeLodPipelines : nullptr);
		const bool parityMake = RevisionParityEnabled() && (ParityDue(frameNumber) || ParityDue(frameNumber + 1));
		shapesKeyUnchanged = key == revisionShapesKey;
		if (shapesKeyUnchanged && !parityMake) {
			++shapesKept;
			AssembleRevision(frameNumber);
			return;
		}
		for (std::size_t g = 0; g < key.size(); ++g)
			shapesMadeBy[g] += key[g] != revisionShapesKey[g] ? 1 : 0;
		revisionShapesKey = key;
		++shapesMade;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			auto& shapes = made.main[shape];
			auto& frames = made.mainFrames[shape];
			shapes[1] = std::move(shapes[0]);
			frames[1] = frames[0];
			shapes[0] = nullptr;
			frames[0] = ~0u;
			// A segment no commit has captured for yet: no viewport, no frame blocks.
			if (!inputs.mainKnown[shape] || !indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			auto in = MainShapeInputsOf(r, depthOnly, inputs.toggles.cullMode, true);
			in.probe = inputs.gbufferProbe;
			in.latch = latches.main;
			in.latchLayout = latches.mainLayout;
			in.viewport = inputs.viewport[shape];
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			ZBucketPlan plan;
			if (depthOnly && r.pool)
				PlanZBuckets(Growths::Get().RevisionSizing<MainSizing>(r), lookups, tables, indirect, plan);
			in.zCalls = plan.calls;
			in.zPlan = std::make_shared<const ZBucketPlan>(std::move(plan));
			in.latched.copies = MainLatchedLayout(r, depthOnly, inputs.blockSizes[shape], static_cast<std::uint32_t>(in.zCalls.size()));
			ReserveLatchedBlock(latches.mainLatched[shape], LatchedBytes(in.latched.copies), host->FrameSlots());
			if (!in.latched.copies.empty())
				in.latched.latch = latches.mainLatched[shape];
			shapes[0] = MakeMainShape(in);
			frames[0] = frameNumber;
		}
		// The shadow and occlusion epochs': a shape for the placements and for each occlusion layout drawn lately.
		for (std::size_t kind = 0; kind < 2; ++kind) {
			auto& shapes = made.shadow[kind];
			auto& frames = made.shadowFrames[kind];
			shapes[1] = std::move(shapes[0]);
			frames[1] = frames[0];
			shapes[0].clear();
			frames[0] = ~0u;
		}
		// The slots the frame made (as posted): a layout past them waits for them.
		const std::size_t shadowSlotCount = inputs.shadowSlots ? inputs.shadowSlots->sequences.size() : 0;
		if ((inputs.shadowKnown || !inputs.shadowPlacements.empty() || !producer.recentOcclusionLayouts.empty()) && shadowState && shadowIndirect.valid) {
			// Sized from the scene's casting bound alone (ShadowBounds), not a payload: the shapes move with the scene's objects, not
			// with the frame's casters or publications.
			const auto bounds = std::make_shared<const ShadowBounds>(ShadowBoundsOf(producer.drawBound, lookups));
			made.bounds = bounds;
			for (std::size_t kind = 0; kind < 2; ++kind) {
				const bool occlusionKind = kind == 1;
				// The layouts the epoch may draw: the shadow views' placements (all of them: DecideShadowCoverage) and the occlusion maps'
				// last.
				std::vector<std::vector<ShadowViewLayout>> sources;
				if (occlusionKind)
					sources = producer.recentOcclusionLayouts;
				else if (!inputs.shadowPlacements.empty())
					sources.push_back(inputs.shadowPlacements);
				std::vector<std::vector<ShadowViewLayout>> seen;
				for (auto& layouts : sources) {
					const ShadowSizing& sizing = Growths::Get().RevisionSizing<ShadowSizing>(*shadowState);
					if (std::find(seen.begin(), seen.end(), layouts) != seen.end() ||
						std::any_of(layouts.begin(), layouts.end(), [&](const auto& a_view) { return a_view.slot >= shadowSlotCount || a_view.slot >= sizing.viewSlots; }))
						continue;
					ShadowShapeInputs in;
					in.resourceHeap = resourceHeap;
					in.samplerHeap = samplerHeap;
					in.indirect = shadowIndirect;
					for (const auto& layout : layouts)
						in.rows.push_back(ShadowRowBuckets(producer.rowBuckets, layout.modeIndex, layout.rasterState, lookups, shadowIndirect));
					in.views = layouts;
					in.bounds = bounds.get();
					in.sizing = &sizing;
					in.materialRows = shadowState->materialRows.RevisionAddress();
					in.viewBlocks = shadowState->viewBlocks.RevisionAddress();
					in.latch = latches.shadow;
					in.latchLayout = latches.shadowLayout;
					in.zeros = latches.shadowZeros;
					// The last shape made for the layout: its slots' capacities only grow.
					for (const auto& candidate : made.shadow[kind][1])
						if (candidate && LayoutOf(*candidate) == layouts)
							in.previous = candidate;
					if (!occlusionKind) {
						in.latched.copies = ShadowLatchedLayout(*shadowState);
						ReserveLatchedBlock(latches.shadowLatched, LatchedBytes(in.latched.copies), host->FrameSlots());
						if (!in.latched.copies.empty())
							in.latched.latch = latches.shadowLatched;
					}
					made.shadow[kind][0].push_back(MakeShadowShape(*shadowState, in));
					seen.push_back(std::move(layouts));
				}
				made.shadowFrames[kind][0] = frameNumber;
			}
		}
		// The reflection's, from the same resources (its faces draw from the main epochs' inputs).
		made.reflection[1] = std::move(made.reflection[0]);
		made.reflectionFrames[1] = made.reflectionFrames[0];
		made.reflection[0] = nullptr;
		made.reflectionFrames[0] = ~0u;
		// Without a commit's: what a reflection shape is made from is the faces' resources and the main sizing (the heaps the graph's).
		if ((inputs.reflectionKnown || inputs.claims) && faceState && indirect.valid && inputs.scene) {
			ReflectionPlan plan;
			PlanReflectionBuckets(Growths::Get().RevisionSizing<MainSizing>(r), slotPipelines, plan);
			auto& sceneState = *inputs.scene;
			ReflectionShapeInputs in;
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			in.buckets = std::move(plan.buckets);
			in.map = std::make_shared<const std::vector<std::uint32_t>>(std::move(plan.map));
			in.materialRows = r.materialRows.RevisionAddress();
			in.pipelineRows = r.pipelineRows.RevisionAddress();
			in.latch = latches.reflection;
			in.latchLayout = latches.reflectionLayout;
			in.zeros = latches.reflectionZeros;
			// The main pass's frame constants and the faces' size as posted (the cube's import is the frame's).
			in.frameConstantsAddress = r.frameConstantsAddress;
			in.width = inputs.reflectionWidth;
			in.height = inputs.reflectionHeight;
			// The revision's sizings: the faces' lists hold the scene's tree slots when the two it names agree.
			const ReflectionSizing& sizing = Growths::Get().RevisionSizing<ReflectionSizing>(*faceState);
			const SceneSizing& sceneSizing = Growths::Get().RevisionSizing<SceneSizing>(sceneState);
			in.sizing = &sizing;
			if (treePipeline.valid() && treeLod && sceneState.treeLodCull && sizing.treeShapeCapacity == sceneSizing.treeLodShapeCapacity) {
				in.tree = treePipeline;
				in.treeSignature = treeLodPipelines.drawSignature;
				in.treeShapes = sceneSizing.treeLodShapeCapacity;
			}
			made.reflection[0] = MakeReflectionShape(*faceState, in);
			made.reflectionFrames[0] = frameNumber;
		}
		// The frame's shape parity compares with what was made (posted: both makes).
		madeSlot.Post(std::make_unique<MadeShapes>(made));
		// The revision of these shapes (R3c b): assembled, its changed epochs recorded for it.
		AssembleRevision(frameNumber);
	}

	IndirectDraws::Impl::ShapesKey IndirectDraws::Impl::RevisionShapesKey(const IndirectState& a_indirect, const ShadowIndirectState& a_shadowIndirect, const Lookups& a_lookups,
		std::size_t a_pipelines, std::span<const rhi::PipelineHandle> a_slotPipelines, rhi::PipelineHandle a_treePipeline, const TreeLodPipelines* a_treeLod) const
	{
		ShapesKey key;
		key.fill(0xcbf29ce484222325ull);
		std::size_t group = 0;
		auto mix = [&key, &group](std::uint64_t a_value) {
			auto& h = key[group];
			h = (h ^ a_value) * 0x100000001b3ull;
			h ^= h >> 29;
		};
		auto ptr = [&mix](const void* a_pointer) { mix(reinterpret_cast<std::uintptr_t>(a_pointer)); };
		auto raw = [&mix]<class T>(const T& a_value) {
			static_assert(std::is_trivially_copyable_v<T>);
			std::array<std::byte, (sizeof(T) + 7) / 8 * 8> bytes{};
			std::memcpy(bytes.data(), &a_value, sizeof(T));
			for (std::size_t b = 0; b < bytes.size(); b += 8) {
				std::uint64_t word;
				std::memcpy(&word, bytes.data() + b, 8);
				mix(word);
			}
		};
		// The revision's inputs (RevisionInputs) and latches (the producer's): what its shapes are made from besides the request.
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The request's lookups (T6b3a: the publication's, which the shapes are made from).
		const auto& lookups = a_lookups;
		// What every shape names: the growths a revision adopts (sizings, rows' addresses), the pipeline sets, the heaps, the toggles.
		group = kKeyGrowths;
		mix(Growths::Get().stamp);
		mix(VersionRegistry::Get().Published());
		group = kKeyPipelines;
		ptr(a_indirect.version.get());
		mix(a_indirect.valid);
		ptr(a_shadowIndirect.version.get());
		mix(a_shadowIndirect.valid);
		mix(inputs.resourceHeap.index);
		mix(inputs.resourceHeap.generation);
		mix(inputs.samplerHeap.index);
		mix(inputs.samplerHeap.generation);
		mix(inputs.toggles.cullMode);
		group = kKeyLookups;
		mix(lookups.instance);
		mix(lookups.generation);
		mix(lookups.versionCounter);
		mix(lookups.shadowGeneration);
		mix(a_pipelines);  // the request's pipeline slots (T6b3a: the publication's tables, not the coordinator's)
		// The main segments': their latch, latched blocks and targets, and what their commits captured (viewport, frame blocks).
		group = kKeyMain;
		const auto& r = *inputs.main;
		ptr(&r);
		ptr(latches.main.get());
		raw(latches.mainLayout);
		ptr(r.latchedTargets.load(std::memory_order_acquire).get());
		ptr(r.pool.get());
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			ptr(latches.mainLatched[shape].get());
			mix(inputs.mainKnown[shape]);
			raw(inputs.viewport[shape]);
			raw(inputs.blockSizes[shape]);
		}
		// The shadow and occlusion epochs': the placements, the occlusion maps' layouts, the scene's casting bound, their latch.
		group = kKeyShadow;
		if (const auto& s = inputs.shadow) {
			ptr(s.get());
			ptr(latches.shadow.get());
			raw(latches.shadowLayout);
			ptr(latches.shadowZeros.get());
			ptr(latches.shadowLatched.get());
			ptr(s->latchedTargets.load(std::memory_order_acquire).get());
			mix(inputs.shadowSlots ? inputs.shadowSlots->sequences.size() : 0);
		}
		mix(inputs.shadowKnown);
		for (const auto& view : inputs.shadowPlacements)
			raw(view);
		for (const auto& layouts : producer.recentOcclusionLayouts) {
			mix(layouts.size());
			for (const auto& view : layouts)
				raw(view);
		}
		group = kKeyBound;
		mix(shadowBoundOutgrown);
		// The reflection's: its resources, the faces' pipelines and tree LOD's (the request's catalog's).
		group = kKeyReflection;
		if (const auto& faces = inputs.reflection) {
			ptr(faces.get());
			mix(inputs.reflectionWidth);
			mix(inputs.reflectionHeight);
			ptr(latches.reflection.get());
			raw(latches.reflectionLayout);
			ptr(latches.reflectionZeros.get());
			for (const auto& pipeline : a_slotPipelines) {
				mix(pipeline.index);
				mix(pipeline.generation);
			}
			mix(a_treePipeline.index);
			mix(a_treePipeline.generation);
		}
		mix(inputs.reflectionKnown);
		if (const auto& sceneState = inputs.scene) {
			if (a_treeLod) {
				mix(a_treeLod->drawSignature.index);
				mix(a_treeLod->drawSignature.generation);
			}
			mix(a_treeLod ? 1u : 0u);
			mix(sceneState->treeLodCull ? 1u : 0u);
		}
		group = kKeyPipelines;
		mix(std::hash<std::string_view>{}(inputs.gbufferProbe));
		return key;
	}

	bool IndirectDraws::Impl::ShadowShapesHold(const ShadowBounds& a_bounds) const
	{
		// The producer's: its inputs' shadow resources and the shapes it made last.
		const auto& shadowState = producer.inputs.shadow;
		if (!shadowState)
			return true;
		const std::uint32_t words = Growths::Get().RevisionSizing<ShadowSizing>(*shadowState).bucketCountWords;
		std::vector<std::uint64_t> need;
		for (std::size_t kind = 0; kind < 2; ++kind)
			for (const auto& shape : producer.made.shadow[kind][0]) {
				if (!shape || !shape->rows)
					continue;
				for (std::size_t v = 0; v < shape->views.size() && v < shape->rows->size(); ++v) {
					const auto& view = shape->views[v];
					const auto& row = (*shape->rows)[v];
					if (row.pipelines.size() >= words || view.modeIndex >= kShadowModeCount || a_bounds.modeDraws[view.modeIndex] > view.capacity)
						return false;
					const auto& draws = a_bounds.keySlotDraws[view.modeIndex];
					need.assign(view.buckets.size(), 0);
					for (std::size_t k = 0; k < row.bucketOfSlot.size() && k < draws.size(); ++k)
						if (const auto bucket = row.bucketOfSlot[k]; bucket != Lookups::kNone && bucket < need.size())
							need[bucket] += draws[k];
					for (std::size_t b = 0; b < need.size(); ++b)
						if (need[b] > view.buckets[b].capacity)
							return false;
				}
			}
		return true;
	}

	void IndirectDraws::Impl::NoteShadowParity(bool a_occlusion, const ShadowFrame& a_frame, const std::vector<LatchedCopy>& a_layout, std::uint32_t a_frameNumber)
	{
		auto& p = shadowParity;
		// A shadow or occlusion commit has run: a revision input (the next revisions make the shapes for it).
		if (!std::exchange(p.known, true))
			PostRevisionInputs();
		if (!a_occlusion && a_frame.latched.copies != a_layout)
			++p.layoutMisses;
		// The producer's shapes, as it last posted them.
		const auto& made = TakeMadeShapes();
		const std::size_t kind = a_occlusion ? 1 : 0;
		const auto layouts = LayoutOf(a_frame);
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[kind][lag];
			const ShadowFrame* revision = nullptr;
			for (std::size_t i = 0; i < 2 && !revision; ++i)
				if (made.shadowFrames[kind][i] + lag == a_frameNumber)
					for (const auto& shape : made.shadow[kind][i])
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
		// A reflection commit has run: a revision input.
		if (!std::exchange(p.known, true))
			PostRevisionInputs();
		p.resourceHeap = a_frame.resourceHeap;
		p.samplerHeap = a_frame.samplerHeap;
		// The producer's shapes, as it last posted them.
		const auto& made = TakeMadeShapes();
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[lag];
			std::size_t at = 2;
			for (std::size_t i = 0; i < 2 && at == 2; ++i)
				if (made.reflection[i] && made.reflectionFrames[i] + lag == a_frameNumber)
					at = i;
			if (at == 2) {
				++counts.missing;
				continue;
			}
			++counts.compared;
			const auto& revision = *made.reflection[at];
			if (revision.SameShape(a_frame)) {
				++counts.same;
				continue;
			}
			++counts.differ[kShapePipelines];
			if (p.logged < 12) {
				++p.logged;
				logger::info("[DCLF] shape parity: frame {} reflection commit against the revision of frame {} differs: latch {}, buckets {} vs {}, pipelines {}, tree {}, push {}, sequences {} vs {}",
					a_frameNumber, made.reflectionFrames[at], revision.latch == a_frame.latch ? "same" : "differs", a_frame.buckets.size(), revision.buckets.size(),
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
		// The producer's shapes, as it last posted them.
		const auto& made = TakeMadeShapes();
		for (std::size_t lag = 0; lag < 2; ++lag) {
			auto& counts = p.counts[a_shape][lag];
			// lag 0: the revision made at this frame's join; 1: the one made the frame before.
			std::size_t at = 2;
			for (std::size_t i = 0; i < 2 && at == 2; ++i)
				if (made.main[a_shape][i] && made.mainFrames[a_shape][i] + lag == a_frameNumber)
					at = i;
			if (at == 2) {
				++counts.missing;
				continue;
			}
			++counts.compared;
			const auto differences = MainShapeDifferences(*made.main[a_shape][at], a_frame);
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
				const auto& revision = *made.main[a_shape][at];
				logger::info("[DCLF] shape parity: frame {} {} commit against the revision of frame {}: {} differ (capacity {} vs {}, sequences {} vs {}, {} vs {} z calls, {} vs {} latched copies)",
					a_frameNumber, a_shape == kDepthShape ? "Z-prepass" : "colour", made.mainFrames[a_shape][at], fields, a_frame.drawCapacity, revision.drawCapacity,
					a_frame.sequenceDraws, revision.sequenceDraws, a_frame.zCalls.size(), revision.zCalls.size(), a_frame.latched.copies.size(), revision.latched.copies.size());
			}
		}
	}

	bool IndirectDraws::ReflectionSlotReady(const void* a_tables, const PipelineCatalog* a_catalog, std::uint32_t a_slot) const
	{
		// T6b3e: the faces' forward pipeline of the slot's pipeline as a_catalog has it for the faces' targets (ReflectionPipelinesOf's rule,
		// which the revision's shapes resolve from their publication's catalog): the scene lane's commit's, from its own catalog, never the
		// render thread's per-frame list (Reflection::slotPipelines, assigned whole by PrepareReflection while the lane read it).
		const auto& tables = *static_cast<const SceneStore::Tables*>(a_tables);
		const ForwardTargets targets = impl->reflection.LaneTargets();  // the published copy (the render thread writes `targets` at every face capture)
		if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
			return false;
		const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
		ForwardPipelineKey key;
		return p < tables.pipelines.size() && Draws::ReflectionForwardKey(tables, p, key) && ForwardPipelineOf(a_catalog, key, targets).valid();
	}
}

#endif
