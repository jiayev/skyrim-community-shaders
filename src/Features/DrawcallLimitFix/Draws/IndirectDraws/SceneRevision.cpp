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
		auto& buffers = VersionRegistry::Get().buffers;
		std::erase_if(buffers, [](const auto& a_buffer) { return a_buffer.expired(); });
		set->versions.reserve(buffers.size());
		set->changes = VersionRegistry::Get().changes;
		for (const auto& weak : buffers)
			if (const auto buffer = weak.lock())
				set->versions.emplace_back(buffer->Key(), buffer->Current());
		std::sort(set->versions.begin(), set->versions.end(), [](const auto& a, const auto& b) { return std::less<const void*>{}(a.first, b.first); });
		return set;
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
		auto draft = rv.assembler.Begin();
		// The versions: again when a buffer was made or a growth adopted.
		if (auto& registry = VersionRegistry::Get(); rv.versionChanges != registry.changes || !draft.Get(R::kVersionsSlot)) {
			draft.Set(R::kVersionsSlot, RevisionFragment::MakeReady(VersionSet::Snapshot()));
			rv.versionChanges = registry.changes;
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
			rv.madeAt[sequence % rv.madeAt.size()] = { sequence, a_frame };
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

	bool IndirectDraws::Impl::RevisionHoldsClaims() const
	{
		// The frame's claims (ApplySet) are the set the frame before's scene work committed, at its join, before its revision was
		// made: a revision made then or later has every pipeline they draw with.
		const auto frame = SceneStore::Get().GetFrame();
		return revisions.activeFrame != ~0u && revisions.activeFrame + 1 >= frame;
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
		std::string text = fmt::format("[DCLF] scene revisions (R3c): {} sealed ({} version sets), {} published, {} selected (made {:.2f} frames before on average), {} pending; "
									   "assembler {} failed, {} superseded, {} abandoned; {}{}\n",
			rv.sealed, rv.versionSets, rv.published, rv.selections, rv.selections ? double(rv.selectedAge) / rv.selections : 0.0, rv.assembler.Pending(), assembled.failed,
			assembled.superseded, assembled.abandoned, epochs, rv.sealFailures ? fmt::format(" <- {} NOT SEALED", rv.sealFailures) : std::string());
		rv.sealed = rv.versionSets = rv.published = rv.selections = rv.selectedAge = 0;
		if (!covered.empty())
			text += fmt::format("[DCLF] epochs submitted (R3c): {}; {} values staged that a revision's latched copies lacked\n", covered, std::exchange(rv.latchedMisses, 0));
		return text;
	}
}

#endif
