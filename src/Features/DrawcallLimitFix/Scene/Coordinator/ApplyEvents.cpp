// The coordinator's event intake (scene lane): CollectEvents drains every queue into the pass's batch, ApplyEvents applies it (the
// mirror first, then attaches, detaches, structural, switch, hidden and LOD segment events), and the retired batch goes back through
// the retirement chain. Also the load screen's discard and the detach safety net.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"

namespace DCLF::Scene
{
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
		fadeSnapEvents.Drain([&](const void* a_node) { batch->fadeSnaps.push_back(a_node); });
		fadeAmountEvents.Drain([&](const void* a_node) { batch->fadeAmounts.push_back(a_node); });
		DrainPropertyEvents(batch->properties);
		DrainNodeEvents(batch->nodes);
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch->switches.push_back(std::move(a_event)); });
		lodSegmentEvents.Drain([&](const void* a_key) { batch->lodSegments.push_back(a_key); });
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
			materialsCaptured.push_back(material);
		});
		// Answers to the joins' requests (a served capture was pushed before its answer): the join may ask again.
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
				ListFadeRoot(slot);
				if (IsResidentSlot(slot))
					ListTree(slot);
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
		DrainMaterialCaptures();
		auto batch = std::move(a_batch);
		BatchUnwindGuard unwindGuard(*this, batch);
		const bool rescanned = rescanPending;
		if (rescanPending) {
			// With everything cleared, RefreshCategoryNodes walks every category node as new: the full rescan.
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
			// The rescan's adds re-tag gated entries.
			ResetGateTags();
		}

		ApplyMirrorEvents(*batch);

		// A detach forces the category refresh: it can take a category node the signature cannot see go until the cell does.
		{
			DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
			bool sawDetach = false;
			for (const auto* event = batch->head; event && !sawDetach; event = event->next)
				sawDetach = event->type == SceneTracker::EventType::Detached && !event->mirrorOnly;
			addSource = rescanned ? TrackSource::Rescan : TrackSource::AttachEvent;
			RefreshCategoryNodes(sawDetach || rescanned);
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

		{
			DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
			// A detach leaves the scene only if nothing reattached the geometry by the batch's end: the engine moves objects between
			// containers (a cell's dynamic and static nodes, as physics wakes and sleeps) by detach plus attach. Entry, slot, binding stay.
			std::vector<RE::BSGeometry*> detached;
			// Prepare the attaches' subtrees first (pure; the mirror already holds the whole batch, detaches apply after the loop), then
			// merge each at its event: event order matters only to the merges, which keep it. Serial mode prepares and merges per event.
			std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> attached;
			if (!categoryNodes.empty())
				for (const auto* event = batch->head; event; event = event->next)
					if (!event->mirrorOnly && event->type == SceneTracker::EventType::Attached)
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
			for (auto* event = batch->head; event; event = event->next) {
				// The render thread's captures are the mirror's alone.
				if (event->mirrorOnly)
					continue;
				// A sun candidate above it: its subtree changed, which its primary-cut plan reads (PlanOf).
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
				// Only a tracked geometry is known alive (its entry holds it), so safe to walk up from.
				if (tracked.contains(geometry) && FindCategoryNode(geometry, nullptr)) {
					++stats.detachMoves;
					continue;
				}
				EraseTracked(geometry);
			}
		}

		{
			// Parity only: check a slice for missed detaches, once a pass. The normal path trusts the detach events.
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (Policy::ValidateTrackedSlice() && std::exchange(validatedFrame, PassStamp()) != PassStamp())
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
		for (const void* node : batch->fadeSnaps)
			ReseedFadeRoot(node);
		// Actors' fadeAmount: the rows' input (the GPU's fade state stands).
		for (const void* node : batch->fadeAmounts)
			RefreshFadeAmount(node);
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
		ApplyLodSegmentEvents(batch->lodSegments);

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
		// Tracked geometries the captures and leaf updates named: hold their current properties from the pins.
		for (const auto* head : { batch->mirrorHead, batch->head })
			for (const auto* event = head; event; event = event->next) {
				const SceneCapture::Records* records = event->type == SceneTracker::EventType::Attached ? event->captured.get() :
				                                       event->type == SceneTracker::EventType::Updated ? event->update.leaf.get() :
				                                                                                         nullptr;
				if (records)
					for (const auto& geometry : records->geometries)
						if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(geometry.key))); it != tracked.end()) {
							HoldProperties(it->second);
							// Waited for its records: write it now.
							if (std::exchange(it->second.leafWaiting, false)) {
								it->second.candidateFrame = 0;
								pendingEvaluation.push_back(it->first);
								++leafStats.rescheduled;
							}
						}
			}
		// For the event-to-publication latency.
		if (batch->oldestNs && (!unpublishedNs || batch->oldestNs < unpublishedNs)) {
			unpublishedNs = batch->oldestNs;
			unpublishedFrame = batch->oldestFrame;
		}
		// Its events hold engine references (detached subtrees a published table may still name): retire them through the chain. An
		// empty batch is dropped here, or its return would wake the pump and every pass would keep the next going.
		if (batch->head || batch->mirrorHead || !batch->switches.empty())
			retirement.Open().events.push_back(std::move(batch));
		// No reference is made from the pins after the apply.
		batchPins.clear();
	}

	void Coordinator::SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event)
	{
		const auto* record = mirror.Geometry(&a_shape);
		std::vector<LodSegments::Range> next = record ? record->segments : std::vector<LodSegments::Range>{};
		auto& ranges = lodRanges[&a_shape];
		// Changed by an event: rewrite the record (its range slot chain) this frame.
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
		Diagnostics::Notify<Diagnostics::ScenePoint::LodSegmentsApplied>();
	}

	void Coordinator::ApplyMirrorEvents(const EventBatch& a_batch)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror");
		mirror.BeginBatch();
		// The batch's pins by key: the only source of references while it applies.
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
						// A mirror request's capture fills only what the mirror lacks.
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
						// Attached out of the world: its record is stale (the event holds the object, so its name is safe).
						mirror.Evict(event->node.get(), fmt::format("'{}' {}", event->node->name.c_str() ? event->node->name.c_str() : "",
															 const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI() ? const_cast<RE::NiAVObject*>(event->node.get())->GetRTTI()->name : "?"));
				} else if (event->type == SceneTracker::EventType::Detached) {
					// Before the records go: the gate goes on without a detached root.
					if (event->detachedRoot && !gateOfRoot.empty())
						NoteGateRootsDetached(event->detachedRoot, event->removedNodes);
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else if (event->type == SceneTracker::EventType::Gate) {
					// Ordered after its incoming blocks' attaches.
					ApplyGateEvent(event->gateToken, event->gateIncoming, event->gateOutgoing, event->stampNs, event->stampFrame);
				} else {
					const auto [type, key] = mirror.Update(event->update);
					Diagnostics::Notify(Diagnostics::MirrorUpdateApplied{ type, key, event->update.fields });
					// A hidden store, for DrainHiddenEvents (same batch as the mirror's bit).
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
			RefreshFadeRootSwitch(node);
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
		// An actor's entries are evaluated every frame, and its sun entry is never tested.
		if (record->IsActorCharacter())
			return;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::NodeEvent)];
		Diagnostics::Notify(Diagnostics::TrackedBelowRead{ *this, MirrorRead::NodeEvent, a_node });
		// Already placed every frame (written in full, or moved by the light path).
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
		// Keys only; the addresses are never read.
		auto& primary = PrimaryCull::Get();
		// A static with a chain event is reclassified; an actor's light-path entry retakes its frame verdict (ActorRecordKept).
		ankerl::unordered_dense::set<RE::BSGeometry*> retake;
		ankerl::unordered_dense::set<const void*> categoryEvents;
		auto announce = [&](RE::BSGeometry* a_geometry, Tracked& a_entry) {
			a_entry.hiddenEventOwed = true;
			if (!(a_entry.lightTraits & kTraitActor))
				retake.insert(a_geometry);
		};
		auto announceKey = [&](const void* a_key) {
			// Not the first-person root (toggled within every frame; no member chain reaches its bit).
			if (categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<void*>(a_key))) && a_key != firstPersonCategory)
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
		// A category node's event announces every entry under it (rare).
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
		hiddenGating = hiddenEventsInstalled && sceneFrame > moveUngatedThrough;
		hiddenWitness = hiddenGating && Policy::HiddenWitness(passSerial);
	}
}
