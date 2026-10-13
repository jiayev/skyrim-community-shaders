#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"

namespace DCLF::Scene
{
	std::shared_ptr<Coordinator::EventBatch> Coordinator::CollectEvents()
	{
		ZoneScopedN("CS.DCLF.Scene.CollectEvents");
		// T6b3d: the coordinator is the queues' one consumer. Each is drained up to where it stood at its drain (the tracker's stack by
		// one exchange, the rings by their head: EventQueue::Drain), all of them here at the pass's start, so a producer extending a queue
		// is the next pass's and a pass always ends. What the render thread posted for the pass was taken before (the category capture,
		// the mirror probe), so every event older than its capture is in this batch or an earlier one.
		auto& tracker = SceneTracker::Get();
		const bool loading = channels.loadingSeen.load(std::memory_order_acquire);
		Diagnostics::Notify(Diagnostics::EventsCollecting{ *this, loading });
		if (loading) {
			// The load screen's branch (NoteLoadingScreen's, T6b3a): the mirror's events carried to the first pass after the load, which
			// applies them before its own; the rest discarded for the tracking, the objects the events without values name kept for the
			// mirror's parity, and the references the discarded events hold (the node and switch events) released at Present.
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
			fadeSnapEvents.Drain(keep);
			fadeAmountEvents.Drain(keep);
			lodSegmentEvents.Drain(keep);
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
		// T6b3e: a throw hands the batch (its events' pins, nodes and switches) to the render thread, never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		if (loadingCarry) {
			batch->AppendMirror(std::exchange(loadingCarry->mirrorHead, nullptr));
			batch->mirrorNamed = std::move(loadingCarry->mirrorNamed);
			loadingCarry.reset();
		}
		// The drain alone: pointer moves into the batch, in push order. Everything that walks or evaluates is ApplyEvents'.
		auto* drained = tracker.Drain();
		batch->Append(drained);
		fadeSnapEvents.Drain([&](const void* a_node) { batch->fadeSnaps.push_back(a_node); });
		fadeAmountEvents.Drain([&](const void* a_node) { batch->fadeAmounts.push_back(a_node); });
		DrainPropertyEvents(batch->properties);
		DrainNodeEvents(batch->nodes);
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch->switches.push_back(std::move(a_event)); });
		lodSegmentEvents.Drain([&](const void* a_key) { batch->lodSegments.push_back(a_key); });
		// The oldest stamp (the latencies), whether a detach came (the next category capture is forced: a detach can take a category
		// node with it before the signature sees its cell go), and what the switches' catch-ups need: engine code that writes, the
		// render thread's at its frame's start, asked with references the batch's events hold now (copies, dropped there).
		std::shared_ptr<SwitchCatchUp> catchUp;
		// T6b3e: on a throw its references (copies) go to the render thread (EngineReleases), never dropped here.
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
		events += static_cast<std::uint32_t>(batch->fadeSnaps.size() + batch->fadeAmounts.size() + batch->properties.size() +
											 batch->nodes.size() + batch->switches.size() + batch->lodSegments.size());
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
		MaterialPort::captures.Drain([&](std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held) {
			++s.captured;
			const RE::BSShaderMaterial* material = a_held->reference;
			auto& entry = materialSnapshots[a_held->reference];
			if (entry.held && entry.held->sequence > a_held->sequence) {
				++s.stale;
				HandBackHeld(std::move(a_held));
				return;
			}
			if (entry.held) {
				++s.replaced;
				HandBackHeld(std::move(entry.held));
			}
			entry.held = std::move(a_held);
			entry.frame = sceneFrame;
			// T6b2c step 7: its slots' records are evaluated again from it (RefreshMaterialRecords).
			materialsCaptured.push_back(material);
		});
		// The render thread's answers to the joins' requests (ServeMaterialRequests): the join may ask for the material again. A served
		// request's capture was pushed before its answer.
		channels.materialRequestsAnswered.Drain([&](MaterialAnswer&& a_answer) {
			materialRequested.erase(a_answer.material);
			++(a_answer.captured ? residentStats.materialsServed : residentStats.materialsStale);
		});
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
		// T6b3d: the load markers posted since (NoteLoadingScreen: once a load) drop what the load invalidated first (ApplyLoading), then
		// the pass's own batch (CollectEvents'; none while a load screen is up: an empty one, which still takes the category capture and
		// the rescan). The markers hold nothing.
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
			// Taken only while the mirror still names that node for the entry's property (its fade root and tree are listed from it).
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
			// Its slots' fade root, and a member tree's node, listed now (the walk left them unlisted for want of the reference).
			for (const std::uint32_t slot : { entry->slot, entry->layerSlot }) {
				if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != geometry)
					continue;
				ListFadeRoot(slot);
				if (IsResidentSlot(slot))
					ListTree(slot);
			}
		});
	}

	void Coordinator::ApplyLoading()
	{
		// NoteLoadingScreen's marker, the coordinator's half (T6b3a): the first frame after the load rescans; what was drained and not
		// applied, and the queues only the coordinator drains, are discarded.
		rescanPending = true;
		{
			std::vector<const void*> keys;
			DrainLodFadeEvents(keys);
			DrainEmittanceEvents(keys);
		}
		propertyChanged.clear();
		// The references these hold are the render thread's to drop (a node's last release runs the engine's destructors: its
		// collision object leaves the Havok world), so they go back through the retirement chain, never cleared here.
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
		// T6b2a: the material captures first: a batch's attach names materials its leaves' captures pushed before it.
		DrainMaterialCaptures();
		auto batch = std::move(a_batch);
		// T6b3e: a throw hands it to the render thread (its pins and references), never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// RefreshCategoryNodes treats every category node as newly appeared and walks it, which is
			// exactly the full rescan wanted here.
			rescanPending = false;
			for (auto& [geometry, entry] : tracked) {
				ReleaseObjectSlot(entry);
				HandBack(std::move(entry.geometry));
				HandBack(std::move(entry.property));
				HandBack(std::move(entry.layerProperty));
				HandBack(std::move(entry.faceHeadRef));
				HandBack(std::move(entry.fadeNodeRef));
			}
			tracked.clear();
			ClearFaceShapes();
			++trackedLayout;
			sceneIdentity.Reset();
			categoryNodes.clear();
			nodeSetsDirty = true;
			for (auto& [root, entry] : alwaysRenderRoots)
				HandBack(std::move(entry.root));
			alwaysRenderRoots.clear();
			validationCursor = 0;
			fullEvaluation = true;
			propertyDependents.clear();
			rootDependents.clear();
			lightDependents.clear();
			lightEntryChanges.push_back({ nullptr, 0 });
			ReleaseRootOwners();
			rootReference.clear();
			referenceRoot.clear();
			dirtyRoots.clear();
			rootMotion.clear();
			movingRoots.clear();
			hiddenDependents.clear();
			hiddenAhead.clear();
			DropSunCandidates();
			buckets = {};
			// T6b5: the gates' tags were the entries'; the rescan's adds tag them again (AddGeometry, by the gates' roots).
			ResetGateTags();
		}

		ApplyMirrorEvents(*batch);

		// The detaches are seen before the category refresh, so one this frame can force it: a detach can take a
		// category node with it, and the signature cannot see that until the cell itself goes.
		{
			DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
			bool sawDetach = false;
			for (const auto* event = batch->head; event && !sawDetach; event = event->next)
				sawDetach = event->type == SceneTracker::EventType::Detached && !event->mirrorOnly;
			addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
			RefreshCategoryNodes(sawDetach || rescanned);
			addSource = TrackSource::AttachEvent;
			// The subtrees the mirror had no chain for last time: the render thread captured them at a frame's start (T6b1b; T6b3d: onto
			// the tracker's stack, so a pass after it applied them).
			// T6b3e: their references handed back first (the retry list may be a root's last holder: never released on the pump, a throw
			// included); the retirement chain holds them through the pass, AddSubtree walking the keys.
			auto retry = std::exchange(pendingSubtrees, {});
			std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> retryRoots;
			retryRoots.reserve(retry.size());
			for (const auto& root : retry)
				retryRoots.emplace_back(root.get(), SubtreeSource::Retry);
			HandBack(retry);
			AddSubtrees(retryRoots);
			// T6b3e: the fade nodes the render thread pinned for entries that had none (FadeNodeRequest).
			ApplyFadeNodeAnswers();
		}

		{
			DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
			// A detach leaves the scene only if nothing attached the geometry again by the end of the batch: the engine moves
			// objects between containers (a cell's dynamic and static nodes, as their physics wakes and sleeps) with a detach and
			// an attach, which is a move. Its entry, slot and binding stay; the attach has evaluated it again (AddGeometry).
			std::vector<RE::BSGeometry*> detached;
			// T6b3e e5: the attaches' subtrees prepared first (PrepareSubtrees: pure over the batch's mirror, on the pool from
			// kSubtreeFanoutMin), each merged below at its event, in event order. Every attach of the batch read the mirror as the whole
			// batch left it already (ApplyMirrorEvents ran first; the detaches are applied after the loop), so an attach a later detach or
			// attach of the same subtree follows prepares what its serial walk found: event order is the merges' alone (the tracked set's
			// insertion order, the identities, the dependents lists), and they keep it. Under CS_DCLF_FANOUT=0 (and the mirror-read
			// parity) each is prepared and merged at its event (AddSubtree, the reference).
			std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> attached;
			if (!categoryNodes.empty())
				for (const auto* event = batch->head; event; event = event->next)
					if (!event->mirrorOnly && event->type == SceneTracker::EventType::Attached)
						attached.emplace_back(event->node.get(), SubtreeSource::AttachEvent);
			const std::uint32_t subtreeMode = attached.empty() ? 0 : FanoutMode(attached.size(), false, kSubtreeFanoutMin);
			if (!attached.empty()) {
				++fanoutStats.subtreeRounds;
				fanoutStats.subtreeRoots += attached.size();
			}
			[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
			if (subtreeMode != 0)
				PrepareSubtrees(attached, subtreeMode, subtreePreps);
			std::size_t nextPrep = 0;
			double mergeMs = 0.0;
			for (auto* event = batch->head; event; event = event->next) {
				// The render thread's captures are the mirror's alone (T6b3d: SceneTracker::PushCaptured).
				if (event->mirrorOnly)
					continue;
				// A sun candidate above it: its subtree changed, which its plan in the primary cut reads (PlanOf), whether or not a
				// tracked geometry came or went.
				for (const void* ancestor : event->ancestors)
					if (const auto it = sunTable.index.find(static_cast<const RE::NiAVObject*>(ancestor)); it != sunTable.index.end()) {
						sunEntriesForced.insert(it->first);
						sunEntriesDirty.push_back(it->first);
					}
				if (event->type == SceneTracker::EventType::Attached) {
					++stats.attachedEvents;
					if (!categoryNodes.empty()) {
						if (subtreeMode == 0) {
							AddSubtree(event->node.get());
						} else {
							const auto mergeStart = std::chrono::steady_clock::now();
							MergeSubtree(subtreePreps[nextPrep++]);
							mergeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mergeStart).count();
						}
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
			for (auto* geometry : detached) {
				// Only a tracked geometry is known to be alive (its entry holds it) and so safe to walk up from.
				if (tracked.contains(geometry) && FindCategoryNode(geometry, nullptr)) {
					++stats.detachMoves;
					continue;
				}
				EraseTracked(geometry);
			}
		}

		{
			// A slice of the tracked geometries checked for a detach the events missed (each one found is dropped, and flagged): the
			// parities' (a Policy question); the normal path trusts the detach events. Once a pass (one batch a pass, one consumer).
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (Policy::ValidateTrackedSlice() && std::exchange(validatedFrame, PassStamp()) != PassStamp())
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
		// Fade roots placed since their listing: their rows again from the node.
		for (const void* node : batch->fadeSnaps)
			ReseedFadeRoot(node);
		// Actors' fadeAmount, written since: their rows' input (the GPU's state stands).
		for (const void* node : batch->fadeAmounts)
			RefreshFadeAmount(node);
		// The structural events (SceneEvents): properties whose flags, material or controllers changed, and nodes Havok
		// moved or gave a controller.
		propertyChanged.insert(propertyChanged.end(), batch->properties.begin(), batch->properties.end());
		for (auto& node : batch->nodes)
			nodeChanged.push_back(std::move(node));
		batch->nodes.clear();
		// The switch events, oldest first, one pending entry per switch: its index before the oldest event decides
		// whether the selection changed (ApplySwitchEvents).
		for (auto& event : batch->switches) {
			++delta.switchEvents;
			const auto [at, inserted] = switchPendingIndex.try_emplace(event.node.get(), static_cast<std::uint32_t>(switchPending.size()));
			if (inserted)
				switchPending.push_back({ std::move(event.node), event.before, event.structural });
			else
				switchPending[at->second].structural |= event.structural;
		}
		ApplyLodSegmentEvents(batch->lodSegments);

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
		// The tracked geometries its captures and leaf updates named: their properties as the mirror has them now, held from its pins
		// (T6b1b).
		for (const auto* head : { batch->mirrorHead, batch->head })
			for (const auto* event = head; event; event = event->next) {
				const SceneCapture::Records* records = event->type == SceneTracker::EventType::Attached ? event->captured.get() :
				                                       event->type == SceneTracker::EventType::Updated ? event->update.leaf.get() :
				                                                                                         nullptr;
				if (records)
					for (const auto& geometry : records->geometries)
						if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(geometry.key))); it != tracked.end()) {
							HoldProperties(it->second);
							// Written without its records (none then): again, now the mirror has them.
							if (std::exchange(it->second.leafWaiting, false)) {
								it->second.candidateFrame = 0;
								pendingEvaluation.push_back(it->first);
								++leafStats.rescheduled;
							}
						}
			}
		// T6b3d: its oldest event, applied now and published with the next publication (the event-to-publication latency).
		if (batch->oldestNs && (!unpublishedNs || batch->oldestNs < unpublishedNs)) {
			unpublishedNs = batch->oldestNs;
			unpublishedFrame = batch->oldestFrame;
		}
		// Its tracker events (and the switch events folded into a pending entry) hold engine references - the detached subtrees a
		// published version of the tables may still name: through the retirement chain (step 6e E3). T6b3d: a batch that holds none (no
		// tracker event, no switch event left in it: an idle pass's) is dropped here instead - its return would wake the pump for nothing,
		// and every pass would keep the next one going.
		if (batch->head || batch->mirrorHead || !batch->switches.empty())
			retirement.Open().events.push_back(std::move(batch));
		// Its pins name the retired batch's references: no reference is made from them after the apply (T6b1c).
		batchPins.clear();
	}

	void Coordinator::SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event)
	{
		// The record's (T6b1b: its segment events carry them, LodSegments::DrawnRanges at the write).
		const auto* record = mirror.Geometry(&a_shape);
		std::vector<LodSegments::Range> next = record ? record->segments : std::vector<LodSegments::Range>{};
		auto& ranges = lodRanges[&a_shape];
		// Changed by an event: its record again this frame (its range slots and their chain, WriteObject).
		if (a_event && next != ranges) {
			++lodSegmentStats.changed;
			pendingEvaluation.push_back(const_cast<RE::BSGeometry*>(&a_shape));
		}
		ranges = std::move(next);
	}

	void Coordinator::ApplyLodSegmentEvents(const std::vector<const void*>& a_shapes)
	{
		for (const void* key : a_shapes) {
			++lodSegmentStats.events;
			// Only a tracked shape is known to be alive (its entry holds it).
			if (const auto it = lodRanges.find(static_cast<const RE::BSGeometry*>(key)); it != lodRanges.end())
				SampleLodRanges(*it->first, true);
		}
		Diagnostics::Notify(Diagnostics::LodSegmentsApplied{ *this });
	}

	void Coordinator::ApplyMirrorEvents(const EventBatch& a_batch)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror");
		mirror.BeginBatch();
		// The batch's pins (T6b1c): its attach captures' references and its leaf updates' (T6b1b), by key, for the scene work's
		// references while it applies it.
		batchPins.clear();
		for (const auto* head : { a_batch.mirrorHead, a_batch.head })
			for (const auto* event = head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached && event->captured)
					PinBatch(event->captured->pins);
				PinBatch(event->pins);
			}
		auto apply = [&](const SceneTracker::Event* a_head) {
			for (const auto* event = a_head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached) {
					if (event->captured && event->mirrorFill) {
						// T6b3d: a mirror request's capture fills what the mirror lacks alone (SceneTracker::Event::mirrorFill).
						const auto& captured = *event->captured;
						SceneCapture::Records fill;
						fill.sequence = captured.sequence;
						for (const auto& n : captured.nodes)
							if (!mirror.Node(n.key))
								fill.nodes.push_back(n);
						for (const auto& g : captured.geometries)
							if (!mirror.Geometry(g.key))
								fill.geometries.push_back(g);
						for (const auto& p : captured.properties)
							if (!mirror.Property(p.key))
								fill.properties.push_back(p);
						for (const auto& a : captured.alphas)
							if (!mirror.Alpha(a.key))
								fill.alphas.push_back(a);
						if (!fill.nodes.empty() || !fill.geometries.empty() || !fill.properties.empty() || !fill.alphas.empty())
							mirror.Apply(fill);
					} else if (event->captured)
						mirror.Apply(*event->captured);
					else if (event->node && mirror.Node(event->node.get()))
						// Attached out of the world, yet held (T6b1b): its record is stale (the event holds the object: its name is safe).
						mirror.Evict(event->node.get(), fmt::format("'{}' {}", event->node->name.c_str() ? event->node->name.c_str() : "",
															 const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI() ? const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI()->name : "?"));
				} else if (event->type == SceneTracker::EventType::Detached) {
					// T6b5: a gate's root among the detached (the gate goes on without it), before its records go.
					if (event->detachedRoot && !gateOfRoot.empty())
						NoteGateRootsDetached(event->detachedRoot, event->removedNodes);
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else if (event->type == SceneTracker::EventType::Gate) {
					// T6b5: in order after its incoming blocks' attaches (their captures applied above): the gate opened.
					ApplyGateEvent(event->gateToken, event->gateIncoming, event->gateOutgoing, event->stampNs, event->stampFrame);
				} else {
					const auto [type, key] = mirror.Update(event->update);
					Diagnostics::Notify(Diagnostics::MirrorUpdateApplied{ type, key, event->update.fields });
					// A hidden store's event (T6b1b): DrainHiddenEvents', the same batch's as the mirror's bit.
					if (event->update.hiddenSite != ~0u)
						if (const auto* record = std::get_if<SceneCapture::NodeRecord>(&event->update.record))
							batchHidden.push_back({ record->key, event->update.hiddenSite, (record->flags & 1u) != 0, event->update.sequence });
				}
			}
		};
		apply(a_batch.mirrorHead);
		apply(a_batch.head);
		RecheckDeferredReads();
		Diagnostics::Notify(Diagnostics::MirrorEventsApplied{ *this, a_batch.lodSegments, a_batch.mirrorNamed });
	}

	void Coordinator::ApplySwitchEvents(bool a_full)
	{
		if (a_full)
			switchResync = true;
		for (auto& pending : switchPending) {
			auto* node = pending.node.get();
			// The mirror's (T6b1b: the switch's stores carry its index).
			const auto* record = node ? mirror.Node(node) : nullptr;
			// Only the scene the tables cover: a switch still loading is brought up to date by its attach (AddSubtree).
			if (!record || !(record->kind & SceneCapture::kKindSwitch) || !FindCategoryNode(node, nullptr))
				continue;
			if (!pending.structural && record->switchIndex == pending.before)
				continue;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SwitchEvent)];
			if (!a_full)
				Diagnostics::Notify(Diagnostics::TrackedBelowRead{ *this, MirrorRead::SwitchEvent, node });
			++delta.switchChanges;
			switchesApplied.push_back(node);
			RefreshFadeRootSwitch(node);
			if (a_full)
				continue;
			// Every entry under it: which of them the switch draws is a classification input (ClassifyFrame).
			ForEachMirror(mirror, node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				return true;
			}, kMaxEventNodes);
		}
		// The walk runs on the coordinator: the switches' references are the render thread's to drop.
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
	}

	void Coordinator::ApplyNodeEvent(RE::NiAVObject* a_node)
	{
		// Only the scene the tables cover: a node outside it (a subtree still loading, the sky) is left alone, and not
		// walked, as AddSubtree does.
		if (!a_node || !FindCategoryNode(a_node, nullptr))
			return;
		// The mirror's (T6b1b): FindCategoryNode found its record.
		const auto* record = mirror.Node(a_node);
		// An actor's entries are evaluated every frame anyway, and its sun entry is never tested.
		if (record->IsActorCharacter())
			return;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::NodeEvent)];
		Diagnostics::Notify(Diagnostics::TrackedBelowRead{ *this, MirrorRead::NodeEvent, a_node });
		// Written every frame already, placement included: a record written in full, or one the light path moves.
		auto placedEveryFrame = [](const Tracked& a_tracked) {
			return a_tracked.perFrame && (!a_tracked.lightTraits || (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves)));
		};
		// Every sun entry node above it: its bound takes this node in, and its motion may have changed. A root already
		// known to move has its bound taken by the root pass (QueueRoots) whenever its reference has a move event, which
		// this one is (DrainMoveEvents).
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
		// Every entry below it: its placement, and its traits (a body or controller it did not have when classified).
		ForEachMirror(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)));
					entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			return true;
		}, kMaxEventNodes);
	}

	void Coordinator::ValidateSlice()
	{
		// Safety net for missed detaches: a tracked geometry must still hang under the category node it
		// was found under. Checks a slice per frame so the cost stays flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		// The entries are the mirror's (the scene as the applied batch left it), the check live: a geometry detached since the batch's
		// ingestion differs until the next batch's detach erases it. A suspect still tracked and still differing a batch later is a
		// detach the events missed.
		for (auto* geometry : std::exchange(validationSuspects, {}))
			if (const auto it = tracked.find(geometry); it != tracked.end() && FindCategoryNodeLive(geometry, nullptr) != it->second.categoryNode)
				stale.push_back(geometry);
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// The map's storage is a dense vector, so the cursor walks it without hashing.
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
		// The scene graph above a tracked geometry does not change while it is tracked: listed once (a full walk lists anew).
		if (!a_tracked.hiddenChain.empty())
			return;
		// ClassifyFrame's walk: the leaf up to its category node, the category node left out (thousands of entries share it: its
		// events are taken by categoryNode, DrainHiddenEvents).
		// The mirror's chain (T6b1b).
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::HiddenChain)];
		for (const void* key = a_geometry; key && key != a_tracked.categoryNode; key = mirror.Parent(key)) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			a_tracked.hiddenChain.push_back(object);
			hiddenDependents[object].push_back(a_geometry);
		}
		Diagnostics::Notify(Diagnostics::HiddenChainRead{ *this, a_geometry, a_tracked.categoryNode, a_tracked.hiddenChain });
	}

	void Coordinator::UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		for (const auto* object : a_tracked.hiddenChain)
			Unlist(hiddenDependents, object, a_geometry);
		a_tracked.hiddenChain.clear();
	}

	void Coordinator::DrainHiddenEvents()
	{
		// A node's key only: an entry listing it is announced, and the address is never read.
		auto& primary = PrimaryCull::Get();
		// T6b4: a static whose chain had an event is classified again (its hidden verdict, and a record or none with it); an actor's
		// light-path entry takes its frame verdict again (ActorRecordKept). Once per entry, after the drain.
		ankerl::unordered_dense::set<RE::BSGeometry*> retake;
		ankerl::unordered_dense::set<const void*> categoryEvents;
		auto announce = [&](RE::BSGeometry* a_geometry, Tracked& a_entry) {
			a_entry.hiddenEventOwed = true;
			if (!(a_entry.lightTraits & kTraitActor))
				retake.insert(a_geometry);
		};
		auto announceKey = [&](const void* a_key) {
			// T7e: not the first-person root's (shown and hidden within every frame): no chain of its members reaches its bit.
			if (categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<void*>(a_key))) && a_key != firstPersonCategory)
				categoryEvents.insert(a_key);
			const auto dependents = hiddenDependents.find(a_key);
			if (dependents == hiddenDependents.end())
				return;
			for (auto* geometry : dependents->second)
				if (const auto entry = tracked.find(geometry); entry != tracked.end())
					announce(entry->first, entry->second);
		};
		// The nodes the frame's capture lists (FrameGlobals::cullHidden) are judged by the capture's bit, not the mirror's (HiddenForWalk).
		// An event on one of them numbered after the capture may be newer than the bit it holds: a pass a hidden store wakes outside the
		// frame's render still holds the frame before's capture (the switch to first person hides the third-person skeleton in
		// FirstPersonState::Begin, FUN_140736150, from Main::Update: judged by the frame before's "shown", the skeleton kept its records
		// and DCLF drew it in first person). Such a key is announced again at the first pass whose capture is numbered after its event;
		// one whose event left the bit the capture holds is judged right already.
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
		// The applied batches' hidden stores (their mirror updates: ApplyMirrorEvents), in order.
		stats.hiddenEvents += batchHidden.size();
		for (const auto& a_event : std::exchange(batchHidden, {})) {
			const void* a_key = a_event.key;
			if (aheadOfCapture(a_event)) {
				auto& sequence = hiddenAhead[a_key];
				sequence = std::max(sequence, a_event.sequence);
			}
			Diagnostics::Notify(Diagnostics::HiddenStoreApplied{ *this, a_key, a_event.site, a_event.hidden });
			// A cell's or a category node's: the kept scene lists are built again (held while the scene list job may run).
			if (holdPrimaryNotes)
				channels.primaryNotes.Push({ PrimaryNote::Kind::HiddenKey, a_key });
			else
				primary.NoteHiddenKey(a_key);
			announceKey(a_key);
		}
		// A category node's: every entry under it (rare: a cell's nodes).
		if (!categoryEvents.empty())
			for (auto& [geometry, entry] : tracked)
				if (categoryEvents.contains(entry.categoryNode))
					announce(geometry, entry);
		for (auto* geometry : retake)
			if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
				entry->second.candidateFrame = 0;  // Reclassify, scheduled with the pending ones (DeltaWalk)
				pendingEvaluation.push_back(geometry);
			}
		stats.hiddenRetaken += retake.size();
		Diagnostics::Notify(Diagnostics::HiddenEventsDrained{ *this });
		hiddenGating = hiddenEventsInstalled && sceneFrame > moveUngatedThrough;
		hiddenWitness = hiddenGating && Policy::HiddenWitness(passSerial);
	}
}
