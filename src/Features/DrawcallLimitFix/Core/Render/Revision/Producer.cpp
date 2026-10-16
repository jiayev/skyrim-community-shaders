// The revision producer, on the snapshot builder (DCLF's preparation pool, one pass at a time): MakeRevision makes a scene
// revision's shapes and latches from a publication and the frame's posted RevisionInputs, then AssembleRevision seals them. Also the
// main shape's plans (shared with the commits).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		void PlanZBuckets(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_layout.zBucketCapacity.size());
			const auto& groups = a_indirect.zGroups;
			const auto& keys = a_tables.pipelines;
			a_out.map.assign(slots, kNoBucket);
			a_out.calls.clear();
			// Per group, its bucket.
			std::vector<std::uint32_t> groupBucket(a_indirect.zPipelines.size(), kNoBucket);
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (!a_layout.zBucketCapacity[p] || p >= a_lookups.pipelines.size() || p >= keys.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == keys[p]) || entry.setIndex >= groups.size())
					continue;
				// No bucket for a family key not a depth segment input (ProgramBuild::ForwardOrDecal, the Effect port's: no forward object is
				// a depth input, and its pipeline has no Z-prepass pipeline).
				if (!Extensions::FamilyRules(keys[p].rasterFlags).depthSegment)
					continue;
				const std::uint32_t group = groups[entry.setIndex];
				auto& bucket = groupBucket[group];
				// A terrain group 1 slot's group (its depth pipeline's key holds no other: DrawPipelinesImpl.h, ZPipelineKey) has a twin
				// bucket right after its own, as large, for its near blocks (BuildDrawsCS, kBucketTerrainPair; MainOpaquePass).
				const bool terrain = RasterTerrainGroup(keys[p].rasterFlags) == 1;
				if (bucket == kNoBucket) {
					bucket = a_out.Buckets();
					a_out.calls.push_back({ bucket, 0u, 0u, a_indirect.zPipelines[group] });
					if (terrain)
						a_out.calls.push_back({ bucket + 1, 0u, 0u, a_indirect.zPipelines[group], true });
				}
				a_out.calls[bucket].capacity += a_layout.zBucketCapacity[p];
				if (terrain)
					a_out.calls[bucket + 1].capacity += a_layout.zBucketCapacity[p];
				a_out.map[p] = terrain ? (bucket | kBucketTerrainPair) : bucket;
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
				a_out.table[2 * (buckets + b)] = first + a_layout.sequenceDraws;
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
			// The frame-wide rows (LatchFrameRows: the lighting, LOD fades, fog, extras and the frame constant rows in ascending row order).
			auto addConstants = [&](std::size_t a_bytes, std::uint64_t a_offset) { add(constants, a_bytes, a_offset); };
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(addConstants)*>(a_add))(a_bytes, a_offset); }, &addConstants);
			if (!a_depthOnly && a_resources.foliage)
				add(constants, 8 * sizeof(std::uint32_t), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame));
			// The features' scene resources' latched buffers (Api::SceneResources::latched: Features/TreeLod's draw row and list header), the
			// depth commit's.
			if (a_depthOnly)
				Extensions::LatchSceneResources(a_resources.scene->sceneResources,
					[](void* a_add, const std::shared_ptr<org::Buffer>& a_buffer, const Versioned& a_versioned, std::size_t a_bytes) {
						(*static_cast<decltype(add)*>(a_add))(a_versioned ? a_versioned->Key() : a_buffer.get(), a_bytes, 0);
					},
					&add);
			add(a_resources.count.get(), a_depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t), 0);
			if (!a_depthOnly) {
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
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

		/**
		 * @brief A view's own frame values' latched copies, in CommitView's order, packed from a_at (its latch layout's FrameOffset): its
		 * copies of the main frame buffers (a_frameBuffers, whole; null: none), then into a_key, its frame constants, the record, CS's
		 * SharedData and FeatureData at the depth commit's sizes (a_depth: the same blocks, this frame's), the frame-wide rows. The faces'
		 * programs read no other frame slot.
		 */
		std::vector<LatchedCopy> ViewFrameLatched(const void* a_key, const std::vector<FrameBuffer>* a_frameBuffers, const FrameBlockSizes& a_depth, std::uint32_t a_at)
		{
			std::vector<LatchedCopy> out;
			std::size_t used = 0;
			auto addTo = [&](const void* a_target, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_bytes)
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_target, static_cast<std::uint32_t>(a_at + at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			auto add = [&](std::size_t a_bytes, std::uint64_t a_offset) { addTo(a_key, a_bytes, a_offset); };
			for (const auto& frameBuffer : a_frameBuffers ? std::span<const FrameBuffer>(*a_frameBuffers) : std::span<const FrameBuffer>())
				if (frameBuffer.copy)
					addTo(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(sizeof(DrawBindings), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			add(a_depth.ps[kSharedDataRegister], FrameSlotOffset(true, kSharedDataRegister));
			add(a_depth.ps[kFeatureDataRegister], FrameSlotOffset(true, kFeatureDataRegister));
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(add)*>(a_add))(a_bytes, a_offset); }, &add);
			return out;
		}

		/** @brief Producer: a latched-copies block holding a_copies (none: nothing asked). */
		void RequireLatchedBlock(GrowthBuild& a_build, LatchHandle a_latch, const std::vector<LatchedCopy>& a_copies)
		{
			if (const std::size_t bytes = LatchedBytes(a_copies))
				SceneBufferSet::Require(a_build, a_latch, static_cast<std::uint32_t>(bytes), 0);
		}

		MainShapeInputs MainShapeInputsOf(const Resources&, const MainLayout& a_layout, const NamedVersions&, bool a_depthOnly, std::uint32_t a_cullMode)
		{
			MainShapeInputs in;
			in.depthOnly = a_depthOnly;
			in.sequenceDraws = a_layout.sequenceDraws;
			in.sequenceDecals = a_layout.sequenceDecals;
			in.terrainCapacity = a_layout.terrainCapacity;
			in.forwardCapacity = a_layout.forwardCapacity;
			in.refractionCapacity = a_layout.refractionCapacity;
			in.cullMode = a_cullMode;
			return in;
		}

		std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in)
		{
			auto frame = std::make_shared<PassFrame>();
			frame->sequenceDraws = a_in.sequenceDraws;
			frame->sequenceDecals = a_in.sequenceDecals;
			// The max counts: the sequence buffer's ranges, which hold every draw the scene can produce (RequireMain). Not the counts of
			// the tables the shape was made from: a publication a revision covers may hold newer tables (a decal group's first decal,
			// more draws), and its commit would find the shape short. They change with the layout alone, whose buffers the revision's set
			// holds. The depth segment is given no decals (BuildDrawsCS).
			frame->drawCapacity = a_in.sequenceDraws;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				frame->decalCapacity[group] = a_in.depthOnly ? 0u : a_in.sequenceDecals;
			// The terrain groups' ranges, the colour segment's alone (BuildDrawsCS appends them there).
			if (!a_in.depthOnly) {
				frame->terrainCapacity = a_in.terrainCapacity;
				frame->forwardCapacity = a_in.forwardCapacity;  // the colour segment's and the forward epoch's
				frame->refractionCapacity = a_in.refractionCapacity;  // likewise
			}
			frame->width = a_in.viewport.width;
			frame->height = a_in.viewport.height;
			frame->minDepth = a_in.viewport.minDepth;
			frame->maxDepth = a_in.viewport.maxDepth;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->cullMode = a_in.cullMode;
			frame->latch = a_in.latch;
			frame->latchLayout = a_in.latchLayout;
			frame->zCalls = a_in.zCalls;
			frame->zPlan = a_in.zPlan;
			frame->latched = a_in.latched;
			return frame;
		}

	}

	void IndirectDraws::Impl::EnsureRevisionLatches()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& latches = producer.latches;
		auto& handles = producer.latchHandles;
		const auto& inputs = producer.inputs;
		// The blocks' handles in the scene's set, once: the seals make the blocks (off the host's world, on the producer's thread).
		if (!handles) {
			auto& buffers = SceneBufferSet::Get();
			const std::uint32_t slots = host->FrameSlots();
			// The latched copies' blocks grow with headroom past what a layout lists (a commit's frame blocks change size).
			const auto latched = org::services::GrowthPolicy::Headroom(100, 4096);
			handles.main = buffers.RegisterLatch("cs.dclf.latch", slots);
			for (auto& block : handles.mainLatched)
				block = buffers.RegisterLatch("cs.dclf.latched-copies", slots, latched);
			handles.shadow = buffers.RegisterLatch("cs.dclf.shadow.latch", slots);
			handles.shadowZeros = buffers.RegisterLatch("cs.dclf.shadow.zeros", 1);
			handles.shadowLatched = buffers.RegisterLatch("cs.dclf.latched-copies", slots, latched);
			// A latch for each view a feature took (one none took draws nothing).
			for (const auto& slot : kViewSlots) {
				if (!Extensions::ViewRegistered(slot.view))
					continue;
				const std::size_t v = ViewIndex(slot.view);
				handles.views[v] = buffers.RegisterLatch(fmt::format("{}.latch", TraitsOf(slot.view).prefix), slots);
				handles.viewZeros[v] = buffers.RegisterLatch(fmt::format("{}.zeros", TraitsOf(slot.view).prefix), 1);
			}
		}
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		// The main latch at the first sizes Setup's resources had (it is no resource of theirs: one block for every main resources made).
		if (!latches.mainLaidOut && inputs.main) {
			latches.main = {};
			latches.main.cascades = smallTables ? 1u : kInitialSunCascades;
			latches.main.shadowVolumes = smallTables ? 1u : kInitialShadowVolumes;
			latches.mainLaidOut = true;
		}
		// The shadow latch at the layout the shadow resources' first slots were made for (SetupShadow).
		if (!latches.shadowLaidOut && inputs.shadow) {
			latches.shadow = InitialShadowLatchLayout();
			latches.shadowLaidOut = true;
		}
		for (const auto& slot : kViewSlots)
			if (const std::size_t v = ViewIndex(slot.view); !latches.viewsLaidOut[v] && inputs.views[v].resources) {
				latches.views[v] = ViewLatchLayout::Initial(TraitsOf(slot.view));
				latches.viewsLaidOut[v] = true;
			}
	}

	void IndirectDraws::Impl::MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame)
	{
		const auto& request = a_publication.revision;
		ZoneScopedN("CS.DCLF.MakeRevisionShapes");
		// The frame's inputs as last posted (RevisionInputs): with the request, all the revision reads.
		auto& made = producer.made;
		revisionInputsSlot.TakeInto(producer.inputs);
		producer.frame = a_frame;
		auto& buffers = SceneBufferSet::Get();
		// The uploader a growth's fills go through, as posted.
		buffers.SetUploads(producer.inputs.uploads);
		const auto& inputs = producer.inputs;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!inputs.claims || !inputs.main || !inputs.main->scene || !host || !a_publication.tables)
			return;
		const auto& tables = *a_publication.tables;
		// The revision is its publication's alone - its tables, its lookups and the catalog they were resolved from (named explicitly:
		// GetIndirectState, GetShadowIndirectState of it, for the generations its inputs hold) - never the frame's installed lookups and
		// catalog: shapes made from those would follow whichever publication the frame has installed, pairing a late revision's tables
		// with older lookups and changing its bucket plans and rows again.
		static const Lookups kNoLookups;
		const Lookups& lookups = a_publication.lookups ? *a_publication.lookups : kNoLookups;
		const PipelineCatalog* catalog = a_publication.catalog.get();
		EnsureRevisionLatches();
		// What the set must hold for the tables as the publication holds them (Begin, the requirements, Seal): the shapes are made from
		// a set that holds every one. Each participant's (Revision/Participant.h: the main segments', the shadow engine's, the views'),
		// then their latched blocks, then every latch at its layout as the reserves left it.
		RevisionMake make{ *this, a_publication, tables, lookups, catalog };
		auto build = buffers.Begin();
		for (auto* participant : participants)
			participant->RequireSequences(make, build);
		for (auto* participant : participants)
			participant->RequireLatched(make, build);
		RequireLatches(build);
		// The set: sealed once it holds every requirement; else a growth batch is in flight and the builder makes the revision again when
		// it settles (it wakes the builder). The same set (its serial, its fragment) while nothing changed.
		auto seal = buffers.Seal(std::move(build));
		if (!seal) {
			++revisions.growthWaits;
			return;
		}
		const auto& sealed = *seal.named;
		// The event lists' staging sets for the capacities the set names, before any shape of it.
		if (const auto& s = inputs.scene) {
			// The features' scene resources' (Api::SceneResources::sealed: Features/Fades' write-back).
			Extensions::SealSceneResources(sealed, s->sceneResources);
			if (s->visibilityEvents && s->visibilityReadback)
				s->visibilityReadback->AddSet(static_cast<std::uint32_t>(sealed.Of(s->visibilityEvents).units));
		}
		if (!revisions.layout || !(*revisions.layout == producer.mainLayout))
			revisions.layout = std::make_shared<const MainLayout>(producer.mainLayout);
		revisions.sealed = std::move(seal);
		const MainLayout& mainLayout = *revisions.layout;
		// The shapes are made again only when what they are made from moved (RevisionShapesKey): otherwise the revision sealed at this
		// make keeps the last ones. What the participants note of the make first (the shadow engine's casting bound outgrown).
		for (auto* participant : participants)
			participant->NoteSealed(make);
		const auto key = RevisionShapesKey(sealed, make, request.pipelines);
		if (key == revisionShapesKey) {
			++shapesKept;
			AssembleRevision(a_frame);
			return;
		}
		for (std::size_t g = 0; g < key.size(); ++g)
			shapesMadeBy[g] += key[g] != revisionShapesKey[g] ? 1 : 0;
		revisionShapesKey = key;
		++shapesMade;
		for (auto* participant : participants)
			participant->MakeShapes(make, sealed, mainLayout);
		Diagnostics::Notify(Diagnostics::RevisionShapesMade{ sealed, mainLayout, made, inputs, tables.geometries.size() });
		// The revision of these shapes: assembled, its changed epochs recorded for it.
		AssembleRevision(a_frame);
	}

	void IndirectDraws::Impl::RequireRevisionSet(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& latches = producer.latches;
		if (inputs.scene)
			RequireSceneTables(a_build, *inputs.scene, tables);
		RequireMain(a_build, r, tables, a_make.publication.tablesGeneration);
		if (inputs.scene)
			RequireIndexPool(a_build, *inputs.scene, tables, a_make.publication.tablesGeneration);
		// The colour commit's sun cascades and local shadow light volumes, a shadow view each (the last Rebuild's candidates): the latch
		// a revision names holds them from its make on, not from a colour commit's growth (which a frame without claims never runs).
		{
			const std::uint32_t views = inputs.shadowCandidates;
			// And the Z-prepass's bucket map for every pipeline slot the tables have: a publication a revision made now covers holds no more
			// (its commit is this make's or older), and a commit past the map draws nothing for the slots it lacks.
			const auto& layout = latches.main;
			ReserveMainLatch(std::max(views, layout.cascades), std::max(views, layout.shadowVolumes), std::max(layout.buckets, request.pipelines));
		}
		a_make.indirect = a_make.catalog ? GetIndirectState(*a_make.catalog, inputs.targetsGeneration) : IndirectState{};
	}

	void IndirectDraws::Impl::RequireRevisionViews(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		// The views' forward pipelines (and the faces' draw families'), as the request's catalog has them for its tables, and each view's plan
		// from the one main layout, its buffers required to hold that plan: the shape made below from the same layout and the set sealed
		// with those requirements can never index past its buffers. Their maps for every pipeline slot the tables have, as the
		// Z-prepass's (above).
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			a_make.familyOn[d] = CatalogFamilyPipelines(a_make.catalog, static_cast<Api::DrawFamily>(d), inputs.targetsGeneration, a_make.familyPipelines[d]);
		auto& viewOn = a_make.viewOn;
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			if (!Extensions::ViewRegistered(kind))
				continue;
			const std::size_t v = ViewIndex(kind);
			const auto& traits = TraitsOf(kind);
			const auto& view = inputs.views[v];
			viewOn[v] = view.targets.colour != DXGI_FORMAT_UNKNOWN && Extensions::ViewOn(inputs.toggles, kind) && inputs.claims;
			ViewPipelinesOf(kind, a_make.catalog, tables, view.targets, viewOn[v], viewRevisionPipelines[v]);
			if (traits.forwardPart)
				ViewForwardSlotsOf(tables, viewOn[v], viewForwardSlots);
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d) {
				const auto family = static_cast<Api::DrawFamily>(d);
				viewRevisionFaces[d] = viewOn[v] && Extensions::DrawFamilyOn(inputs.toggles, family) ?
				                           ForwardPipelineOf(a_make.catalog, ForwardPipelineKey{ 0, 0, FamilySlotOf(family).forwardFlags }, view.targets) :
				                           rhi::PipelineHandle{};
			}
			if (!view.resources || (traits.depthPass && !viewOn[v]))
				continue;
			// A view's own frame values (the faces': its frame buffers' copies, then its frame constants'): its latch's frame region holds them
			// at the depth commit's block sizes, grown by a page (another layout: a new block at the seal).
			if (OwnsFrameConstants(traits) && view.resources->frameConstants && producer.latches.viewsLaidOut[v]) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				const auto bytes = static_cast<std::uint32_t>(LatchedBytes(ViewFrameLatched(view.resources->frameConstants.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], 0)));
				if (auto& layout = producer.latches.views[v]; bytes > layout.frameBytes)
					layout.frameBytes = (bytes + 4095u) & ~4095u;
			}
			ViewPlan plan;
			PlanViewBuckets(traits, producer.mainLayout, &a_make.lookups, a_make.indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, plan);
			// The forward range's table entry and count word after the buckets'.
			RequireView(a_build, *view.resources, static_cast<std::uint32_t>(std::max<std::size_t>(plan.map.size(), request.pipelines)),
				static_cast<std::uint32_t>(plan.buckets.size() + (plan.forwardCapacity ? 1 : 0)), plan.draws);
		}
	}

	void IndirectDraws::Impl::RequireRevisionLatched(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		// The main segments' Z plans and latched copies, from what the shapes are made of: the set holds their latched blocks.
		auto& zPlans = a_make.zPlans;
		auto& mainLatched = a_make.mainLatched;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			if (depthOnly && r.pool)
				PlanZBuckets(producer.mainLayout, a_make.lookups, a_make.tables, a_make.indirect, zPlans[shape]);
			mainLatched[shape] = MainLatchedLayout(r, depthOnly, inputs.blockSizes[shape], static_cast<std::uint32_t>(zPlans[shape].calls.size()));
			RequireLatchedBlock(a_build, producer.latchHandles.mainLatched[shape], mainLatched[shape]);
		}
	}

	void IndirectDraws::Impl::MakeMainShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		// The heaps every recording binds: the graph's own, as built (posted at the build point).
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			auto& mainShape = producer.made.main[shape];
			mainShape = nullptr;
			// A segment no commit has captured for yet: no viewport, no frame blocks.
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			auto in = MainShapeInputsOf(r, a_layout, a_sealed, depthOnly, inputs.toggles.cullMode);
			in.latch = a_sealed.Latch(handles.main);
			in.latchLayout = producer.latches.main;
			in.viewport = inputs.viewport[shape];
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = a_make.indirect;
			in.zCalls = a_make.zPlans[shape].calls;
			in.zPlan = std::make_shared<const ZBucketPlan>(std::move(a_make.zPlans[shape]));
			in.latched.copies = std::move(a_make.mainLatched[shape]);
			if (!in.latched.copies.empty())
				in.latched.latch = a_sealed.Latch(handles.mainLatched[shape]);
			mainShape = MakeMainShape(in);
		}
	}

	void IndirectDraws::Impl::MakeViewShapes(const RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& indirect = a_make.indirect;
		const auto& viewOn = a_make.viewOn;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		// The views', from their resources and the main layout (the heaps the graph's); first person's render area is the colour commit's
		// viewport. The faces' lists hold each draw family's units (its faces.lists: tree LOD's shape slots; one unit group with its
		// tables: the least any of them holds).
		std::array<std::uint32_t, Extensions::kDrawFamilies> familyUnits{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			if (const auto& s = inputs.scene; s && faces.lists && FamilySetUp(*s, family)) {
				UnitGroup units;
				Api::DrawFamilyLists lists{ *s, {}, &units };
				faces.lists(lists);
				for (const auto& view : inputs.views)
					if (view.resources)
						for (const auto& list : view.resources->faceLists[d])
							units.Add(list);
				familyUnits[d] = static_cast<std::uint32_t>(std::min<std::uint64_t>(units.Units(a_sealed), UINT32_MAX));
			}
		}
		for (const auto& slot : kViewSlots) {
			const std::size_t v = ViewIndex(slot.view);
			const auto& traits = TraitsOf(slot.view);
			const auto& view = inputs.views[v];
			auto& shape = producer.made.views[v];
			shape = nullptr;
			// The view's latch and zeros as the set names them, at the layout they were required at.
			ViewLatches viewLatches;
			if (latches.viewsLaidOut[v])
				viewLatches = { a_sealed.Latch(handles.views[v]), a_sealed.Latch(handles.viewZeros[v]), latches.views[v] };
			const bool ready = traits.depthPass ? viewOn[v] && inputs.mainKnown[kColourShape] && viewLatches.latch : inputs.claims && inputs.scene;
			if (!view.resources || !indirect.valid || !ready)
				continue;
			ViewShapeInputs in;
			PlanViewBuckets(traits, a_layout, &a_make.lookups, indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, in.plan);
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			in.latches = &viewLatches;
			// The view's own frame constants (the faces': its commit's, from this frame's sources) or the colour segment's.
			const auto& own = view.resources->frameConstants;
			in.frameConstantsAddress = own ? view.resources->frameConstantsAddress : r.frameConstantsAddress;
			if (own && viewLatches.latch && viewLatches.layout.frameBytes) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				in.frameCopies = ViewFrameLatched(own.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], viewLatches.layout.FrameOffset());
				if (LatchedBytes(in.frameCopies) > viewLatches.layout.Bytes())
					in.frameCopies.clear();  // laid out for smaller blocks: the commit stages them
			}
			in.width = view.width;
			in.height = view.height;
			in.depthRange = view.depthRange;
			// The range the named version holds: the set holds this plan's draws (RequireView), so the plan never reaches past it.
			in.sequenceDraws = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_sealed.Of(view.resources->sequences).units, UINT32_MAX));
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d)
				if (viewRevisionFaces[d].valid() && a_make.familyOn[d] && familyUnits[d])
					in.faces[d] = { viewRevisionFaces[d], a_make.familyPipelines[d].drawSignature, familyUnits[d] };
			if (!traits.depthPass || ((!in.plan.buckets.empty() || in.plan.forwardCapacity) && viewLatches.zeros))
				shape = MakeViewShape(*view.resources, in);
		}
	}

	IndirectDraws::Impl::ShapesKey IndirectDraws::Impl::RevisionShapesKey(const NamedVersions& a_sealed, const RevisionMake& a_make, std::size_t a_pipelines) const
	{
		ShapesKey key;
		key.fill(0xcbf29ce484222325ull);
		ShapesKeyMixer mixer{ key };
		auto& group = mixer.group;
		auto mix = [&mixer](std::uint64_t a_value) { mixer.Mix(a_value); };
		auto ptr = [&mixer](const void* a_pointer) { mixer.Ptr(a_pointer); };
		auto raw = [&mixer]<class T>(const T& a_value) { mixer.Raw(a_value); };
		// The revision's inputs (RevisionInputs): what its shapes are made from besides the request.
		const auto& inputs = producer.inputs;
		// The request's lookups (the publication's, which the shapes are made from).
		const auto& lookups = a_make.lookups;
		// What every shape names: the sealed set (its versions' addresses and capacities, and its latch blocks: a replaced block moves
		// its serial; the same serial while nothing changed) and the main layout, the heaps, the toggles (the pipeline sets: each
		// participant's, below).
		group = kKeyVersions;
		mix(a_sealed.Serial());
		if (const auto& layout = revisions.layout) {
			mix(layout->zBucketsLayout);
			raw(std::array<std::uint32_t, 6>{ layout->sequenceDraws, layout->sequenceDecals, layout->terrainCapacity[0], layout->terrainCapacity[1], layout->forwardCapacity,
				layout->refractionCapacity });
		}
		group = kKeyPipelines;
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
		mix(a_pipelines);  // the request's pipeline slots (the publication's tables, not the coordinator's)
		// Each participant's parts, in order: the main segments', the shadow engine's, the views'.
		for (const auto* participant : participants)
			participant->MixShapesKey(a_make, mixer);
		return key;
	}

	void IndirectDraws::Impl::MixMainShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		auto& group = a_mixer.group;
		auto mix = [&a_mixer](std::uint64_t a_value) { a_mixer.Mix(a_value); };
		auto ptr = [&a_mixer](const void* a_pointer) { a_mixer.Ptr(a_pointer); };
		auto raw = [&a_mixer]<class T>(const T& a_value) { a_mixer.Raw(a_value); };
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The main pipeline set.
		group = kKeyPipelines;
		ptr(a_make.indirect.version.get());
		mix(a_make.indirect.valid);
		// The main segments': their latch's layout, latched targets, and what their commits captured (viewport, frame blocks). The
		// blocks are the set's (its serial).
		group = kKeyMain;
		const auto& r = *inputs.main;
		ptr(&r);
		raw(latches.main);
		ptr(r.latchedTargets.load(std::memory_order_acquire).get());
		ptr(r.pool.get());
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			mix(inputs.mainKnown[shape]);
			raw(inputs.viewport[shape]);
			raw(inputs.blockSizes[shape]);
		}
	}

	void IndirectDraws::Impl::ViewParticipant::MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		// By draw family, its pipelines as the request's catalog has them (null: none).
		FamilyPipelinesSet families{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			families[d] = a_make.familyOn[d] ? &a_make.familyPipelines[d] : nullptr;
		owner.MixViewShapesKey(a_mixer, families);
	}

	void IndirectDraws::Impl::MixViewShapesKey(ShapesKeyMixer& a_mixer, const FamilyPipelinesSet& a_families) const
	{
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The views': their resources, latch layouts, targets, render area and depth range, the slots' pipelines, first person's forward slots,
		// the faces' draw families' (the request's catalog's).
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			const std::size_t v = ViewIndex(kind);
			a_mixer.group = slot.keyGroup;
			const auto& view = inputs.views[v];
			if (!view.resources)
				continue;
			a_mixer.Ptr(view.resources.get());
			// A view's own frame buffers' copies (its frame values' latched copies name them), made again with the main frame buffers' shape.
			if (const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire))
				a_mixer.Ptr(frameBuffers.get());
			a_mixer.Mix(latches.viewsLaidOut[v]);
			a_mixer.Raw(latches.views[v]);
			a_mixer.Raw(view.targets);
			a_mixer.Mix(view.width);
			a_mixer.Mix(view.height);
			a_mixer.Raw(view.depthRange);
			for (const auto& pipeline : viewRevisionPipelines[v]) {
				a_mixer.Mix(pipeline.index);
				a_mixer.Mix(pipeline.generation);
			}
			if (TraitsOf(kind).forwardPart)
				for (const std::uint8_t forward : viewForwardSlots)
					a_mixer.Mix(forward);
			if (TraitsOf(kind).trees)
				for (const auto& pipeline : viewRevisionFaces) {
					a_mixer.Mix(pipeline.index);
					a_mixer.Mix(pipeline.generation);
				}
		}
		// Every draw family slot's (tree LOD's), in the group of the view that draws them per face; each view whose shape is dropped while
		// it is off, its toggle.
		for (const auto& slot : kViewSlots) {
			if (!TraitsOf(slot.view).trees)
				continue;
			a_mixer.group = slot.keyGroup;
			if (const auto& sceneState = inputs.scene)
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
					if (const auto* family = a_families[d]) {
						a_mixer.Mix(family->drawSignature.index);
						a_mixer.Mix(family->drawSignature.generation);
					}
					a_mixer.Mix(a_families[d] ? 1u : 0u);
					a_mixer.Mix(FamilySetUp(*sceneState, static_cast<Api::DrawFamily>(d)) ? 1u : 0u);
				}
		}
		for (const auto& slot : kViewSlots)
			if (Extensions::Views().views[ViewIndex(slot.view)].life == Api::ViewShapeLife::DropWhenOff) {
				a_mixer.group = slot.keyGroup;
				a_mixer.Mix(Extensions::ViewOn(inputs.toggles, slot.view));
			}
	}
}
