// The shadow engine as the shadow shape kind's revision participant (Revision/Participant.h): on the snapshot builder, its
// requirements of the set (the occlusion layouts lately drawn, the slots' sequences, the latch), its latched copies, the casting bound
// its shapes outgrew, its parts of the shapes key and its shapes, and the build ahead's shadow payload with its exclusions; on the
// render thread, its revision inputs and its part of the ahead context.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowExclusionBuild.h"

namespace DCLF
{
	namespace
	{
		/**
		 * @brief A mode's exclusion from a_payload (Api::ShadowExclusionContribution: Features/Shadows/Local's builder and caches), over
		 * a_candidates. Null without the contribution.
		 */
		std::shared_ptr<void> BuildExclusion(Api::ShadowSource a_source, const std::shared_ptr<const EntrySpace>& a_candidates,
			const ShadowPayload& a_payload, std::uint32_t a_mode, const SceneTables& a_tables)
		{
			Api::ShadowExclusionBuild build(a_source, a_candidates, a_tables, a_payload.regionInputs, a_payload.inputList);
			build.modeBits = a_payload.modeBits[a_mode];
			build.membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
			build.frameNumber = a_payload.inputs.frameNumber;
			build.tablesGeneration = a_payload.inputs.tablesGeneration;
			build.keptBuild = a_payload.keptBuild;
			build.movesValid = a_payload.movesValid[a_mode];
			build.joined = a_payload.joined[a_mode];
			build.left = a_payload.left[a_mode];
			Extensions::BuildShadowExclusion(build);
			return std::move(build.result);
		}
	}

	void ShadowEngine::RequireSequences(RevisionMake& a_make, GrowthBuild& a_build)
	{
		auto& producer = a_make.producer.producer;
		const auto& request = a_make.publication.revision;
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		auto& scratch = revisionScratch;
		scratch.latched.clear();
		scratch.indirect = a_make.catalog ? GetShadowIndirectState(*a_make.catalog, inputs.shadowFormat) : ShadowIndirectState{};
		// The occlusion maps' layout as last captured: one the revisions make a shape for (OcclusionReady asks for it). The inputs carry
		// every captured map's; the maps on under the publication's toggles are the revision's (the frame's once it is installed).
		{
			auto layouts = inputs.occlusionLayouts;
			std::erase_if(layouts, [&producer](const ShadowViewLayout& a_layout) {
				return !Extensions::OcclusionOn(producer.toggles, OcclusionOfMode(a_layout.modeIndex));
			});
			auto& recent = producer.recentOcclusionLayouts;
			if (!layouts.empty() && (recent.empty() || recent.front() != layouts)) {
				std::erase(recent, layouts);
				recent.insert(recent.begin(), std::move(layouts));
				if (recent.size() > kRecentOcclusionLayouts)
					recent.resize(kRecentOcclusionLayouts);
			}
		}
		// The placements recent shadow epochs drew: a variant for each, which records their views alone.
		if (const auto active = inputs.shadowActiveViews; active != 0) {
			auto& recent = producer.recentActiveViews;
			if (recent.empty() || recent.front() != active) {
				std::erase(recent, active);
				recent.insert(recent.begin(), active);
				if (recent.size() > kRecentActiveViews)
					recent.resize(kRecentActiveViews);
			}
		}
		// The shadow buffers for what the revisions' shapes name, required at every make.
		const auto& shadowState = inputs.shadow;
		if (shadowState && scratch.indirect.valid) {
			// The slots hold every draw the scene can produce (the draw bound as the make brought it up to date: MakeRevision).
			RequireShadowSequences(a_make.producer, a_build);
			// The latch for what the frame's epochs can name (over every mode): its views, key slots, the states registered and the
			// sun's processes, a view's each at most. The key slots of the publication's lookups (the scene lane resolved
			// the slots of these tables' keys already; append-only per instance), and the keys its casters and occluders use.
			const std::size_t slotKeys = std::max(a_make.lookups.shadowSlotKeys.size(), request.shadowSlotKeys);
			const std::size_t keys = slotKeys + request.shadowKeys;
			const auto views = static_cast<std::uint32_t>(inputs.shadowPlacements.size());
			ReserveShadowLatch(a_make.producer, a_build, *shadowState, views, static_cast<std::uint32_t>(keys), inputs.shadowRasterStates,
				std::max(views, latches.shadow.sunProcesses));
		}
	}

	void ShadowEngine::RequireLatched(RevisionMake& a_make, GrowthBuild& a_build)
	{
		// The shadow commit's latched copies, from what the shapes are made of: the set holds their latched block.
		const auto& producer = a_make.producer.producer;
		auto& scratch = revisionScratch;
		if (const auto& shadowState = producer.inputs.shadow; shadowState && scratch.indirect.valid) {
			scratch.latched = ShadowLatchedLayout(*shadowState);
			RequireLatchedBlock(a_build, producer.latchHandles.shadowLatched, scratch.latched);
		}
	}

