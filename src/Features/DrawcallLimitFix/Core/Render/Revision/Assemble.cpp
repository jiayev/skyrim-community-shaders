// The revision producer's assembly (snapshot builder): a scene revision's versions (the sealed set), its epochs' shapes and the
// recordings requested for them on ORG's host thread, sealed; and the render thread's use of the selected revision (RevisionOf).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"

namespace DCLF
{
	using org::async::RevisionFragment;

	namespace
	{
		// Render thread: the submissions of each epoch outside its trigger (SubmitRevisionRecording), the first logged.
		std::array<std::uint64_t, kMaxEpochs> outsideTrigger{};

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
			std::uint32_t hosted = 0;  // the hosted epochs recorded with their shapes, a bit each (RevisionRecordings::hosted)
			// The build the revision's versions are of (RevisionInputs::buildGeneration): a recording of another is none of its.
			std::uint64_t buildGeneration = 0;
			// The epoch's counters (SceneRevisions::EpochStats), the failures logged, and the recordings outstanding (Impl::recordingsOutstanding).
			std::atomic<std::uint64_t>*recorded = nullptr, *failedCount = nullptr, *droppedCount = nullptr, *shapes = nullptr;
			std::atomic<std::uint32_t>* logged = nullptr;
			std::atomic<std::uint32_t>* outstanding = nullptr;
			std::string_view name;
		};

		using Revisions = ImplProducer::SceneRevisions;
		using Draft = org::async::RevisionAssembler::Draft;

