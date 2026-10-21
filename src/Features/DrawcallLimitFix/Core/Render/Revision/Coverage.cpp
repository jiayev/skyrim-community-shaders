// The frame's side of the scene revisions, render thread: the build point, the revision inputs it posts (latest wins) and the coverage
// it decides at FrameDriver::BeginFrame, before the engine draws: which epochs the adopted snapshot's revision covers.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	void IndirectDraws::BuildPoint()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		// DCLF draws only from scene revisions' recordings, which need async, revision-driven epochs (CS_ORG_ASYNC_EPOCHS and
		// CS_ORG_REUSE_RECORDINGS): without them no frame is DCLF's.
		if (!host->AsyncEpochs() && !failed) {
			logger::error("[DCLF] the render graph runs without async epochs: no scene revision is recorded, and DCLF draws nothing");
			failed = true;
		}
		auto& rv = impl->revisions;
		// Explicit while scene revisions draw: an extension added or removed during a frame waits for this point, so the graph a
		// frame's revision was recorded on runs through that frame (a build invalidates every recording: the frame waits at its start for
		// a snapshot recorded on the new build, AdoptCurrentSnapshot). Failed, the graph builds at its next submission.
		const bool explicitBuilds = !failed;
		if (rv.explicitBuilds != explicitBuilds) {
			rv.explicitBuilds = explicitBuilds;
			host->SetExplicitBuilds(explicitBuilds);
		}
		impl->BuildIfAsked(*host);
		// The epochs the last frame did not submit (no water in view) give their tickets back while a revision waits for recordings.
		impl->ReleaseTicketsForRecordings(*host, RenderGraphRuntime::Get().TakeSubmittedSegments());
	}

	void IndirectDraws::Impl::BuildIfAsked(org::PersistentGraphHost& a_host)
	{
		// What the revision code asked of the owner thread (shadow view slots: their buffers and the extension declaring them), built
		// here with the rest.
		ServeRevisionRequests();
		if (revisions.explicitBuilds && a_host.BuildIfRequested())
			++revisions.builds;
		// The graph's build and its heaps, the pipelines' generations, the toggles: revision inputs, posted when they moved.
		PostRevisionInputs();
	}

	void IndirectDraws::Impl::ReleaseTicketsForRecordings(org::PersistentGraphHost& a_host, std::uint32_t a_submitted)
	{
		// A live epoch's recording waits for the slot its ready, unsubmitted ticket holds: an epoch outside a_submitted gives that ticket
		// back while a revision waits for recordings, so none waits on it. Recordings outstanding: an atomic the completions count down
		// (never the assembler's pending list, the builder's). A hosted epoch has no ticket of its own (its segment is its host's).
		if (!revisions.explicitBuilds || !recordingsOutstanding.load(std::memory_order_acquire))
			return;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e)
			if (const auto segment = EpochTraitsOf(e).segment;
				EpochTraitsOf(e).hostedBy == kNoEpoch && !(a_submitted & (1u << static_cast<std::uint32_t>(segment))))
				revisions.ticketsReleased += a_host.ReleaseEpochTicket(RenderGraphRuntime::EpochOf(segment)) ? 1 : 0;
	}

	void IndirectDraws::NoteRevisionInputs()
	{
		if (impl)
			impl->PostRevisionInputs();
	}

	namespace
	{
		/** @brief Whether a_next holds what a_previous posted (its generations aside): no new revision inputs to post. */
		bool SameRevisionInputs(const RevisionInputs& a_next, const RevisionInputs& a_previous)
		{
			return a_next.main == a_previous.main && a_next.shadow == a_previous.shadow && a_next.views == a_previous.views && a_next.scene == a_previous.scene &&
			       a_next.buildGeneration == a_previous.buildGeneration && SameHandle(a_next.resourceHeap, a_previous.resourceHeap) &&
			       SameHandle(a_next.samplerHeap, a_previous.samplerHeap) && a_next.targetsGeneration == a_previous.targetsGeneration &&
			       a_next.shadowFormat == a_previous.shadowFormat && a_next.shadowRasterStates == a_previous.shadowRasterStates && a_next.claims == a_previous.claims &&
			       a_next.shadowCandidates == a_previous.shadowCandidates && a_next.occlusionLayouts == a_previous.occlusionLayouts &&
			       a_next.shadowPlacements == a_previous.shadowPlacements && a_next.shadowActiveViews == a_previous.shadowActiveViews && a_next.mainKnown == a_previous.mainKnown && a_next.viewport == a_previous.viewport &&
			       a_next.blockSizes == a_previous.blockSizes &&
			       a_next.resourceInputs == a_previous.resourceInputs &&
			       a_next.shadowSlots == a_previous.shadowSlots && a_next.asyncEpochs == a_previous.asyncEpochs &&
			       !a_next.uploads.owner_before(a_previous.uploads) && !a_previous.uploads.owner_before(a_next.uploads);
		}
	}

	void IndirectDraws::Impl::PostRevisionInputs()
	{
		// Everything a revision reads of the frame, as the frame has it now (render thread).
		RevisionInputs next;
		next.main = resources;
		// The participants' parts (Revision/Participant.h: the shadow engine's, its resources, pipelines' format and states, layouts and
		// slots); the main segments' and the views' are below.
		for (auto* participant : participants)
			participant->PostInputs(next);
		next.scene = scene;
		if (auto* host = RenderGraphRuntime::Get().Host()) {
			next.buildGeneration = host->BuildGeneration();
			if (auto* descriptors = host->Descriptors()) {
				next.resourceHeap = descriptors->GetSRVDescriptorHeap().GetHandle();
				next.samplerHeap = descriptors->GetSamplerDescriptorHeap().GetHandle();
			}
			next.asyncEpochs = host->AsyncEpochs();
			next.uploads = host->RetainUploads();
		}
		auto& drawPipelines = DrawPipelines::Get();
		next.targetsGeneration = drawPipelines.Generation();
		next.claims = !IndirectDraws::Get().Failed();
		next.shadowCandidates = ShadowViews::Get().Candidates();
		next.mainKnown = mainEpochInputs.known;
		next.viewport = mainEpochInputs.viewport;
		next.blockSizes = mainEpochInputs.blockSizes;
		// The views', in slot order: a Target view's render area its imported target's, a MainColourViewport one's the colour commit's
		// viewport; its depth range its work's (the shape's until then). A view no feature took posts its defaults (no resources or
		// targets).
		for (const auto& slot : kViewSlots) {
			const auto& view = View(slot.view);
			const bool target = Extensions::ViewEntryOf(slot.view).area == Api::ViewArea::Target;
			next.views[ViewIndex(slot.view)] = { view.resources, view.targets,
				target ? (view.resources ? view.resources->width : 0u) : mainEpochInputs.viewport[kColourShape].width,
				target ? (view.resources ? view.resources->height : 0u) : mainEpochInputs.viewport[kColourShape].height, DepthRangeOf(slot.view) };
		}
		Extensions::PostSceneResourceInputs(next.resourceInputs);
		const auto& previous = postedInputs;
		next.mainGeneration = previous.mainGeneration + (next.main != previous.main ? 1u : 0u);
		const bool same = SameRevisionInputs(next, previous);
		if (same && previous.generation)
			return;
		next.generation = previous.generation + 1;
		postedInputs = next;
		revisionInputsSlot.Post(std::make_unique<RevisionInputs>(std::move(next)));
		// The snapshot builder makes its snapshot again for them (a revision of the frame's inputs as they are now).
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	bool IndirectDraws::DecideCoverage()
	{
		auto& rv = impl->revisions;
		if (failed) {
			impl->CoverFailedGraph();
			return true;
		}
		auto* host = RenderGraphRuntime::Get().Host();
		impl->DecideEpochCoverage(host);
		// The epochs flagged claims decide the frame's claims (the main ones): without their recordings DCLF has nothing to draw them
		// with, which only a frame before DCLF's first covered one may be (FrameDriver::BeginFrame; the frame waited for a current
		// snapshot first, AdoptCurrentSnapshot). A gated one's only while its class is on (the forward epoch's: its members' passes are
		// withheld with the frame's claims, and only the forward epoch draws them; a frame without its recording would lose them).
		bool main = true;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs && main; ++e)
			if (const auto& traits = EpochTraitsOf(e); traits.claims && !rv.covered[e])
				main = traits.gate != Api::ObjectClass::Count && !Extensions::ObjectClassOn(ActiveToggles(), traits.gate);
		rv.uncoveredFrames += main ? 0 : 1;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		// Each registered view's, in slot order, by its feature's callback.
		for (const auto& slot : kViewSlots)
			if (Extensions::ViewEntryOf(slot.view).covered)
				impl->DecideViewCoverage(slot.view, host, main, frame);
		return main;
	}

	std::string IndirectDraws::CoverageState() const
	{
		const auto& s = *impl;
		const auto& adopted = s.adoptedSnapshot;
		const auto* payload = s.InstalledPayload();
		auto* host = RenderGraphRuntime::Get().Host();
		std::string epochs;
		for (std::uint32_t e = 0, count = EpochCount(); e < count; ++e)
			epochs += fmt::format("{}{} {}{}", e ? ", " : "", EpochTraitsOf(e).name, s.revisions.covered[e] ? "covered" : "UNCOVERED", EpochTraitsOf(e).claims ? " (claims)" : "");
		return fmt::format("adopted snapshot serial {} (revision {}, draws complete {}, stale kind {} of {}, build generation {} against {}, main generation {} against {}), "
						   "newest taken stale {}, payload {}, adopted set {}; epochs: {}",
			adopted ? adopted->serial : 0, adopted && adopted->revision, adopted && adopted->drawsComplete, adopted ? s.StaleKindOf(*adopted) : static_cast<std::size_t>(Impl::kStaleKinds),
			static_cast<std::size_t>(Impl::kStaleKinds), adopted ? adopted->buildGeneration : 0, host ? host->BuildGeneration() : 0,
			adopted ? adopted->mainGeneration : 0, s.postedInputs.mainGeneration, s.snapshotStale, !payload ? "none" : payload->Ready() ? "ready" : "NOT READY",
			s.adoptedSet ? "named" : "none", epochs);
	}

	void IndirectDraws::Impl::CoverFailedGraph()
	{
		revisions.covered.fill(true);
		// Each registered view as its entry says, in slot order (the reflection's covered; nothing of the first person's is withheld from a
		// failed graph: the engine draws it).
		for (const auto& slot : kViewSlots) {
			if (!Extensions::ViewRegistered(slot.view))
				continue;
			const bool covered = Extensions::ViewEntryOf(slot.view).coveredOnFailedGraph;
			SetViewCovered(slot.view, covered);
			PassCapture::Get().SetViewCovered(slot.view, covered);
		}
	}

	void IndirectDraws::Impl::DecideEpochCoverage(org::PersistentGraphHost* a_host)
	{
		using R = SceneRevisions;
		auto& rv = revisions;
		auto* host = a_host;
		// Each epoch's recordings in the adopted snapshot's revision, of the graph as built now (the build point just ran), and its set the
		// adopted one (its versions made current at the adoption) - when the snapshot is current (AdoptSnapshot found none stale), its
		// draws are for the main resources and its payload is installed and ready (no payload: not covered; no fallback).
		rv.covered.fill(false);
		const auto& active = rv.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<NamedVersions>() : nullptr;
		const auto* payload = InstalledPayload();
		bool snapshot = adoptedSnapshot && adoptedSnapshot->drawsComplete && !snapshotStale && payload && payload->Ready();
		const bool current = host && snapshot && versions && versions.get() == adoptedSet;
		// The adopted draws embed the addresses of the versions its revision names (BuildSnapshotDraws builds them from the sealed set;
		// Diagnostics' SnapshotSealed checks it): the first frame whose draws embed the versions a growth's adoption made current.
		if (current && snapshot && growthAdoptedFrame != ~0u) {
			const std::uint32_t frameNow = Scene::FrameView::Get().GetFrame();
			growthToDraws.push_back(frameNow >= growthAdoptedFrame ? frameNow - growthAdoptedFrame : 0u);
			growthAdoptedFrame = ~0u;
		}
		if (current && snapshot)
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				// An epoch's recordings are all of the build its versions were made at (AssembleRevision asks for them again after one). A
				// hosted epoch's are its host's (an earlier row), covered while they were recorded with its shape (RevisionRecordings::
				// Carries: their subsets with it present carry its passes' work).
				const auto hostedBy = EpochTraitsOf(e).hostedBy;
				const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + (hostedBy != kNoEpoch ? hostedBy : e));
				rv.covered[e] = recordings && !recordings->recordings.empty() && recordings->recordings.front() &&
				                (hostedBy == kNoEpoch ? host->EpochRecordingCurrent(*recordings->recordings.front()) : rv.covered[hostedBy] && recordings->Carries(e));
			}
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e)
			rv.uncovered[e] += rv.covered[e] ? 0 : 1;
		Diagnostics::Notify(Diagnostics::EpochCoverageDecided{ Scene::FrameView::Get().GetFrame(), std::span<const bool>(rv.covered.data(), EpochCount()),
			adoptedSnapshot ? adoptedSnapshot->serial : 0, adoptedSnapshot && adoptedSnapshot->drawsComplete, snapshotStale, payload, versions != nullptr, current });
	}

	void IndirectDraws::Impl::DecideViewCoverage(ViewKind a_kind, org::PersistentGraphHost* a_host, bool a_main, std::uint32_t a_frame)
	{
		// What a registered view's coverage is decided from (Api::ViewCoverage), every input read whatever the others: its feature's
		// callback decides (the first person's: its members withheld from the engine, PassCapture, and drawn by its epochs on a frame the
		// engine draws it, whose claims stand, both epochs covered - its depth epoch's passes in the depth epoch's recordings, its colour
		// epoch's own recording - and its target imported into the graph as built; the reflection's: on a frame whose claims stand, its
		// host's recordings with its passes in the revision (the depth epoch's: a hosted epoch is covered with its host,
		// DecideEpochCoverage) and its cube imported into the graph as built).
		const auto& rv = revisions;
		const auto& epochs = ViewEpochsOf(a_kind).epochs;
		auto& view = View(a_kind);
		const auto& r = view.resources;
		Api::ViewCoverage coverage;
		coverage.main = a_main;
		coverage.on = Extensions::ViewOn(ActiveToggles(), a_kind);
		coverage.epochsCovered = rv.covered[epochs[0]] && (epochs[1] == kNoEpoch || rv.covered[epochs[1]]);
		coverage.targetImported = r && r->target;
		coverage.rebuildPending = r && r->rebuildPending;
		coverage.mainResourcesCurrent = r && r->main && r->main == resources && r->main->native[1];
		coverage.rebuildRequested = a_host && a_host->RebuildRequested();
		coverage.frame = a_frame;
		const bool covered = Extensions::ViewEntryOf(a_kind).covered(coverage);
		SetViewCovered(a_kind, covered);
		PassCapture::Get().SetViewCovered(a_kind, covered);
	}

	bool IndirectDraws::Impl::EpochCovered(std::uint32_t a_epoch) const
	{
		return a_epoch < revisions.covered.size() && revisions.covered[a_epoch];
	}
}
