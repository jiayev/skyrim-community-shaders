#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	using org::async::RevisionFragment;

	std::shared_ptr<const org::BufferVersion> Draws::VersionSet::Find(const org::VersionedBuffer& a_buffer) const noexcept
	{
		const void* key = a_buffer.Key();
		const auto it = std::lower_bound(versions.begin(), versions.end(), key,
			[](const auto& a_entry, const void* a_key) { return std::less<const void*>{}(a_entry.first, a_key); });
		return it != versions.end() && it->first == key ? it->second : nullptr;
	}

	std::shared_ptr<const Draws::VersionSet> Draws::VersionSet::Snapshot()
	{
		auto set = std::make_shared<VersionSet>();
		auto& registry = VersionRegistry::Get();
		// The buffers registered since (from any thread), then the live ones: the revision code's own list.
		registry.TakeRegistered();
		auto& buffers = registry.buffers;
		std::erase_if(buffers, [](const auto& a_buffer) { return a_buffer.expired(); });
		struct Named
		{
			const void* key;
			std::shared_ptr<const org::BufferVersion> version;
			std::weak_ptr<org::VersionedBuffer> buffer;
		};
		std::vector<Named> named;
		named.reserve(buffers.size());
		// Current under the frame's changes as last published (the frame's own value moves only at its adoptions).
		set->changes = registry.Published();
		const auto& growths = Growths::Get();
		bool pending = false;
		for (const auto& weak : buffers)
			if (const auto buffer = weak.lock()) {
				auto version = growths.Ready(buffer);
				pending = pending || version;
				named.push_back({ buffer->Key(), version ? std::move(version) : buffer->Current(), weak });
			}
		if (pending)
			set->changes = registry.Next();
		std::sort(named.begin(), named.end(), [](const Named& a, const Named& b) { return std::less<const void*>{}(a.key, b.key); });
		set->versions.reserve(named.size());
		set->buffers.reserve(named.size());
		for (auto& entry : named) {
			set->versions.emplace_back(entry.key, std::move(entry.version));
			set->buffers.push_back(std::move(entry.buffer));
		}
		return set;
	}

	bool Draws::VersionSet::Current() const
	{
		// The set's own buffers (the frame never reads the revision code's list): a buffer made after the set (not in it) is one no
		// recording of its revision names, so it moves nothing the set binds.
		for (std::size_t i = 0; i < buffers.size() && i < versions.size(); ++i)
			if (const auto buffer = buffers[i].lock())
				if (versions[i].second && versions[i].second != buffer->Current())
					return false;
		return true;
	}

	bool Draws::Growths::Deferred()
	{
		// As the frame posted it (PostRevisionInputs: the host's async epochs and uploader are the owner thread's).
		return Get().deferred.load(std::memory_order_acquire);
	}

	bool Draws::Growths::OwnerDeferred()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host && host->AsyncEpochs() && host->Uploads();
	}

	namespace
	{
		// Revision shapes' generations (their passes' invocation revisions): apart from the live shapes'.
		std::uint64_t NextRevisionGeneration()
		{
			static std::uint64_t generation = 1ull << 48;
			return ++generation;
		}

		/** @brief a_made, unless a_old is the same shape (then a_old: its fragment, and its recording, stand). */
		template <class Frame>
		std::shared_ptr<const Frame> Kept(const std::shared_ptr<const Frame>& a_old, const std::shared_ptr<const Frame>& a_made, bool& a_changed)
		{
			if (a_old && a_made && a_old->SameShape(*a_made))
				return a_old;
			a_changed = true;
			auto copy = std::make_shared<Frame>(*a_made);
			copy->generation = NextRevisionGeneration();
			return copy;
		}

		/** @brief The recordings of one epoch's request, completed on ORG's host thread: the fragment resolves once all are recorded. */
		struct PendingRecordings
		{
			std::shared_ptr<RevisionFragment> fragment;
			std::vector<std::shared_ptr<const org::PersistentGraphHost::EpochRecording>> recordings;
			std::atomic<std::uint32_t> remaining = 0;
			std::atomic<bool> failed = false;
			// The build the revision's versions are of (RevisionInputs::buildGeneration): a recording of another is none of its.
			std::uint64_t buildGeneration = 0;
			// The epoch's counters (SceneRevisions::Epoch), the failures logged, and the recordings outstanding (Impl::recordingsOutstanding).
			std::atomic<std::uint64_t>*recorded = nullptr, *failedCount = nullptr, *droppedCount = nullptr, *shapes = nullptr;
			std::atomic<std::uint32_t>* logged = nullptr;
			std::atomic<std::uint32_t>* outstanding = nullptr;
			const char* name = "";
		};
	}

	void IndirectDraws::Impl::AssembleRevision(std::uint32_t a_frame)
	{
		ZoneScopedN("CS.DCLF.AssembleRevision");
		using R = SceneRevisions;
		auto& rv = revisions;
		// A growth still pending (G2): the publication's claims may reach past the versions a revision made now could name, so none is
		// made; the builder makes it again when the growth settles (it wakes the builder).
		if (rv.growthPending) {
			++rv.growthWaits;
			return;
		}
		// A graph build asked of the frame (shadow view slots: ServeRevisionRequests) not made yet: a recording now would be of the graph
		// without what it declares, which that build supersedes. None is sealed until the inputs carry it (one integer compare; the
		// inputs' post wakes the builder).
		if (producer.buildWanted && producer.inputs.buildGeneration < producer.buildWanted) {
			++rv.buildWaits;
			return;
		}
		producer.buildWanted = 0;
		auto draft = rv.assembler.Begin();
		// The versions: again when a growth was adopted or became ready to name, or the graph was built again (with the buffers made
		// since: a recording is of the build it was made on, so every epoch is recorded again for the new set). The build as the frame
		// posted it (RevisionInputs: BuildPoint's).
		const std::uint64_t builds = producer.inputs.buildGeneration;
		if (const std::uint64_t changes = VersionRegistry::Get().Published(); rv.versionChanges != changes || rv.growthStamp != Growths::Get().stamp ||
			rv.graphBuilds != builds || !draft.Get(R::kVersionsSlot)) {
			draft.Set(R::kVersionsSlot, RevisionFragment::MakeReady(VersionSet::Snapshot()));
			rv.versionChanges = changes;
			rv.growthStamp = Growths::Get().stamp;
			rv.graphBuilds = builds;
			++rv.versionSets;
		}
		const auto& versionsFragment = draft.Get(R::kVersionsSlot);
		const auto versions = versionsFragment->Value<VersionSet>();

		// Each epoch's shape (MakeRevisionShapes'): its fragment kept while the shape is the same.
		auto setShape = [&]<class Value>(std::uint32_t a_epoch, std::shared_ptr<const Value> a_value) {
			draft.Set(R::kShapeSlot + a_epoch, RevisionFragment::MakeReady(std::move(a_value)));
			++rv.epochs[a_epoch].changed;
			// A parity make under the last make's key changed it: an input RevisionShapesKey does not name.
			if (shapesKeyUnchanged && shapeKeyMisses[a_epoch]++ < 4)
				logger::warn("[DCLF] revision parity: the {} epoch's shape changed at frame {} under an unchanged shapes key <- SHAPE KEY", R::kNames[a_epoch], a_frame);
		};
		// The producer's newest shapes (MakeRevision's).
		const auto& shapes = producer.made;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			const auto& made = shapes.main[shape][0];
			if (!made)
				continue;
			const auto& current = draft.Get(R::kShapeSlot + static_cast<std::uint32_t>(shape));
			bool changed = false;
			auto kept = Kept<PassFrame>(current ? current->Value<PassFrame>() : nullptr, made, changed);
			if (changed)
				setShape(static_cast<std::uint32_t>(shape), std::move(kept));
		}
		for (std::uint32_t kind = 0; kind < 2; ++kind) {
			const auto& made = shapes.shadow[kind][0];
			if (made.empty())
				continue;
			const std::uint32_t epoch = 2 + kind;
			const auto& current = draft.Get(R::kShapeSlot + epoch);
			const auto old = current ? current->Value<ShadowVariants>() : nullptr;
			auto variants = std::make_shared<ShadowVariants>();
			bool changed = !old || old->shapes.size() != made.size();
			for (std::size_t v = 0; v < made.size(); ++v)
				variants->shapes.push_back(Kept<ShadowFrame>(old && v < old->shapes.size() ? old->shapes[v] : nullptr, made[v], changed));
			if (changed)
				setShape(epoch, std::shared_ptr<const ShadowVariants>(std::move(variants)));
		}
		if (const auto& made = shapes.reflection[0]) {
			const auto& current = draft.Get(R::kShapeSlot + 4);
			bool changed = false;
			auto kept = Kept<ReflectionFrame>(current ? current->Value<ReflectionFrame>() : nullptr, made, changed);
			if (changed)
				setShape(4, std::move(kept));
		}

		// Each epoch's recording: kept while it requires exactly the draft's shape and versions; else requested for them. A failed
		// one is dropped (the epoch has none in the revision) and not asked for again until its shape or the versions change.
		auto* host = RenderGraphRuntime::Get().Host();
		const bool recordable = host && producer.inputs.asyncEpochs;
		// The scene's sizing its versions have (the growths it names): what its passes size by.
		const auto& sceneState = producer.inputs.scene;
		const auto sceneSizing = sceneState ? std::make_shared<const SceneSizing>(Growths::Get().RevisionSizing<SceneSizing>(*sceneState)) : nullptr;
		for (std::uint32_t e = 0; e < R::kEpochs; ++e) {
			const std::uint32_t slot = R::kRecordingSlot + e;
			const auto& shapeFragment = draft.Get(R::kShapeSlot + e);
			const auto current = draft.Get(slot);
			auto requiresExactly = [&](const RevisionFragment& a_fragment) {
				const auto& requirements = a_fragment.Requirements();
				return requirements.size() == 2 && requirements[0].fragment == versionsFragment && requirements[1].fragment == shapeFragment;
			};
			if (!recordable || !shapeFragment) {
				draft.Set(slot, nullptr);
				continue;
			}
			if (current && requiresExactly(*current)) {
				if (current->GetState() != RevisionFragment::State::Failed)
					continue;
				// Failed for these exact inputs: none in this revision, and none asked for until they change.
				draft.Set(slot, nullptr);
				rv.failedFor[e] = { versionsFragment, shapeFragment };
				continue;
			}
			if (rv.failedFor[e].first == versionsFragment && rv.failedFor[e].second == shapeFragment) {
				draft.Set(slot, nullptr);
				continue;
			}
			// The host data of each of the epoch's shapes (a shadow epoch's variants).
			std::vector<std::shared_ptr<const EpochRevisionData>> requests;
			auto request = [&](auto&& a_fill) {
				auto data = std::make_shared<EpochRevisionData>();
				data->versions = versions;
				a_fill(data->shapes);
				data->shapes.scene = sceneSizing;
				requests.push_back(std::move(data));
			};
			if (e < 2) {
				request([&](RevisionShapes& a_shapes) { a_shapes.main[e] = shapeFragment->Value<PassFrame>(); });
			} else if (e < 4) {
				for (const auto& variant : shapeFragment->Value<ShadowVariants>()->shapes)
					request([&](RevisionShapes& a_shapes) { (e == 2 ? a_shapes.shadow : a_shapes.occlusion) = variant; });
			} else {
				request([&](RevisionShapes& a_shapes) { a_shapes.reflection = shapeFragment->Value<ReflectionFrame>(); });
			}
			auto work = std::make_shared<PendingRecordings>();
			work->fragment = std::make_shared<RevisionFragment>(std::vector<RevisionFragment::Requirement>{ { R::kVersionsSlot, versionsFragment },
				{ R::kShapeSlot + e, shapeFragment } });
			work->recordings.resize(requests.size());
			work->remaining.store(static_cast<std::uint32_t>(requests.size()), std::memory_order_relaxed);
			work->buildGeneration = builds;
			work->recorded = &rv.epochs[e].recorded;
			work->failedCount = &rv.epochs[e].failed;
			work->droppedCount = &rv.epochs[e].dropped;
			work->shapes = &rv.epochs[e].shapes;
			work->logged = &rv.failuresLogged;
			work->outstanding = &recordingsOutstanding;
			work->name = R::kNames[e];
			const auto epoch = RenderGraphRuntime::EpochOf(R::kSegments[e]);
			bool refused = false;
			for (std::size_t i = 0; i < requests.size() && !refused; ++i) {
				recordingsOutstanding.fetch_add(1, std::memory_order_acq_rel);
				refused = !host->RequestEpochRecording(epoch, requests[i], [work, i](std::shared_ptr<const org::PersistentGraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					// Dropped (a build stopped ORG's host thread before it was recorded) or recorded on another build than the one the
					// revision's versions are of: no recording of this revision's, and nothing failed. The fragment fails, so no revision
					// waits on it; the next make asks again once, for the new build's graph (its versions change with the build).
					bool dropped = false;
					if (a_error) {
						try {
							std::rethrow_exception(a_error);
						} catch (const org::PersistentGraphHost::EpochRecordingDropped&) {
							dropped = true;
						} catch (...) {
						}
					} else if (a_recording && a_recording->buildGeneration != work->buildGeneration) {
						dropped = true;
					}
					if (dropped || a_error || !a_recording) {
						if (!work->failed.exchange(true, std::memory_order_acq_rel)) {
							if (dropped) {
								++*work->droppedCount;
							} else {
								++*work->failedCount;
								if ((*work->logged).fetch_add(1, std::memory_order_relaxed) < 8) {
									std::string what = "no recording";
									try {
										if (a_error)
											std::rethrow_exception(a_error);
									} catch (const std::exception& error) {
										what = error.what();
									} catch (...) {
										what = "unknown";
									}
									logger::warn("[DCLF] scene revision: the {} epoch's recording failed: {}", work->name, what);
								}
							}
							(void)work->fragment->Fail(a_error ? a_error : std::make_exception_ptr(std::runtime_error(dropped ? "recorded on another graph build" : "no recording")));
						}
					} else {
						work->recordings[i] = std::move(a_recording);
					}
					if (work->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 && !work->failed.load(std::memory_order_acquire)) {
						++*work->recorded;
						*work->shapes += work->recordings.size();
						auto recordings = std::make_shared<RevisionRecordings>();
						recordings->recordings = std::move(work->recordings);
						(void)work->fragment->Resolve(std::shared_ptr<const RevisionRecordings>(std::move(recordings)));
					}
					work->outstanding->fetch_sub(1, std::memory_order_acq_rel);
				});
				if (refused)
					recordingsOutstanding.fetch_sub(1, std::memory_order_acq_rel);
			}
			if (refused) {
				// Async epochs stopped under it: what was asked settles the fragment, which this revision does not name.
				++rv.epochs[e].refused;
				draft.Set(slot, nullptr);
				continue;
			}
			++rv.epochs[e].requested;
			draft.Set(slot, work->fragment);
		}

		// The ready changes are this revision's (its versions and shapes were made from them), handed over in it by value: the frame adopts
		// them with its selection (Growths::AdoptNamed).
		draft.Set(R::kGrowthsSlot, RevisionFragment::MakeReady(Growths::Get().Naming()));
		try {
			(void)rv.assembler.Seal(std::move(draft));
			++rv.sealed;
		} catch (const std::exception& error) {
			if (rv.sealFailures++ < 4)
				logger::error("[DCLF] scene revision of frame {} not sealed: {}", a_frame, error.what());
		}
	}

	bool IndirectDraws::Impl::ActiveRevision(std::uint32_t a_epoch, std::shared_ptr<const RevisionFragment>& a_shape, std::shared_ptr<const RevisionRecordings>& a_recordings)
	{
		using R = SceneRevisions;
		a_shape = nullptr;
		a_recordings = nullptr;
		const auto& active = revisions.active;
		if (!active || !RenderGraphRuntime::Get().Host()) {
			NoteRevisionMiss(a_epoch, R::kNoRevision);
			return false;
		}
		// Its versions current: every buffer its recordings read is the one the commit writes.
		const auto& versionsFragment = active->Fragment(R::kVersionsSlot);
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		if (!versions || versions->changes != VersionRegistry::Get().changes) {
			NoteRevisionMiss(a_epoch, R::kVersions);
			return false;
		}
		a_shape = active->Fragment(R::kShapeSlot + a_epoch);
		a_recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + a_epoch);
		if (!a_shape || !a_recordings) {
			NoteRevisionMiss(a_epoch, R::kNoRecording);
			return false;
		}
		return true;
	}

	Draws::EpochRevision IndirectDraws::Impl::RevisionOf(std::uint32_t a_epoch)
	{
		using R = SceneRevisions;
		EpochRevision out;
		if (const auto& active = revisions.active) {
			out.shape = active->Fragment(R::kShapeSlot + a_epoch);
			out.recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + a_epoch);
		}
		if (!out) {
			if (revisions.unrevised[a_epoch]++ == 0)
				logger::warn("[DCLF] the {} epoch of frame {} has no shape or recording in the selected revision: not submitted <- UNREVISED", R::kNames[a_epoch],
					SceneStore::Get().GetFrame());
			return {};
		}
		return out;
	}

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
				if (const auto segment = Impl::SceneRevisions::kSegments[e]; !(submitted & (1u << static_cast<std::uint32_t>(segment))))
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
		next.reflection = reflection.resources;
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
		next.gbufferProbe = SwitchValue(Switch::GBufferProbe);
		next.claims = IndirectDraws::Get().RevisionClaims();
		next.shadowCandidates = ShadowViews::Get().Candidates();
		next.occlusionLayouts = PredictedOcclusion();
		next.shadowPlacements = shadowPlacements;
		next.mainKnown = shapeParity.known;
		next.viewport = shapeParity.viewport;
		next.blockSizes = shapeParity.blockSizes;
		next.shadowKnown = shadowParity.known;
		next.reflectionKnown = reflectionParity.known;
		next.reflectionTargets = reflection.targets;
		next.reflectionWidth = reflection.resources ? reflection.resources->width : 0u;
		next.reflectionHeight = reflection.resources ? reflection.resources->height : 0u;
		next.portalWords = static_cast<std::uint32_t>(PortalViews::Get().Encoded().size());
		auto& mirror = SceneStore::Get().TreeLodMirror();
		next.treeLodShapeSlots = mirror.ShapeSlots();
		next.treeLodMeshSlots = mirror.MeshSlots();
		next.shadowRowsWanted = shadowRowsWanted;
		if (shadow && (!postedShadowSlots || postedShadowSlots->sequences.size() != shadow->sequences.size()))
			postedShadowSlots = std::make_shared<const ShadowSlotBuffers>(ShadowSlotBuffers{ shadow->sequences, shadow->bucketCounts });
		next.shadowSlots = shadow ? postedShadowSlots : nullptr;
		const auto& previous = postedInputs;
		next.mainGeneration = previous.mainGeneration + (next.main != previous.main ? 1u : 0u);
		const bool same = next.main == previous.main && next.shadow == previous.shadow && next.reflection == previous.reflection && next.scene == previous.scene &&
		                  next.buildGeneration == previous.buildGeneration && SameHandle(next.resourceHeap, previous.resourceHeap) &&
		                  SameHandle(next.samplerHeap, previous.samplerHeap) && next.targetsGeneration == previous.targetsGeneration &&
		                  next.shadowFormat == previous.shadowFormat && next.shadowRasterStates == previous.shadowRasterStates && next.toggles == previous.toggles &&
		                  next.togglesGeneration == previous.togglesGeneration && next.gbufferProbe == previous.gbufferProbe && next.claims == previous.claims &&
		                  next.shadowCandidates == previous.shadowCandidates && next.occlusionLayouts == previous.occlusionLayouts &&
		                  next.shadowPlacements == previous.shadowPlacements && next.mainKnown == previous.mainKnown && next.viewport == previous.viewport &&
		                  next.blockSizes == previous.blockSizes && next.shadowKnown == previous.shadowKnown && next.reflectionKnown == previous.reflectionKnown &&
		                  next.reflectionTargets == previous.reflectionTargets && next.reflectionWidth == previous.reflectionWidth &&
		                  next.reflectionHeight == previous.reflectionHeight && next.portalWords == previous.portalWords &&
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
		const auto& reflection = impl->reflection;
		if (failed) {
			rv.covered.fill(true);
			impl->reflectionCovered = true;
			PassCapture::Get().SetReflectionCovered(true);
			return true;
		}
		// Each epoch's recordings in the adopted snapshot's revision, of the graph as built now (the build point just ran), and its versions
		// the current ones - when the snapshot is current (AdoptSnapshot found none stale) and its draws are for the main resources.
		rv.covered.fill(false);
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& active = rv.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		const bool snapshot = impl->adoptedSnapshot && impl->adoptedSnapshot->drawsComplete && !impl->snapshotStale;
		if (host && snapshot && versions && versions->changes == VersionRegistry::Get().changes)
			for (std::uint32_t e = 0; e < R::kEpochs; ++e) {
				// An epoch's recordings are all of the build its versions were made at (AssembleRevision asks for them again after one).
				const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + e);
				rv.covered[e] = recordings && !recordings->recordings.empty() && recordings->recordings.front() &&
				                host->EpochRecordingCurrent(*recordings->recordings.front());
			}
		for (std::uint32_t e = 0; e < R::kEpochs; ++e)
			rv.uncovered[e] += rv.covered[e] ? 0 : 1;
		// The main epochs decide the frame's claims: without their recordings the frame is the engine's (SceneStore::WithdrawSet).
		const bool main = rv.covered[kDepthShape] && rv.covered[kColourShape];
		rv.withdrawn += main ? 0 : 1;
		// The faces' withholding (PassCapture), before the engine renders them: the claims stand and the cube the epoch draws into is
		// in the graph as built (its import, at the epoch, is built at the next build point).
		const auto& resources = impl->resources;
		const std::uint32_t frame = SceneStore::Get().GetFrame();
		// The faces draw from the frame's scene list and the frame before's colour frame record (ExecuteReflection): not
		// after a frame whose main epochs were not submitted (no claims), nor once a growth of the rows was adopted since (this frame's
		// selection), which leaves those addresses on a version nothing holds.
		const bool inputs = resources && resources->committed[kDepthShape].frame == resources->committed[kColourShape].frame &&
		                    frame - resources->committed[kDepthShape].frame <= 1 && resources->committed[kDepthShape].rowsGeneration == resources->MainRowsGeneration() &&
		                    resources->committed[kColourShape].rowsGeneration == resources->MainRowsGeneration() &&
		                    impl->scene && resources->committed[kDepthShape].sceneGeneration == impl->scene->generation &&
		                    resources->committed[kDepthShape].objectCapacity == resources->objectCapacity;
		impl->reflectionCovered = main && inputs && rv.covered[4] && reflection.resources && reflection.resources->cube && !host->RebuildRequested();
		PassCapture::Get().SetReflectionCovered(impl->reflectionCovered);
		return main;
	}

	bool IndirectDraws::Impl::EpochCovered(std::uint32_t a_epoch) const
	{
		return a_epoch < revisions.covered.size() && revisions.covered[a_epoch];
	}

	bool IndirectDraws::RevisionClaims() const
	{
		return !failed;
	}

	void IndirectDraws::Impl::SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index)
	{
		if (auto* host = RenderGraphRuntime::Get().Host(); host && a_index < a_recordings.recordings.size()) {
			host->UseEpochRecording(a_recordings.recordings[a_index]);
			++revisions.coverage[a_epoch].covered;
		}
	}

	namespace
	{
		// The snapshot builder's pump, once configured (Impl::SnapshotPump): what WakeSnapshotBuilder notifies.
		std::atomic<org::async::SerializedTaskPump*> builderPump{ nullptr };

		/** @brief avg / p95 / max of a_values (sorted here), "-" without any. */
		std::string Spread(std::vector<double>& a_values)
		{
			if (a_values.empty())
				return "-";
			std::sort(a_values.begin(), a_values.end());
			double sum = 0.0;
			for (const double value : a_values)
				sum += value;
			const std::size_t p95 = (std::min)(a_values.size() - 1, a_values.size() * 95 / 100);
			return fmt::format("{:.2f}/{:.2f}/{:.2f}", sum / double(a_values.size()), a_values[p95], a_values.back());
		}
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
			auto* created = new org::async::SerializedTaskPump;
			created->Configure(
				[](org::async::SerializedTaskPump::Task a_task) {
					return SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
						"DCLF snapshot builder", [task = std::move(a_task)](const org::async::TaskContext&) { task(); });
				},
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
		retiredSnapshots.Drain([](std::shared_ptr<const SceneSnapshot>&& a_snapshot) { const auto dropped = std::move(a_snapshot); });
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
				// The newest work, replacing the current one (latest wins: the publications it skipped are coalesced; their log deltas
				// reach the frame with it).
				if (auto next = snapshotWorkSlot.Take()) {
					const std::uint64_t sequence = next->publication ? next->publication->sequence : 0;
					if (b.lastPublication && sequence > b.lastPublication + 1)
						snapshotsCoalesced.fetch_add(sequence - b.lastPublication - 1, std::memory_order_relaxed);
					b.lastPublication = sequence;
					b.work = std::move(next);
					b.draws.reset();
					b.timing = {};
					b.stage = Stage::Ahead;
				}
				if (!b.work || !b.work->publication)
					return;
				if (b.stage == Stage::Ahead) {
					BuildSnapshotDraws();
					b.stage = Stage::Make;
				} else if (b.stage == Stage::Idle) {
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
				MakeRevision(publication.revision, publication.commitFrame);
				b.timing.shapesMs += msSince(start);
				if (rv.assembler.LatestSequence() != before) {
					b.sealed = rv.assembler.LatestSequence();
					b.sealedAt = std::chrono::steady_clock::now();
					b.stage = Stage::Recordings;
					continue;
				}
				// None sealed. Without what a revision is made of (no main resources yet, no claims): the publication alone, so the frame has
				// its tables and catalog (its epochs then capture what the next revision needs). Else a growth or a build it asked for is
				// pending: made again when woken.
				if (!producer.inputs.claims || !producer.inputs.main) {
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
		// Its draws: both main payloads, for the main resources its revision is of (or the epochs build their own). The builder's verdict:
		// without, its frames have no claims (DecideCoverage), whatever the frame finds.
		bool complete = SwitchEnabled(Switch::BindlessParity);
		if (!complete) {
			complete = b.draws != nullptr && inputs.main != nullptr;
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				complete = complete && b.draws->payloads[j] && b.draws->payloads[j]->inputs.addresses.identity == inputs.main.get();
		}
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
		// Adopted whole: the growths its revision names become current, then its versions (the registry takes the set's value when they are
		// every buffer's current ones), its revision the frame's epochs', its draws theirs.
		std::shared_ptr<const Impl::SceneSnapshot> snapshot(std::move(taken));
		if (const auto& lease = snapshot->revision) {
			if (const auto named = lease->Get<Growths::NamedChanges>(Impl::SceneRevisions::kGrowthsSlot))
				(void)Growths::Get().AdoptNamed(*named);
			if (const auto versions = lease->Get<VersionSet>(Impl::SceneRevisions::kVersionsSlot); versions && versions->Current())
				VersionRegistry::Get().SetChanges(versions->changes);
		}
		s.revisions.active = snapshot->revision;
		s.installedDraws = snapshot->draws;
		s.snapshotStale = false;
		++s.snapshotsAdopted;
		if (const auto& publication = snapshot->publication) {
			const std::uint32_t frameNow = SceneStore::Get().GetFrame();
			s.adoptionLatency.push_back(frameNow >= publication->commitFrame ? frameNow - publication->commitFrame : 0u);
		}
		// The replaced one back to the builder last, once nothing of the frame's holds it alone: its release is the builder's.
		if (auto replaced = std::exchange(s.adoptedSnapshot, snapshot)) {
			s.retiredSnapshots.Push(std::move(replaced));
			WakeSnapshotBuilder();
		}
		return snapshot->publication;
	}

	std::string IndirectDraws::Impl::SnapshotReport()
	{
		// The builder's times since the last report (its queue, drained here alone).
		std::vector<double> ahead, shapes, recordings;
		snapshotTimings.Drain([&](SnapshotTiming&& a_timing) {
			if (a_timing.ahead)
				ahead.push_back(a_timing.aheadMs);
			shapes.push_back(a_timing.shapesMs);
			recordings.push_back(a_timing.recordingsMs);
		});
		const std::size_t aheadBuilt = ahead.size();
		std::sort(adoptionLatency.begin(), adoptionLatency.end());
		const auto percentile = [this](std::size_t a_percent) {
			return adoptionLatency.empty() ? 0u : adoptionLatency[(std::min)(adoptionLatency.size() - 1, adoptionLatency.size() * a_percent / 100)];
		};
		std::string text = fmt::format("[DCLF] scene snapshots (T6b3b): {} built, {} publications coalesced by the builder, {} adopted, {} frames adopting nothing new; "
									   "passed over as stale: build {}, targets {}, shadow format {}, toggles {}, main resources {}; built without draws for the main "
									   "resources {}\n",
			snapshotsBuilt.exchange(0, std::memory_order_relaxed), snapshotsCoalesced.exchange(0, std::memory_order_relaxed), snapshotsAdopted, snapshotFramesKept,
			snapshotsStale[kStaleBuild], snapshotsStale[kStaleTargets], snapshotsStale[kStaleShadowFormat], snapshotsStale[kStaleToggles], snapshotsStale[kStaleMain],
			snapshotsWithoutDraws.exchange(0, std::memory_order_relaxed));
		text += fmt::format("[DCLF] snapshot builds (ms, avg/p95/max): draws ahead {} ({} built), shapes {}, recordings wait {}; commit to adoption (frames): p50 {}, p95 {}, "
							"max {}\n",
			Spread(ahead), aheadBuilt, Spread(shapes), Spread(recordings), percentile(50), percentile(95), adoptionLatency.empty() ? 0u : adoptionLatency.back());
		snapshotsAdopted = snapshotFramesKept = 0;
		snapshotsStale = {};
		adoptionLatency.clear();
		// The builder's own lines (the last it posted: a report behind), and the next asked for.
		producerReportSlot.TakeInto(producerReport);
		text += producerReport;
		producerReportWanted.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
		return text;
	}

	std::string IndirectDraws::Impl::ProducerReport()
	{
		// The snapshot builder's (T6b3b: composed in its pass, posted for the frame's report): nothing here is the frame's.
		auto& rv = revisions;
		std::string epochs;
		for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e) {
			auto& s = rv.epochs[e];
			const auto recorded = s.recorded.exchange(0), failedCount = s.failed.exchange(0), dropped = s.dropped.exchange(0), shapes = s.shapes.exchange(0);
			epochs += fmt::format("{}{} {} changed, {} requested, {} recorded ({} shapes), {} failed{}{}", epochs.empty() ? "" : "; ", SceneRevisions::kNames[e], s.changed,
				s.requested, recorded, shapes, failedCount, dropped ? fmt::format(", {} dropped by a graph build", dropped) : std::string(),
				s.refused ? fmt::format(", {} refused", s.refused) : std::string());
			s.changed = s.requested = s.refused = 0;
		}
		const auto& assembled = rv.assembler.GetStats();
		const auto growths = Growths::Get().Report();
		std::string keyMisses;
		for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e)
			if (const auto misses = std::exchange(shapeKeyMisses[e], 0))
				keyMisses += fmt::format("{}{} {}", keyMisses.empty() ? "" : ", ", SceneRevisions::kNames[e], misses);
		std::string madeBy;
		for (std::size_t g = 0; g < kKeyGroups; ++g)
			if (const auto made = std::exchange(shapesMadeBy[g], 0))
				madeBy += fmt::format("{}{} {}", madeBy.empty() ? " (moved: " : ", ", kKeyGroupNames[g], made);
		if (!madeBy.empty())
			madeBy += ")";
		std::string text = fmt::format("[DCLF] revision shapes: made at {} makes{}, kept at {}{}\n", std::exchange(shapesMade, 0), madeBy, std::exchange(shapesKept, 0),
			keyMisses.empty() ? std::string() : "; changed under an unchanged key: " + keyMisses + " <- SHAPE KEY");
		text += fmt::format("[DCLF] scene revisions (R3c, the snapshot builder's): {} sealed ({} version sets), {} pending; assembler {} published, {} failed, {} superseded, "
							"{} abandoned; {}{}\n",
			rv.sealed, rv.versionSets, rv.assembler.Pending(), assembled.published, assembled.failed, assembled.superseded, assembled.abandoned, epochs,
			rv.sealFailures ? fmt::format(" <- {} NOT SEALED", rv.sealFailures) : std::string());
		rv.sealed = rv.versionSets = 0;
		if (!growths.empty() || rv.growthWaits || rv.buildWaits)
			text += fmt::format("{}[DCLF] {} makes sealed no revision for a pending growth, {} for a graph build asked of the frame\n", growths,
				std::exchange(rv.growthWaits, 0), std::exchange(rv.buildWaits, 0));
		if (auto& bound = producer.drawBound; bound.updates) {
			text += fmt::format("[DCLF] scene draw bound: {} updates, {:.1f} slots changed an update, {} draws over {} slots, {} resyncs; parity {} checked, {} differ{}\n",
				bound.updates, static_cast<double>(bound.changes) / bound.updates, bound.draws, bound.produced.size(), bound.resyncs, bound.parity.checks,
				bound.parity.mismatches, bound.parity.Verdict(true));
			bound.updates = bound.changes = bound.resyncs = 0;
			bound.parity.Reset();
		}
		return text;
	}

	std::string IndirectDraws::Impl::RevisionReport()
	{
		auto& rv = revisions;
		std::string covered;
		for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e) {
			auto& c = rv.coverage[e];
			std::string misses;
			for (std::uint32_t m = 0; m < SceneRevisions::kMisses; ++m)
				if (c.missed[m])
					misses += fmt::format("{}{} {}", misses.empty() ? "" : ", ", SceneRevisions::kMissNames[m], c.missed[m]);
			const auto unrevised = std::exchange(rv.unrevised[e], 0), clamped = std::exchange(rv.latchClamped[e], 0), checks = std::exchange(rv.parityChecks[e], 0);
			covered += fmt::format("{}{} {}{}{}{}", covered.empty() ? "" : "; ", SceneRevisions::kNames[e], c.covered,
				unrevised ? fmt::format(", {} without the revision's shape <- UNREVISED", unrevised) : std::string(),
				clamped ? fmt::format(", {} with values past its latch <- LATCH", clamped) : std::string(),
				checks ? fmt::format(", {} checked{}", checks, misses.empty() ? std::string(" <- OK") : " (" + misses + ") <- REVISION") : std::string());
			c = {};
		}
		std::string text;
		if (!covered.empty())
			text += fmt::format("[DCLF] epochs submitted by the revision: {}; {} values staged that a revision's latched copies lacked\n", covered,
				std::exchange(rv.latchedMisses, 0));
		{
			std::string uncovered;
			for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e)
				uncovered += fmt::format("{}{} {}", e ? ", " : "", SceneRevisions::kNames[e], std::exchange(rv.uncovered[e], 0));
			const std::uint64_t unbuiltTotal = unbuilt[kAsyncZPrepass] + unbuilt[kAsyncColour] + shadowUnbuilt;
			text += fmt::format("[DCLF] strict epochs: {} frames without the adopted snapshot's main recordings (claims withdrawn: the engine's), {} graph builds at the "
								"build point, {} tickets of epochs not submitted given back for recordings; frames whose revision lacked an epoch's current recordings: {}; "
								"shadow views left to the engine: {} frames without the revision's shadow recording or the installed shadow payload; occlusion maps "
								"left to the engine for want of the revision's shape: {}; covered epochs without the installed publication's payload: Z-prepass {}, "
								"colour {}, shadow {}{}\n",
				std::exchange(rv.withdrawn, 0), std::exchange(rv.builds, 0), std::exchange(rv.ticketsReleased, 0), uncovered, std::exchange(shadowUnrecorded, 0),
				std::exchange(occlusionUnrecorded, 0), unbuilt[kAsyncZPrepass], unbuilt[kAsyncColour], shadowUnbuilt, unbuiltTotal ? " <- UNBUILT" : "");
			unbuilt = {};
			shadowUnbuilt = 0;
		}
		return text;
	}
}

#endif
