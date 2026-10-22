// The snapshot builder (one pass at a time on DCLF's preparation pool): the builds ahead of a publication's draws, from the context
// the render thread posts at the frame's start, its revision made and sealed, the snapshot posted; and the render thread's adoption of
// the newest one (AdoptSnapshot; once DCLF draws, AdoptCurrentSnapshot waits for a current one). PostSnapshotWork and the fits
// (FitsScene) are the coordinator's.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include <thread>

namespace DCLF
{
	using org::async::RevisionFragment;

	void IndirectDraws::PostAheadContext()
	{
		// Without main resources: the cleared context, the fits as they stood (the coordinator builds nothing ahead from it).
		const bool fit = impl->MakeAheadContext(impl->aheadContext, failed);
		impl->PostAhead(fit);
	}

	bool IndirectDraws::Impl::MakeAheadContext(AheadContext& a_out, bool a_failed)
	{
		auto& c = a_out;
		c = {};
		// For the coordinator's next commit, made under the newest toggles (the frame inputs': SceneToggles), not the frame's.
		c.toggles = SceneToggles();
		if (a_failed || !resources || !resources->scene)
			return false;
		auto* lighting = EngineShaders::Lighting();
		if (!lighting || !DrawPipelines::Get().Enabled() || !FrameIndirectState().valid)
			return true;
		// The capacities the builds are made against are MakeRevision's (it reserves them from the newest publication's tables, the only
		// tables the kept draw bound follows).
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			const auto& masks = epochMasks[j];
			// The frame slots the epochs last supplied; the colour build replays the Z-prepass's vertex inputs.
			if (!masks.known || (j == kAsyncColour && !prepassInputs))
				continue;
			c.inputs[j] = PrepareMainInputs(j == kAsyncZPrepass, *resources, masks.vsMask, masks.psMask);
			c.build[j] = true;
		}
		c.target = resources;
		// The participants' parts (Revision/Participant.h: the shadow engine's payload inputs, for the last epoch's views).
		for (auto* participant : participants)
			participant->PrepareAhead(c);
		c.valid = c.build[kAsyncZPrepass] || c.build[kAsyncColour] || c.shadow;
		return true;
	}

	void IndirectDraws::Impl::PostResourcesAhead(bool a_failed)
	{
		// The builder's alone: the coordinator's context stays the frame start's (a second post in a frame would read to it as the next
		// frame's input). A context without both main builds builds no complete draws, so none is posted: the frame start's is the next.
		auto context = std::make_unique<AheadContext>();
		if (!MakeAheadContext(*context, a_failed) || !context->target || !context->build[kAsyncZPrepass] || !context->build[kAsyncColour])
			return;
		resourcesAheadSlot.Post(std::move(context));
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	bool IndirectDraws::Impl::RefreshWorkContext()
	{
		resourcesAheadSlot.TakeInto(resourcesAhead);
		auto& context = snapshotBuilder.work->context;
		const auto& main = producer.inputs.main;
		// Its draws are for the revision inputs' main resources already, or the frame posted no context for them yet (its post wakes the
		// builder, which makes the snapshot again).
		if (!main || context.target == main || resourcesAhead.target != main)
			return false;
		// The frame's context, under its toggles: one other than the publication's drops its shadow part (BuildSnapshotDraws) and is not
		// passed through to.
		context = resourcesAhead;
		++snapshotBuilder.contextsReplaced;
		return true;
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
		const auto& shadow = shadowEngine->shadow;
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
			s.snapshotsPosted.fetch_add(1, std::memory_order_release);
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
		// The builder takes the newest once idle and the frame took its last snapshot (one build per adoption: WorkToBuild), so the
		// publications made meanwhile coalesce here.
		s.snapshotWorkSlot.Post(std::move(work));
		(void)s.SnapshotPump();
		WakeSnapshotBuilder();
	}

	namespace
	{
		// RunAhead's: the most unread change-log entries (object records and geometry rows together) for which its two stages run on
		// the builder's thread, one after the other, instead of on two pool tasks.
		constexpr std::uint64_t kInlineStageChanges = 128;

		template <class Resources>
		void NamedAddressesOf(const NamedVersions& a_sealed, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
		{
			// The payload's parts (objects, extras, rows, inputs) are none of theirs: the epochs take them from the installed payload.
			const auto& scene = *a_resources.scene;
			std::uint64_t objects = a_sealed.Of(a_resources.visibility).units;
			if constexpr (requires { a_resources.frustum; })
				objects = std::min(objects, a_sealed.Of(a_resources.frustum).units);
			Extensions::FrameBindingValuesOf(a_sealed, scene, a_frame, a_out.frameBindings, false);
			// The vertex stream's buffer while its entry class's objects are DCLF's (Features/Faces' face positions: it takes the face shapes
			// once its engine side is in).
			a_out.streamBuffer = Extensions::VertexStreamTaken() ? a_sealed.Of(scene.vertexStream).deviceAddress : 0;
			a_out.fit = SceneFitOf(a_sealed, scene, objects);
		}

		/**
		 * @brief SnapshotPass: whether a_work's publication would build into the last snapshot posted (a_posted): the coordinator
		 * publishes its tables, lookups, claims, category nodes and candidates again only when they changed (a pass whose only change is
		 * the frame's deltas shares them all), so the same objects with the same generations, revision counts and ahead context's
		 * shape are the same build. The frame number a build reads moves on as it does for a snapshot no publication replaced.
		 */
		bool PassesThrough(const ImplSnapshotBuilder::SnapshotBuilder::Posted& a_posted, const ImplSnapshotBuilder::SnapshotWork& a_work)
		{
			if (!a_posted.publication || !a_posted.revision || !a_work.publication)
				return false;
			const auto& next = *a_work.publication;
			const auto& prior = *a_posted.publication;
			if (next.tables != prior.tables || next.lookups != prior.lookups || next.catalog != prior.catalog || next.claims != prior.claims ||
				next.sunCandidates != prior.sunCandidates || next.lightCandidates != prior.lightCandidates || next.categoryNodes != prior.categoryNodes)
				return false;
			if (next.tablesGeneration != prior.tablesGeneration || next.togglesGeneration != prior.togglesGeneration || next.lackingCount != prior.lackingCount ||
				next.lightEntriesAppeared != prior.lightEntriesAppeared || next.revision.pipelines != prior.revision.pipelines ||
				next.revision.shadowSlotKeys != prior.revision.shadowSlotKeys || next.revision.shadowKeys != prior.revision.shadowKeys)
				return false;
			if (a_work.sun != a_posted.sun || a_work.light != a_posted.light)
				return false;
			const auto& c = a_work.context;
			if (c.valid != a_posted.valid || c.shadow != a_posted.shadow || c.build != a_posted.build || c.target.get() != a_posted.target ||
				c.shadowTarget.get() != a_posted.shadowTarget)
				return false;
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (c.inputs[j].vsFrameMask != a_posted.vsMask[j] || c.inputs[j].psFrameMask != a_posted.psMask[j])
					return false;
			return true;
		}

		/** @brief RunAhead's: every payload of a_out given the scene list's region as both builds left it (see there). */
		void ShareAheadRegion(ImplSnapshotBuilder::DrawPublication& a_out, BuildCache& a_mainCache)
		{
			KeptView<DrawInput> region;
			if (a_out.shadow)
				region = a_out.shadow->regionInputs;
			else if (Policy::ReuseResidentRegion() && a_mainCache.region.begun)
				region = a_mainCache.region.inputs.View();
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (const auto& payload = a_out.payloads[j])
					payload->resident = region;
			if (a_out.shadow) {
				const auto* main = ImplSnapshotBuilder::NewestMainPayload(a_out);
				a_out.shadow->mainTail = main ? main->body->inputList.size() : 0;
			}
		}
	}

	std::unique_ptr<ImplSnapshotBuilder::SnapshotWork> ImplSnapshotBuilder::WorkToBuild()
	{
		auto& b = snapshotBuilder;
		if (auto next = snapshotWorkSlot.Take())
			b.held = std::move(next);
		if (!b.held)
			return nullptr;
		bool closed = PublicationGateClosed();
		if (closed) {
			// The scene pass's end wakes the builder only while it waits (NoteScenePassDone); the gate read again after saying so.
			publicationGated.store(true, std::memory_order_seq_cst);
			closed = PublicationGateClosed();
		}
		// A publication that moved only deltas, while the last snapshot stands unchanged: passed through at once (nothing built).
		if (!closed || (b.stage == SnapshotBuilder::Stage::Idle && !snapshotInputsMoved.load(std::memory_order_acquire) && PassesThrough(b.posted, *b.held)))
			return std::move(b.held);
		++b.heldPasses;
		return nullptr;
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
		// A context made under other toggles than the publication's commit (its pass took the frame inputs and the ahead context of two
		// frame starts with a toggle between them): its shadow part was prepared for those (the shadow toggle, the occlusion maps' modes),
		// so it is dropped, the publication's frames leave the shadow views to the engine, and its snapshot is never passed through to
		// (PostSnapshot): the next publication's builds take a context of its own toggles.
		if (context.toggles != publication.toggles) {
			context.shadow = false;
			context.shadowTarget.reset();
			context.valid = context.build[kAsyncZPrepass] || context.build[kAsyncColour];
		}
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
		b.drawsPosted = false;
		++b.buildsStarted;
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
		std::shared_ptr<const EntrySpace> a_sunCandidates, std::shared_ptr<const EntrySpace> a_lightCandidates)
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
		// The publication's stream views (the stores are this task's alone: no build in the frame updates them), then the participants' shared
		// halves, beside the main build, which reads the views only in its Finish: two pool tasks, unless the parity keeps the fan-outs on
		// this thread (Policy::SerialFanout).
		auto& pairStats = mainRows.pairStats;
		const auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto stagesStart = std::chrono::steady_clock::now();
		Draws::MainBuildStages mainBuild(segmentIn, *a_tables, Scene::FrameView::Get().GetFrameTables(), a_lookups, segmentOut, mainRows, &mainCache);
		// The participants' build (Revision/Participant.h), from the same tables, stream views and lookups: the shadow engine's, with the
		// publication's candidates. Their shared halves run in the first stage, after the stream views they read and beside the main build
		// (BuildPayloadShared's contract: they touch neither its region nor its payloads); their payloads after it (BuildPayload).
		const AheadBuild build{ *out, c, *a_tables, a_lookups, a_generation, a_frame, a_sunCandidates, a_lightCandidates, oldestTables, payloadBuilder, mainCache.region,
			pairStats };
		const auto stage = [&](std::size_t a_stage) {
			const auto start = std::chrono::steady_clock::now();
			if (a_stage == 0) {
				out->streams = MakeStreamViews(objectStore, extrasStore, geometryStore, oldestTables, a_tables, *a_tables, a_generation, a_frame);
				pairStats.streamsMs += msSince(start);
				// Only for a build whose participants' payloads follow (BuildPayload after the main build): every shared half is.
				if (buildMain)
					for (auto* participant : participants)
						participant->BuildPayloadShared(build);
			} else {
				mainBuild.Run();
				pairStats.mainMs += msSince(start);
			}
		};
		// A small change runs both stages here, in order: the stream views (and the shadow engine's kept records after them) catch up only
		// the change log's unread entries (the object records' and the geometry rows', both stores continuing), so overlapping them with the
		// main build saves less than this thread's wait on the helper costs.
		const auto unread = [a_generation](const LogCursor& a_cursor, const auto& a_log) -> std::uint64_t {
			return a_cursor.Continues(a_log, a_generation) ? a_cursor.Unread(a_log).size() : kInlineStageChanges + 1;
		};
		const bool smallChange = unread(objectStore.cursor, a_tables->changeLog) + unread(geometryStore.cursor, a_tables->geometryLog) <= kInlineStageChanges;
		if (buildMain && !smallChange && !Policy::SerialFanout()) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildAhead.Stages", 2, 1, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.BuildAhead.StagesShards");
				for (std::size_t s = a_begin; s < a_end; ++s)
					stage(s);
			});
			++pairStats.overlapped;
			++snapshotBuilder.stagesPooled;
		} else {
			stage(0);
			if (buildMain)
				stage(1);
			snapshotBuilder.stagesInline += buildMain && smallChange ? 1u : 0u;
		}
		++pairStats.aheads;
		// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by each backing that holds them, as it is caught up (PayloadBuilder).
		if (!buildMain) {
			pairStats.stagesMs += msSince(stagesStart);
			return out;
		}
		mainBuild.Finish(out->streams);
		pairStats.stagesMs += msSince(stagesStart);
		// The participants' payloads: their halves after the main build (the shadow engine's region half, into the main region as it left
		// it), and the next frame's exclusions from it.
		for (auto* participant : participants)
			participant->BuildPayload(build);
		// Every payload of the publication holds the scene list's region as both builds left it: the shadow payload's view, taken
		// after its words, or without it the main region's; and the shadow payload where its own frame inputs follow the main ones'.
		ShareAheadRegion(*out, mainCache);
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
			b.buildsDiscarded += b.draws && !b.drawsPosted ? 1u : 0u;
			b.draws.reset();
			b.timing = {};
			b.stage = Stage::Make;
			b.fresh = true;
			publicationGated.store(false, std::memory_order_relaxed);  // new draws: the scene pass's end wakes nobody until they wait
		};
		++b.passes;
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
					// Not published yet: published once the frame will adopt it (the frame's take and the scene pass's end wake the
					// builder). No newer work replaces it: it waits held (a build starts only while the gate is open).
					if (!b.payload && PayloadWanted())
						PublishPayload();
					(void)rv.assembler.TrySelect();
					auto lease = rv.assembler.AcquireActive();
					const bool payloadReady = b.payload ? b.payload->Ready() : !PayloadWanted();
					if (lease && lease->Sequence() >= b.sealedSequence && payloadReady) {
						b.timing.recordingsMs = msSince(b.sealedAt);
						PostSnapshot(std::move(lease));
						b.stage = Stage::Idle;
					} else if (failedHere) {
						// A recording failed or was dropped by a graph build: made again (a failed epoch has none in it until its inputs
						// change; a dropped one is asked again once the inputs carry the new build), never kept (RevisionReuse).
						revisionReuse.valid = false;
						b.stage = Stage::Make;
					} else {
						return;
					}
					continue;
				}
				// One build in flight, and one build per adoption: the next starts from the newest work only while the publication gate
				// is open (the frame took the last snapshot posted and its scene pass ended: PublishPayload's), so a sealed build never
				// waits there for newer work to replace it. Work posted meanwhile is held untaken (latest wins); the coordinator publishes
				// no more until it is taken (SnapshotWorkTaken), so its passes fold into the frame's own publication, and the gate's
				// opening wakes the builder (publicationGated). A publication that moved only deltas passes through at once.
				if (auto next = WorkToBuild())
					takeWork(std::move(next));
				if (!b.work || !b.work->publication) {
					++b.idlePasses;
					return;
				}
				// Only deltas moved since the last snapshot posted (PassesThrough): that snapshot again for this publication, whose deltas
				// reach the frame with it (HandOverAtFrameStart walks the log up to the publication installed). Nothing built, sealed or
				// uploaded: a build would make these draws, this revision and this payload again from the same objects.
				if (b.stage == Stage::Make && std::exchange(b.fresh, false) && !snapshotInputsMoved.load(std::memory_order_acquire) &&
					PassesThrough(b.posted, *b.work)) {
					b.draws = b.posted.draws;
					b.payload = b.posted.payload;
					b.drawsSerial = b.posted.drawsSerial;
					b.publicationWaited = false;
					// The builds ahead done up to this item's number (as BuildSnapshotDraws: the frame's fallback streams wait on it).
					aheadDone.store(b.work->aheadNumber, std::memory_order_release);
					aheadDone.notify_all();
					++b.passedThrough;
					PostSnapshot(b.posted.revision);
					b.stage = Stage::Idle;
					continue;
				}
				if (b.stage == Stage::Idle) {
					// A snapshot of the work stands: made again when the frame's inputs moved (a graph build, a capture, new
					// resources), so its revision is the frame's as it is now.
					if (!snapshotInputsMoved.exchange(false, std::memory_order_acq_rel)) {
						++b.idlePasses;
						return;
					}
					b.timing = {};
				}
				// The revision (Stage::Make, or Waiting: made again at every wake - a growth settled, the inputs moved).
				snapshotInputsMoved.store(false, std::memory_order_release);
				const auto start = std::chrono::steady_clock::now();
				const std::uint64_t before = rv.assembler.LatestSequence();
				const auto& publication = *b.work->publication;
				MakeRevision(publication, publication.commitFrame);
				b.timing.shapesMs += msSince(start);
				// Sealed, or the last revision kept (its requirement pass unchanged: RevisionReuse), which is the assembler's latest.
				if (rv.assembler.LatestSequence() != before || revisionReuse.reusedNow) {
					b.sealedSequence = rv.assembler.LatestSequence();
					b.sealedAt = std::chrono::steady_clock::now();
					// The draws, for exactly the versions the sealed revision names (its adoption makes them current): built now, while its
					// recordings are made, or again after a make whose set names other versions than the ones they embed (a growth became
					// ready meanwhile), or for the frame's context when the work's was made for other main resources (RefreshWorkContext).
					// One consistent build: a snapshot never pairs draws and a revision of different versions.
					const auto& versions = rv.revisionVersions;
					const bool contextReplaced = RefreshWorkContext();
					if (!b.draws || contextReplaced || b.drawsSerial != versions->Serial())
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
			// The wake it waits for may have come already: inputs posted for a graph build (whose uploader replaced the one a sealed
			// revision's payload goes through) moved the inputs and woke this pass, which then failed on the old ones. Woken again, the next
			// pass makes the revision from the posted inputs; it clears the flag first, so a failure there does not wake it once more.
			if (b.work && snapshotInputsMoved.load(std::memory_order_acquire))
				WakeSnapshotBuilder();
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
		// the last one posted is still in the slot (the frame's take wakes the builder), nor before the scene pass of the frame that took
		// it has ended (its end wakes the builder). A build starts only while that gate is open (SnapshotPass), so only draws made again
		// for a snapshot already posted wait here. Posted on its uploads' completion, the snapshot is the frame's next.
		if (PublicationGateClosed()) {
			// The scene pass's end wakes the builder only while it waits here (NoteScenePassDone); the gate read again after saying so.
			publicationGated.store(true, std::memory_order_seq_cst);
			if (PublicationGateClosed()) {
				b.publicationsWaited += std::exchange(b.publicationWaited, true) ? 0u : 1u;
				return;
			}
		}
		publicationGated.store(false, std::memory_order_relaxed);
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
		b.drawsPosted = true;
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
		// What a publication changing only deltas passes through to (PassesThrough): a snapshot with a revision, complete draws and
		// their payload, and what it was built from (a context of its publication's toggles: BuildSnapshotDraws).
		auto& posted = b.posted;
		if (snapshot->revision && snapshot->drawsComplete && snapshot->payload && b.work->context.toggles == b.work->publication->toggles) {
			const auto& context = b.work->context;
			posted.publication = b.work->publication;
			posted.sun = b.work->sun;
			posted.light = b.work->light;
			posted.draws = b.draws;
			posted.payload = b.payload;
			posted.revision = snapshot->revision;
			posted.drawsSerial = b.drawsSerial;
			posted.valid = context.valid;
			posted.shadow = context.shadow;
			posted.build = context.build;
			posted.target = context.target.get();
			posted.shadowTarget = context.shadowTarget.get();
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
				posted.vsMask[j] = context.inputs[j].vsFrameMask;
				posted.psMask[j] = context.inputs[j].psFrameMask;
			}
		} else {
			posted = {};
		}
		// Latest wins: one the frame has not taken is dropped here, on the builder. Counted after the post (a waiting frame polls it).
		snapshotSlot.Post(std::move(snapshot));
		snapshotsPosted.fetch_add(1, std::memory_order_release);
		snapshotsBuilt.fetch_add(1, std::memory_order_relaxed);
		snapshotTimings.Push(b.timing);
	}

	std::shared_ptr<const void> IndirectDraws::AdoptSnapshot()
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
		using Stale = Impl::SnapshotStale;
		if (const std::size_t stale = s.StaleKindOf(*taken); stale != Stale::kStaleKinds) {
			// Not adopted (the adopted one is older still): the frame waits for a current one once DCLF draws (AdoptCurrentSnapshot). The
			// builder makes the snapshot again for the frame's inputs.
			++s.snapshotsStale[stale];
			s.snapshotStale = true;
			s.retiredSnapshots.Push(std::shared_ptr<const Impl::SceneSnapshot>(std::move(taken)));
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
			if (s.installedPayload)
				s.payloadsInstalled.fetch_add(1, std::memory_order_relaxed);
			// Its products, for the features that registered them (Api::PayloadProduct::install).
			Extensions::InstallPayloadProducts(s.installedPayload ? &s.installedPayload->products : nullptr);
		}
		s.snapshotStale = false;
		++s.snapshotsAdopted;
		if (Diagnostics::Armed<Diagnostics::SnapshotAdopted>()) {
			const std::uint32_t frameNow = Scene::FrameView::Get().GetFrame();
			Diagnostics::Notify(Diagnostics::SnapshotAdopted{ frameNow, snapshot->serial, versions.get(), snapshot->payload.get(), s.growthAdoptedFrame == frameNow });
		}
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

	std::size_t IndirectDraws::Impl::StaleKindOf(const SceneSnapshot& a_snapshot) const
	{
		// The one check: what a snapshot cannot know ahead, by generation. Its recordings of the graph as built now, its pipeline sets for the
		// targets and the shadow format as they are, its draws and shapes for the main resources as they are. Its commit's toggles are
		// none of it: installed, they become the frame's (Toggles::Adopt), and its revision was made for them (MakeRevision). One without
		// a revision (the publication alone) is adopted for its tables whatever the graph.
		if (!a_snapshot.revision)
			return kStaleKinds;
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& drawPipelines = DrawPipelines::Get();
		if (host && a_snapshot.buildGeneration != host->BuildGeneration())
			return kStaleBuild;
		if (a_snapshot.targetsGeneration != drawPipelines.Generation())
			return kStaleTargets;
		if (a_snapshot.shadowFormat != drawPipelines.ShadowFormat())
			return kStaleShadowFormat;
		if (a_snapshot.mainGeneration != postedInputs.mainGeneration)
			return kStaleMain;
		return kStaleKinds;
	}

	bool IndirectDraws::Impl::AdoptedCurrent() const
	{
		// What DecideEpochCoverage asks of the snapshot before it looks at the epochs' recordings, the generations checked again: a
		// snapshot adopted before this frame's build point is stale for a build it made.
		const auto& adopted = adoptedSnapshot;
		return adopted && adopted->revision && adopted->drawsComplete && StaleKindOf(*adopted) == kStaleKinds && installedPayload && installedPayload->Ready();
	}

	std::shared_ptr<const void> IndirectDraws::AdoptCurrentSnapshot(bool a_wait)
	{
		const std::uint64_t postedBefore = impl ? impl->snapshotsPosted.load(std::memory_order_acquire) : 0;
		auto publication = AdoptSnapshot();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!a_wait || !impl || failed || !host || impl->AdoptedCurrent())
			return publication;
		ZoneScopedN("CS.DCLF.WaitCurrentSnapshot");
		auto& s = *impl;
		using Clock = std::chrono::steady_clock;
		// Generous: a snapshot made again for a build (its recordings, its draws for new main resources and their payload) takes
		// milliseconds; five seconds without one is a builder that cannot make it, never a slow one.
		constexpr auto kTimeout = std::chrono::seconds(5);
		constexpr auto kSpin = std::chrono::milliseconds(2);
		constexpr auto kTicketPeriod = std::chrono::milliseconds(16);
		const auto start = Clock::now();
		auto ticketsAt = start;  // the build point gave back the unsubmitted epochs' tickets just now
		const auto staleBefore = s.snapshotsStale;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		// The frame adopts whatever is posted next: the publication gate's scene pass half open (this frame's pass is woken only when its
		// start ends), and the builder told to make its snapshot again for the inputs the build point posted.
		s.frameWaiting.store(true, std::memory_order_seq_cst);
		(void)s.SnapshotPump();
		s.snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
		std::uint64_t seen = postedBefore;
		for (;;) {
			// The owner thread's part of the build point, served while it waits: the shadow view slots a make asks for (sealed only once
			// the inputs carry their build) and an extension added or removed (built here, still before anything decides what the frame's
			// recordings cover); and the ready tickets a recording's slot waits for given back now and then (a released ticket is prepared
			// again, so not every poll).
			s.ServeRevisionRequests();
			if (s.revisions.explicitBuilds && host->RebuildRequested())
				s.BuildIfAsked(*host);
			if (const auto now = Clock::now(); now - ticketsAt >= kTicketPeriod) {
				s.ReleaseTicketsForRecordings(*host, 0);
				ticketsAt = now;
			}
			if (const std::uint64_t posted = s.snapshotsPosted.load(std::memory_order_acquire); posted != seen) {
				seen = posted;
				publication = AdoptSnapshot();
				if (s.AdoptedCurrent())
					break;
			}
			const auto waited = Clock::now() - start;
			if (waited > kTimeout) {
				const auto& adopted = s.adoptedSnapshot;
				const auto pumpStats = s.SnapshotPump().GetStats();
				std::string stale;
				for (std::size_t k = 0; k < Impl::kStaleKinds; ++k)
					stale += fmt::format("{}{}", k ? "/" : "", s.snapshotsStale[k] - staleBefore[k]);
				stl::report_and_fail(fmt::format(
					"Drawcall Limit Fix: frame {} waited {} s at its start for a scene snapshot current for the graph as built and never got one, and DCLF does "
					"not hand drawing back to the engine. Waiting for: build generation {} (rebuild requested: {}), main resources generation {}, targets "
					"generation {}. Stale snapshots taken meanwhile (build/targets/shadow format/main): {}; snapshots posted {}, recordings outstanding {}, "
					"inputs moved {}, publication gated {}, build ahead running {}. Adopted snapshot: serial {}, revision {}, draws complete {}, build "
					"generation {}, main generation {}, payload {}. The builder (read racily, for this message): stage {}, passes {} ({} idle), work {}, draws {}, "
					"payload {}; its pump: runner active {}, requested epoch {}, drained {}, notifications {}, passes {}, rejected {}",
					frame, std::chrono::duration_cast<std::chrono::seconds>(kTimeout).count(), host->BuildGeneration(), host->RebuildRequested(),
					s.postedInputs.mainGeneration, DrawPipelines::Get().Generation(), stale, seen, s.recordingsOutstanding.load(std::memory_order_acquire),
					s.snapshotInputsMoved.load(std::memory_order_acquire), s.publicationGated.load(std::memory_order_acquire),
					s.aheadRunning.load(std::memory_order_acquire), adopted ? adopted->serial : 0, adopted && adopted->revision, adopted && adopted->drawsComplete,
					adopted ? adopted->buildGeneration : 0, adopted ? adopted->mainGeneration : 0,
					!s.installedPayload ? "none" : s.installedPayload->Ready() ? "ready" : "NOT READY", static_cast<int>(s.snapshotBuilder.stage),
					s.snapshotBuilder.passes, s.snapshotBuilder.idlePasses, s.snapshotBuilder.work != nullptr, s.snapshotBuilder.draws != nullptr,
					s.snapshotBuilder.payload != nullptr, pumpStats.runnerActive, pumpStats.requestedEpoch, pumpStats.drainedEpoch, pumpStats.notifications,
					pumpStats.drainPasses, pumpStats.rejected));
			}
			if (waited < kSpin)
				std::this_thread::yield();
			else
				std::this_thread::sleep_for(std::chrono::microseconds(250));
		}
		s.frameWaiting.store(false, std::memory_order_release);
		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		++s.snapshotWaits;
		s.snapshotWaitMs += ms;
		s.snapshotWaitMaxMs = std::max(s.snapshotWaitMaxMs, ms);
		logger::info("[DCLF] frame {} waited {:.2f} ms at its start for a snapshot current for the graph as built (build generation {}, main resources "
					 "generation {}): adopted serial {}",
			frame, ms, host->BuildGeneration(), s.postedInputs.mainGeneration, s.adoptedSnapshot->serial);
		return publication;
	}

	void IndirectDraws::NoteScenePassDone(std::uint32_t a_frame)
	{
		if (!impl)
			return;
		// A builder holding a sealed snapshot's publication for this frame's pass publishes now; the pass's own publication woke it
		// already (PostSnapshotWork). Any other builder has nothing to do for the pass's end: not woken (publicationGated).
		impl->scenePassDoneFrame.store(a_frame, std::memory_order_seq_cst);
		if (impl->publicationGated.load(std::memory_order_seq_cst))
			WakeSnapshotBuilder();
		else
			impl->passDoneWakesSkipped.fetch_add(1, std::memory_order_relaxed);
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
		if (Diagnostics::Armed<Diagnostics::AdoptedBound>())
			Diagnostics::Notify(Diagnostics::AdoptedBound{ Scene::FrameView::Get().GetFrame(), point, FrameRetirement()->Completed() });
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
