#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

namespace DCLF
{
	/*
	 * The frame's side of the scene revisions: the build point, the revision inputs it posts (latest wins) and the coverage it
	 * decides at BeginSceneFrame, before the engine draws: which epochs the adopted snapshot's revision covers (render thread).
	 */

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
		// engine's, DecideCoverage). Without them (or failed), the graph builds at its next submission, as before.
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
		// The graph's build and its heaps, the pipelines' generations, the toggles: revision inputs (T6b3b b2a), posted when they moved.
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

	void IndirectDraws::Impl::PostRevisionInputs()
	{
		// Everything a revision reads of the frame, as the frame has it now (render thread).
		RevisionInputs next;
		next.main = resources;
		next.shadow = shadow;
		next.scene = scene;
		bool deferred = false;
		if (auto* host = RenderGraphRuntime::Get().Host()) {
			next.buildGeneration = host->BuildGeneration();
			if (auto* descriptors = host->Descriptors()) {
				next.resourceHeap = descriptors->GetSRVDescriptorHeap().GetHandle();
				next.samplerHeap = descriptors->GetSamplerDescriptorHeap().GetHandle();
			}
			next.asyncEpochs = host->AsyncEpochs();
			next.uploads = host->RetainUploads();
			deferred = next.asyncEpochs && host->Uploads();
		}
		// Whether growths are graph work: read by the revision code and the commits alike (Growths::Deferred), so set here at once.
		Growths::Get().deferred.store(deferred, std::memory_order_release);
		auto& drawPipelines = DrawPipelines::Get();
		next.targetsGeneration = drawPipelines.Generation();
		next.shadowFormat = drawPipelines.ShadowFormat();
		next.shadowRasterStates = drawPipelines.ShadowRasterStateCount();
		next.toggles = ActiveToggles();
		next.togglesGeneration = Toggles::Get().Generation();
		next.gbufferProbe = Diagnostics::Observe().gbufferProbe;
		next.claims = !IndirectDraws::Get().Failed();
		next.shadowCandidates = ShadowViews::Get().Candidates();
		next.occlusionLayouts = PredictedOcclusion();
		next.shadowPlacements = shadowPlacements;
		next.mainKnown = mainEpochInputs.known;
		next.viewport = mainEpochInputs.viewport;
		next.blockSizes = mainEpochInputs.blockSizes;
		next.views[ViewIndex(ViewKind::Reflection)] = { reflection.resources, reflection.targets, reflection.resources ? reflection.resources->width : 0u,
			reflection.resources ? reflection.resources->height : 0u, 1.0f };
		next.views[ViewIndex(ViewKind::FirstPerson)] = { firstPerson.resources, firstPerson.targets, mainEpochInputs.viewport[kColourShape].width,
			mainEpochInputs.viewport[kColourShape].height, firstPerson.depthRange };
		next.portalWords = static_cast<std::uint32_t>(PortalViews::Get().Encoded().size());
		auto& mirror = Scene::FrameView::Get().TreeLodMirror();
		next.treeLodShapeSlots = mirror.ShapeSlots();
		next.treeLodMeshSlots = mirror.MeshSlots();
		next.shadowRowsWanted = shadowRowsWanted;
		if (shadow && (!postedShadowSlots || postedShadowSlots->sequences.size() != shadow->sequences.size()))
			postedShadowSlots = std::make_shared<const ShadowSlotBuffers>(ShadowSlotBuffers{ shadow->sequences, shadow->bucketCounts });
		next.shadowSlots = shadow ? postedShadowSlots : nullptr;
		const auto& previous = postedInputs;
		next.mainGeneration = previous.mainGeneration + (next.main != previous.main ? 1u : 0u);
		const bool same = next.main == previous.main && next.shadow == previous.shadow && next.views == previous.views && next.scene == previous.scene &&
		                  next.buildGeneration == previous.buildGeneration && SameHandle(next.resourceHeap, previous.resourceHeap) &&
		                  SameHandle(next.samplerHeap, previous.samplerHeap) && next.targetsGeneration == previous.targetsGeneration &&
		                  next.shadowFormat == previous.shadowFormat && next.shadowRasterStates == previous.shadowRasterStates && next.toggles == previous.toggles &&
		                  next.togglesGeneration == previous.togglesGeneration && next.gbufferProbe == previous.gbufferProbe && next.claims == previous.claims &&
		                  next.shadowCandidates == previous.shadowCandidates && next.occlusionLayouts == previous.occlusionLayouts &&
		                  next.shadowPlacements == previous.shadowPlacements && next.mainKnown == previous.mainKnown && next.viewport == previous.viewport &&
		                  next.blockSizes == previous.blockSizes && next.portalWords == previous.portalWords &&
		                  next.treeLodShapeSlots == previous.treeLodShapeSlots && next.treeLodMeshSlots == previous.treeLodMeshSlots &&
		                  next.shadowRowsWanted == previous.shadowRowsWanted && next.shadowSlots == previous.shadowSlots && next.asyncEpochs == previous.asyncEpochs &&
		                  !next.uploads.owner_before(previous.uploads) && !previous.uploads.owner_before(next.uploads);
		if (same && previous.generation)
			return;
		next.generation = previous.generation + 1;
		postedInputs = next;
		revisionInputsSlot.Post(std::make_unique<RevisionInputs>(std::move(next)));
		// T6b3b: the snapshot builder makes its snapshot again for them (a revision of the frame's inputs as they are now).
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	bool IndirectDraws::DecideCoverage()
	{
		using R = Impl::SceneRevisions;
		auto& rv = impl->revisions;
		auto& reflection = impl->reflection;
		auto& fp = impl->firstPerson;
		if (failed) {
			rv.covered.fill(true);
			reflection.covered = true;
			PassCapture::Get().SetReflectionCovered(true);
			// Nothing of the first person's is withheld from a failed graph (the engine draws it).
			fp.covered = false;
			PassCapture::Get().SetFirstPersonCovered(false);
			return true;
		}
		// Each epoch's recordings in the adopted snapshot's revision, of the graph as built now (the build point just ran), and its versions
		// the current ones - when the snapshot is current (AdoptSnapshot found none stale) and its draws are for the main resources.
		rv.covered.fill(false);
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& active = rv.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		bool snapshot = impl->adoptedSnapshot && impl->adoptedSnapshot->drawsComplete && !impl->snapshotStale;
		const bool current = host && snapshot && versions && versions->changes == VersionRegistry::Get().changes;
		// T6b3b, the guard (unconditional, one compare a payload): the adopted draws embed the face positions of the version current now
		// (the one the revision names: BuildSnapshotDraws builds them so). A snapshot whose draws embed another would have its vertex
		// shaders read a version nothing holds: its frames are withdrawn, and the builder makes the snapshot again.
		if (current && impl->scene && impl->adoptedSnapshot->draws) {
			if (!Impl::DrawsEmbedFacePositions(*impl->adoptedSnapshot->draws, impl->scene->facePositionsAddress)) {
				if (impl->staleDraws++ == 0)
					logger::warn("[DCLF] the adopted snapshot's draws of frame {} embed face positions of another version than the current one: withdrawn <- STALE DRAWS",
						Scene::FrameView::Get().GetFrame());
				snapshot = false;
				impl->snapshotInputsMoved.store(true, std::memory_order_release);
				WakeSnapshotBuilder();
			} else if (impl->growthAdoptedFrame != ~0u) {
				// The first frame whose draws embed the versions a growth's adoption made current.
				const std::uint32_t frameNow = Scene::FrameView::Get().GetFrame();
				impl->growthToDraws.push_back(frameNow >= impl->growthAdoptedFrame ? frameNow - impl->growthAdoptedFrame : 0u);
				impl->growthAdoptedFrame = ~0u;
			}
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
		// The main epochs decide the frame's claims: without their recordings the frame is the engine's (Coordinator::WithdrawSet).
		// T7d: with the forward toggle on, the forward epoch's too: its members' passes are withheld with the frame's claims, and only the
		// forward epoch draws them (a frame without its recording would lose them).
		const bool main = rv.covered[kDepthShape] && rv.covered[kColourShape] && (!ActiveToggles().forward || rv.covered[Epoch::Forward]);
		rv.withdrawn += main ? 0 : 1;
		// The faces' withholding (PassCapture), before the engine renders them: the claims stand and the cube the epoch draws into is
		// in the graph as built (its import, at the epoch, is built at the next build point).
		const auto& resources = impl->resources;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		// The faces draw from the frame's scene list and the frame before's colour frame record (ExecuteReflection): not
		// after a frame whose main epochs were not submitted (no claims), nor once a growth of the rows was adopted since (this frame's
		// selection), which leaves those addresses on a version nothing holds.
		const bool inputs = resources && resources->committed[kDepthShape].frame == resources->committed[kColourShape].frame &&
		                    frame - resources->committed[kDepthShape].frame <= 1 && resources->committed[kDepthShape].rowsGeneration == resources->MainRowsGeneration() &&
		                    resources->committed[kColourShape].rowsGeneration == resources->MainRowsGeneration() &&
		                    impl->scene && resources->committed[kDepthShape].sceneGeneration == impl->scene->generation &&
		                    resources->committed[kDepthShape].objectCapacity == resources->objectCapacity;
		reflection.covered = main && inputs && rv.covered[Epoch::Reflection] && reflection.resources && reflection.resources->target && !host->RebuildRequested();
		PassCapture::Get().SetReflectionCovered(reflection.covered);
		// T7e: the first-person members are withheld from the engine (PassCapture) and drawn by the first-person epochs on a frame the engine
		// draws first person (the latch), whose claims stand, both epochs' recordings in the revision and the depth copy imported into the
		// graph as built. Its members need no frame before's inputs: both epochs draw this frame's scene list with this frame's camera.
		fp.covered = main && ActiveToggles().firstPerson && fp.latch && fp.latchFrame == frame && rv.covered[Epoch::FirstPersonDepth] && rv.covered[Epoch::FirstPersonColour] &&
		             fp.resources && fp.resources->target && !fp.resources->rebuildPending && fp.resources->main && fp.resources->main == impl->resources &&
		             fp.resources->main->native[1] && !host->RebuildRequested();
		PassCapture::Get().SetFirstPersonCovered(fp.covered);
		return main;
	}

	bool IndirectDraws::Impl::EpochCovered(std::uint32_t a_epoch) const
	{
		return a_epoch < revisions.covered.size() && revisions.covered[a_epoch];
	}
}
#endif
