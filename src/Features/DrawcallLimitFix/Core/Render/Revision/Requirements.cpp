// The revision producer's requirements (snapshot builder): what the scene's set must hold for a revision made now - the scene's
// tables, the main layout and the buffers it indexes, the shadow view slots and the views - and the latches kept with them. Every
// requirement is the publication's tables', the frame's posted counts' or the builds ahead's; a set is sealed only once it holds them
// all (SceneBufferSet), so a shape made from that set never indexes past a buffer it names.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF
{
	namespace
	{
		/** @brief The largest max count one indirect draw may have on this device (IndirectCommandsFeatureInfo::maxSequenceCount). */
		std::uint32_t DeviceMaxSequences()
		{
			static const std::uint32_t max = [] {
				::IndirectCommandsFeatureInfo indirect{};
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host || host->GetDesc().device.QueryFeatureInfo(&indirect.header) != rhi::Result::Ok)
					return 0u;
				logger::info("[DCLF] indirect draws: up to {} sequences per call", indirect.maxSequenceCount);
				return indirect.maxSequenceCount;
			}();
			return max;
		}

		/** @brief A draw range one call cannot execute or the sort cannot rank: a hard failure until draws split across calls. */
		[[noreturn]] void SequenceLimitFailure(const char* a_what, std::uint32_t a_draws, std::uint32_t a_limit)
		{
			const auto message = fmt::format("Drawcall Limit Fix: {} needs {} draws in one indirect draw, over the limit of {}", a_what, a_draws, a_limit);
			logger::critical("[DCLF] {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}

		void CheckSequenceLimits(const char* a_what, std::uint32_t a_draws, bool a_sorted)
		{
			const std::uint32_t device = DeviceMaxSequences();
			if (device && a_draws > device)
				SequenceLimitFailure(a_what, a_draws, device);
			if (a_sorted && a_draws > kSortRankLimit)
				SequenceLimitFailure(a_what, a_draws, kSortRankLimit);
		}

	}

	void IndirectDraws::Impl::RequireSceneTables(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables)
	{
		auto& buffers = SceneBufferSet::Get();
		const auto& inputs = producer.inputs;
		auto& s = a_scene;
		// An object slot of every per-object buffer (the depth segment's verdicts, the frustum stamps, the reported verdicts). The
		// payload's parts (records, extras, geometry rows, the scene list, the row tables) are not the set's: the payload holds them.
		UnitGroup objects;
		objects.Add(s.visibilityObjectReported);
		if (inputs.main)
			objects.Add(inputs.main->visibility).Add(inputs.main->frustum);
		if (inputs.shadow)
			objects.Add(inputs.shadow->visibility);
		objects.Require(a_build, a_tables.objects.size());
		std::uint64_t faceVertices = 0;
		for (const auto& stream : a_tables.faceStreams)
			if (stream.object != SceneTables::kNoFaceObject)
				faceVertices = std::max<std::uint64_t>(faceVertices, std::uint64_t(stream.region) + stream.vertexCount);
		buffers.Require(a_build, s.facePositions, faceVertices);
		// Tree rows, clocks and wind by tree slot.
		if (s.trees)
			UnitGroup{}.Add(s.trees).Add(s.treeClocks).Add(s.treeWindRows[0]).Add(s.treeWindRows[1]).Require(a_build, a_tables.trees.size());
		// Fade roots by root slot: the static rows, the GPU state, the programs, the animation stamps and the readbacks' words.
		if (s.fadeRoots) {
			UnitGroup roots;
			roots.Add(s.fadeRoots).Add(s.fadeRootPrograms).Add(s.fadeAnimated).Add(s.fadeStates).Add(s.fadeReported).Add(s.visibilityRootReported);
			roots.Add(s.fadeStatesOut[0]).Add(s.fadeStatesOut[1]);
			roots.Require(a_build, a_tables.fadeRoots.size());
		}
		// Portal programs, to the most a frame's encoding needs.
		buffers.Require(a_build, s.portalPrograms, inputs.portalWords);
		// The event lists, to the most a frame wanted (dropped roots append again); their staging sets follow the named capacity.
		if (s.fadeEvents && s.fadeWriteBack)
			buffers.Require(a_build, s.fadeEvents, s.fadeWriteBack->wanted.load(std::memory_order_acquire));
		if (s.visibilityEvents && s.visibilityReadback)
			buffers.Require(a_build, s.visibilityEvents, s.visibilityReadback->wanted.load(std::memory_order_acquire));
		// Tree LOD's tables by the mirror's posted shape and mesh slots; the views' faces' tree lists hold the same shape slots.
		if (s.treeLodCull) {
			UnitGroup shapes;
			shapes.Add(s.treeLodShapes).Add(s.treeLodInstances).Add(s.treeLodVisible);
			for (const auto& view : inputs.views)
				if (view.resources)
					for (const auto& visible : view.resources->treeVisible)
						shapes.Add(visible);
			shapes.Require(a_build, inputs.treeLodShapeSlots);
			buffers.Require(a_build, s.treeLodMeshes, inputs.treeLodMeshSlots);
		}
	}

	void IndirectDraws::Impl::RequireMain(GrowthBuild& a_build, Resources& a_resources, const SceneTables& a_tables, std::uint32_t a_generation)
	{
		auto& buffers = SceneBufferSet::Get();
		auto& r = a_resources;
		UpdateDrawBound(a_tables, a_generation);
		const auto& drawBound = producer.drawBound;
		std::uint32_t draws = drawBound.Draws();
		auto& layout = producer.mainLayout;
		if (!std::exchange(producer.mainLayoutMade, true))
			layout = InitialMainLayout();
		// Z-prepass buckets: each pipeline slot's draws, capacity kept while they fit and grown to a power of two past them, so the
		// recorded plain draws change only then.
		const auto slots = static_cast<std::uint32_t>(a_tables.pipelines.size());
		bool moved = layout.zBucketCapacity.size() < slots;
		if (moved)
			layout.zBucketCapacity.resize(slots, 0u);
		for (std::uint32_t p = 0; p < slots; ++p) {
			// Effect keys draw only in the forward range and have no Z-prepass pipeline.
			if (RasterEffect(a_tables.pipelines[p].rasterFlags))
				continue;
			if (const std::uint32_t pipelineDraws = drawBound.PipelineDraws(p); pipelineDraws > layout.zBucketCapacity[p]) {
				layout.zBucketCapacity[p] = std::bit_ceil(pipelineDraws);
				moved = true;
			}
		}
		if (moved) {
			layout.zBucketFirst.resize(layout.zBucketCapacity.size());
			std::uint64_t first = 0;
			for (std::size_t p = 0; p < layout.zBucketCapacity.size(); ++p) {
				layout.zBucketFirst[p] = static_cast<std::uint32_t>(first);
				first += layout.zBucketCapacity[p];
			}
			draws = static_cast<std::uint32_t>(std::max<std::uint64_t>(draws, std::min<std::uint64_t>(first, UINT32_MAX)));
			++layout.zBucketsLayout;
		} else if (!layout.zBucketFirst.empty()) {
			draws = std::max(draws, layout.zBucketFirst.back() + layout.zBucketCapacity.back());
		}
		// A terrain group 1 bucket has an equal twin for its near blocks (PlanZBuckets). The colour segment's terrain groups take
		// ranges of phase 2's (kTerrainGroups), sized like buckets.
		std::uint64_t terrainTwins = 0, terrainSlots = 0;
		std::array<std::uint64_t, kTerrainGroups> terrainDraws{};
		for (std::uint32_t p = 0; p < slots; ++p) {
			const std::uint32_t group = RasterTerrainGroup(a_tables.pipelines[p].rasterFlags);
			if (!group)
				continue;
			terrainDraws[group - 1] += drawBound.PipelineDraws(p);
			if (group == 1) {
				terrainTwins += layout.zBucketCapacity[p];
				++terrainSlots;
			}
		}
		for (std::uint32_t g = 0; g < kTerrainGroups; ++g)
			if (terrainDraws[g] > layout.terrainCapacity[g])
				layout.terrainCapacity[g] = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(terrainDraws[g], 1u << 31)));
		// The forward range, sized like the terrain ranges, twice over (staging and sorted halves), after them in phase 2's.
		// Refraction slots' draws go to the refraction range after the forward halves, sized likewise.
		std::uint64_t forwardDraws = 0, refractionDraws = 0;
		for (std::uint32_t p = 0; p < slots; ++p)
			if (RasterForward(a_tables.pipelines[p].rasterFlags))
				(RasterRefraction(a_tables.pipelines[p].rasterFlags) ? refractionDraws : forwardDraws) += drawBound.PipelineDraws(p);
		if (forwardDraws > layout.forwardCapacity)
			layout.forwardCapacity = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(forwardDraws, 1u << 30)));
		if (refractionDraws > layout.refractionCapacity)
			layout.refractionCapacity = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(refractionDraws, 1u << 30)));
		const std::uint64_t phaseDraws = (layout.zBucketFirst.empty() ? 0ull : std::uint64_t(layout.zBucketFirst.back()) + layout.zBucketCapacity.back()) + terrainTwins;
		const std::uint64_t terrainRange = std::uint64_t(layout.terrainCapacity[0]) + layout.terrainCapacity[1] + 2ull * layout.forwardCapacity + layout.refractionCapacity;
		draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>({ draws, phaseDraws, terrainRange }), UINT32_MAX));
		// The buckets' count words, each phase's.
		layout.buckets = static_cast<std::uint32_t>(layout.zBucketCapacity.size() + terrainSlots);
		UnitGroup{}.Add(r.zBucketCounts[0]).Add(r.zBucketCounts[1]).Require(a_build, layout.buckets);
		// The main latch's layout holds every bucket (its block: RequireLatches).
		ReserveMainLatch(producer.latches.main.cascades, producer.latches.main.shadowVolumes, layout.buckets);
		std::uint32_t decals = 0;
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			decals = std::max(decals, a_tables.decalCount[group]);
		CheckSequenceLimits("the main pass", draws, true);
		CheckSequenceLimits("a decal group", decals, false);
		// The sequence ranges: doubled past the bound, the buffers sized to them exactly.
		if (draws > layout.sequenceDraws || decals > layout.sequenceDecals) {
			const std::uint32_t newDraws = org::services::GrowthPolicy::Doubling().NextAs(layout.sequenceDraws, draws),
								newDecals = org::services::GrowthPolicy::Doubling().NextAs(layout.sequenceDecals, decals);
			logger::info("[DCLF] main sequences: {} draws and {} per decal group grown to {} and {}", layout.sequenceDraws, layout.sequenceDecals, newDraws, newDecals);
			layout.sequenceDraws = newDraws;
			layout.sequenceDecals = newDecals;
		}
		buffers.Require(a_build, r.sequences, SequenceSlots(layout.sequenceDraws, layout.sequenceDecals));
		if (r.sort) {
			buffers.Require(a_build, r.sort->staging, layout.sequenceDraws);
			buffers.Require(a_build, r.sort->ranks, layout.sequenceDraws);
		}
	}

	void IndirectDraws::Impl::RequireShadowSequences(GrowthBuild& a_build, const SceneTables& a_tables, std::uint32_t a_generation)
	{
		UpdateDrawBound(a_tables, a_generation);
		const std::uint32_t draws = producer.drawBound.Draws();
		CheckSequenceLimits("a shadow view", draws, false);
		// Every slot the frame made (as posted) holds the scene's draw bound: a view's claims always fit the slot they draw in.
		UnitGroup slots;
		if (const auto& slotBuffers = producer.inputs.shadowSlots)
			for (const auto& sequences : slotBuffers->sequences)
				slots.Add(sequences);
		slots.Require(a_build, draws);
	}

	void IndirectDraws::Impl::ReserveShadowLatch(GrowthBuild& a_build, ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates,
		std::uint32_t a_sunProcesses)
	{
		auto& latches = producer.latches;
		if (!latches.shadowLaidOut)
			return;
		auto& buffers = SceneBufferSet::Get();
		ShadowLatchLayout layout = latches.shadow;
		const std::uint32_t slots = kFirstShadowViewSlot + a_views;
		const bool newSlots = slots > layout.viewSlots;
		if (newSlots) {
			// The slots' buffers are the render thread's to make (ShadowSlotsRequest below: a graph build declares them).
			layout.viewSlots = org::services::GrowthPolicy::Doubling().NextAs(layout.viewSlots, slots);
			logger::info("[DCLF] shadow view slots: {} grown to {}", latches.shadow.viewSlots, layout.viewSlots);
		}
		if (a_keys > layout.keySlots) {
			layout.keySlots = org::services::GrowthPolicy::Doubling().NextAs(layout.keySlots, a_keys);
			logger::info("[DCLF] shadow key slots: {} grown to {}", latches.shadow.keySlots, layout.keySlots);
		}
		// The view blocks (rewritten whole by every epoch: nothing to fill) hold every view slot, and every slot's bucket counts a word
		// per key slot; until a set holds them, no revision is sealed.
		buffers.Require(a_build, a_shadow.viewBlocks.buffer, layout.viewSlots);
		UnitGroup counts;
		if (const auto& slotBuffers = producer.inputs.shadowSlots)
			for (const auto& words : slotBuffers->bucketCounts)
				counts.Add(words);
		counts.Require(a_build, layout.keySlots);
		if (a_rasterStates > layout.rasterStates) {
			layout.rasterStates = org::services::GrowthPolicy::Doubling().NextAs(layout.rasterStates, a_rasterStates);
			logger::info("[DCLF] shadow view rasterizer state rows: {} grown to {}", latches.shadow.rasterStates, layout.rasterStates);
		}
		if (a_sunProcesses > layout.sunProcesses) {
			layout.sunProcesses = org::services::GrowthPolicy::Doubling().NextAs(layout.sunProcesses, a_sunProcesses);
			logger::info("[DCLF] sun full-frustum processes: {} grown to {}", latches.shadow.sunProcesses, layout.sunProcesses);
		}
		if (layout == latches.shadow)
			return;
		// A new layout: the next seal makes a new block (RequireLatches). Every execution writes its frame slot's region whole; frames
		// in flight keep the block their set names.
		latches.shadow = layout;
		// The render thread makes the slots' buffers at the sizes asked of the others and builds the graph that declares them
		// (ServeRevisionRequests). No revision is sealed until that build.
		if (newSlots) {
			std::uint64_t draws = 64;
			if (const auto& slotBuffers = producer.inputs.shadowSlots)
				for (const auto& sequences : slotBuffers->sequences)
					draws = std::max(draws, buffers.Asked(a_build, sequences));
			shadowSlotsSlot.Post(std::make_unique<ShadowSlotsRequest>(
				ShadowSlotsRequest{ layout.viewSlots, static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX)), std::max(layout.keySlots, 64u) }));
			producer.buildWanted = producer.inputs.buildGeneration + 1;
		}
	}

	void IndirectDraws::Impl::RequireView(GrowthBuild& a_build, ViewResources& a_view, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws)
	{
		const auto& traits = *a_view.traits;
		const std::size_t v = ViewIndex(traits.kind);
		if (!producer.latches.viewsLaidOut[v])
			return;
		auto& buffers = SceneBufferSet::Get();
		ViewLatchLayout layout = producer.latches.views[v];
		if (a_slots > layout.slots)
			layout.slots = org::services::GrowthPolicy::Doubling().NextAs(layout.slots, a_slots);
		if (a_buckets > layout.buckets)
			layout.buckets = org::services::GrowthPolicy::Doubling().NextAs(layout.buckets, a_buckets);
		if (!(layout == producer.latches.views[v])) {
			// A new layout: the next seal makes a new block (RequireLatches); frames in flight keep their own.
			producer.latches.views[v] = layout;
			logger::info("[DCLF] {} latch: {} pipeline slots, {} buckets", traits.name, layout.slots, layout.buckets);
		}
		// A face's bucket count words for the latch's buckets, and a face's range of the sequences for the plan's draws (its buckets
		// and forward range): a set that does not hold them is never sealed, so the view's shape never indexes past them.
		for (std::uint32_t f = 0; f < traits.faces; ++f)
			buffers.Require(a_build, a_view.bucketCounts[f], layout.buckets);
		buffers.Require(a_build, a_view.sequences, a_draws);
	}

	void IndirectDraws::Impl::RequireLatches(GrowthBuild& a_build)
	{
		const auto& latches = producer.latches;
		const auto& handles = producer.latchHandles;
		if (!handles)
			return;
		// Each latch at its layout: another layout is a new block, so a frame in flight never reads one laid out anew.
		if (latches.mainLaidOut)
			SceneBufferSet::Require(a_build, handles.main, latches.main.Bytes(), LatchLayoutKey(latches.main));
		// The latched copies' zeros (one region for every frame slot, zeroed when made): a view's draw count and bucket counts, kept
		// while they hold the layout's.
		if (latches.shadowLaidOut) {
			SceneBufferSet::Require(a_build, handles.shadow, latches.shadow.Bytes(), LatchLayoutKey(latches.shadow));
			SceneBufferSet::Require(a_build, handles.shadowZeros, std::max<std::uint32_t>(latches.shadow.keySlots * 4, sizeof(kZeroCounts)), 0);
		}
		for (std::size_t v = 0; v < kViews; ++v) {
			if (!latches.viewsLaidOut[v])
				continue;
			const auto& layout = latches.views[v];
			SceneBufferSet::Require(a_build, handles.views[v], layout.Bytes(), LatchLayoutKey(layout));
			SceneBufferSet::Require(a_build, handles.viewZeros[v], std::max<std::uint32_t>(layout.buckets * 4, sizeof(kZeroCounts)), 0);
		}
	}
}