	void ShadowEngine::NoteSealed(RevisionMake& a_make)
	{
		// The casting bound's version last checked: an outgrown one makes the shapes again (kKeyBound); a move alone keeps them.
		auto& owner = a_make.producer;
		const auto& producer = owner.producer;
		if (producer.drawBound.shadowVersion != owner.shadowBoundChecked) {
			owner.shadowBoundChecked = producer.drawBound.shadowVersion;
			if (!ShadowShapesHold(owner, ShadowBoundsOf(producer.drawBound, a_make.lookups)))
				++owner.shadowBoundOutgrown;
		}
	}

	void ShadowEngine::MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const
	{
		auto& group = a_mixer.group;
		auto mix = [&a_mixer](std::uint64_t a_value) { a_mixer.Mix(a_value); };
		auto ptr = [&a_mixer](const void* a_pointer) { a_mixer.Ptr(a_pointer); };
		auto raw = [&a_mixer]<class T>(const T& a_value) { a_mixer.Raw(a_value); };
		const auto& owner = a_make.producer;
		const auto& producer = owner.producer;
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The shadow pipeline set.
		group = ImplProducer::kKeyPipelines;
		ptr(revisionScratch.indirect.version.get());
		mix(revisionScratch.indirect.valid);
		// The shadow and occlusion epochs': the placements, the occlusion maps' layouts, the scene's casting bound, their latch's layout.
		group = ImplProducer::kKeyShadow;
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
		mix(producer.recentActiveViews.size());
		for (const auto active : producer.recentActiveViews)
			mix(active);
		group = ImplProducer::kKeyBound;
		mix(owner.shadowBoundOutgrown);
	}

