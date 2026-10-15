// The frame's side of the scene revisions, render thread: the build point, the revision inputs it posts (latest wins) and the coverage
// it decides at FrameDriver::BeginFrame, before the engine draws: which epochs the adopted snapshot's revision covers.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

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
		// frame's revision was recorded on runs through that frame (a build invalidates every recording: the frame after one is the
		// engine's, DecideCoverage). Failed, the graph builds at its next submission.
		const bool explicitBuilds = !failed;
		if (rv.explicitBuilds != explicitBuilds) {
			rv.explicitBuilds = explicitBuilds;
			host->SetExplicitBuilds(explicitBuilds);
		}
		// What the revision code asked of the owner thread (shadow view slots: their buffers and the extension declaring them), built
		// here with the rest.
		impl->ServeRevisionRequests();
		if (explicitBuilds && host->BuildIfRequested())
			++rv.builds;
		// The graph's build and its heaps, the pipelines' generations, the toggles: revision inputs, posted when they moved.
		impl->PostRevisionInputs();
		// A live epoch's recording waits for the slot its ready, unsubmitted ticket holds: an epoch the last frame did not submit (no
		// water in view, a frame without claims) gives that ticket back while a revision waits for recordings, so none waits on it.
		const std::uint32_t submitted = RenderGraphRuntime::Get().TakeSubmittedSegments();
		// Recordings outstanding: an atomic the completions count down (never the assembler's pending list, the coordinator's).
		if (explicitBuilds && impl->recordingsOutstanding.load(std::memory_order_acquire))
			for (std::uint32_t e = 0; e < Impl::SceneRevisions::kEpochs; ++e)
				if (const auto segment = kEpochTraits[e].segment; !(submitted & (1u << static_cast<std::uint32_t>(segment))))
					rv.ticketsReleased += host->ReleaseEpochTicket(RenderGraphRuntime::EpochOf(segment)) ? 1 : 0;
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
			       a_next.shadowFormat == a_previous.shadowFormat && a_next.shadowRasterStates == a_previous.shadowRasterStates && a_next.toggles == a_previous.toggles &&
			       a_next.togglesGeneration == a_previous.togglesGeneration && a_next.claims == a_previous.claims &&
			       a_next.shadowCandidates == a_previous.shadowCandidates && a_next.occlusionLayouts == a_previous.occlusionLayouts &&
			       a_next.shadowPlacements == a_previous.shadowPlacements && a_next.mainKnown == a_previous.mainKnown && a_next.viewport == a_previous.viewport &&
			       a_next.blockSizes == a_previous.blockSizes && a_next.portalWords == a_previous.portalWords &&
			       a_next.treeLodShapeSlots == a_previous.treeLodShapeSlots && a_next.treeLodMeshSlots == a_previous.treeLodMeshSlots &&
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
		next.shadow = shadow;
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
		next.shadowFormat = drawPipelines.ShadowFormat();
		next.shadowRasterStates = drawPipelines.ShadowRasterStateCount();
		next.toggles = ActiveToggles();
		next.togglesGeneration = Toggles::Get().Generation();
		next.claims = !IndirectDraws::Get().Failed();
		next.shadowCandidates = ShadowViews::Get().Candidates();
		next.occlusionLayouts = PredictedOcclusion();
		next.shadowPlacements = shadowPlacements;
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
		next.portalWords = static_cast<std::uint32_t>(PortalViews::Get().Encoded().size());
		auto& mirror = Scene::FrameView::Get().TreeLodMirror();
		next.treeLodShapeSlots = mirror.ShapeSlots();
		next.treeLodMeshSlots = mirror.MeshSlots();
		Extensions::PostSceneResourceInputs(next.resourceInputs);
		if (shadow && (!postedShadowSlots || postedShadowSlots->sequences.size() != shadow->sequences.size()))
			postedShadowSlots = std::make_shared<const ShadowSlotBuffers>(ShadowSlotBuffers{ shadow->sequences, shadow->bucketCounts });
		next.shadowSlots = shadow ? postedShadowSlots : nullptr;
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
		// The main epochs decide the frame's claims: without their recordings the frame is the engine's (FrameView::WithdrawSet).
		// With the forward toggle on, the forward epoch's too: its members' passes are withheld with the frame's claims, and only the
		// forward epoch draws them (a frame without its recording would lose them).
		const bool main = rv.covered[kDepthShape] && rv.covered[kColourShape] && (!ActiveToggles().forward || rv.covered[Epoch::Forward]);
		rv.withdrawn += main ? 0 : 1;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		// Each registered view's, in slot order, by its feature's callback.
		for (const auto& slot : kViewSlots)
			if (Extensions::ViewEntryOf(slot.view).covered)
				impl->DecideViewCoverage(slot.view, host, main, frame);
		return main;
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
			for (std::uint32_t e = 0; e < R::kEpochs; ++e) {
				// An epoch's recordings are all of the build its versions were made at (AssembleRevision asks for them again after one).
				const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + e);
				rv.covered[e] = recordings && !recordings->recordings.empty() && recordings->recordings.front() &&
				                host->EpochRecordingCurrent(*recordings->recordings.front());
			}
		for (std::uint32_t e = 0; e < R::kEpochs; ++e)
			rv.uncovered[e] += rv.covered[e] ? 0 : 1;
	}

	void IndirectDraws::Impl::DecideViewCoverage(ViewKind a_kind, org::PersistentGraphHost* a_host, bool a_main, std::uint32_t a_frame)
	{
		// What a registered view's coverage is decided from (Api::ViewCoverage), every input read whatever the others: its feature's
		// callback decides (the first person's: its members withheld from the engine, PassCapture, and drawn by its epochs on a frame the
		// engine draws it, whose claims stand, both epochs' recordings in the revision and its target imported into the graph as built; the
		// reflection's: on a frame whose claims stand, drawing the frame before's colour inputs, its cube imported into the graph as built).
		const auto& rv = revisions;
		const auto& slot = kViewSlots[ViewIndex(a_kind)];
		auto& view = View(a_kind);
		const auto& r = view.resources;
		Api::ViewCoverage coverage;
		coverage.main = a_main;
		coverage.on = Extensions::ViewOn(ActiveToggles(), a_kind);
		coverage.epochsCovered = rv.covered[slot.epochs[0]] && (slot.epochs[1] == Epoch::Count || rv.covered[slot.epochs[1]]);
		coverage.targetImported = r && r->target;
		coverage.rebuildPending = r && r->rebuildPending;
		coverage.mainResourcesCurrent = r && r->main && r->main == resources && r->main->native[1];
		coverage.rebuildRequested = a_host && a_host->RebuildRequested();
		// The frame before's colour inputs (a view drawing the frame's scene list with the frame before's colour frame record): both commits
		// of one frame, this one or the one before, of the adopted set, after the installed payload.
		const std::uint64_t adoptedSerial = Adopted().Serial();
		coverage.frameBeforeColourHeld = resources && resources->committed[kDepthShape].frame == resources->committed[kColourShape].frame &&
		                                 a_frame - resources->committed[kDepthShape].frame <= 1 && resources->committed[kDepthShape].adoptedSerial == adoptedSerial &&
		                                 resources->committed[kColourShape].adoptedSerial == adoptedSerial &&
		                                 installedPayloadFrame <= resources->committed[kColourShape].frame && scene;
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