		/** @brief Each epoch's shape (MakeRevision's) in a_draft: its fragment kept while the shape is the same. */
		void AssembleShapes(Revisions& a_rv, Draft& a_draft, const MadeShapes& a_shapes, const ToggleSet& a_toggles)
		{
			using R = Revisions;
			auto& draft = a_draft;
			auto setShape = [&]<class Value>(std::uint32_t a_epoch, std::shared_ptr<const Value> a_value) {
				draft.Set(R::kShapeSlot + a_epoch, RevisionFragment::MakeReady(std::move(a_value)));
				++a_rv.epochs[a_epoch].changed;
			};
			// The producer's newest shapes (MakeRevision's), by the epochs' traits.
			const auto& shapes = a_shapes;
			const auto& toggles = a_toggles;
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				const auto& traits = EpochTraitsOf(e);
				const auto& current = draft.Get(R::kShapeSlot + e);
				bool changed = false;
				if (traits.sharesWith != kNoEpoch) {
					// The shared shape's own value (a recording kept for this epoch stays while the shape does); none while the epoch's
					// gate class is off.
					const auto& source = draft.Get(R::kShapeSlot + traits.sharesWith);
					const bool on = source && (traits.gate == Api::ObjectClass::Count || Extensions::ObjectClassOn(toggles, traits.gate));
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
					const auto& made = shapes.shadow;
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
					// A DropWhenOff view's shape while its toggle is on (none while it is off: first person's); a KeepLast view's last one
					// stands without a new one (the reflection's).
					const auto view = static_cast<ViewKind>(traits.index);
					const bool dropWhenOff = Extensions::Views().views[traits.index].life == Api::ViewShapeLife::DropWhenOff;
					const auto& made = dropWhenOff && !Extensions::ViewOn(toggles, view) ? nullptr : shapes.views[traits.index];
					const auto currentShape = current ? current->Value<ViewFrame>() : nullptr;
					if (made) {
						auto kept = Kept<ViewFrame>(currentShape, made, changed);
						if (changed)
							setShape(e, std::move(kept));
					} else if (currentShape && dropWhenOff) {
						draft.Set(R::kShapeSlot + e, nullptr);
					}
				}
			}
		}

		/** @brief A recording of a_work's request a_index completed (ORG's host thread): the fragment resolves once all are, or fails. */
		void RecordingCompleted(const std::shared_ptr<PendingRecordings>& a_work, std::size_t a_index, std::shared_ptr<const org::PersistentGraphHost::EpochRecording> a_recording,
			std::exception_ptr a_error)
		{
			const auto& work = a_work;
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
				work->recordings[a_index] = std::move(a_recording);
			}
			if (work->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 && !work->failed.load(std::memory_order_acquire)) {
				++*work->recorded;
				*work->shapes += work->recordings.size();
				auto recordings = std::make_shared<RevisionRecordings>();
				recordings->recordings = std::move(work->recordings);
				recordings->hosted = work->hosted;
				(void)work->fragment->Resolve(std::shared_ptr<const RevisionRecordings>(std::move(recordings)));
			}
			work->outstanding->fetch_sub(1, std::memory_order_acq_rel);
		}

		/**
		 * @brief Epoch a_epoch's recordings of its inputs a_inputs - the versions, its shape, then the shapes of the epochs it hosts in
		 * a_hosted (a bit each, by row: those that have one) - requested of ORG's host thread: the fragment they resolve, or null when
		 * async epochs stopped under it.
		 */
		std::shared_ptr<RevisionFragment> RequestRecordings(org::PersistentGraphHost& a_host, Revisions& a_rv, std::atomic<std::uint32_t>& a_outstanding, std::uint32_t a_epoch,
			const std::shared_ptr<const NamedVersions>& a_versions, std::span<const std::shared_ptr<const RevisionFragment>> a_inputs, std::uint32_t a_hosted,
			std::uint64_t a_builds)
		{
			using R = Revisions;
			const std::uint32_t e = a_epoch;
			const auto& shapeFragment = a_inputs[1];
			// The host data of each of the epoch's shapes (a shadow epoch's variants).
			std::vector<std::shared_ptr<const EpochRevisionData>> requests;
			auto request = [&](auto&& a_fill) {
				auto data = std::make_shared<EpochRevisionData>();
				data->versions = a_versions;
				a_fill(data->shapes);
				requests.push_back(std::move(data));
			};
			// The epoch's passes prepare against its shape (the forward epoch's, the colour shape: CurrentFrame's MainOpaque).
			const auto& traits = EpochTraitsOf(e);
			if (traits.shape == ShapeKind::Main) {
				// With the view epochs it hosts that have a shape (the depth epoch: the reflection's faces, first person's depth), each a
				// dimension (RevisionRecordings::hosted): for every subset of them absent, none absent first, its variants with the present
				// ones' shapes (their passes' work) and none of the absent ones' (their passes record nothing: a frame without faces, a
				// frame not in first person). A hosted epoch without a shape (its view's toggle off) is no dimension.
				std::array<std::pair<std::size_t, std::shared_ptr<const ViewFrame>>, kMaxEpochs> hosted{};
				std::uint32_t dimensions = 0;
				for (std::uint32_t h = 0, input = 2; h < kMaxEpochs; ++h)
					if ((a_hosted >> h) & 1)
						hosted[dimensions++] = { EpochTraitsOf(h).index, a_inputs[input++]->Value<ViewFrame>() };  // its view (a View shape's index)
				for (std::uint32_t absent = 0; absent < (1u << dimensions); ++absent) {
					auto fill = [&](RevisionShapes& a_shapes, bool a_withoutIndexPool) {
						a_shapes.main[traits.index] = shapeFragment->Value<PassFrame>();
						a_shapes.depthWithoutIndexPool = a_withoutIndexPool;
						for (std::uint32_t d = 0; d < dimensions; ++d)
							if (!((absent >> d) & 1))
								a_shapes.views[hosted[d].first] = hosted[d].second;
					};
					request([&](RevisionShapes& a_shapes) { fill(a_shapes, false); });
					// The depth epoch's again, without the index pool's dispatch: the commit submits it when it gave out no range (its own
					// UpdateIndexPool decides, whichever commit ran first), so such a frame records no copy of zero groups.
					if (traits.index == kDepthShape && traits.sharesWith == kNoEpoch)
						request([&](RevisionShapes& a_shapes) { fill(a_shapes, true); });
				}
			} else if (traits.shape == ShapeKind::View)
				request([&](RevisionShapes& a_shapes) { a_shapes.views[traits.index] = shapeFragment->Value<ViewFrame>(); });
			else
				for (const auto& variant : shapeFragment->Value<ShadowVariants>()->shapes)
					request([&](RevisionShapes& a_shapes) { a_shapes.shadow = variant; });
			auto work = std::make_shared<PendingRecordings>();
			std::vector<RevisionFragment::Requirement> requirements{ { R::kVersionsSlot, a_inputs[0] }, { R::kShapeSlot + e, shapeFragment } };
			for (std::uint32_t h = 0, input = 2; h < kMaxEpochs; ++h)
				if ((a_hosted >> h) & 1)
					requirements.push_back({ R::kShapeSlot + h, a_inputs[input++] });
			work->fragment = std::make_shared<RevisionFragment>(std::move(requirements));
			work->hosted = traits.shape == ShapeKind::Main ? a_hosted : 0u;
			work->recordings.resize(requests.size());
			work->remaining.store(static_cast<std::uint32_t>(requests.size()), std::memory_order_relaxed);
			work->buildGeneration = a_builds;
			work->recorded = &a_rv.epochs[e].recorded;
			work->failedCount = &a_rv.epochs[e].failed;
			work->droppedCount = &a_rv.epochs[e].dropped;
			work->shapes = &a_rv.epochs[e].shapes;
			work->logged = &a_rv.failuresLogged;
			work->outstanding = &a_outstanding;
			work->name = traits.name;
			const auto epoch = RenderGraphRuntime::EpochOf(traits.segment);
			bool refused = false;
			for (std::size_t i = 0; i < requests.size() && !refused; ++i) {
				a_outstanding.fetch_add(1, std::memory_order_acq_rel);
				refused = !a_host.RequestEpochRecording(epoch, requests[i], [work, i](std::shared_ptr<const org::PersistentGraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					RecordingCompleted(work, i, std::move(a_recording), std::move(a_error));
				});
				if (refused)
					a_outstanding.fetch_sub(1, std::memory_order_acq_rel);
			}
			if (refused)
				return nullptr;
			return work->fragment;
		}
	}

	void IndirectDraws::Impl::AssembleRevision(std::uint32_t a_frame)
	{
		ZoneScopedN("CS.DCLF.AssembleRevision");
		using R = SceneRevisions;
		auto& rv = revisions;
		// A graph build asked of the frame (shadow view slots: ServeRevisionRequests) not made yet: a recording now would be of the graph
		// without what it declares, which that build supersedes. None is sealed until the inputs carry it (one integer compare; the
		// inputs' post wakes the builder).
		if (producer.buildWanted && producer.inputs.buildGeneration < producer.buildWanted) {
			++rv.buildWaits;
			return;
		}
		producer.buildWanted = 0;
		auto draft = rv.assembler.Begin();
		// The versions: the make's sealed set (the same set and fragment while nothing changed, so every recording of it stands), as a
		// fragment of its own after a graph build (a recording is of the build it was made on, so every epoch is recorded again). The
		// build as the frame posted it (RevisionInputs: BuildPoint's).
		const std::uint64_t builds = producer.inputs.buildGeneration;
		const auto& seal = rv.sealed;
		if (rv.versionsFragmentSet != seal.named || rv.graphBuilds != builds) {
			rv.versionsFragment = rv.versionsFragmentSet == seal.named ? RevisionFragment::MakeReady(seal.named) : seal.fragment;
			rv.versionsFragmentSet = seal.named;
			rv.graphBuilds = builds;
			++rv.versionSets;
		}
		if (draft.Get(R::kVersionsSlot) != rv.versionsFragment)
			draft.Set(R::kVersionsSlot, rv.versionsFragment);
		const auto& versionsFragment = draft.Get(R::kVersionsSlot);
		const auto versions = versionsFragment->Value<NamedVersions>();

		// Each epoch's shape (MakeRevision's): its fragment kept while the shape is the same.
		AssembleShapes(rv, draft, producer.made, producer.toggles);

		// Each epoch's recording: kept while it requires exactly the draft's shape and versions (and the shapes of the epochs it hosts
		// that have one); else requested for them. A failed one is dropped (the epoch has none in the revision) and not asked for again
		// until its inputs change. A hosted epoch has none of its own: its host's carry its passes.
		auto* host = RenderGraphRuntime::Get().Host();
		const bool recordable = host && producer.inputs.asyncEpochs;
		std::vector<std::shared_ptr<const RevisionFragment>> inputs;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			const std::uint32_t slot = R::kRecordingSlot + e;
			const auto& traits = EpochTraitsOf(e);
			const auto& shapeFragment = draft.Get(R::kShapeSlot + e);
			const auto current = draft.Get(slot);
			// Its inputs: the versions, its shape, then the shapes of the epochs it hosts that have one (by row; hosted, a bit each).
			inputs.assign({ versionsFragment, shapeFragment });
			std::uint32_t hosted = 0;
			for (std::uint32_t h = 0; h < epochs; ++h)
				if ((traits.hostsMask >> h) & 1)
					if (const auto& hostedFragment = draft.Get(R::kShapeSlot + h)) {
						inputs.push_back(hostedFragment);
						hosted |= 1u << h;
					}
			auto requiresExactly = [&](const RevisionFragment& a_fragment) {
				const auto& requirements = a_fragment.Requirements();
				if (requirements.size() != inputs.size())
					return false;
				for (std::size_t i = 0; i < inputs.size(); ++i)
					if (requirements[i].fragment != inputs[i])
						return false;
				return true;
			};
			if (!recordable || !shapeFragment || traits.hostedBy != kNoEpoch) {
				draft.Set(slot, nullptr);
				continue;
			}
			if (current && requiresExactly(*current)) {
				if (current->GetState() != RevisionFragment::State::Failed)
					continue;
				// Failed for these exact inputs: none in this revision, and none asked for until they change.
				draft.Set(slot, nullptr);
				rv.failedFor[e] = inputs;
				continue;
			}
			if (rv.failedFor[e] == inputs) {
				draft.Set(slot, nullptr);
				continue;
			}
			auto fragment = RequestRecordings(*host, rv, recordingsOutstanding, e, versions, inputs, hosted, builds);
			if (!fragment) {
				// Async epochs stopped under it: what was asked settles the fragment, which this revision does not name.
				++rv.epochs[e].refused;
				draft.Set(slot, nullptr);
				continue;
			}
			++rv.epochs[e].requested;
			draft.Set(slot, std::move(fragment));
		}

		// The frame adopts the revision's set with its selection (AdoptSnapshot: NamedVersions::AdoptAll).
		try {
			(void)rv.assembler.Seal(std::move(draft));
			rv.revisionVersions = versions;
			rv.revisionLayout = rv.layout;
			++rv.seals;
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
				logger::warn("[DCLF] the {} epoch of frame {} has no shape or recording in the selected revision: not submitted <- UNREVISED", EpochTraitsOf(a_epoch).name,
					Scene::FrameView::Get().GetFrame());
			return {};
		}
		return out;
	}

	void IndirectDraws::Impl::SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index)
	{
		// Each epoch is submitted at its trigger's engine point (Schedule/Frame.cpp): anywhere else, a hook is out of step with the table.
		if (const auto trigger = EpochTrigger(a_epoch); Extensions::ActiveTrigger() != trigger) [[unlikely]] {
			if (outsideTrigger[a_epoch]++ == 0)
				logger::warn("[DCLF] the {} epoch was submitted outside its trigger {} (active: {})", EpochTraitsOf(a_epoch).name, TriggerName(trigger),
					TriggerName(Extensions::ActiveTrigger()));
		}
		if (auto* host = RenderGraphRuntime::Get().Host(); host && a_index < a_recordings.recordings.size()) {
			host->UseEpochRecording(a_recordings.recordings[a_index]);
			++revisions.submitted[a_epoch];
			revisions.variants[a_epoch] += a_index != 0;
		}
	}

}
