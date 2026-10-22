// The coordinator's event intake (scene lane): CollectEvents drains every queue into the pass's batch, ApplyEvents applies it (the
// mirror first, then attaches, detaches, structural, switch, hidden and the features' engine channels' events), and the retired batch
// goes back through the retirement chain. Also the load screen's discard and the detach safety net.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

namespace DCLF::Scene
{
	namespace
	{
		using ChannelKeys = std::array<std::vector<const void*>, Extensions::kEngineChannelSlots>;

		// The engine channels a pass applies at a_apply, each drained into its slot (in registration order), where Core's own queues of
		// that point drain. A SceneStack channel has no queue (its events are on the scene's stack).
		void DrainChannels(Api::ChannelApply a_apply, ChannelKeys& a_keys)
		{
			const auto engineChannels = Extensions::RegisteredEngineChannels();
			for (std::size_t k = 0; k < engineChannels.size(); ++k)
				if (engineChannels[k].apply == a_apply && engineChannels[k].kind != Api::ChannelKind::SceneStack) {
					Api::KeySink sink(&a_keys[k]);
					engineChannels[k].drain(sink);
				}
		}

		// A load screen's drain of every engine channel, in registration order: its keys kept as named (ChannelLoading::KeepKeys) or dropped.
		// A SceneStack channel has no queue (its events are on the scene's stack).
		void DrainChannelsLoading(std::vector<const void*>& a_named)
		{
			for (const auto& channel : Extensions::RegisteredEngineChannels()) {
				if (channel.kind == Api::ChannelKind::SceneStack)
					continue;
				Api::KeySink sink(channel.loading == Api::ChannelLoading::KeepKeys ? &a_named : nullptr);
				channel.drain(sink);
			}
		}

		std::size_t ChannelKeyCount(const ChannelKeys& a_keys)
		{
			std::size_t count = 0;
			for (const auto& keys : a_keys)
				count += keys.size();
			return count;
		}
	}

	struct Coordinator::BatchIndex
	{
		std::vector<SceneTracker::Event*> structural;      // head's attach and detach events, not mirror-only, in order
		std::vector<const SceneCapture::Records*> leaves;  // both heads' captures and leaf updates' records, in order
		bool sawDetach = false;                            // one of structural is a detach
	};

