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
		post->rows = postedRows;
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
		const auto& named = Adopted();
		const auto units = [&](const Versioned& a_buffer) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(NamedUnits(named, a_buffer), UINT32_MAX)); };
		const std::array<SceneFit, 2> fit{ SceneFitOf(named, *resources->scene, std::min(units(resources->visibility), units(resources->frustum))),
			shadow && shadow->scene ? SceneFitOf(named, *shadow->scene, units(shadow->visibility)) : SceneFit{} };
		const std::array<std::uint32_t, 2> rows{ units(resources->materialRows.buffer), units(resources->pipelineRows.buffer) };
		if (fit != postedFit || rows != postedRows) {
			postedFit = fit;
			postedRows = rows;
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
		if (a_slot >= tables.objects.size() || !ObjectFits(tables, a_slot, posted.fit[a_shadow ? 1 : 0]))
			return false;
		// The main pair's rows (MainBuild::ResolvePair: a row past its table waits for its growth).
		const auto& object = tables.objects[a_slot];
		return a_shadow || (object.flags & kObjectNoBindings) || (object.materialIndex < posted.rows[0] && object.pipelineIndex < posted.rows[1]);
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
		void NamedAddressesOf(const NamedVersions& a_named, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
		{
			const auto& scene = *a_resources.scene;
			a_out.records = a_named.Of(a_resources.materialRows.buffer).deviceAddress;
			a_out.recordCapacity = a_resources.materialRows.RowsIn(a_named);
			std::uint64_t objects = a_named.Of(a_resources.visibility).units;
			if constexpr (requires { a_resources.pipelineRows; }) {
				a_out.pipelineRows = a_named.Of(a_resources.pipelineRows.buffer).deviceAddress;
				a_out.pipelineCapacity = a_resources.pipelineRows.RowsIn(a_named);
				objects = std::min(objects, a_named.Of(a_resources.frustum).units);
			}
			a_out.objectsIndex = a_named.Of(scene.objects).srvIndex;
			a_out.extrasIndex = a_named.Of(scene.extras).srvIndex;
			a_out.treeWindIndex = scene.TreeWindReadIndex(a_named, a_frame);
			a_out.facePositions = FaceSnapshots::Enabled() ? a_named.Of(scene.facePositions).deviceAddress : 0;
			a_out.fit = SceneFitOf(a_named, scene, objects);
		}
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_named, const MainLayout& a_layout, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_named, a_resources, a_frame, a_out);
		a_out.sequenceDecals = a_layout.sequenceDecals;
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_named, const ShadowResources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_named, a_resources, a_frame, a_out);
	}

	bool IndirectDraws::Impl::InputsFit(const DrawPublication& a_draws, const NamedVersions& a_named)
	{
		// The inputs each payload's commit puts in its input buffer: the resident region, then its own (the main pass's); the whole
		// shadow list (the shadow views'). A payload for other resources than the producer's draws nothing (its snapshot is incomplete).
		const auto& inputs = producer.inputs;
		bool fits = true;
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
			if (const auto& payload = a_draws.payloads[j]; payload && inputs.main && payload->inputs.addresses.identity == inputs.main.get()) {
				const std::uint64_t count = payload->resident.Count() + payload->inputList.size();
				producer.mainInputs = std::max(producer.mainInputs, count);
				fits = fits && count <= a_named.Of(*inputs.main->SegmentInputs(payload->inputs.depthOnly).first).units;
			}
		if (const auto& payload = a_draws.shadow; payload && inputs.shadow && payload->inputs.addresses.identity == inputs.shadow.get()) {
			const std::uint64_t count = payload->Inputs();
			producer.shadowInputs = std::max(producer.shadowInputs, count);
			fits = fits && count <= a_named.Of(inputs.shadow->inputs).units;
		}
		return fits;
	}

	void IndirectDraws::Impl::BuildSnapshotDraws(const NamedVersions* a_versions, const MainLayout* a_layout)
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
		if (a_versions && a_layout) {
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (context.build[j] && context.target && context.target->scene)
					AddressesOf(*a_versions, *a_layout, *context.target, context.inputs[j].frameNumber, context.inputs[j].addresses);
			if (context.shadow && context.shadowTarget && context.shadowTarget->scene)
				AddressesOf(*a_versions, *context.shadowTarget, context.shadowInputs.frameNumber, context.shadowInputs.addresses);
		}
		b.drawsNamed = a_versions ? a_versions->Serial() : 0;
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
		const auto& holders = ringHolders;
		// The publication's stream views: the stores are this task's alone (no build in the frame updates them). Their journals keep
		// what the oldest ring entry lacks.
		const TablesHeld from{ holders.objects.Oldest(), holders.extras.Oldest(), holders.geometries.Oldest() };
		out->streams = MakeStreamViews(objectStore, extrasStore, geometryStore, from, a_tables, *a_tables, a_generation, a_frame);
		// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by each buffer that holds them, as it is sent them (the ring's
		// producer, CommitSceneStreams).
		const auto& c = a_context;
		if (!c.valid || !c.target || !c.target->scene)
			return out;
		// The scene list: the main build's region, into which the shadow build writes its words; its journal's build begins once a
		// publication, with whichever writes first.
		mainCache.region.begun = false;
		// Both segments in one build: one list, read by each through its view bits.
		std::array<MainInputs, 2> inputs;
		std::array<const MainInputs*, 2> segmentIn{};
		std::array<MainPayload*, 2> segmentOut{};
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			if (!c.build[j])
				continue;
			MainInputs& in = inputs[j];
			in = c.inputs[j];
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.generation;
			// The oldest versions the ring's entries hold: what the rows' and the region's journals keep changes back to.
			in.materialRowsHeld = holders.materialRows.Oldest();
			in.pipelineRowsHeld = holders.pipelineRows.Oldest();
			in.residentUploaded = holders.resident.Oldest();
			in.tablesHeld = from;
			out->payloads[j] = AcquirePayload();
			segmentIn[j] = &in;
			segmentOut[j] = out->payloads[j].get();
		}
		BuildMainPayloads(segmentIn, *a_tables, Scene::FrameView::Get().GetFrameTables(), a_lookups, segmentOut, mainRows, &mainCache, out->streams);
		// The shadow payload, from the same tables, stream views and lookups, with the publication's candidates, and the
		// next frame's exclusions from it.
		if (c.shadow && c.shadowTarget && c.shadowTarget->scene) {
			ShadowInputs in = c.shadowInputs;
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.shadowGeneration;
			in.sunCandidates = a_sunCandidates;
			in.lightCandidates = a_lightCandidates;
			in.tablesHeld = from;
			// The oldest versions the ring's entries hold: what the shadow journals keep changes back to.
			in.materialRowsHeld = ringHolders.shadowRows.Oldest();
			in.inputsHeld = ringHolders.resident.Oldest();  // the scene list's region
			auto payload = AcquireShadowPayload();
			BuildShadowPayload(in, *a_tables, a_lookups, *payload, out->streams, ShadowKeptState(), Policy::ReuseResidentRegion() ? &mainCache.region : nullptr);
			// The publication holds the views; the payload, which the epochs may hold past it, does not.
			payload->streams.reset();
			if (PassCapture::ShadowWithholdingEnabled()) {
				ZoneScopedN("CS.DCLF.BuildShadow.Exclusions");
				if (in.modeUsed[kSunShadowMode])
					payload->sunExclusion = BuildSunExclusion(in.sunCandidates, *payload, kSunShadowMode, *a_tables, &sunExclusionCache);
				if (in.modeUsed[kParabolicShadowMode])
					payload->parabolicExclusion = BuildSunExclusion(in.lightCandidates, *payload, kParabolicShadowMode, *a_tables, &parabolicExclusionCache);
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
				const auto& main = out->payloads[kAsyncColour] ? out->payloads[kAsyncColour] : out->payloads[kAsyncZPrepass];
				out->shadow->mainTail = main ? main->inputList.size() : 0;
			}
		}
		Diagnostics::Notify(Diagnostics::DrawsBuiltAhead{ *out, *a_tables, a_frame });
		return out;
	}

	std::uint64_t IndirectDraws::TakeStreamsRefused()
	{
		return impl ? std::exchange(impl->streamsRefused, 0) : 0;
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
		try {
			for (;;) {
				if (b.stage == Stage::Recordings) {
					// One snapshot in flight: its revision complete (the assembler's newest, at or past the one sealed for it), failed, or
					// still recording (its completion wakes the builder). Producer-side alone: the exchange's consumer is this builder.
					const auto collected = rv.assembler.Collect();
					const bool failedHere = std::any_of(collected.failures.begin(), collected.failures.end(),
						[&](const auto& a_failure) { return a_failure.sequence == b.sealed; });
					(void)rv.assembler.TrySelect();
					auto lease = rv.assembler.AcquireActive();
					if (lease && lease->Sequence() >= b.sealed) {
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
				// One build in flight, the next started as soon as the builder is idle, from the newest work (latest wins: the publications
				// it skipped coalesce into it, and their log deltas reach the frame with it). A posted snapshot the frame has not taken is
				// replaced by the newer complete one; the build's own time is the rate (publications come only on a change), and an unchanged
				// revision is cheap (its recordings kept).
				if (auto next = snapshotWorkSlot.Take()) {
					const std::uint64_t sequence = next->publication ? next->publication->sequence : 0;
					if (b.lastPublication && sequence > b.lastPublication + 1)
						snapshotsCoalesced.fetch_add(sequence - b.lastPublication - 1, std::memory_order_relaxed);
					b.lastPublication = sequence;
					b.work = std::move(next);
					b.draws.reset();
					b.timing = {};
					b.stage = Stage::Make;
				}
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
					b.sealed = rv.assembler.LatestSequence();
					b.sealedAt = std::chrono::steady_clock::now();
					// The draws, for exactly the versions the sealed revision names (its adoption makes them current): built now, while its
					// recordings are made, or again after a make whose set names other versions than the ones they embed (a growth became
					// ready meanwhile). One consistent build: a snapshot never pairs draws and a revision of different versions.
					const auto& versions = rv.sealedVersions;
					if (!b.draws || b.drawsNamed != versions->Serial())
						BuildSnapshotDraws(versions.get(), rv.sealedLayout.get());
					// Draws whose inputs the set does not hold (a resident region past the per-object buffers): the revision is made
					// again, requiring them, and the snapshot is posted with a set that holds them (never with these).
					if (b.draws && !InputsFit(*b.draws, *versions)) {
						++rv.inputWaits;
						b.stage = Stage::Make;
						continue;
					}
					if (b.draws)
						Diagnostics::Notify(Diagnostics::SnapshotSealed{ *versions, *b.draws, b.work->context });
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

	void IndirectDraws::Impl::PostSnapshot(org::async::RevisionAssembler::Lease a_revision)
	{
		auto& b = snapshotBuilder;
		const auto& inputs = producer.inputs;
		auto snapshot = std::make_unique<SceneSnapshot>();
		snapshot->serial = ++b.serial;
		snapshot->publication = b.work->publication;
		snapshot->draws = b.draws;
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
		s.adoptedVersions = versions.get();
		s.installedDraws = snapshot->draws;
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

	void IndirectDraws::NoteEventAdoption(double a_ms, std::uint32_t a_frames)
	{
		if (!impl)
			return;
		impl->eventAdoptionMs.push_back(a_ms);
		impl->eventAdoptionFrames.push_back(a_frames);
	}
}
#endif
