// The revision producer, on the snapshot builder (DCLF's preparation pool, one pass at a time): MakeRevision makes a scene
// revision's shapes and latches from a publication and the frame's posted RevisionInputs, then AssembleRevision seals them. Also the
// main shape's plans (shared with the commits).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
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
			add(constants, sizeof(Scene::FrameView::FrameCapture::lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			add(constants, sizeof(LodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
			add(constants, sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
			add(constants, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
			add(constants, sizeof(ExtrasFrame::projectedGlobals), kExtrasPixelFrameOffset);
			// The frame constant rows, in ascending row order (the Effect port's camera planes, light colour and blood).
			for (const auto& row : Extensions::RegisteredFrameConstantRows())
				add(constants, sizeof(float[4]), FrameConstantRowOffset(row.row));
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

	/** @brief A make's publication (what its shapes are made from) and what its requirements hand the shapes. */
	struct IndirectDraws::Impl::RevisionMake
	{
		const ScenePublication& publication;
		const SceneTables& tables;
		const Lookups& lookups;
		const PipelineCatalog* catalog = nullptr;
		IndirectState indirect;
		ShadowIndirectState shadowIndirect;
		// By draw family, its pipelines as the catalog has them, and whether it has them.
		std::array<FamilyPipelines, Extensions::kDrawFamilies> familyPipelines;
		std::array<bool, Extensions::kDrawFamilies> familyOn{};
		std::array<bool, kViews> viewOn{};
		std::array<ZBucketPlan, 2> zPlans;
		std::array<std::vector<LatchedCopy>, 2> mainLatched;
		std::vector<LatchedCopy> shadowLatched;
	};

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
		// a set that holds every one.
		RevisionMake make{ a_publication, tables, lookups, catalog };
		auto build = buffers.Begin();
		RequireRevisionSet(make, build);
		RequireRevisionViews(make, build);
		RequireRevisionLatched(make, build);
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
		// make keeps the last ones.
		if (producer.drawBound.shadowVersion != shadowBoundChecked) {
			shadowBoundChecked = producer.drawBound.shadowVersion;
			if (!ShadowShapesHold(ShadowBoundsOf(producer.drawBound, lookups)))
				++shadowBoundOutgrown;
		}
		FamilyPipelinesSet families{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			families[d] = make.familyOn[d] ? &make.familyPipelines[d] : nullptr;
		const auto key = RevisionShapesKey(sealed, make.indirect, make.shadowIndirect, lookups, request.pipelines, families);
		if (key == revisionShapesKey) {
			++shapesKept;
			AssembleRevision(a_frame);
			return;
		}
		for (std::size_t g = 0; g < key.size(); ++g)
			shapesMadeBy[g] += key[g] != revisionShapesKey[g] ? 1 : 0;
		revisionShapesKey = key;
		++shapesMade;
		MakeMainShapes(make, sealed, mainLayout);
		MakeShadowShapes(make, sealed);
		MakeViewShapes(make, sealed, mainLayout);
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
		a_make.shadowIndirect = a_make.catalog ? GetShadowIndirectState(*a_make.catalog, inputs.shadowFormat) : ShadowIndirectState{};
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
		// The shadow and view buffers for what the revisions' shapes name, required at every make.
		const auto& shadowState = inputs.shadow;
		if (shadowState && a_make.shadowIndirect.valid) {
			// The slots hold every draw the scene can produce.
			RequireShadowSequences(a_build, tables, a_make.publication.tablesGeneration);
			// The latch for what the frame's epochs can name (over every mode): its views, key slots, the states registered and the
			// sun's processes, a view's each at most. The key slots of the publication's lookups (the scene lane resolved
			// the slots of these tables' keys already; append-only per instance), and the keys its casters and occluders use.
			const std::size_t slotKeys = std::max(a_make.lookups.shadowSlotKeys.size(), request.shadowSlotKeys);
			const std::size_t keys = slotKeys + request.shadowKeys;
			const auto views = static_cast<std::uint32_t>(inputs.shadowPlacements.size());
			ReserveShadowLatch(a_build, *shadowState, views, static_cast<std::uint32_t>(keys), inputs.shadowRasterStates, std::max(views, latches.shadow.sunProcesses));
		}
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
		// The main segments' Z plans and latched copies, and the shadow commit's, from what the shapes below are made of: the set holds
		// their latched blocks, then every latch at its layout as the reserves left it.
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
		if (const auto& shadowState = inputs.shadow; shadowState && a_make.shadowIndirect.valid) {
			a_make.shadowLatched = ShadowLatchedLayout(*shadowState);
			RequireLatchedBlock(a_build, producer.latchHandles.shadowLatched, a_make.shadowLatched);
		}
		RequireLatches(a_build);
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

	void IndirectDraws::Impl::MakeShadowShapes(const RevisionMake& a_make, const NamedVersions& a_sealed)
	{
		const auto& inputs = producer.inputs;
		auto& made = producer.made;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& shadowState = inputs.shadow;
		const auto& shadowIndirect = a_make.shadowIndirect;
		const auto& lookups = a_make.lookups;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		// The shadow and occlusion epochs': a shape for the placements and for each occlusion layout drawn lately. The last make's
		// shapes are what a layout's capacities grow from.
		std::array<std::vector<std::shared_ptr<const ShadowFrame>>, 2> previousShadow;
		for (std::size_t kind = 0; kind < 2; ++kind)
			previousShadow[kind] = std::exchange(made.shadow[kind], {});
		// The slots the frame made (as posted): a layout past them waits for them.
		const std::size_t shadowSlotCount = inputs.shadowSlots ? inputs.shadowSlots->sequences.size() : 0;
		if ((!inputs.shadowPlacements.empty() || !producer.recentOcclusionLayouts.empty()) && shadowState && shadowIndirect.valid) {
			// Sized from the scene's casting bound alone (ShadowBounds), not a payload: the shapes move with the scene's objects, not
			// with the frame's casters or publications. Indexed by what the named versions of the slots' buffers hold, the least slot's.
			const auto bounds = std::make_shared<const ShadowBounds>(ShadowBoundsOf(producer.drawBound, lookups));
			ShadowCapacity capacity;
			{
				UnitGroup sequences, counts;
				if (const auto& slotBuffers = inputs.shadowSlots) {
					for (const auto& buffer : slotBuffers->sequences)
						sequences.Add(buffer);
					for (const auto& buffer : slotBuffers->bucketCounts)
						counts.Add(buffer);
				}
				capacity.sequenceDraws = static_cast<std::uint32_t>(std::min<std::uint64_t>(sequences.Units(a_sealed), UINT32_MAX));
				capacity.bucketCountWords = static_cast<std::uint32_t>(std::min<std::uint64_t>(counts.Units(a_sealed), UINT32_MAX));
				capacity.viewSlots = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_sealed.Of(shadowState->viewBlocks.buffer).units, UINT32_MAX));
			}
			producer.shadowBucketWords = capacity.bucketCountWords;
			const std::uint64_t viewBlocks = a_sealed.Of(shadowState->viewBlocks.buffer).deviceAddress;
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
					if (std::find(seen.begin(), seen.end(), layouts) != seen.end() ||
						std::any_of(layouts.begin(), layouts.end(), [&](const auto& a_view) { return a_view.slot >= shadowSlotCount || a_view.slot >= capacity.viewSlots; }))
						continue;
					ShadowShapeInputs in;
					in.resourceHeap = resourceHeap;
					in.samplerHeap = samplerHeap;
					in.indirect = shadowIndirect;
					for (const auto& layout : layouts)
						in.rows.push_back(ShadowRowBuckets(producer.rowBuckets, layout.modeIndex, layout.rasterState, lookups, shadowIndirect));
					in.views = layouts;
					in.bounds = bounds.get();
					in.capacity = capacity;
					in.viewBlocks = viewBlocks;
					in.latch = a_sealed.Latch(handles.shadow);
					in.latchLayout = latches.shadow;
					in.zeros = a_sealed.Latch(handles.shadowZeros);
					// The last shape made for the layout: its slots' capacities only grow.
					for (const auto& candidate : previousShadow[kind])
						if (candidate && LayoutOf(*candidate) == layouts)
							in.previous = candidate;
					if (!occlusionKind) {
						in.latched.copies = a_make.shadowLatched;
						if (!in.latched.copies.empty())
							in.latched.latch = a_sealed.Latch(handles.shadowLatched);
					}
					made.shadow[kind].push_back(MakeShadowShape(*shadowState, in));
					seen.push_back(std::move(layouts));
				}
			}
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
			in.frameConstantsAddress = r.frameConstantsAddress;
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

	namespace Draws
	{
		/** @brief RevisionShapesKey's hash: each value mixed into the current group's word. */
		struct ShapesKeyMixer
		{
			ImplProducer::ShapesKey& key;
			std::size_t group = 0;
			void Mix(std::uint64_t a_value)
			{
				auto& h = key[group];
				h = (h ^ a_value) * 0x100000001b3ull;
				h ^= h >> 29;
			}
			void Ptr(const void* a_pointer) { Mix(reinterpret_cast<std::uintptr_t>(a_pointer)); }
			template <class T>
			void Raw(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				std::array<std::byte, (sizeof(T) + 7) / 8 * 8> bytes{};
				std::memcpy(bytes.data(), &a_value, sizeof(T));
				for (std::size_t b = 0; b < bytes.size(); b += 8) {
					std::uint64_t word;
					std::memcpy(&word, bytes.data() + b, 8);
					Mix(word);
				}
			}
		};
	}

	IndirectDraws::Impl::ShapesKey IndirectDraws::Impl::RevisionShapesKey(const NamedVersions& a_sealed, const IndirectState& a_indirect, const ShadowIndirectState& a_shadowIndirect,
		const Lookups& a_lookups, std::size_t a_pipelines, const FamilyPipelinesSet& a_families) const
	{
		ShapesKey key;
		key.fill(0xcbf29ce484222325ull);
		ShapesKeyMixer mixer{ key };
		auto& group = mixer.group;
		auto mix = [&mixer](std::uint64_t a_value) { mixer.Mix(a_value); };
		auto ptr = [&mixer](const void* a_pointer) { mixer.Ptr(a_pointer); };
		auto raw = [&mixer]<class T>(const T& a_value) { mixer.Raw(a_value); };
		// The revision's inputs (RevisionInputs) and latch layouts (the producer's): what its shapes are made from besides the request.
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The request's lookups (the publication's, which the shapes are made from).
		const auto& lookups = a_lookups;
		// What every shape names: the sealed set (its versions' addresses and capacities, and its latch blocks: a replaced block moves
		// its serial; the same serial while nothing changed) and the main layout, the pipeline sets, the heaps, the toggles.
		group = kKeyVersions;
		mix(a_sealed.Serial());
		if (const auto& layout = revisions.layout) {
			mix(layout->zBucketsLayout);
			raw(std::array<std::uint32_t, 6>{ layout->sequenceDraws, layout->sequenceDecals, layout->terrainCapacity[0], layout->terrainCapacity[1], layout->forwardCapacity,
				layout->refractionCapacity });
		}
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
		mix(a_pipelines);  // the request's pipeline slots (the publication's tables, not the coordinator's)
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
		// The shadow and occlusion epochs': the placements, the occlusion maps' layouts, the scene's casting bound, their latch's layout.
		group = kKeyShadow;
		if (const auto& s = inputs.shadow) {
			ptr(s.get());
			raw(latches.shadow);
			ptr(s->latchedTargets.load(std::memory_order_acquire).get());
			mix(inputs.shadowSlots ? inputs.shadowSlots->sequences.size() : 0);
		}
		for (const auto& view : inputs.shadowPlacements)
			raw(view);
		for (const auto& layouts : producer.recentOcclusionLayouts) {
			mix(layouts.size());
			for (const auto& view : layouts)
				raw(view);
		}
		group = kKeyBound;
		mix(shadowBoundOutgrown);
		MixViewShapesKey(mixer, a_families);
		return key;
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

	bool IndirectDraws::Impl::ShadowShapesHold(const ShadowBounds& a_bounds) const
	{
		// The producer's: its inputs' shadow resources and the shapes it made last.
		const auto& shadowState = producer.inputs.shadow;
		if (!shadowState)
			return true;
		const std::uint32_t words = producer.shadowBucketWords;
		std::vector<std::uint64_t> need;
		for (std::size_t kind = 0; kind < 2; ++kind)
			for (const auto& shape : producer.made.shadow[kind]) {
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
}