	void ShadowEngine::MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout&)
	{
		MakeShadowShapes(a_make.producer, a_sealed, revisionScratch.indirect, a_make.lookups, revisionScratch.latched);
	}

	void ShadowEngine::MakeShadowShapes(ImplProducer& a_producer, const NamedVersions& a_sealed, const ShadowIndirectState& a_indirect, const Lookups& a_lookups,
		const std::vector<LatchedCopy>& a_latched)
	{
		auto& producer = a_producer.producer;
		const auto& inputs = producer.inputs;
		auto& made = producer.made;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& shadowState = inputs.shadow;
		const auto& shadowIndirect = a_indirect;
		const auto& lookups = a_lookups;
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
			// Each key slot's vertex layout and caster technique: the draws find theirs by slot (BuildDrawsLatch::slotLayoutsOffset), not
			// by bucket.
			auto slotLayouts = std::make_shared<std::vector<ShadowSlotEntry>>();
			slotLayouts->reserve(lookups.shadowSlotKeys.size());
			for (const auto& key : lookups.shadowSlotKeys)
				slotLayouts->push_back({ key.vertexLayout, key.technique | kShadowSlotTechniqueKnown, 0 });
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
					in.slotLayouts = slotLayouts;
					in.zeros = a_sealed.Latch(handles.shadowZeros);
					// The last shape made for the layout: its slots' capacities only grow.
					for (const auto& candidate : previousShadow[kind])
						if (candidate && LayoutOf(*candidate) == layouts)
							in.previous = candidate;
					if (!occlusionKind) {
						in.latched.copies = a_latched;
						if (!in.latched.copies.empty())
							in.latched.latch = a_sealed.Latch(handles.shadowLatched);
					}
					made.shadow[kind].push_back(MakeShadowShape(*shadowState, in));
					// The shadow epoch's variants for the placements recent frames drew: the same shape, recording those views alone.
					if (!occlusionKind) {
						const auto full = made.shadow[kind].back();
						const std::uint64_t all = full->views.size() >= 64 ? ~0ull : ((1ull << full->views.size()) - 1);
						for (const auto active : producer.recentActiveViews) {
							if ((active & all) == all || (active & all) == 0)
								continue;
							auto variant = std::make_shared<ShadowFrame>(*full);
							variant->activeViews = active & all;
							made.shadow[kind].push_back(std::move(variant));
						}
					}
					seen.push_back(std::move(layouts));
				}
			}
		}
	}

	bool ShadowEngine::ShadowShapesHold(const ImplProducer& a_producer, const ShadowBounds& a_bounds)
	{
		// The producer's: its inputs' shadow resources and the shapes it made last.
		const auto& producer = a_producer.producer;
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

	void ShadowEngine::PostInputs(RevisionInputs& a_next)
	{
		// The shadow resources, the pipelines' shadow format and rasterizer states, the occlusion maps' layouts as last captured, the
		// placements and the slots' buffers.
		a_next.shadow = shadow;
		auto& drawPipelines = DrawPipelines::Get();
		a_next.shadowFormat = drawPipelines.ShadowFormat();
		a_next.shadowRasterStates = drawPipelines.ShadowRasterStateCount();
		a_next.occlusionLayouts = PredictedOcclusion(nullptr);
		a_next.shadowPlacements = shadowPlacements;
		a_next.shadowActiveViews = lastActiveViews;
		if (shadow && (!postedShadowSlots || postedShadowSlots->sequences.size() != shadow->sequences.size()))
			postedShadowSlots = std::make_shared<const ShadowSlotBuffers>(ShadowSlotBuffers{ shadow->sequences, shadow->bucketCounts });
		a_next.shadowSlots = shadow ? postedShadowSlots : nullptr;
	}

	void ShadowEngine::PrepareAhead(ImplSnapshotBuilder::AheadContext& a_context)
	{
		// The shadow payload's, for the last epoch's views; the shadow lookups the scene lane resolves for them (PostLookupInputs). Under
		// the context's toggles (the next commit's), as lastShadow's modes are (UpdateShadowCapability).
		auto& c = a_context;
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (c.toggles.shadows && shadow && lastShadow.known && globals::game::utilityShader && !tables.objects.empty() &&
			tables.shadowTechnique.size() == tables.objects.size()) {
			c.shadowInputs = PrepareShadowInputs(*shadow, lastShadow.modes, lastShadow.rasterStates);
			c.shadowTarget = shadow;
			c.shadow = true;
		}
	}

	void ShadowEngine::BuildPayloadShared(const AheadBuild& a_build)
	{
		const auto& c = a_build.context;
		if (!c.shadow || !c.shadowTarget || !c.shadowTarget->scene)
			return;
		// As RunAhead's: the journals' oldest content epochs, the stage's time.
		const auto oldest = [&a_build](PayloadPart a_part) { return a_build.payloads.OldestContentEpoch(a_part).value_or(~std::uint64_t{ 0 }); };
		auto& a = aheadShadow;
		auto& in = a.in;
		in = c.shadowInputs;
		in.frameNumber = a_build.frame;
		in.tablesGeneration = a_build.generation;
		in.lookupGeneration = a_build.lookups.shadowGeneration;
		in.sunCandidates = a_build.sunCandidates;
		in.lightCandidates = a_build.lightCandidates;
		in.tablesOldestContentEpochs = a_build.oldestTables;
		// What the shadow journals keep changes back to.
		in.materialRowsOldestContentEpoch = oldest(PayloadPart::ShadowRows);
		in.inputsOldestContentEpoch = oldest(PayloadPart::Inputs);  // the scene list's region
		a.payload = AcquireShadowPayload();
		a.sharedRegion = Policy::ReuseResidentRegion();
		a.publication = &a_build.out;
		a.frame = a_build.frame;
		const auto shadowStart = std::chrono::steady_clock::now();
		BuildShadowPayloadShared(in, a_build.tables, a_build.lookups, *a.payload, a_build.out.streams, ShadowKeptState(), a.sharedRegion, a.split);
		a_build.stats.shadowMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowStart).count();
	}

	void ShadowEngine::BuildPayload(const AheadBuild& a_build)
	{
		const auto& c = a_build.context;
		if (!c.shadow || !c.shadowTarget || !c.shadowTarget->scene)
			return;
		auto& pairStats = a_build.stats;
		const auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto& tables = a_build.tables;
		auto& a = aheadShadow;
		{
			ZoneScopedN("CS.DCLF.BuildShadowPayload");
			// The shared half, unless RunAhead ran it for this build (BuildPayloadShared); then the region half, into the main build's
			// region as it left it.
			if (!a.split.pending || a.publication != &a_build.out || a.frame != a_build.frame)
				BuildPayloadShared(a_build);
			const auto shadowStart = std::chrono::steady_clock::now();
			BuildShadowPayloadRegion(a.in, tables, a_build.lookups, *a.payload, ShadowKeptState(), a.sharedRegion ? &a_build.mainRegion : nullptr, a.split);
			pairStats.shadowMs += msSince(shadowStart);
		}
		a.publication = nullptr;
		const auto& in = a.in;
		auto payload = std::move(a.payload);
		// The publication holds the views; the payload, which the epochs may hold past it, does not.
		payload->streams.reset();
		if (PassCapture::ShadowWithholdingEnabled()) {
			ZoneScopedN("CS.DCLF.BuildShadow.Exclusions");
			const auto exclusionsStart = std::chrono::steady_clock::now();
			// The sun's and the paraboloid lights' exclusions read the payload and each its own cache (the contribution's): two pool tasks.
			const bool sun = in.modeUsed[kSunShadowMode], parabolic = in.modeUsed[kParabolicShadowMode];
			const auto exclusion = [&](std::size_t a_which) {
				if (a_which == 0 && sun)
					payload->sunExclusion = BuildExclusion(Api::ShadowSource::Sun, in.sunCandidates, *payload, kSunShadowMode, tables);
				else if (a_which == 1 && parabolic)
					payload->parabolicExclusion = BuildExclusion(Api::ShadowSource::Point, in.lightCandidates, *payload, kParabolicShadowMode, tables);
			};
			if (sun && parabolic && !Policy::SerialFanout()) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildShadow.Exclusions", 2, 1, [&](std::size_t a_begin, std::size_t a_end) {
					ZoneScopedN("CS.DCLF.BuildShadow.ExclusionsShards");
					for (std::size_t x = a_begin; x < a_end; ++x)
						exclusion(x);
				});
			} else {
				exclusion(0);
				exclusion(1);
			}
			pairStats.exclusionsMs += msSince(exclusionsStart);
		}
		a_build.out.shadow = std::move(payload);
	}
}
