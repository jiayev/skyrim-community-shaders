// The snapshot builder (one pass at a time on DCLF's preparation pool): the builds ahead of a publication's draws, from the context
// the render thread posts at the frame's start, its revision made and sealed, the snapshot posted; and the render thread's adoption of
// the newest one (AdoptSnapshot). PostSnapshotWork and the fits (FitsScene) are the coordinator's.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	using org::async::RevisionFragment;

	void IndirectDraws::PostAheadContext()
	{
		auto& c = impl->aheadContext;
		c = {};
		if (failed || !impl->resources || !impl->resources->scene) {
			// The cleared context, the fits as they stood (the coordinator builds nothing ahead from it).
			if (impl)
				impl->PostAhead(false);
			return;
		}
		auto* lighting = EngineShaders::Lighting();
		if (!lighting || !DrawPipelines::Get().Enabled() || !FrameIndirectState().valid) {
			impl->PostAhead();
			return;
		}
		// The capacities the builds are made against are MakeRevision's (it reserves them from the newest publication's tables, the only
		// tables the kept draw bound follows).
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			const auto& masks = impl->epochMasks[j];
			// The frame slots the epochs last supplied; the colour build replays the Z-prepass's vertex inputs.
			if (!masks.known || (j == kAsyncColour && !impl->prepassInputs))
				continue;
			c.inputs[j] = impl->PrepareMainInputs(j == kAsyncZPrepass, *impl->resources, masks.vsMask, masks.psMask);
			c.build[j] = true;
		}
		c.target = impl->resources;
		// The shadow payload's, for the last epoch's views; the shadow lookups the scene lane resolves for them (PostLookupInputs).
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (ActiveToggles().shadows && impl->shadow && impl->lastShadow.known && globals::game::utilityShader && !tables.objects.empty() &&
			tables.shadowTechnique.size() == tables.objects.size()) {
			c.shadowInputs = impl->PrepareShadowInputs(*impl->shadow, impl->lastShadow.modes, impl->lastShadow.rasterStates);
			c.shadowTarget = impl->shadow;
			c.shadow = true;
		}
		c.valid = c.build[kAsyncZPrepass] || c.build[kAsyncColour] || c.shadow;
		impl->PostAhead();
	}

	void IndirectDraws::Impl::PostAhead(bool a_fit)
	{
		// Immutable once posted, latest wins; the coordinator takes it at its passes' start (TakeAheadContext).
		if (a_fit)
			PostFit();
		auto post = std::make_unique<AheadPost>();
		post->context = aheadContext;
		post->fit = postedFit;
		post->fitSerial = postedFitSerial;
		aheadSlot.Post(std::move(post));
		// A frame input of the coordinator's: its pump woken (folded with the frame start's other posts).
		WakeScenePass(SceneWake::FrameInput);
	}

	void IndirectDraws::TakeAheadContext()
	{
		if (impl)
			impl->aheadSlot.TakeInto(impl->aheadTaken);
	}

	void IndirectDraws::Impl::PostFit()
	{
		// What the commit's set may claim (FitsScene): the buffers' versions the adopted set names (none before the first adoption).
		const auto& adopted = Adopted();
		const auto units = [&](const Versioned& a_buffer) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(NamedUnits(adopted, a_buffer), UINT32_MAX)); };
		const std::array<SceneFit, 2> fit{ SceneFitOf(adopted, *resources->scene, std::min(units(resources->visibility), units(resources->frustum))),
			shadow && shadow->scene ? SceneFitOf(adopted, *shadow->scene, units(shadow->visibility)) : SceneFit{} };
		if (fit != postedFit) {
			postedFit = fit;
			++postedFitSerial;
		}
	}

	bool IndirectDraws::FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const
	{
		if (!impl)
			return true;
		// The coordinator's copy of the posted fits (TakeAheadContext), never the render thread's.
		const auto& posted = impl->aheadTaken;
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		// The rows have no capacity to wait for: the payload's tables grow with it (PayloadBuilder).
		return a_slot < tables.objects.size() && ObjectFits(tables, a_slot, posted.fit[a_shadow ? 1 : 0]);
	}

	std::uint64_t IndirectDraws::SceneFitSerial() const
	{
		return impl ? impl->aheadTaken.fitSerial : 0;
	}

	void IndirectDraws::PostSnapshotWork(std::shared_ptr<const void> a_publication)
	{
		if (!impl || !a_publication)
			return;
		auto& s = *impl;
		auto publication = std::static_pointer_cast<const ScenePublication>(std::move(a_publication));
		if (failed) {
			// No draws and no revision: the publication alone (its tables and claims; DCLF draws nothing, DecideCoverage).
			auto snapshot = std::make_unique<Impl::SceneSnapshot>();
			// Nothing to build: taken at once (publication pacing waits on nothing).
			s.snapshotWorkTaken.store(publication->sequence, std::memory_order_release);
			snapshot->publication = std::move(publication);
			snapshot->drawsComplete = true;
			s.snapshotSlot.Post(std::move(snapshot));
			return;
		}
		auto& store = Scene::Coordinator::Get();
		// What the builds ahead read that the coordinator or the frame write later is copied now: the context (the coordinator's copy of
		// the frame's post, TakeAheadContext), the frame, the candidates. The tables and lookups are the publication's, immutable.
		auto work = std::make_unique<Impl::SnapshotWork>();
		work->publication = std::move(publication);
		work->context = s.aheadTaken.context;
		work->frame = store.GetFrame();  // the coordinator's frame (its pass's frame inputs)
		work->sun = store.CoordinatorSunCandidates();
		work->light = store.CoordinatorLightCandidates();
		work->aheadNumber = s.aheadKicked.fetch_add(1, std::memory_order_acq_rel) + 1;
		// Latest wins: one the builder has not taken yet is replaced (dropped here, on the coordinator, as a returned publication is).
		// The builder takes the newest once idle (one build in flight), so the publications made during a build coalesce here.
		s.snapshotWorkSlot.Post(std::move(work));
		(void)s.SnapshotPump();
		WakeSnapshotBuilder();
	}

	namespace
	{
		template <class Resources>
		void NamedAddressesOf(const NamedVersions& a_sealed, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
		{
			// The payload's parts (objects, extras, rows, inputs) are none of theirs: the epochs take them from the installed payload.
			const auto& scene = *a_resources.scene;
			std::uint64_t objects = a_sealed.Of(a_resources.visibility).units;
			if constexpr (requires { a_resources.frustum; })
				objects = std::min(objects, a_sealed.Of(a_resources.frustum).units);
			a_out.treeWindIndex = scene.TreeWindReadIndex(a_sealed, a_frame);
			a_out.facePositions = FaceSnapshots::Enabled() ? a_sealed.Of(scene.facePositions).deviceAddress : 0;
			a_out.fit = SceneFitOf(a_sealed, scene, objects);
		}
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_sealed, const MainLayout& a_layout, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_sealed, a_resources, a_frame, a_out);
		a_out.sequenceDecals = a_layout.sequenceDecals;
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_sealed, const ShadowResources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_sealed, a_resources, a_frame, a_out);
	}

	void IndirectDraws::Impl::BuildSnapshotDraws(const NamedVersions* a_sealed, const MainLayout* a_layout)
	{
		auto& b = snapshotBuilder;
		auto& work = *b.work;
		const auto start = std::chrono::steady_clock::now();
		const auto& publication = *work.publication;
		// The draws embed versioned buffers' addresses and SRV indices, and a snapshot's draws must embed the versions its revision names -
		// the ones its adoption makes current - never what the frame had when it posted the context (a grown version replaces it at that
		// adoption, and the old one is released once its last holder goes: a shader reading it loses the device). Every one is taken from
		// the sealed set, so draws and revision are one consistent build.
		AheadContext context = work.context;
		if (a_sealed && a_layout) {
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (context.build[j] && context.target && context.target->scene)
					AddressesOf(*a_sealed, *a_layout, *context.target, context.inputs[j].frameNumber, context.inputs[j].addresses);
			if (context.shadow && context.shadowTarget && context.shadowTarget->scene)
				AddressesOf(*a_sealed, *context.shadowTarget, context.shadowInputs.frameNumber, context.shadowInputs.addresses);
		}
		b.drawsSerial = a_sealed ? a_sealed->Serial() : 0;
		std::shared_ptr<const DrawPublication> built;
		aheadRunning.store(true, std::memory_order_release);
		if (publication.tables && publication.lookups) {
			try {
				built = RunAhead(publication.tables, context, *publication.lookups, publication.tablesGeneration, work.frame, work.sun, work.light);
			} catch (const std::exception& e) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged++ < 4)
					logger::error("[DCLF] builds ahead of publication {} failed: {}; its snapshot draws nothing", publication.sequence, e.what());
			}
		}
		aheadRunning.store(false, std::memory_order_release);
		aheadRunning.notify_all();
		// The builds ahead done up to this item's number: the ones it replaced included.
		aheadDone.store(work.aheadNumber, std::memory_order_release);
		aheadDone.notify_all();
		b.draws = std::move(built);
		b.payload.reset();  // new draws: their payload is published with the snapshot (PublishPayload)
		b.publicationWaited = false;
		b.timing.ahead = true;
		b.timing.aheadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::Impl::WaitAhead()
	{
		// The build ahead running now (teardown, the toggle, a load screen): one the builder has not started reads nothing until it does,
		// and the frame's fallback streams are refused until it is done (aheadDone).
		while (aheadRunning.load(std::memory_order_acquire))
			aheadRunning.wait(true, std::memory_order_acquire);
	}

	std::shared_ptr<const IndirectDraws::Impl::DrawPublication> IndirectDraws::Impl::RunAhead(std::shared_ptr<const SceneTables> a_tables,
		const AheadContext& a_context, const Lookups& a_lookups, std::uint32_t a_generation, std::uint32_t a_frame,
		std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates)
	{
		ZoneScopedN("CS.DCLF.BuildAhead");
		auto out = std::make_shared<DrawPublication>();
		out->tables = a_tables;
		// The journals keep their changes back to the oldest content epoch a backing of the part's pool holds
		// (PayloadBuilder::OldestContentEpoch); none holding anything, every change may go (a backing then is written whole).
		const auto oldest = [this](PayloadPart a_part) { return payloadBuilder.OldestContentEpoch(a_part).value_or(~std::uint64_t{ 0 }); };
		// The publication's stream views: the stores are this task's alone (no build in the frame updates them).
		const TablesContentEpochs oldestTables{ oldest(PayloadPart::Objects), oldest(PayloadPart::Extras), oldest(PayloadPart::Geometries) };
		const auto& c = a_context;
		const bool buildMain = c.valid && c.target && c.target->scene;
		// The scene list: the main build's region, into which the shadow build writes its words; its journal's build begins once a
		// publication, with whichever writes first.
		if (buildMain)
			mainCache.region.begun = false;
		// Both segments in one build: one list, read by each through its view bits.
		std::array<MainInputs, 2> inputs;
		std::array<const MainInputs*, 2> segmentIn{};
		std::array<MainPayload*, 2> segmentOut{};
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			if (!buildMain || !c.build[j])
				continue;
			MainInputs& in = inputs[j];
			in = c.inputs[j];
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.generation;
			// What the rows' and the region's journals keep changes back to.
			in.materialRowsOldestContentEpoch = oldest(PayloadPart::MaterialRows);
			in.pipelineRowsOldestContentEpoch = oldest(PayloadPart::PipelineRows);
			in.inputsOldestContentEpoch = oldest(PayloadPart::Inputs);
			in.tablesOldestContentEpochs = oldestTables;
			out->payloads[j] = AcquirePayload();
			segmentIn[j] = &in;
			segmentOut[j] = out->payloads[j].get();
		}
		// The publication's stream views (the stores are this task's alone: no build in the frame updates them) beside the main build, which
		// reads them only in its Finish: two pool tasks, unless the parity keeps the fan-outs on this thread (Policy::SerialFanout).
		auto& pairStats = mainRows.pairStats;
		const auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto stagesStart = std::chrono::steady_clock::now();
		Draws::MainBuildStages mainBuild(segmentIn, *a_tables, Scene::FrameView::Get().GetFrameTables(), a_lookups, segmentOut, mainRows, &mainCache);
		const auto stage = [&](std::size_t a_stage) {
			const auto start = std::chrono::steady_clock::now();
			if (a_stage == 0) {
				out->streams = MakeStreamViews(objectStore, extrasStore, geometryStore, oldestTables, a_tables, *a_tables, a_generation, a_frame);
				pairStats.streamsMs += msSince(start);
			} else {
				mainBuild.Run();
				pairStats.mainMs += msSince(start);
			}
		};
		if (buildMain && !Policy::SerialFanout()) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildAhead.Stages", 2, 1, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.BuildAhead.StagesShards");
				for (std::size_t s = a_begin; s < a_end; ++s)
					stage(s);
			});
			++pairStats.overlapped;
		} else {
			stage(0);
			if (buildMain)
				stage(1);
		}
		++pairStats.aheads;
		// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by each backing that holds them, as it is caught up (PayloadBuilder).
		if (!buildMain) {
			pairStats.stagesMs += msSince(stagesStart);
			return out;
		}
		mainBuild.Finish(out->streams);
		pairStats.stagesMs += msSince(stagesStart);
		// The shadow payload, from the same tables, stream views and lookups, with the publication's candidates, and the
		// next frame's exclusions from it.
		if (c.shadow && c.shadowTarget && c.shadowTarget->scene) {
			ShadowInputs in = c.shadowInputs;
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.shadowGeneration;
			in.sunCandidates = a_sunCandidates;
			in.lightCandidates = a_lightCandidates;
			in.tablesOldestContentEpochs = oldestTables;
			// What the shadow journals keep changes back to.
			in.materialRowsOldestContentEpoch = oldest(PayloadPart::ShadowRows);
			in.inputsOldestContentEpoch = oldest(PayloadPart::Inputs);  // the scene list's region
			auto payload = AcquireShadowPayload();
			const auto shadowStart = std::chrono::steady_clock::now();
			BuildShadowPayload(in, *a_tables, a_lookups, *payload, out->streams, ShadowKeptState(), Policy::ReuseResidentRegion() ? &mainCache.region : nullptr);
			pairStats.shadowMs += msSince(shadowStart);
			// The publication holds the views; the payload, which the epochs may hold past it, does not.
			payload->streams.reset();
			if (PassCapture::ShadowWithholdingEnabled()) {
				ZoneScopedN("CS.DCLF.BuildShadow.Exclusions");
				const auto exclusionsStart = std::chrono::steady_clock::now();
				// The sun's and the paraboloid lights' exclusions read the payload and each its own cache: two pool tasks.
				const bool sun = in.modeUsed[kSunShadowMode], parabolic = in.modeUsed[kParabolicShadowMode];
				const auto exclusion = [&](std::size_t a_which) {
					if (a_which == 0 && sun)
						payload->sunExclusion = BuildSunExclusion(in.sunCandidates, *payload, kSunShadowMode, *a_tables, &sunExclusionCache);
					else if (a_which == 1 && parabolic)
						payload->parabolicExclusion = BuildSunExclusion(in.lightCandidates, *payload, kParabolicShadowMode, *a_tables, &parabolicExclusionCache);
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
			out->shadow = std::move(payload);
		}
		// Every payload of the publication holds the scene list's region as both builds left it: the shadow payload's view, taken
		// after its words, or without it the main region's; and the shadow payload where its own frame inputs follow the main ones'.
		{
			KeptView<DrawInput> region;
			if (out->shadow)
				region = out->shadow->regionInputs;
			else if (Policy::ReuseResidentRegion() && mainCache.region.begun)
				region = mainCache.region.inputs.View();
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (const auto& payload = out->payloads[j])
					payload->resident = region;
			if (out->shadow) {
				const auto* main = NewestMainPayload(*out);
				out->shadow->mainTail = main ? main->inputList.size() : 0;
			}
		}
		Diagnostics::Notify(Diagnostics::DrawsBuiltAhead{ *out, *a_tables, a_frame });
		return out;
	}

	namespace
	{
		// The snapshot builder's pump, once configured (Impl::SnapshotPump): what WakeSnapshotBuilder notifies.
		std::atomic<org::async::SerializedTaskPump*> builderPump{ nullptr };
	}

	void Draws::WakeSnapshotBuilder()
	{
		if (auto* pump = builderPump.load(std::memory_order_acquire))
			(void)pump->Notify();
	}

	org::async::SerializedTaskPump& IndirectDraws::Impl::SnapshotPump()
	{
		// Configured once (the first work or inputs posted), never destroyed: a revision's completion may still wake it while the process
		// tears down. Its passes run on DCLF's preparation pool, one at a time (level-triggered, lock-free: SerializedTaskPump).
		static auto* pump = [this] {
			auto* created = SceneScheduler::MakeLanePump(
				SceneScheduler::Executor(), SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF snapshot builder",
				[this] { SnapshotPass(); },
				[] { logger::error("[DCLF] the snapshot builder was refused by DCLF's executor; no scene snapshot is built from here on"); });
			builderPump.store(created, std::memory_order_release);
			return created;
		}();
		return *pump;
	}

	void IndirectDraws::Impl::SnapshotPass()
	{
		ZoneScopedN("CS.DCLF.SnapshotBuilder");
		// What the frame replaced: dropped here, never on the render thread.
		retiredSnapshots.Drop();
		// The frame's report asked for the builder's lines.
		if (producerReportWanted.exchange(false, std::memory_order_acq_rel))
			producerReportSlot.Post(std::make_unique<std::string>(ProducerReport()));
		auto& b = snapshotBuilder;
		auto& rv = revisions;
		using Stage = SnapshotBuilder::Stage;
		auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		// The newest work taken (latest wins: the publications it skipped coalesce into it, and their log deltas reach the frame with it).
		const auto takeWork = [&](std::unique_ptr<SnapshotWork> a_next) {
			const std::uint64_t sequence = a_next->publication ? a_next->publication->sequence : 0;
			if (b.lastPublication && sequence > b.lastPublication + 1)
				snapshotsCoalesced.fetch_add(sequence - b.lastPublication - 1, std::memory_order_relaxed);
			b.lastPublication = sequence;
			// The coordinator publishes again once this one is taken (SnapshotWorkTaken: publication pacing).
			snapshotWorkTaken.store(sequence, std::memory_order_release);
			b.work = std::move(a_next);
			b.draws.reset();
			b.timing = {};
			b.stage = Stage::Make;
		};
		try {
			for (;;) {
				if (b.stage == Stage::Recordings) {
					// One snapshot in flight: its revision complete (the assembler's newest, at or past the one sealed for it), failed, or
					// still recording (its completion wakes the builder). Producer-side alone: the exchange's consumer is this builder.
					const auto collected = rv.assembler.Collect();
					const bool failedHere = std::any_of(collected.failures.begin(), collected.failures.end(),
						[&](const auto& a_failure) { return a_failure.sequence == b.sealedSequence; });
					// Its payload's uploads: failed, they are sent again (whole, into backings that hold nothing); else it is posted only once
					// they completed (their completion wakes the builder).
					if (b.payload && b.payload->ready && b.payload->ready->Failed()) {
						logger::error("[DCLF] the payload's uploads failed ({}); sent again", b.payload->ready->Failure());
						payloadBuilder.Discard(*b.payload);
						b.payload.reset();
					}
					if (!b.payload && PayloadWanted()) {
						// Not published yet (the frame would not adopt it yet): newer work replaces it, nothing sent for it; else it is
						// published once the frame will adopt it (the frame's take and the scene pass's end wake the builder).
						if (auto next = snapshotWorkSlot.Take()) {
							++b.replacedUnpublished;
							takeWork(std::move(next));
							continue;
						}
						PublishPayload();
					}
					(void)rv.assembler.TrySelect();
					auto lease = rv.assembler.AcquireActive();
					const bool payloadReady = b.payload ? b.payload->Ready() : !PayloadWanted();
					if (lease && lease->Sequence() >= b.sealedSequence && payloadReady) {
						b.timing.recordingsMs = msSince(b.sealedAt);
						PostSnapshot(std::move(lease));
						b.stage = Stage::Idle;
					} else if (failedHere) {
						// A recording failed or was dropped by a graph build: made again (a failed epoch has none in it until its inputs
						// change; a dropped one is asked again once the inputs carry the new build).
						b.stage = Stage::Make;
					} else {
						return;
					}
					continue;
				}
				// One build in flight, the next started as soon as the builder is idle, from the newest work. A sealed build waits for the
				// frame to take the last posted snapshot before its payload is published (PublishPayload), and newer work replaces it until
				// then; the build's own time is the rate (publications come only on a change), and an unchanged revision is cheap (its
				// recordings kept).
				if (auto next = snapshotWorkSlot.Take())
					takeWork(std::move(next));
				if (!b.work || !b.work->publication)
					return;
				if (b.stage == Stage::Idle) {
					// A snapshot of the work stands: made again when the frame's inputs moved (a graph build, a capture, a toggle, new
					// resources), so its revision is the frame's as it is now.
					if (!snapshotInputsMoved.exchange(false, std::memory_order_acq_rel))
						return;
					b.timing = {};
				}
				// The revision (Stage::Make, or Waiting: made again at every wake - a growth settled, the inputs moved).
				snapshotInputsMoved.store(false, std::memory_order_release);
				const auto start = std::chrono::steady_clock::now();
				const std::uint64_t before = rv.assembler.LatestSequence();
				const auto& publication = *b.work->publication;
				MakeRevision(publication, publication.commitFrame);
				b.timing.shapesMs += msSince(start);
				if (rv.assembler.LatestSequence() != before) {
					b.sealedSequence = rv.assembler.LatestSequence();
					b.sealedAt = std::chrono::steady_clock::now();
					// The draws, for exactly the versions the sealed revision names (its adoption makes them current): built now, while its
					// recordings are made, or again after a make whose set names other versions than the ones they embed (a growth became
					// ready meanwhile). One consistent build: a snapshot never pairs draws and a revision of different versions.
					const auto& versions = rv.revisionVersions;
					if (!b.draws || b.drawsSerial != versions->Serial())
						BuildSnapshotDraws(versions.get(), rv.revisionLayout.get());
					if (b.draws)
						Diagnostics::Notify(Diagnostics::SnapshotSealed{ *versions, *b.draws, b.work->context });
					// The draws' payload, sent while the recordings are made once the frame will adopt it (kept when the draws are).
					PublishPayload();
					b.stage = Stage::Recordings;
					continue;
				}
				// None sealed. Without what a revision is made of (no main resources yet, no claims): the publication alone, so the frame has
				// its tables and catalog (its epochs then capture what the next revision needs; its draws, without a revision, draw nothing:
				// DecideCoverage). Else a growth or a build it asked for is pending: made again when woken.
				if (!producer.inputs.claims || !producer.inputs.main) {
					if (!b.draws)
						BuildSnapshotDraws(nullptr, nullptr);
					PostSnapshot(nullptr);
					b.stage = Stage::Idle;
					return;
				}
				b.stage = Stage::Waiting;
				return;
			}
		} catch (const std::exception& e) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged++ < 4)
				logger::error("[DCLF] the snapshot builder's pass failed: {}; it makes the snapshot again when woken", e.what());
			b.stage = b.work ? Stage::Waiting : Stage::Idle;
		}
	}

	bool IndirectDraws::Impl::PayloadWanted() const
	{
		// Nothing to draw: the snapshot is posted without one and is not covered (DecideCoverage).
		const auto& b = snapshotBuilder;
		return b.draws && b.draws->streams && (NewestMainPayload(*b.draws) || b.draws->shadow);
	}

	void IndirectDraws::Impl::PublishPayload()
	{
		auto& b = snapshotBuilder;
		// Once per draws: a snapshot made again for the same draws keeps their payload, and uploads nothing.
		if (b.payload || !PayloadWanted())
			return;
		// Only a snapshot the frame will adopt is published, so every publication is installed and none is sent for nothing: not while
		// the last one posted is still in the slot (the frame's take wakes the builder; newer work replaces these draws meanwhile), nor
		// before the scene pass of the frame that took it has ended (a publication it makes is posted first and replaces them; its end
		// wakes the builder). Posted on its uploads' completion, the snapshot is the frame's next.
		const std::uint64_t taken = snapshotTaken.load(std::memory_order_acquire);
		if (taken < b.serial || scenePassDoneFrame.load(std::memory_order_acquire) < snapshotTakenFrame.load(std::memory_order_acquire)) {
			b.publicationsWaited += std::exchange(b.publicationWaited, true) ? 0u : 1u;
			return;
		}
		const auto* mainPayload = NewestMainPayload(*b.draws);
		const auto* shadowPayload = b.draws->shadow.get();
		const auto uploads = producer.inputs.uploads.lock();
		if (!uploads)
			throw std::runtime_error("no uploader for the payload (the graph host's is gone)");
		b.payload = payloadBuilder.Publish(*b.draws->streams, mainPayload, shadowPayload, *uploads);
		// Event-driven: the uploads' completion (or failure) wakes the builder, which then posts the snapshot.
		if (const auto& ready = b.payload ? b.payload->ready : nullptr; ready && !ready->Complete() && ready->subscribe)
			ready->subscribe([] { WakeSnapshotBuilder(); });
	}

	void IndirectDraws::Impl::PostSnapshot(org::async::RevisionAssembler::Lease a_revision)
	{
		auto& b = snapshotBuilder;
		const auto& inputs = producer.inputs;
		auto snapshot = std::make_unique<SceneSnapshot>();
		snapshot->serial = ++b.serial;
		snapshot->publication = b.work->publication;
		snapshot->draws = b.draws;
		snapshot->payload = b.payload;
		snapshot->revision = std::move(a_revision);
		// Its draws: both main payloads, for the main resources its revision is of. The builder's verdict: without, its frames have no
		// claims (DecideCoverage), whatever the frame finds.
		bool complete = b.draws != nullptr && inputs.main != nullptr;
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
			complete = complete && b.draws->payloads[j] && b.draws->payloads[j]->inputs.addresses.identity == inputs.main.get();
		snapshot->drawsComplete = complete && snapshot->revision != nullptr;
		if (!snapshot->drawsComplete)
			snapshotsWithoutDraws.fetch_add(1, std::memory_order_relaxed);
		// The generations it was built for (its last make's inputs), which the adoption compares.
		snapshot->buildGeneration = inputs.buildGeneration;
		snapshot->mainGeneration = inputs.mainGeneration;
		snapshot->targetsGeneration = inputs.targetsGeneration;
		snapshot->shadowFormat = inputs.shadowFormat;
		// Latest wins: one the frame has not taken is dropped here, on the builder.
		snapshotSlot.Post(std::move(snapshot));
		snapshotsBuilt.fetch_add(1, std::memory_order_relaxed);
		snapshotTimings.Push(b.timing);
	}

	std::shared_ptr<const void> IndirectDraws::AdoptSnapshot(std::uint32_t a_togglesGeneration)
	{
		if (!impl)
			return nullptr;
		auto& s = *impl;
		const auto adoptedPublication = [&s]() -> std::shared_ptr<const void> { return s.adoptedSnapshot ? s.adoptedSnapshot->publication : nullptr; };
		auto taken = s.snapshotSlot.Take();
		if (!taken) {
			// The producer's lag alone: nothing newer was built since the last adoption.
			++s.snapshotFramesKept;
			return adoptedPublication();
		}
		// The slot is free: the builder may publish the next snapshot's payload once this frame's scene pass has ended (PublishPayload).
		// Told whether adopted or stale (the builder's posts are numbered; a failed feature's are not); the wake below is the builder's.
		if (taken->serial) {
			s.snapshotTakenFrame.store(Scene::FrameView::Get().GetFrame(), std::memory_order_release);
			s.snapshotTaken.store(taken->serial, std::memory_order_release);
		}
		// The one check: what a snapshot cannot know ahead, by generation. Its recordings of the graph as built now, its pipeline sets for the
		// targets and the shadow format as they are, its draws and shapes for the main resources as they are, its commit's toggles the
		// frame's. One without a revision (the publication alone) is adopted for its tables whatever the graph.
		using Stale = Impl::SnapshotStale;
		std::size_t stale = Stale::kStaleKinds;
		if (taken->revision) {
			auto* host = RenderGraphRuntime::Get().Host();
			auto& drawPipelines = DrawPipelines::Get();
			if (host && taken->buildGeneration != host->BuildGeneration())
				stale = Stale::kStaleBuild;
			else if (taken->targetsGeneration != drawPipelines.Generation())
				stale = Stale::kStaleTargets;
			else if (taken->shadowFormat != drawPipelines.ShadowFormat())
				stale = Stale::kStaleShadowFormat;
			else if (taken->mainGeneration != s.postedInputs.mainGeneration)
				stale = Stale::kStaleMain;
		}
		if (taken->publication && taken->publication->togglesGeneration < a_togglesGeneration)
			stale = Stale::kStaleToggles;
		if (stale != Stale::kStaleKinds) {
			// Not adopted: the frame has no claims until a current one (the adopted one is older still). The builder makes the snapshot
			// again for the frame's inputs (a toggle waits for the coordinator's publication under it instead).
			++s.snapshotsStale[stale];
			s.snapshotStale = true;
			s.retiredSnapshots.Push(std::shared_ptr<const Impl::SceneSnapshot>(std::move(taken)));
			if (stale != Stale::kStaleToggles)
				s.snapshotInputsMoved.store(true, std::memory_order_release);
			WakeSnapshotBuilder();
			return adoptedPublication();
		}
		// Adopted whole: the versions its revision's set names become current (the graph host told of new ones), its revision the frame's
		// epochs', its draws theirs.
		std::shared_ptr<const Impl::SceneSnapshot> snapshot(std::move(taken));
		const auto versions = snapshot->revision ? snapshot->revision->Get<NamedVersions>(Impl::SceneRevisions::kVersionsSlot) : nullptr;
		if (versions && versions->AdoptAll()) {
			if (auto* host = RenderGraphRuntime::Get().Host())
				host->NoteNewVersions();
			if (s.growthAdoptedFrame == ~0u)
				s.growthAdoptedFrame = Scene::FrameView::Get().GetFrame();
		}
		s.revisions.active = snapshot->revision;
		s.adoptedSet = versions.get();
		s.installedDraws = snapshot->draws;
		if (s.installedPayload != snapshot->payload) {
			s.installedPayload = snapshot->payload;  // the snapshot holds it too: its release is the builder's
			s.installedPayloadFrame = Scene::FrameView::Get().GetFrame();
			if (s.installedPayload)
				s.payloadsInstalled.fetch_add(1, std::memory_order_relaxed);
			// Its rows carrying TexcoordOffset, for the frame values (the producers sample them; TextureTransformsCS patches the rows).
			FrameValues::Get().SetTextureTransforms(s.installedPayload ? s.installedPayload->textureTransforms : nullptr);
		}
		s.snapshotStale = false;
		++s.snapshotsAdopted;
		if (const auto& publication = snapshot->publication) {
			const std::uint32_t frameNow = Scene::FrameView::Get().GetFrame();
			s.adoptionLatency.push_back(frameNow >= publication->commitFrame ? frameNow - publication->commitFrame : 0u);
		}
		// The replaced one back to the builder last, once nothing of the frame's holds it alone: its release is the builder's.
		if (auto replaced = std::exchange(s.adoptedSnapshot, snapshot))
			s.retiredSnapshots.Push(std::move(replaced));
		// The slot is free (a builder idle with inputs moved remakes for the frame as it is now).
		WakeSnapshotBuilder();
		return snapshot->publication;
	}

	void IndirectDraws::NoteScenePassDone(std::uint32_t a_frame)
	{
		if (!impl)
			return;
		// A builder holding a sealed snapshot's publication for this frame's pass publishes now (or has taken the pass's publication).
		impl->scenePassDoneFrame.store(a_frame, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	std::uint64_t IndirectDraws::SnapshotWorkTaken() const
	{
		return impl ? impl->snapshotWorkTaken.load(std::memory_order_acquire) : ~0ull;
	}

	void IndirectDraws::BindAdopted()
	{
		// After the frame's last submission, never at its start: the point must cover the frame's own GPU work. Every frame binds the
		// set and the payload it holds (the next adoption replaces them after this frame bound them), stale or not: they are its.
		if (!impl)
			return;
		const std::uint64_t point = SubmittedRetirementPoint();
		if (impl->adoptedSet)
			impl->adoptedSet->Bind(point);
		if (impl->installedPayload)
			impl->installedPayload->Bind(point);
	}

	void IndirectDraws::NoteEventAdoption(double a_ms, std::uint32_t a_frames)
	{
		if (!impl)
			return;
		impl->eventAdoptionMs.push_back(a_ms);
		impl->eventAdoptionFrames.push_back(a_frames);
	}
}
#endif
