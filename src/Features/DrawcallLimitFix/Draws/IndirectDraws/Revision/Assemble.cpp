// The revision producer's assembly (snapshot builder): a scene revision's versions (VersionSet), its epochs' shapes and the
// recordings requested for them on ORG's host thread, sealed; and the render thread's use of the selected revision (RevisionOf).

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"

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
			// The epoch's counters (SceneRevisions::EpochStats), the failures logged, and the recordings outstanding (Impl::recordingsOutstanding).
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
		// A growth still pending: the publication's claims may reach past the versions a revision made now could name, so none is
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

		// Each epoch's shape (MakeRevision's): its fragment kept while the shape is the same.
		auto setShape = [&]<class Value>(std::uint32_t a_epoch, std::shared_ptr<const Value> a_value) {
			draft.Set(R::kShapeSlot + a_epoch, RevisionFragment::MakeReady(std::move(a_value)));
			++rv.epochs[a_epoch].changed;
		};
		// The producer's newest shapes (MakeRevision's), by the epochs' traits.
		const auto& shapes = producer.made;
		const auto& toggles = producer.inputs.toggles;
		for (std::uint32_t e = 0; e < R::kEpochs; ++e) {
			const auto& traits = kEpochTraits[e];
			const auto& current = draft.Get(R::kShapeSlot + e);
			bool changed = false;
			if (traits.sharesWith != Epoch::Count) {
				// The shared shape's own value (a recording kept for this epoch stays while the shape does); none while the forward
				// toggle is off.
				const auto& source = draft.Get(R::kShapeSlot + traits.sharesWith);
				const bool on = source && (e != Epoch::Forward || toggles.forward);
				auto share = [&]<class Frame>() {
					const auto shared = on ? source->Value<Frame>() : nullptr;
					if (shared == (current ? current->Value<Frame>() : nullptr))
						return;
					if (shared)
						setShape(e, shared);
					else
						draft.Set(R::kShapeSlot + e, nullptr);
				};
				if (traits.shape == ShapeKind::Main)
					share.template operator()<PassFrame>();
				else
					share.template operator()<ViewFrame>();
				continue;
			}
			if (traits.shape == ShapeKind::Main) {
				if (const auto& made = shapes.main[traits.index]) {
					auto kept = Kept<PassFrame>(current ? current->Value<PassFrame>() : nullptr, made, changed);
					if (changed)
						setShape(e, std::move(kept));
				}
			} else if (traits.shape == ShapeKind::Shadow) {
				const auto& made = shapes.shadow[traits.index];
				if (made.empty())
					continue;
				const auto old = current ? current->Value<ShadowVariants>() : nullptr;
				auto variants = std::make_shared<ShadowVariants>();
				changed = !old || old->shapes.size() != made.size();
				for (std::size_t v = 0; v < made.size(); ++v)
					variants->shapes.push_back(Kept<ShadowFrame>(old && v < old->shapes.size() ? old->shapes[v] : nullptr, made[v], changed));
				if (changed)
					setShape(e, std::shared_ptr<const ShadowVariants>(std::move(variants)));
			} else {
				// First person's shape while CS_DCLF_FIRST_PERSON is on (none while it is off); the reflection's last one stands without a
				// new one.
				const bool isFirstPerson = traits.index == ViewIndex(ViewKind::FirstPerson);
				const auto& made = isFirstPerson && !toggles.firstPerson ? nullptr : shapes.views[traits.index];
				const auto currentShape = current ? current->Value<ViewFrame>() : nullptr;
				if (made) {
					auto kept = Kept<ViewFrame>(currentShape, made, changed);
					if (changed)
						setShape(e, std::move(kept));
				} else if (currentShape && isFirstPerson) {
					draft.Set(R::kShapeSlot + e, nullptr);
				}
			}
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
			// The epoch's passes prepare against its shape (the forward epoch's, the colour shape: CurrentFrame's MainOpaque).
			const auto& traits = kEpochTraits[e];
			if (traits.shape == ShapeKind::Main)
				request([&](RevisionShapes& a_shapes) { a_shapes.main[traits.index] = shapeFragment->Value<PassFrame>(); });
			else if (traits.shape == ShapeKind::View)
				request([&](RevisionShapes& a_shapes) { a_shapes.views[traits.index] = shapeFragment->Value<ViewFrame>(); });
			else
				for (const auto& variant : shapeFragment->Value<ShadowVariants>()->shapes)
					request([&](RevisionShapes& a_shapes) { (traits.index ? a_shapes.occlusion : a_shapes.shadow) = variant; });
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
			work->name = kEpochTraits[e].name;
			const auto epoch = RenderGraphRuntime::EpochOf(kEpochTraits[e].segment);
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
			rv.sealedVersions = versions;
			++rv.sealed;
		} catch (const std::exception& error) {
			if (rv.sealFailures++ < 4)
				logger::error("[DCLF] scene revision of frame {} not sealed: {}", a_frame, error.what());
		}
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
				logger::warn("[DCLF] the {} epoch of frame {} has no shape or recording in the selected revision: not submitted <- UNREVISED", kEpochTraits[a_epoch].name,
					Scene::FrameView::Get().GetFrame());
			return {};
		}
		return out;
	}

	void IndirectDraws::Impl::SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index)
	{
		if (auto* host = RenderGraphRuntime::Get().Host(); host && a_index < a_recordings.recordings.size()) {
			host->UseEpochRecording(a_recordings.recordings[a_index]);
			++revisions.submitted[a_epoch];
		}
	}

}

#endif