	std::shared_ptr<Coordinator::EventBatch> Coordinator::CollectEvents()
	{
		ZoneScopedN("CS.DCLF.Scene.CollectEvents");
		// The queues' one consumer. Each is drained to where it stands now, so later pushes are the next pass's and a pass always ends.
		// The render thread's posts were taken before, so every event older than its capture is in this batch or an earlier one.
		auto& tracker = SceneTracker::Get();
		const bool loading = channels.loadingSeen.load(std::memory_order_acquire);
		Diagnostics::Notify(Diagnostics::EventsCollecting{ *this, loading });
		if (loading) {
			// Load screen: mirror events are carried to the first pass after the load; the rest are discarded (their keys kept for the
			// mirror's parity, their references released at Present).
			if (!loadingCarry)
				loadingCarry = std::make_shared<EventBatch>();
			loadingCarry->AppendMirror(tracker.Drain());
			auto& named = loadingCarry->mirrorNamed;
			auto keep = [&named](const void* a_key) { named.push_back(a_key); };
			auto discarded = std::make_shared<EventBatch>();
			BatchUnwindGuard discardedGuard(*this, discarded);
			{
				std::vector<const void*> keys;
				DrainPropertyEvents(keys);
			}
			DrainNodeEvents(discarded->nodes);
			DrainChannelsLoading(named);
			switchEvents.Drain([&](SwitchEvent&& a_event) {
				keep(a_event.node.get());
				discarded->switches.push_back(std::move(a_event));
			});
			if (!discarded->nodes.empty() || !discarded->switches.empty())
				EngineReleases::PushOwner(std::move(discarded));
			++pumpStats.loading;
			return nullptr;
		}
		auto batch = std::make_shared<EventBatch>();
		// A throw hands the batch (and its engine references) to the render thread, never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		if (loadingCarry) {
			batch->AppendMirror(std::exchange(loadingCarry->mirrorHead, nullptr));
			batch->mirrorNamed = std::move(loadingCarry->mirrorNamed);
			loadingCarry.reset();
		}
		// Only pointer moves here, in push order; walking and evaluating is ApplyEvents'.
		auto* drained = tracker.Drain();
		batch->Append(drained);
		// The fade snaps and fade amounts first among them (Core's fade roots' channels, registered snaps then amounts).
		DrainChannels(Api::ChannelApply::BeforeStructural, batch->channelKeys);
		DrainPropertyEvents(batch->properties);
		DrainNodeEvents(batch->nodes);
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch->switches.push_back(std::move(a_event)); });
		DrainChannels(Api::ChannelApply::AfterSwitches, batch->channelKeys);
		// The oldest stamp; whether a detach came (forces the next category capture: a detach can take a category node before the
		// signature sees its cell go); and the switch catch-ups, engine code that writes, run by the render thread at frame start with
		// reference copies dropped there.
		std::shared_ptr<SwitchCatchUp> catchUp;
		// On a throw its reference copies go to the render thread, never dropped here.
		struct CatchUpUnwind
		{
			std::shared_ptr<SwitchCatchUp>& held;
			int exceptions = std::uncaught_exceptions();
			~CatchUpUnwind()
			{
				if (!held || std::uncaught_exceptions() <= exceptions)
					return;
				for (auto& node : held->attached)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(node)));
				for (auto& event : held->switches)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(event.node)));
			}
		} catchUpUnwind{ catchUp };
		std::uint32_t events = 0;
		for (const auto* event = drained; event; event = event->next) {
			++events;
			if (event->stampNs && (!batch->oldestNs || event->stampNs < batch->oldestNs)) {
				batch->oldestNs = event->stampNs;
				batch->oldestFrame = event->stampFrame;
			}
			if (event->mirrorOnly)
				continue;
			if (event->type == SceneTracker::EventType::Detached)
				channels.categoryDetachSeen.store(true, std::memory_order_relaxed);
			else if (event->type == SceneTracker::EventType::Attached && event->node) {
				if (!catchUp)
					catchUp = std::make_shared<SwitchCatchUp>();
				catchUp->attached.push_back(event->node);
			}
		}
		if (!batch->switches.empty()) {
			if (!catchUp)
				catchUp = std::make_shared<SwitchCatchUp>();
			catchUp->switches.reserve(batch->switches.size());
			for (const auto& event : batch->switches)
				catchUp->switches.push_back(SwitchEvent{ event.node, event.before, event.structural });
		}
		if (catchUp)
			channels.switchCatchUps.Push(std::move(catchUp));
		events += static_cast<std::uint32_t>(batch->properties.size() + batch->nodes.size() + batch->switches.size() + ChannelKeyCount(batch->channelKeys));
		pumpStats.passEvents.push_back(events);
		return batch;
	}

	void Coordinator::HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held)
	{
		if (!a_held || !a_held->reference)
			return;
		MaterialReference reference;
		reference.Adopt(std::exchange(a_held->reference, nullptr));
		EngineReleases::PushOwner(std::make_shared<MaterialReference>(std::move(reference)));
	}

	void Coordinator::DrainMaterialCaptures()
	{
		auto& s = materialSnapshotStats;
		// The parity's passes keep the deduplicated materials, which RewriteCapturedMaterials evaluates as every capture once was.
		const bool parity = Diagnostics::Observe().scene && Diagnostics::ParityDue(passSerial, kMaterialDedupParityOffset);
		MaterialPort::captures.Drain([&](std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held) {
			++s.captured;
			const RE::BSShaderMaterial* material = a_held->reference;
			auto& entry = materialSnapshots[a_held->reference];
			if (entry.held && entry.held->sequence > a_held->sequence) {
				++s.stale;
				HandBackHeld(std::move(a_held));
				return;
			}
			// A capture equal to the held one (every attached leaf captures its shared material; a controller rewrites the same value)
			// changes no record: the newer is kept, nothing is rewritten. One whose frame-owned bytes alone moved (the transform buffers, a
			// port's declared frame variables: the Effect port's falloff and base colour) is counted as watched while a feature patches them
			// every frame (Extensions::FrameVSVariablePatched: Features/TextureTransforms, the one writer of them; a rewrite keeps the slots'
			// own); unpatched, they are the records': it is rewritten.
			auto change = MaterialPort::SnapshotChange::Fields;
			if (entry.held)
				change = MaterialPort::SnapshotChangeOf(entry.held->snapshot, a_held->snapshot);
			if (change == MaterialPort::SnapshotChange::Transforms && !Extensions::FrameVSVariablePatched())
				change = MaterialPort::SnapshotChange::Fields;
			if (entry.held && change == MaterialPort::SnapshotChange::Fields && entry.held->snapshot.size == a_held->snapshot.size) {
				++s.fieldsMoved;
				if (!s.fieldsMovedVtable)
					s.fieldsMovedVtable = a_held->snapshot.vtable;
				for (auto& [vtable, count] : s.fieldsMovedBy)
					if (!vtable || vtable == a_held->snapshot.vtable) {
						vtable = a_held->snapshot.vtable;
						++count;
						break;
					}
				const auto& was = entry.held->snapshot.bytes;
				const auto& now = a_held->snapshot.bytes;
				for (std::size_t w = 0; w < s.wordMoved.size() && w * 4 + 4 <= a_held->snapshot.size; ++w)
					s.wordMoved[w] += std::memcmp(was.data() + w * 4, now.data() + w * 4, 4) != 0;
			}
			if (entry.held) {
				++s.replaced;
				HandBackHeld(std::move(entry.held));
			}
			entry.held = std::move(a_held);
			entry.frame = sceneFrame;
			if (change != MaterialPort::SnapshotChange::Fields) {
				++s.deduped;
				if (change == MaterialPort::SnapshotChange::Transforms) {
					++s.transformsOnly;
					// Nothing to follow (the frame values read the material's frame buffer and the GPU patches its rows): counted only.
					if (materialDependents.contains(material))
						++materialRecordStats.transformsWatched;
				}
				if (parity && !entry.listed)
					materialsDeduped.push_back(material);
				return;
			}
			if (entry.listed) {
				++s.relisted;
				return;
			}
			entry.listed = true;
			materialsCaptured.push_back(material);
		});
		// Answers to the joins' requests (a served capture was pushed before its answer): the join may ask again.
		channels.materialRequestsAnswered.Drain([&](MaterialAnswer&& a_answer) {
			materialRequested.erase(a_answer.material);
			++(a_answer.captured ? residentStats.materialsServed : residentStats.materialsStale);
		});
		// Aged by frame, so once a frame: a later pass of the same frame would find nothing older (frames only move forward).
		if (std::exchange(materialSnapshotsAgedFrame, sceneFrame) == sceneFrame)
			return;
		for (auto it = materialSnapshots.begin(); it != materialSnapshots.end();) {
			if (sceneFrame - it->second.frame > kMaterialSnapshotFrames) {
				++s.aged;
				HandBackHeld(std::move(it->second.held));
				it = materialSnapshots.erase(it);
			} else {
				++it;
			}
		}
	}

	void Coordinator::ApplyEvents(std::shared_ptr<EventBatch> a_batch)
	{
		// Load markers first, then the batch (empty during a load screen, which still takes the category capture and rescan).
		channels.loadMarkers.Drain([this](bool) { ApplyLoading(); });
		ApplyBatch(a_batch ? std::move(a_batch) : std::make_shared<EventBatch>());
	}

	void Coordinator::ApplyFadeNodeAnswers()
	{
		auto& r = referenceStats;
		channels.fadeNodeAnswers.Drain([&](FadeNodeAnswer&& a_answer) {
			auto* geometry = const_cast<RE::BSGeometry*>(a_answer.geometry);
			const auto it = tracked.find(geometry);
			auto* entry = it != tracked.end() ? &it->second : nullptr;
			if (entry && entry->fadeNodeRequested == a_answer.fadeNode)
				entry->fadeNodeRequested = nullptr;
			// Taken only while the mirror still names that node for the entry's property.
			const auto* record = entry ? mirror.Geometry(geometry) : nullptr;
			const auto* property = record && record->property ? mirror.Property(record->property) : nullptr;
			if (!entry || !a_answer.node || !property || property->fadeNode != a_answer.fadeNode || entry->fadeNodeRef.get() == a_answer.fadeNode) {
				r.fadeNodesAnsweredNone += a_answer.node ? 0 : 1;
				HandBack(std::move(a_answer.node));
				return;
			}
			++r.fadeNodesAnswered;
			HandBack(std::move(entry->fadeNodeRef));
			entry->fadeNodeRef = std::move(a_answer.node);
			// List what the walk left unlisted for want of the reference.
			for (const std::uint32_t slot : { entry->slot, entry->layerSlot }) {
				if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != geometry)
					continue;
				ComponentsSlotWritten(slot);
				if (IsResidentSlot(slot))
					ComponentsResident(slot);
			}
		});
	}

	void Coordinator::ApplyLoading()
	{
		// The first frame after the load rescans; drained-but-unapplied state and coordinator-only queues are discarded.
		rescanPending = true;
		{
			std::vector<const void*> keys;
			DrainLodFadeEvents(keys);
			DrainEmittanceEvents(keys);
		}
		propertyChanged.clear();
		// Released on the render thread (a node's last release runs engine destructors: its collision object leaves the Havok world).
		HandBack(nodeChanged);
		moveEvents.Discard();
		movedFrame.clear();
		batchHidden.clear();
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
		switchResync = true;
	}

	void Coordinator::ApplyBatch(std::shared_ptr<EventBatch> a_batch)
	{
		// Material captures first: an attach names materials whose captures were pushed before it.
		{
			ZoneScopedN("CS.DCLF.Scene.MaterialCaptures");
			DrainMaterialCaptures();
		}
		auto batch = std::move(a_batch);
		BatchUnwindGuard unwindGuard(*this, batch);
		const bool rescanned = rescanPending;
		if (rescanPending) {
			ZoneScopedN("CS.DCLF.Scene.Rescan");
			ClearForRescan();
		}

		BatchIndex index;
		ApplyMirrorEvents(*batch, index);

		// A detach forces the category refresh: it can take a category node the signature cannot see go until the cell does.
		RefreshBatchCategories(index, rescanned);
		ApplyAttachDetach(index);

		{
			// Parity only: check a slice for missed detaches, once a pass. The normal path trusts the detach events.
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (Policy::ValidateTrackedSlice() && std::exchange(validatedFrame, PassStamp()) != PassStamp())
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
		// The fade snaps' reseeds, then Actors' fadeAmounts (the rows' input; the GPU's fade state stands): the fade roots' channels.
		ComponentsChannelKeys(Api::ChannelApply::BeforeStructural, *batch);
		// Structural events: property flag/material/controller changes, and nodes Havok moved or gave a controller.
		propertyChanged.insert(propertyChanged.end(), batch->properties.begin(), batch->properties.end());
		for (auto& node : batch->nodes)
			nodeChanged.push_back(std::move(node));
		batch->nodes.clear();
		// One pending entry per switch; its index before the oldest event decides whether the selection changed.
		for (auto& event : batch->switches) {
			++delta.switchEvents;
			const auto [at, inserted] = switchPendingIndex.try_emplace(event.node.get(), static_cast<std::uint32_t>(switchPending.size()));
			if (inserted)
				switchPending.push_back({ std::move(event.node), event.before, event.structural });
			else
				switchPending[at->second].structural |= event.structural;
		}
		ComponentsChannelKeys(Api::ChannelApply::AfterSwitches, *batch);

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
		// Tracked geometries the captures and leaf updates named (ApplyMirrorEvents indexed them): hold their current properties from
		// the pins.
		HoldLeafProperties(index);
		// For the event-to-publication latency.
		if (batch->oldestNs && (!unpublishedNs || batch->oldestNs < unpublishedNs)) {
			unpublishedNs = batch->oldestNs;
			unpublishedFrame = batch->oldestFrame;
		}
		// Its events hold engine references (detached subtrees a published table may still name): retire them through the chain. An
		// empty batch is dropped here, or its return would wake the pump and every pass would keep the next going.
		if (batch->head || batch->mirrorHead || !batch->switches.empty())
			retirement.Open().events.push_back(std::move(batch));
		// No reference is made from the pins after the apply. Let go, not cleared: a burst's buckets would be cleared again by every pass.
		batchPins = {};
	}

	void Coordinator::ClearForRescan()
	{
		// With everything cleared, RefreshCategoryNodes walks every category node as new: the full rescan.
		rescanPending = false;
		for (auto& [geometry, entry] : tracked) {
			ReleaseObjectSlot(entry);
			HandBack(std::move(entry.geometry));
			HandBack(std::move(entry.property));
			HandBack(std::move(entry.layerProperty));
			HandBack(std::move(entry.sourceRef));
			HandBack(std::move(entry.fadeNodeRef));
		}
		tracked.clear();
		ComponentsForgotAll();
		++trackedLayout;
		sceneIdentity.Reset();
		categoryNodes.clear();
		categoryMembers.clear();
		categoryMembersAt.clear();
		nodeSetsDirty = true;
		for (auto& [root, entry] : alwaysRenderRoots)
			HandBack(std::move(entry.root));
		alwaysRenderRoots.clear();
		validationCursor = 0;
		fullEvaluation = true;
		propertyDependents.clear();
		rootDependents.clear();
		lightDependents.clear();
		propertyDependentsAt.clear();
		rootDependentsAt.clear();
		lightDependentsAt.clear();
		hiddenDependentsAt.clear();
		lightEntryChanges.push_back({ nullptr, 0 });
		ReleaseRootOwners();
		rootReference.clear();
		referenceRoot.clear();
		dirtyRoots.clear();
		rootMotion.clear();
		movingRoots.clear();
		stillRoots.clear();
		moverSet.clear();
		placementDirty = true;
		mirrorMotionNodes.clear();
		mirrorSkinGeometries.clear();
		hiddenDependents.clear();
		hiddenAhead.clear();
		DropSunCandidates();
		buckets = {};
		// The rescan's adds re-tag gated entries (the gates' rescan hook drops their tags).
		ComponentsRescan();
	}

	void Coordinator::RefreshBatchCategories(const BatchIndex& a_index, bool a_rescanned)
	{
		DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
		addSource = a_rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
		RefreshCategoryNodes(a_index.sawDetach || a_rescanned);
		addSource = TrackSource::AttachEvent;
		// Subtrees the mirror lacked last time, since captured by the render thread. Their references are handed back first (the
		// list may be a root's last holder; never released on the pump); the retirement chain holds them while AddSubtrees walks keys.
		auto retry = std::exchange(pendingSubtrees, {});
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> retryRoots;
		retryRoots.reserve(retry.size());
		for (const auto& root : retry)
			retryRoots.emplace_back(root.get(), SubtreeSource::Retry);
		HandBack(retry);
		AddSubtrees(retryRoots);
		ApplyFadeNodeAnswers();
	}

	void Coordinator::ApplyAttachDetach(const BatchIndex& a_index)
	{
		DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
		// A detach leaves the scene only if nothing reattached the geometry by the batch's end: the engine moves objects between
		// containers (a cell's dynamic and static nodes, as physics wakes and sleeps) by detach plus attach. Entry, slot, binding stay.
		std::vector<RE::BSGeometry*> detached;
		// Prepare the attaches' subtrees first (pure; the mirror already holds the whole batch, detaches apply after the loop), then
		// merge each at its event: event order matters only to the merges, which keep it. Serial mode prepares and merges per event.
		// The batch's attach and detach events alone (ApplyMirrorEvents indexed them): never the updates, which outnumber them.
		auto& a = attachStats;
		a.structuralEvents += a_index.structural.size();
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> attached;
		if (!categoryNodes.empty())
			for (const auto* event : a_index.structural)
				if (event->type == SceneTracker::EventType::Attached)
					attached.emplace_back(event->node.get(), SubtreeSource::AttachEvent);
		const std::uint32_t subtreeMode = attached.empty() ? 0 : FanoutMode(attached.size(), kSubtreeFanoutMin);
		if (!attached.empty()) {
			++fanoutStats.subtreeRounds;
			fanoutStats.subtreeRoots += attached.size();
		}
		[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
		if (subtreeMode != 0)
			PrepareSubtrees(attached, subtreeMode, subtreePreps);
		std::size_t nextPrep = 0;
		double mergeMs = 0.0;
		double ancestorMs = 0.0, serialMs = 0.0;
		for (auto* event : a_index.structural) {
			// A sun candidate above it: its subtree changed, which its primary-cut plan reads (PlanOf).
			if (!event->ancestors.empty() && !sunTable.index.empty()) {
				const auto ancestorStart = std::chrono::steady_clock::now();
				for (const void* ancestor : event->ancestors)
					if (const auto it = sunTable.index.find(static_cast<const RE::NiAVObject*>(ancestor)); it != sunTable.index.end()) {
						sunEntriesForced.insert(it->first);
						sunEntriesDirty.push_back(it->first);
					}
				ancestorMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ancestorStart).count();
			}
			if (event->type == SceneTracker::EventType::Attached) {
				++stats.attachedEvents;
				if (!categoryNodes.empty()) {
					ZoneScopedN("CS.DCLF.Scene.SubtreesMerge");
					const auto mergeStart = std::chrono::steady_clock::now();
					if (subtreeMode == 0)
						AddSubtree(event->node.get());
					else
						MergeSubtree(subtreePreps[nextPrep++]);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mergeStart).count();
					if (subtreeMode == 0)
						serialMs += ms;
					else
						mergeMs += ms;
				}
			} else if (event->type == SceneTracker::EventType::Detached) {
				++stats.detachedEvents;
				detached.insert(detached.end(), event->removed.begin(), event->removed.end());
			}
		}
		if (subtreeMode != 0) {
			assert(nextPrep == subtreePreps.size() && mirror.Writes() == mirrorWrites);
			AddScenePartMs(ScenePart::SubtreesMerge, mergeMs);
			fanoutStats.subtreeMergeMs += mergeMs;
		}
		a.ancestorMs += ancestorMs;
		a.mergeMs += mergeMs + serialMs;
		ApplyDetaches(detached);
		// Parity: the category members' index against the tracked entries, on a schedule.
		if (Policy::CheckCategoryMembers(false, passSerial))
			CheckCategoryMembers("the attach/detach merge");
		if (Policy::CheckAncestorKeys(passSerial))
			CheckAncestorKeys();
	}

	void Coordinator::ApplyDetaches(const std::vector<RE::BSGeometry*>& a_detached)
	{
		ZoneScopedN("CS.DCLF.Scene.Detaches");
		auto& a = attachStats;
		const auto detachStart = std::chrono::steady_clock::now();
		a.detachedGeometries += a_detached.size();
		for (auto* geometry : a_detached) {
			// Only a tracked geometry is known alive (its entry holds it), so safe to walk up from.
			if (!tracked.contains(geometry))
				continue;
			if (FindCategoryNode(geometry, nullptr)) {
				++stats.detachMoves;
				continue;
			}
			++a.detachedErased;
			EraseTracked(geometry);
		}
		a.detachMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - detachStart).count();
	}

	void Coordinator::HoldLeafProperties(const BatchIndex& a_index)
	{
		ZoneScopedN("CS.DCLF.Scene.LeafHolds");
		const auto leafStart = std::chrono::steady_clock::now();
		for (const auto* records : a_index.leaves) {
			attachStats.leafKeys += records->geometries.size();
			for (const auto& geometry : records->geometries)
				if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(geometry.key))); it != tracked.end()) {
					++attachStats.leafHolds;
					HoldProperties(it->second);
					// Waited for its records: write it now.
					if (std::exchange(it->second.leafWaiting, false)) {
						it->second.candidateFrame = 0;
						pendingEvaluation.push_back(it->first);
						++leafStats.rescheduled;
					}
				}
		}
		attachStats.leafMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - leafStart).count();
	}

	void Coordinator::PrepareMirrorCaptures(const std::vector<const SceneCapture::Records*>& a_captures, std::size_t a_records)
	{
		// Each capture ApplyMirrorEvents applies whole (not a fill, which reads the mirror), in its apply order, copied and stamped here so
		// the serial apply only moves records into the maps. Pure: the events' own records; a job per capture and record kind.
		const auto& captures = a_captures;
		const std::size_t records = a_records;
		mirrorPreps.clear();
		if (captures.empty())
			return;
		auto& a = attachStats;
		++a.mirrorBatches;
		a.mirrorCaptures += captures.size();
		a.mirrorRecords += records;
		mirrorPreps.resize(captures.size());
		const std::uint32_t frame = SceneCapture::Frame();
		const std::size_t jobs = captures.size() * SceneMirror::kTypes;
		auto body = [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.Scene.Mirror.PrepareShards");
			for (std::size_t j = a_begin; j < a_end; ++j)
				SceneMirror::Prepare(*captures[j / SceneMirror::kTypes], static_cast<std::uint32_t>(j % SceneMirror::kTypes), frame, mirrorPreps[j / SceneMirror::kTypes]);
		};
		ZoneScopedN("CS.DCLF.Scene.Mirror.Prepare");
		const auto start = std::chrono::steady_clock::now();
		// The serial modes prepare here too: the parity runs apply what the normal path does (moved, not copied).
		if (FanoutMode(records, kMirrorFanoutRecords) == 2) {
			++a.mirrorPooled;
			SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.Mirror.Prepare", jobs, 2, body);
		} else {
			body(0, jobs);
		}
		a.mirrorPrepareMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	namespace
	{
		// An attach into the mirror: a mirror request's fill, a prepared capture, or the stale record of one attached out of the world.
		void ApplyMirrorAttach(SceneMirror& a_mirror, std::vector<SceneMirror::Prepared>& a_mirrorPreps, SceneTracker::Event& a_event, std::size_t& a_nextPrep)
		{
			if (a_event.captured && a_event.mirrorFill) {
				// A mirror request's capture fills only what the mirror lacks.
				const auto& captured = *a_event.captured;
				SceneCapture::Records fill;
				fill.sequence = captured.sequence;
				for (const auto& n : captured.nodes)
					if (!a_mirror.Node(n.key))
						fill.nodes.push_back(n);
				for (const auto& g : captured.geometries)
					if (!a_mirror.Geometry(g.key))
						fill.geometries.push_back(g);
				for (const auto& p : captured.properties)
					if (!a_mirror.Property(p.key))
						fill.properties.push_back(p);
				for (const auto& a : captured.alphas)
					if (!a_mirror.Alpha(a.key))
						fill.alphas.push_back(a);
				if (!fill.nodes.empty() || !fill.geometries.empty() || !fill.properties.empty() || !fill.alphas.empty())
					a_mirror.Apply(fill);
			} else if (a_event.captured) {
				assert(a_nextPrep < a_mirrorPreps.size());
				a_mirror.Apply(*a_event.captured, &a_mirrorPreps[a_nextPrep++]);
			}
			else if (a_event.node && a_mirror.Node(a_event.node.get()))
				// Attached out of the world: its record is stale (the event holds the object, so its name is safe).
				a_mirror.Evict(a_event.node.get(), fmt::format("'{}' {}", a_event.node->name.c_str() ? a_event.node->name.c_str() : "",
													 const_cast<RE::NiAVObject*>(a_event.node.get())->GetRTTI() ? const_cast<RE::NiAVObject*>(a_event.node.get())->GetRTTI()->name : "?"));
		}

		// A mirror update, its record moved into the mirror; the trait and skin inputs it names and its hidden store noted.
		void ApplyMirrorUpdate(SceneMirror& a_mirror, SceneTracker::Event& a_event, ankerl::unordered_dense::set<const void*>& a_mirrorMotionNodes,
			ankerl::unordered_dense::set<const void*>& a_mirrorSkinGeometries, std::vector<CoordinatorHidden::BatchHidden>& a_batchHidden)
		{
			// The update's record is the mirror's to keep from here (moved; the event's fields, leaf, sequence and site stay).
			const auto* hidden = a_event.update.hiddenSite != ~0u ? std::get_if<SceneCapture::NodeRecord>(&a_event.update.record) : nullptr;
			const CoordinatorHidden::BatchHidden hiddenEvent = hidden ? CoordinatorHidden::BatchHidden{ hidden->key, a_event.update.hiddenSite, (hidden->flags & 1u) != 0, a_event.update.sequence } : CoordinatorHidden::BatchHidden{};
			const auto [type, key] = a_mirror.UpdateTaking(a_event.update);
			Diagnostics::Notify(Diagnostics::MirrorUpdateApplied{ type, key, a_event.update.fields });
			// A trait's input (a node's controllers or rigid body) or a skin's (its instance, its dismember partitions' shown flags):
			// the entries it names are classified again at the next delta walk (keys only), not polled every frame.
			if (key && type == 0 && (a_event.update.fields & (SceneCapture::NodeRecord::kControllers | SceneCapture::NodeRecord::kBody)))
				a_mirrorMotionNodes.insert(key);
			else if (key && type == 1 && (a_event.update.fields & (SceneCapture::GeometryRecord::kDismember | SceneCapture::GeometryRecord::kSkin)))
				a_mirrorSkinGeometries.insert(key);
			// A hidden store, for DrainHiddenEvents (same batch as the mirror's bit).
			if (hidden)
				a_batchHidden.push_back(hiddenEvent);
		}
	}

	void Coordinator::ApplyMirrorEvents(const EventBatch& a_batch, BatchIndex& a_index)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror");
		mirror.SetChecking(Policy::CheckMirrorApply());
		mirror.BeginBatch();
		std::vector<const SceneCapture::Records*> captures;
		std::size_t records = 0;
		PinMirrorBatch(a_batch, captures, records);
		PrepareMirrorCaptures(captures, records);
		ZoneNamedN(mirrorApplyZone, "CS.DCLF.Scene.Mirror.Apply", true);
		const auto applyStart = std::chrono::steady_clock::now();
		std::size_t nextPrep = 0;
		// The apply's time by event kind (AttachStats::mirrorKindMs), taken where the kind changes: runs of one kind cost one clock read.
		auto& attach = attachStats;
		std::size_t kind = AttachStats::kMirrorKinds;
		auto runStart = applyStart;
		auto enter = [&](std::size_t a_kind) {
			++attach.mirrorKindEvents[a_kind];
			if (a_kind == kind)
				return;
			const auto now = std::chrono::steady_clock::now();
			if (kind != AttachStats::kMirrorKinds)
				attach.mirrorKindMs[kind] += std::chrono::duration<double, std::milli>(now - runStart).count();
			kind = a_kind;
			runStart = now;
		};
		auto apply = [&](SceneTracker::Event* a_head, bool a_tracker) {
			for (auto* event = a_head; event; event = event->next) {
				// The later steps' index (BatchIndex), from this one walk.
				if (event->type == SceneTracker::EventType::Attached || event->type == SceneTracker::EventType::Detached) {
					if (event->type == SceneTracker::EventType::Attached && event->captured)
						a_index.leaves.push_back(event->captured.get());
					if (a_tracker && !event->mirrorOnly) {
						a_index.structural.push_back(event);
						a_index.sawDetach |= event->type == SceneTracker::EventType::Detached;
					}
				} else if (event->type == SceneTracker::EventType::Updated && event->update.leaf) {
					a_index.leaves.push_back(event->update.leaf.get());
				}
				enter(static_cast<std::size_t>(event->type == SceneTracker::EventType::Attached ? (event->captured ? (event->mirrorFill ? 1 : 0) : 2) :
											   event->type == SceneTracker::EventType::Detached ? 3 :
											   event->type == SceneTracker::EventType::Stacked  ? 4 :
											                                                      5));
				if (event->type == SceneTracker::EventType::Attached) {
					ApplyMirrorAttach(mirror, mirrorPreps, *event, nextPrep);
				} else if (event->type == SceneTracker::EventType::Detached) {
					// Before the records go: the marks on the detached roots erased, their components told (a gate goes on without the root).
					if (event->detachedRoot && !rootMarks.empty())
						UnmarkDetached(event->detachedRoot, event->removedNodes);
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else if (event->type == SceneTracker::EventType::Stacked) {
					// Ordered after the attaches pushed before it (a LOD gate's: its incoming blocks').
					ComponentsStacked(Api::EngineChannelId{ event->stackedChannel },
						Api::Stacked{ event->stackedToken, event->stackedPinned, event->stackedNamed, event->stampNs, event->stampFrame });
				} else {
					ApplyMirrorUpdate(mirror, *event, mirrorMotionNodes, mirrorSkinGeometries, batchHidden);
				}
			}
		};
		apply(a_batch.mirrorHead, false);
		apply(a_batch.head, true);
		if (kind != AttachStats::kMirrorKinds)
			attach.mirrorKindMs[kind] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
		CountMirrorWork();
		assert(nextPrep == mirrorPreps.size());
		// Moved-from records: their memory goes now, not with the next burst.
		mirrorPreps.clear();
		attachStats.mirrorApplyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count();
		if (Policy::CheckMirrorUses(passSerial))
			CheckMirrorRecordUses();
		RecheckDeferredReads();
		Diagnostics::Notify(Diagnostics::MirrorEventsApplied{ *this, std::span(a_batch.channelKeys).first(Extensions::RegisteredEngineChannels().size()), a_batch.mirrorNamed });
	}

	void Coordinator::PinMirrorBatch(const EventBatch& a_batch, std::vector<const SceneCapture::Records*>& a_captures, std::size_t& a_records)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror.Pins");
		// One walk for the pins and the captures to prepare; the pins from the events that carry any. The batch's pins by key: the
		// only source of references while it applies.
		std::vector<const std::vector<RE::NiPointer<RE::NiRefObject>>*> pinned;
		std::size_t pins = 0;
		for (const auto* head : { a_batch.mirrorHead, a_batch.head })
			for (const auto* event = head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached && event->captured) {
					const auto& captured = *event->captured;
					if (!captured.pins.empty()) {
						pinned.push_back(&captured.pins);
						pins += captured.pins.size();
					}
					if (!event->mirrorFill) {
						a_captures.push_back(&captured);
						a_records += captured.nodes.size() + captured.geometries.size() + captured.properties.size() + captured.alphas.size();
					}
				}
				if (!event->pins.empty()) {
					pinned.push_back(&event->pins);
					pins += event->pins.size();
				}
			}
		batchPins.clear();
		batchPins.reserve(pins);
		for (const auto* list : pinned)
			PinBatch(*list);
	}

	void Coordinator::CountMirrorWork()
	{
		auto& attach = attachStats;
		const auto work = mirror.TakeWork();
		attach.replaysSkipped += work.replaysSkipped;
		attach.replaysWalked += work.replaysWalked;
		attach.updatesKept += work.remembered;
		attach.updatesForgotten += work.forgotten;
		attach.applyChecks += work.skipsChecked + work.preparedChecked + work.logsChecked;
		const std::uint64_t wrong = work.skipsWrong + work.preparedWrong + work.logsWrong;
		if (wrong && attach.applyWrong == 0)
			logger::warn("[DCLF] scene mirror apply (W8) at pass {}: {} skipped replays that had updates to apply, {} prepared captures unlike their records, {} update logs "
						 "unlike their chains <- MIRROR APPLY",
				passSerial, work.skipsWrong, work.preparedWrong, work.logsWrong);
		attach.applyWrong += wrong;
	}

	void Coordinator::CheckMirrorRecordUses()
	{
		// Parity: a capture drops only its own keys' uncounted records; none may be left elsewhere.
		++attachStats.usesChecks;
		if (const std::size_t unused = mirror.UnusedRecords()) {
			if (attachStats.usesUnused == 0)
				logger::warn("[DCLF] scene mirror: {} property or alpha records no geometry counts after pass {}'s captures <- MIRROR USES", unused, passSerial);
			attachStats.usesUnused += unused;
		}
	}

	void Coordinator::ApplySwitchEvents(bool a_full)
	{
		if (a_full)
			switchResync = true;
		for (auto& pending : switchPending) {
			auto* node = pending.node.get();
			const auto* record = node ? mirror.Node(node) : nullptr;
			// Only the covered scene: a switch still loading is caught up by its attach.
			if (!record || !(record->kind & SceneCapture::kKindSwitch) || !FindCategoryNode(node, nullptr))
				continue;
			if (!pending.structural && record->switchIndex == pending.before)
				continue;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SwitchEvent)];
			if (!a_full)
				Diagnostics::Notify(Diagnostics::TrackedBelowRead{ *this, MirrorRead::SwitchEvent, node });
			++delta.switchChanges;
			switchesApplied.push_back(node);
			ComponentsSwitched(node);
			if (a_full)
				continue;
			// The selection is a classification input for every entry under it.
			ForEachMirror(mirror, node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				return true;
			}, kMaxEventNodes);
		}
		// The switches' references are the render thread's to drop.
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
	}

	void Coordinator::ApplyNodeEvent(RE::NiAVObject* a_node)
	{
		// Only the covered scene (not a loading subtree, not the sky).
		if (!a_node || !FindCategoryNode(a_node, nullptr))
			return;
		// Non-null: FindCategoryNode found it.
		const auto* record = mirror.Node(a_node);
		// An actor's entries are per frame (its placement the plan's, its verdict its hidden events'), and its sun entry is never tested:
		// its owner class's own motion.
		if (Extensions::OwnerClassOf(*record).ownMotion)
			return;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::NodeEvent)];
		Diagnostics::Notify(Diagnostics::TrackedBelowRead{ *this, MirrorRead::NodeEvent, a_node });
		// Already placed every frame (written in full, or a mover: the plan's, FrameValues').
		auto placedEveryFrame = [](const Tracked& a_tracked) {
			return a_tracked.perFrame && (!a_tracked.lightTraits || (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves)));
		};
		// Sun entry nodes above it: bound and motion may have changed. A root known to move is the root pass's (QueueRoots) already.
		for (const void* key = a_node; key;) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			const auto* at = mirror.Node(key);
			key = at ? at->parent : nullptr;
			if (!rootDependents.contains(object))
				continue;
			if (const auto motion = rootMotion.find(object); motion != rootMotion.end() && motion->second)
				continue;
			ScheduleRoot(object);
		}
		// Entries below it: placement, and traits (a new body or controller).
		ForEachMirror(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)));
					entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			return true;
		}, kMaxEventNodes);
	}

	void Coordinator::ApplyMotionUpdate(const void* a_node)
	{
		// Keys only: the mirror's records (a node the mirror no longer holds was detached, its entries erased with it).
		const auto* record = mirror.Node(a_node);
		Ineligible reason = Ineligible::None;
		bool unmirrored = false;
		if (!record || !MirrorCategoryNode(a_node, reason, unmirrored))
			return;
		// An actor's entries are per frame whatever their traits (kTraitOwnerClass): its owner class's own motion.
		if (Extensions::OwnerClassOf(*record).ownMotion)
			return;
		// Roots above it: their motion (RootMoves) is judged again with their dependents.
		for (const void* key = a_node; key;) {
			const auto* at = mirror.Node(key);
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			key = at ? at->parent : nullptr;
			if (rootDependents.contains(object))
				ScheduleRoot(object);
		}
		// Entries below it: their traits (kTraitMoves) and so whether they are per frame, movers and in the plan.
		ForEachMirror(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end())
					Reclassify(entry->first, entry->second);
			return true;
		}, kMaxEventNodes);
	}

	void Coordinator::ValidateSlice()
	{
		// Missed-detach net: a tracked geometry must still hang under its category node. A slice per frame keeps the cost flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		// The check is live, the entries the mirror's: a recent detach differs until the next batch erases it, so only a suspect still
		// differing a batch later counts.
		for (auto* geometry : std::exchange(validationSuspects, {}))
			if (const auto it = tracked.find(geometry); it != tracked.end() && FindCategoryNodeLive(geometry, nullptr) != it->second.categoryNode)
				stale.push_back(geometry);
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// Dense storage: the cursor walks it without hashing.
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNodeLive(it->first, &reason) != it->second.categoryNode) {
				validationSuspects.push_back(it->first);
			} else if (it->second.parentReason != reason) {
				it->second.parentReason = reason;
				it->second.candidateFrame = 0;
				pendingEvaluation.push_back(it->first);
			}
		}
		for (auto* geometry : stale) {
			EraseTracked(geometry);
			++stats.validationDrops;
		}
	}


	void Coordinator::ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// The chain above a tracked geometry does not change while tracked: listed once (a full walk lists anew).
		if (!a_tracked.hiddenChain.empty())
			return;
		// Up to, not including, the category node (thousands share it; DrainHiddenEvents handles its events by categoryNode).
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::HiddenChain)];
		for (const void* key = a_geometry; key && key != a_tracked.categoryNode; key = mirror.Parent(key)) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			a_tracked.hiddenChain.push_back(object);
			List(hiddenDependents, hiddenDependentsAt, object, a_geometry);
		}
		Diagnostics::Notify(Diagnostics::HiddenChainRead{ *this, a_geometry, a_tracked.categoryNode, a_tracked.hiddenChain });
	}

	void Coordinator::UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		for (const auto* object : a_tracked.hiddenChain)
			Unlist(hiddenDependents, hiddenDependentsAt, object, a_geometry);
		a_tracked.hiddenChain.clear();
	}

	void Coordinator::DrainHiddenEvents()
	{
		// Keys only; the addresses are never read.
		auto& primary = PrimaryCull::Get();
		// A static with a chain event is reclassified; an actor's light-path entry retakes its frame verdict (FrameVerdictKept).
		ankerl::unordered_dense::set<RE::BSGeometry*> retake;
		ankerl::unordered_dense::set<const void*> categoryEvents;
		auto announce = [&](RE::BSGeometry* a_geometry, Tracked& a_entry) {
			a_entry.hiddenEventOwed = true;
			if (!(a_entry.lightTraits & kTraitOwnerClass))
				retake.insert(a_geometry);
		};
		auto announceKey = [&](const void* a_key) {
			// Not a CategoryRoot view's root (the first-person root: toggled within every frame; no member chain reaches its bit).
			if (categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<void*>(a_key))) && !ViewRoot(a_key))
				categoryEvents.insert(a_key);
			const auto dependents = hiddenDependents.find(a_key);
			if (dependents == hiddenDependents.end())
				return;
			for (auto* geometry : dependents->second)
				if (const auto entry = tracked.find(geometry); entry != tracked.end())
					announce(entry->first, entry->second);
		};
		// Nodes in the frame's capture (cullHidden) are judged by the capture's bit. An event numbered after the capture may be newer:
		// e.g. switching to first person hides the third-person skeleton in FirstPersonState::Begin (FUN_140736150, from Main::Update)
		// after the capture, and the old "shown" would keep it drawn. Such a key is announced again at the first pass whose capture is
		// newer than its event.
		const auto& globals = FrameGlobals::Current();
		for (auto it = hiddenAhead.begin(); it != hiddenAhead.end();) {
			if (it->second < globals.cullHiddenSequence) {
				announceKey(it->first);
				++stats.hiddenAheadAnnounced;
				it = hiddenAhead.erase(it);
			} else
				++it;
		}
		auto aheadOfCapture = [&](const BatchHidden& a_event) {
			if (a_event.sequence < globals.cullHiddenSequence)
				return false;
			const auto& listed = globals.cullHidden;
			const auto* object = static_cast<const RE::NiAVObject*>(a_event.key);
			const auto it = std::lower_bound(listed.begin(), listed.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_object) { return a_entry.first < a_object; });
			return it != listed.end() && it->first == object && it->second != a_event.hidden;
		};
		// The applied batches' hidden stores, in order.
		stats.hiddenEvents += batchHidden.size();
		for (const auto& a_event : std::exchange(batchHidden, {})) {
			const void* a_key = a_event.key;
			if (aheadOfCapture(a_event)) {
				auto& sequence = hiddenAhead[a_key];
				sequence = std::max(sequence, a_event.sequence);
			}
			// PrimaryCull rebuilds its kept scene lists (held while the scene list job may run).
			if (holdPrimaryNotes)
				channels.primaryNotes.Push({ PrimaryNote::Kind::HiddenKey, a_key });
			else
				primary.NoteHiddenKey(a_key);
			announceKey(a_key);
		}
		// A category node's event announces every entry under it (rare): its members.
		for (const void* key : categoryEvents)
			if (const auto members = categoryMembers.find(static_cast<const RE::NiNode*>(key)); members != categoryMembers.end())
				for (auto* geometry : members->second)
					if (const auto entry = tracked.find(geometry); entry != tracked.end() && entry->second.categoryNode == key)
						announce(entry->first, entry->second);
		for (auto* geometry : retake)
			if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
				entry->second.candidateFrame = 0;  // Reclassify, scheduled with the pending ones (DeltaWalk)
				pendingEvaluation.push_back(geometry);
			}
		stats.hiddenRetaken += retake.size();
		hiddenGating = hiddenEventsInstalled && sceneFrame > moveUngatedThrough;
		hiddenWitness = hiddenGating && Policy::HiddenWitness(passSerial);
	}
}
