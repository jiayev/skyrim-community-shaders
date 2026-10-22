// The revision producer's requirements (snapshot builder): what the scene's set must hold for a revision made now - the scene's
// tables, the main layout and the buffers it indexes, the shadow view slots and the views - and the latches kept with them. Every
// requirement is the publication's tables', the frame's posted counts' or the builds ahead's; a set is sealed only once it holds them
// all (SceneBufferSet), so a shape made from that set never indexes past a buffer it names.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"

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

		/**
		 * @brief The pipeline slots' capacities (MainLayout::slotCapacity: each slot's draws, kept while they fit and grown to a power of two
		 * past them) and the phase 2 ranges summed from them: each terrain group's and the forward range's (their state classes' sub-ranges
		 * are their slots', PlanSetClasses), and refraction's (a plain draw: a power of two past its slots' draws). Returns the main
		 * sub-ranges' total, which the draw range must hold. A slot's range is its key's, as BuildDrawsCS routes its objects (PlanSetClasses'
		 * SlotClassRange): a decal group's (no slot capacity: decal ranges are sequenceDecals), forward, a terrain group's, else main.
		 */
		std::uint64_t GrowSlotRanges(MainLayout& a_layout, const DrawBoundStore& a_bound, const SceneTables& a_tables)
		{
			const auto slots = static_cast<std::uint32_t>(a_tables.pipelines.size());
			if (a_layout.slotCapacity.size() < slots) {
				a_layout.slotCapacity.resize(slots, 0u);
				++a_layout.slotsLayout;
			}
			std::uint64_t main = 0, forward = 0, refractionDraws = 0;
			std::array<std::uint64_t, kTerrainGroups> terrain{};
			for (std::uint32_t p = 0; p < slots; ++p) {
				const std::uint32_t flags = a_tables.pipelines[p].rasterFlags;
				const std::uint32_t draws = a_bound.PipelineDraws(p);
				if (RasterDecalGroup(flags))
					continue;
				if (RasterForward(flags) && RasterRefraction(flags)) {
					refractionDraws += draws;
					continue;
				}
				auto& capacity = a_layout.slotCapacity[p];
				if (draws > capacity) {
					capacity = std::bit_ceil(std::min(draws, 1u << 30));
					++a_layout.slotsLayout;
				}
				if (RasterForward(flags))
					forward += capacity;
				else if (const std::uint32_t group = RasterTerrainGroup(flags))
					terrain[group - 1] += capacity;
				else
					main += capacity;
			}
			for (std::uint32_t g = 0; g < kTerrainGroups; ++g)
				a_layout.terrainCapacity[g] = static_cast<std::uint32_t>(std::min<std::uint64_t>(terrain[g], 1u << 31));
			a_layout.forwardCapacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(forward, 1u << 30));
			if (refractionDraws > a_layout.refractionCapacity)
				a_layout.refractionCapacity = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(refractionDraws, 1u << 30)));
			return main;
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
			objects.Add(inputs.main->visibility).Add(inputs.main->frustum).Add(inputs.main->retests);
		if (inputs.main && inputs.main->writers)
			objects.Add(inputs.main->writers);
		if (inputs.shadow)
			objects.Add(inputs.shadow->visibility);
		objects.Require(a_build, a_tables.objects.size());
		// The features' scene resources (Api::SceneResources::require: the vertex stream's first, its rows' vertices; Features/Fades'
		// buffers last).
		Extensions::RequireSceneResources(a_build, a_tables, s.sceneResources, inputs.resourceInputs);
		// The visibility readback's words by root slot, to the root states' family's rows.
		if (s.visibilityRootReported)
			buffers.Require(a_build, s.visibilityRootReported, Extensions::TableFamilyRows(a_tables.families, Extensions::RootStatesFamily()));
		// The event list, to the most a frame wanted (dropped roots append again); its staging sets follow the named capacity.
		if (s.visibilityEvents && s.visibilityReadback)
			buffers.Require(a_build, s.visibilityEvents, s.visibilityReadback->wanted.load(std::memory_order_acquire));
		// The views' faces' lists hold each draw family's units, as its scene resources posted them (its faces.lists: tree LOD's shape
		// slots; its tables are required by its scene resources, Api::SceneResources::require).
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& entry = Extensions::DrawFamilyOf(family);
			if (!entry.contribution.faces.lists || !FamilySetUp(s, family))
				continue;
			Api::DrawFamilyLists lists{ s, Extensions::SceneResourceInputsOf(inputs.resourceInputs, entry.contribution.resources) };
			entry.contribution.faces.lists(lists);
			UnitGroup faces;
			for (const auto& view : inputs.views)
				if (view.resources)
					for (const auto& list : view.resources->faceLists[d])
						faces.Add(list);
			faces.Require(a_build, lists.required);
		}
	}

	void IndirectDraws::Impl::RequireMain(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make)
	{
		auto& buffers = SceneBufferSet::Get();
		auto& r = a_resources;
		const SceneTables& tables = a_make.tables;
		// The draw bound as the make brought it up to date from them (MakeRevision, before the reuse key reads it).
		const auto& drawBound = producer.drawBound;
		std::uint32_t draws = drawBound.Draws();
		auto& layout = producer.mainLayout;
		if (!std::exchange(producer.mainLayoutMade, true))
			layout = InitialMainLayout();
		// Z-prepass buckets: each pipeline slot's draws, capacity kept while they fit and grown to a power of two past them, so the
		// recorded plain draws change only then.
		const auto slots = static_cast<std::uint32_t>(tables.pipelines.size());
		bool moved = layout.zBucketCapacity.size() < slots;
		if (moved)
			layout.zBucketCapacity.resize(slots, 0u);
		for (std::uint32_t p = 0; p < slots; ++p) {
			// A family key not a depth segment input (ProgramBuild::ForwardOrDecal, the Effect port's) draws only in the forward range and
			// has no Z-prepass pipeline.
			if (!Extensions::FamilyRules(tables.pipelines[p].rasterFlags).depthSegment)
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
		// A terrain group 1 bucket has an equal twin for its near blocks (PlanZBuckets).
		std::uint64_t terrainTwins = 0, terrainSlots = 0;
		for (std::uint32_t p = 0; p < slots; ++p)
			if (RasterTerrainGroup(tables.pipelines[p].rasterFlags) == 1) {
				terrainTwins += layout.zBucketCapacity[p];
				++terrainSlots;
			}
		// The colour segment's ranges from the slots' capacities: the main draws' (in the draw range), the terrain groups' (ranges of phase
		// 2's, kTerrainGroups), the forward range twice over (staging and sorted halves) after them, then refraction's.
		const std::uint64_t mainDraws = GrowSlotRanges(layout, drawBound, tables);
		const std::uint64_t phaseDraws = (layout.zBucketFirst.empty() ? 0ull : std::uint64_t(layout.zBucketFirst.back()) + layout.zBucketCapacity.back()) + terrainTwins;
		const std::uint64_t terrainRange = std::uint64_t(layout.terrainCapacity[0]) + layout.terrainCapacity[1] + 2ull * layout.forwardCapacity + layout.refractionCapacity;
		draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>({ draws, phaseDraws, terrainRange, mainDraws }), UINT32_MAX));
		// The buckets' count words, each phase's.
		layout.buckets = static_cast<std::uint32_t>(layout.zBucketCapacity.size() + terrainSlots);
		UnitGroup{}.Add(r.zBucketCounts[0]).Add(r.zBucketCounts[1]).Require(a_build, layout.buckets);
		// The main latch's layout holds every bucket (its block: RequireLatches).
		ReserveMainLatch(producer.latches.main.cascades, producer.latches.main.shadowVolumes, layout.buckets);
		std::uint32_t decals = 0;
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			decals = std::max(decals, tables.rangeCount[group]);
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
		// The state classes' sub-ranges of these ranges (their decal sub-ranges' count too), before the sequences are asked for them.
		RequireMainClasses(a_build, r, a_make);
		buffers.Require(a_build, r.sequences, SequenceSlots(layout.sequenceDraws, layout.sequenceDecals, layout.decalRanges));
		if (r.sort) {
			buffers.Require(a_build, r.sort->staging, layout.sequenceDraws);
			buffers.Require(a_build, r.sort->ranks, layout.sequenceDraws);
		}
	}

	void IndirectDraws::Impl::RequireMainClasses(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make)
	{
		auto& layout = producer.mainLayout;
		auto plan = std::make_shared<SetClassPlan>();
		PlanSetClasses(layout, a_make.lookups, a_make.tables, a_make.indirect, *plan);
		// The decal sub-ranges the sequences hold: doubled past the (group, class) pairs present, so a class's first decal of a group moves
		// the decal region's size only now and then.
		if (plan->decalRanges > layout.decalRanges) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling().NextAs(layout.decalRanges, plan->decalRanges);
			logger::info("[DCLF] main sequences: {} decal sub-ranges grown to {} ({} decal groups' state classes)", layout.decalRanges, grown, plan->decalRanges);
			layout.decalRanges = grown;
		}
		// A count word per (class, range), and the latch's class table and map for the set version's classes and set indices.
		SceneBufferSet::Get().Require(a_build, a_resources.classCounts, std::max(plan->Words(), 1u));
		const auto& latch = producer.latches.main;
		ReserveMainLatch(latch.cascades, latch.shadowVolumes, latch.buckets, static_cast<std::uint32_t>(plan->classes.size()),
			static_cast<std::uint32_t>(plan->map.size() - kClassHeaderWords));
		a_make.classPlan = std::move(plan);
	}

	void ShadowEngine::RequireShadowSequences(ImplProducer& a_producer, GrowthBuild& a_build)
	{
		const auto& producer = a_producer.producer;
		const std::uint32_t draws = producer.drawBound.Draws();
		CheckSequenceLimits("a shadow view", draws, false);
		// Every slot the frame made (as posted) holds the scene's draw bound: a view's claims always fit the slot they draw in.
		UnitGroup slots;
		if (const auto& slotBuffers = producer.inputs.shadowSlots)
			for (const auto& sequences : slotBuffers->sequences)
				slots.Add(sequences);
		slots.Require(a_build, draws);
	}

	void ShadowEngine::ReserveShadowLatch(ImplProducer& a_producer, GrowthBuild& a_build, ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys,
		std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses)
	{
		auto& producer = a_producer.producer;
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
			a_producer.shadowSlotsSlot.Post(std::make_unique<ShadowSlotsRequest>(
				ShadowSlotsRequest{ layout.viewSlots, static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX)), std::max(layout.keySlots, 64u) }));
			producer.buildWanted = producer.inputs.buildGeneration + 1;
		}
	}

	void IndirectDraws::Impl::RequireView(GrowthBuild& a_build, ViewResources& a_view, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws,
		std::uint32_t a_classMapWords)
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
		if (a_classMapWords > layout.classMapWords)
			layout.classMapWords = org::services::GrowthPolicy::Doubling(256).NextAs(layout.classMapWords, a_classMapWords);
		if (!(layout == producer.latches.views[v])) {
			// A new layout: the next seal makes a new block (RequireLatches); frames in flight keep their own.
			producer.latches.views[v] = layout;
			logger::info("[DCLF] {} latch: {} pipeline slots, {} buckets, {} class map words", traits.name, layout.slots, layout.buckets, layout.classMapWords);
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
