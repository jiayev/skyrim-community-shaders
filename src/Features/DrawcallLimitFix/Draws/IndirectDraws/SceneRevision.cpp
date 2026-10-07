#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	using org::async::RevisionFragment;

	namespace
	{
		/** @brief CS_DCLF_REVISIONS: on unless 0 or off. */
		bool RevisionsEnabled()
		{
			const auto& value = SwitchValue(Switch::Revisions);
			return value != "0" && value != "off";
		}
	}

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
		auto& buffers = registry.buffers;
		std::erase_if(buffers, [](const auto& a_buffer) { return a_buffer.expired(); });
		set->versions.reserve(buffers.size());
		set->changes = registry.changes;
		const auto& growths = Growths::Get();
		bool pending = false;
		for (const auto& weak : buffers)
			if (const auto buffer = weak.lock()) {
				auto version = growths.Ready(buffer);
				pending = pending || version;
				set->versions.emplace_back(buffer->Key(), version ? std::move(version) : buffer->Current());
			}
		if (pending)
			set->changes = ++registry.next;
		std::sort(set->versions.begin(), set->versions.end(), [](const auto& a, const auto& b) { return std::less<const void*>{}(a.first, b.first); });
		return set;
	}

	bool Draws::VersionSet::Current() const
	{
		// A buffer made after the set (not in it) is one no recording of its revision names: it moves nothing the set binds.
		for (const auto& weak : VersionRegistry::Get().buffers)
			if (const auto buffer = weak.lock())
				if (const auto version = Find(*buffer); version && version != buffer->Current())
					return false;
		return true;
	}

	bool Draws::Growths::Deferred()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return RevisionsEnabled() && host && host->AsyncEpochs() && host->Uploads();
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
			// The epoch's counters (SceneRevisions::Epoch) and the failures logged.
			std::atomic<std::uint64_t>*recorded = nullptr, *failedCount = nullptr, *shapes = nullptr;
			std::atomic<std::uint32_t>* logged = nullptr;
			const char* name = "";
		};
	}

	void IndirectDraws::Impl::AssembleRevision(std::uint32_t a_frame)
	{
		ZoneScopedN("CS.DCLF.AssembleRevision");
		using R = SceneRevisions;
		auto& rv = revisions;
		// A growth still pending (G2): the commit's claims may reach past the versions a revision made now could name, so none is
		// made; the commit waits for the next join's (SetApplicable: a revision is owed for this frame).
		if (rv.growthPending) {
			rv.sealedFrame = a_frame;
			++rv.growthWaits;
			return;
		}
		auto draft = rv.assembler.Begin();
		// The versions: again when a growth was adopted or became ready to name, or the graph was built again (with the buffers made
		// since: a recording is of the build it was made on, so every epoch is recorded again for the new set).
		const std::uint64_t builds = RenderGraphRuntime::Get().Host() ? RenderGraphRuntime::Get().Host()->BuildGeneration() : 0;
		if (auto& registry = VersionRegistry::Get(); rv.versionChanges != registry.changes || rv.growthStamp != Growths::Get().stamp || rv.graphBuilds != builds ||
			!draft.Get(R::kVersionsSlot)) {
			draft.Set(R::kVersionsSlot, RevisionFragment::MakeReady(VersionSet::Snapshot()));
			rv.versionChanges = registry.changes;
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
		};
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			const auto& made = shapeParity.revisions[shape][0];
			if (!made)
				continue;
			const auto& current = draft.Get(R::kShapeSlot + static_cast<std::uint32_t>(shape));
			bool changed = false;
			auto kept = Kept<PassFrame>(current ? current->Value<PassFrame>() : nullptr, made, changed);
			if (changed)
				setShape(static_cast<std::uint32_t>(shape), std::move(kept));
		}
		for (std::uint32_t kind = 0; kind < 2; ++kind) {
			const auto& made = shadowParity.revisions[kind][0];
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
		if (const auto& made = reflectionParity.revisions[0]) {
			const auto& current = draft.Get(R::kShapeSlot + 4);
			bool changed = false;
			auto kept = Kept<ReflectionFrame>(current ? current->Value<ReflectionFrame>() : nullptr, made, changed);
			if (changed)
				setShape(4, std::move(kept));
		}

		// Each epoch's recording: kept while it requires exactly the draft's shape and versions; else requested for them. A failed
		// one is dropped (the epoch has none in the revision) and not asked for again until its shape or the versions change.
		auto* host = RenderGraphRuntime::Get().Host();
		const bool recordable = host && host->AsyncEpochs();
		// The scene's sizing its versions have (the growths it names): what its passes size by.
		const auto sceneSizing = scene ? std::make_shared<const SceneSizing>(Growths::Get().RevisionSizing<SceneSizing>(*scene)) : nullptr;
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
			work->recorded = &rv.epochs[e].recorded;
			work->failedCount = &rv.epochs[e].failed;
			work->shapes = &rv.epochs[e].shapes;
			work->logged = &rv.failuresLogged;
			work->name = R::kNames[e];
			const auto epoch = RenderGraphRuntime::EpochOf(R::kSegments[e]);
			bool refused = false;
			for (std::size_t i = 0; i < requests.size() && !refused; ++i) {
				refused = !host->RequestEpochRecording(epoch, requests[i], [work, i](std::shared_ptr<const org::PersistentGraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					if (a_error || !a_recording) {
						if (!work->failed.exchange(true, std::memory_order_acq_rel)) {
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
							(void)work->fragment->Fail(a_error ? a_error : std::make_exception_ptr(std::runtime_error("no recording")));
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
				});
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

		try {
			const auto sequence = rv.assembler.Seal(std::move(draft));
			// The ready changes are this revision's (its versions and shapes were made from them): adopted with its selection.
			Growths::Get().Sealed(sequence);
			rv.madeAt[sequence % rv.madeAt.size()] = { sequence, a_frame };
			rv.sealedFrame = a_frame;
			++rv.sealed;
		} catch (const std::exception& error) {
			if (rv.sealFailures++ < 4)
				logger::error("[DCLF] scene revision of frame {} not sealed: {}", a_frame, error.what());
		}
	}

	void IndirectDraws::SelectRevision()
	{
		if (failed)
			return;
		auto& rv = impl->revisions;
		const auto collected = rv.assembler.Collect();
		if (collected.published)
			++rv.published;
		const bool selected = rv.assembler.TrySelect() == org::async::PublicationExchange<org::async::AssembledRevision>::Selection::Selected;
		rv.active = rv.assembler.AcquireActive();
		if (!selected)
			return;
		const auto* active = rv.assembler.Active();
		if (!active)
			return;
		++rv.selections;
		rv.selected = active->Sequence();
		// The growths it names become current (G2), before anything of the frame writes or builds: its versions are then the
		// current ones (the registry takes the set's value).
		if (const auto versions = active->Get<VersionSet>(Impl::SceneRevisions::kVersionsSlot); Growths::Get().AdoptSelected(rv.selected) && versions && versions->Current())
			VersionRegistry::Get().changes = versions->changes;
		rv.activeFrame = ~0u;
		if (const auto& [sequence, frame] = rv.madeAt[rv.selected % rv.madeAt.size()]; sequence == rv.selected) {
			rv.selectedAge += SceneStore::Get().GetFrame() - frame;
			rv.activeFrame = frame;
		}
	}

	bool IndirectDraws::Impl::ActiveRevision(std::uint32_t a_epoch, std::shared_ptr<const RevisionFragment>& a_shape, std::shared_ptr<const RevisionRecordings>& a_recordings)
	{
		using R = SceneRevisions;
		a_shape = nullptr;
		a_recordings = nullptr;
		if (!RevisionsEnabled())
			return false;
		// A frame no revision covers draws nothing of DCLF's (SceneStore::WithdrawSet): its commits prepare an empty epoch.
		if (SceneStore::Get().SetWithdrawn()) {
			NoteRevisionMiss(a_epoch, R::kWithdrawn);
			return false;
		}
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

	bool IndirectDraws::SetApplicable(std::uint32_t a_commitFrame) const
	{
		if (failed || !RevisionsEnabled())
			return true;
		auto& rv = impl->revisions;
		// No revision was made for the commit (no resources yet, a load screen): the claims are not a revision's.
		if (rv.sealedFrame == ~0u || rv.sealedFrame < a_commitFrame)
			return true;
		const bool applicable = rv.activeFrame != ~0u && rv.activeFrame >= a_commitFrame;
		if (!applicable)
			++rv.setsHeld;
		return applicable;
	}

	void IndirectDraws::BuildPoint()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& rv = impl->revisions;
		// Explicit while scene revisions draw: an extension added or removed during a frame waits for this point, so the graph a
		// frame's revision was recorded on runs through that frame (a build invalidates every recording: the frame after one is the
		// engine's, DecideCoverage). Without them (or failed), the graph builds at its next submission, as before.
		const bool explicitBuilds = !failed && RevisionsEnabled();
		if (rv.explicitBuilds != explicitBuilds) {
			rv.explicitBuilds = explicitBuilds;
			host->SetExplicitBuilds(explicitBuilds);
		}
		if (explicitBuilds && host->BuildIfRequested())
			++rv.builds;
		// A live epoch's recording waits for the slot its ready, unsubmitted ticket holds: an epoch the last frame did not submit (no
		// water in view, a frame without claims) gives that ticket back while a revision waits for recordings, so none waits on it.
		const std::uint32_t submitted = RenderGraphRuntime::Get().TakeSubmittedSegments();
		if (explicitBuilds && rv.assembler.Pending())
			for (std::uint32_t e = 0; e < Impl::SceneRevisions::kEpochs; ++e)
				if (const auto segment = Impl::SceneRevisions::kSegments[e]; !(submitted & (1u << static_cast<std::uint32_t>(segment))))
					rv.ticketsReleased += host->ReleaseEpochTicket(RenderGraphRuntime::EpochOf(segment)) ? 1 : 0;
	}

	bool IndirectDraws::DecideCoverage()
	{
		using R = Impl::SceneRevisions;
		auto& rv = impl->revisions;
		const auto& reflection = impl->reflection;
		if (failed || !RevisionsEnabled()) {
			rv.covered.fill(true);
			impl->reflectionCovered = true;
			PassCapture::Get().SetReflectionCovered(true);
			return true;
		}
		// Each epoch's recordings in the selected revision, of the graph as built now (the build point just ran), and its versions the
		// current ones.
		rv.covered.fill(false);
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& active = rv.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		if (host && versions && versions->changes == VersionRegistry::Get().changes)
			for (std::uint32_t e = 0; e < R::kEpochs; ++e) {
				const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + e);
				bool current = recordings && !recordings->recordings.empty();
				for (std::size_t i = 0; current && i < recordings->recordings.size(); ++i)
					current = recordings->recordings[i] && host->EpochRecordingCurrent(*recordings->recordings[i]);
				rv.covered[e] = current;
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
		// The faces draw from the frame before's main commits (ExecuteReflection), whose inputs name the main rows by address: not
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
		return !RevisionsEnabled() || (a_epoch < revisions.covered.size() && revisions.covered[a_epoch]);
	}

	bool IndirectDraws::RevisionClaims() const
	{
		return !failed && RevisionsEnabled();
	}

	void IndirectDraws::NoteSetApplied(std::uint32_t a_commitFrame)
	{
		impl->revisions.claimsFrame = a_commitFrame;
	}

	bool IndirectDraws::Impl::RevisionHoldsClaims() const
	{
		// The frame's claims (ApplySet) are the set a commit decided before its join made a revision (SetApplicable): a revision
		// made then or later has every pipeline they draw with (the sets only append).
		return revisions.activeFrame != ~0u && revisions.claimsFrame != ~0u && revisions.activeFrame >= revisions.claimsFrame;
	}

	bool IndirectDraws::Impl::RecordingAdmitted(const RevisionRecordings& a_recordings, std::size_t a_index) const
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || a_index >= a_recordings.recordings.size() || !a_recordings.recordings[a_index])
			return false;
		std::string why;
		if (host->CanUseEpochRecording(*a_recordings.recordings[a_index], &why))
			return true;
		static std::uint32_t logged = 0;
		if (logged++ < 8)
			logger::info("[DCLF] a revision's recording is not its epoch's ticket's: {}", why);
		return false;
	}

	void IndirectDraws::Impl::SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index)
	{
		if (auto* host = RenderGraphRuntime::Get().Host(); host && a_index < a_recordings.recordings.size()) {
			host->UseEpochRecording(a_recordings.recordings[a_index]);
			++revisions.coverage[a_epoch].covered;
		}
	}

	void IndirectDraws::Impl::ChooseRevisionRecording(std::uint32_t a_epoch, const std::function<std::size_t(const RevisionFragment&)>& a_match)
	{
		using R = SceneRevisions;
		if (!RevisionsEnabled())
			return;
		auto& coverage = revisions.coverage[a_epoch];
		const auto& active = revisions.active;
		auto* host = RenderGraphRuntime::Get().Host();
		if (SceneStore::Get().SetWithdrawn()) {
			++coverage.missed[R::kWithdrawn];
			return;
		}
		if (!active || !host) {
			++coverage.missed[R::kNoRevision];
			return;
		}
		// Its versions current: every buffer its recordings read is the one this commit wrote.
		const auto& versionsFragment = active->Fragment(R::kVersionsSlot);
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		if (!versions || versions->changes != VersionRegistry::Get().changes) {
			++coverage.missed[R::kVersions];
			return;
		}
		const auto& shapeFragment = active->Fragment(R::kShapeSlot + a_epoch);
		const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + a_epoch);
		if (!shapeFragment || !recordings) {
			++coverage.missed[R::kNoRecording];
			return;
		}
		const std::size_t index = a_match(*shapeFragment);
		if (index >= recordings->recordings.size()) {
			++coverage.missed[R::kShape];
			return;
		}
		host->UseEpochRecording(recordings->recordings[index]);
		++coverage.covered;
	}

	std::string IndirectDraws::Impl::RevisionReport()
	{
		auto& rv = revisions;
		std::string epochs;
		for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e) {
			auto& s = rv.epochs[e];
			const auto recorded = s.recorded.exchange(0), failedCount = s.failed.exchange(0), shapes = s.shapes.exchange(0);
			epochs += fmt::format("{}{} {} changed, {} requested, {} recorded ({} shapes), {} failed{}", epochs.empty() ? "" : "; ", SceneRevisions::kNames[e], s.changed,
				s.requested, recorded, shapes, failedCount, s.refused ? fmt::format(", {} refused", s.refused) : std::string());
			s.changed = s.requested = s.refused = 0;
		}
		std::string covered;
		if (RevisionsEnabled())
			for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e) {
				auto& c = rv.coverage[e];
				std::string misses;
				for (std::uint32_t m = 0; m < SceneRevisions::kMisses; ++m)
					if (c.missed[m])
						misses += fmt::format("{}{} {}", misses.empty() ? "" : ", ", SceneRevisions::kMissNames[m], c.missed[m]);
				covered += fmt::format("{}{} {} by the revision{}", covered.empty() ? "" : "; ", SceneRevisions::kNames[e], c.covered,
					misses.empty() ? std::string() : " (own: " + misses + ")");
				c = {};
			}
		const auto& assembled = rv.assembler.GetStats();
		const auto growths = Growths::Get().Report();
		std::string text = fmt::format("[DCLF] scene revisions (R3c): {} sealed ({} version sets), {} published, {} selected (made {:.2f} frames before on average), {} pending; "
									   "assembler {} failed, {} superseded, {} abandoned; {}{}\n",
			rv.sealed, rv.versionSets, rv.published, rv.selections, rv.selections ? double(rv.selectedAge) / rv.selections : 0.0, rv.assembler.Pending(), assembled.failed,
			assembled.superseded, assembled.abandoned, epochs, rv.sealFailures ? fmt::format(" <- {} NOT SEALED", rv.sealFailures) : std::string());
		rv.sealed = rv.versionSets = rv.published = rv.selections = rv.selectedAge = 0;
		if (!growths.empty() || rv.growthWaits)
			text += fmt::format("{}[DCLF] {} joins sealed no revision for a pending growth\n", growths, std::exchange(rv.growthWaits, 0));
		if (!covered.empty())
			text += fmt::format("[DCLF] epochs submitted (R3c): {}; {} values staged that a revision's latched copies lacked; {} publications passed over at a frame's start for want of their commit's revision\n", covered,
				std::exchange(rv.latchedMisses, 0), std::exchange(rv.setsHeld, 0));
		if (RevisionsEnabled()) {
			std::string uncovered;
			for (std::uint32_t e = 0; e < SceneRevisions::kEpochs; ++e)
				uncovered += fmt::format("{}{} {}", e ? ", " : "", SceneRevisions::kNames[e], std::exchange(rv.uncovered[e], 0));
			text += fmt::format("[DCLF] strict epochs: {} frames without the selected revision's main recordings (claims withdrawn: the engine's), {} graph builds at the "
								"build point, {} tickets of epochs not submitted given back for recordings; frames whose revision lacked an epoch's current recordings: {}; "
								"shadow views left to the engine: {} frames with a view not seen yet, {} with a layout no revision had a shape for; {} covered frames "
								"whose views did not come as predicted{}; occlusion maps left to the engine for want of the revision's shape: {}\n",
				std::exchange(rv.withdrawn, 0), std::exchange(rv.builds, 0), std::exchange(rv.ticketsReleased, 0), uncovered, std::exchange(shadowUnobserved, 0),
				std::exchange(shadowUnrecorded, 0), shadowMispredicted, shadowMispredicted ? " <- MISPREDICTED" : "", std::exchange(occlusionUnrecorded, 0));
			shadowMispredicted = 0;
		}
		return text;
	}
}

#endif
