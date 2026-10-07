#include "Internal.h"
#include "Features/DrawcallLimitFix/Common/FrameTrace.h"

#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"

namespace DCLF
{
	void SceneStore::DrainMoveEvents(bool a_full)
	{
		// A reference's animation pushes several in a row.
		const void* last = nullptr;
		std::swap(movedKeys[0], movedKeys[1]);
		movedKeys[0].clear();
		stats.moveEvents += moveEvents.Drain([&](const void* a_key) {
			if (a_key != std::exchange(last, a_key)) {
				movedFrame[a_key] = frame;
				movedKeys[0].push_back(a_key);
			}
		});
		// A node event names a node: the reference above it, or its category node when no node up to it has one.
		for (const auto& node : nodeChanged) {
			const void* key = nullptr;
			for (const RE::NiAVObject* object = node.get(); object && !key; object = object->parent)
				key = object->GetUserData();
			if (!key)
				key = FindCategoryNode(node.get(), nullptr);
			if (key) {
				movedFrame[key] = frame;
				movedKeys[0].push_back(key);
			}
		}
		if (a_full)
			moveUngatedThrough = frame + 1;
		if ((frame & 0xFF) == 0)
			std::erase_if(movedFrame, [&](const auto& a_entry) { return frame - a_entry.second > 1; });
		static const bool enabled = SwitchValue(Switch::MoveEvents) != "0";
		moveGating = enabled && MoveEventsLive() && frame > moveUngatedThrough;
		moveWitness = moveGating && (SwitchEnabled(Switch::WalkParity) || SwitchEnabled(Switch::PersistentParity)) && ParityDue(frame);
	}

	const void* SceneStore::MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode)
	{
		const void* key = nullptr;
		for (const RE::NiAVObject* object = &a_geometry; object && object != a_categoryNode; object = object->parent) {
			const void* reference = object->GetUserData();
			if (!reference)
				continue;
			if (key && key != reference)
				return nullptr;
			key = reference;
		}
		return key;
	}

	SceneStore::MoveReason SceneStore::MoveReasonOf(const Tracked& a_tracked) const
	{
		if (!moveGating)
			return kMoveUngated;
		// A tree's wind moves its bones with no event (BSTreeManager).
		if (!a_tracked.moveKey || (!a_tracked.actorOwned && (a_tracked.lightTraits & kTraitSkin)))
			return kMoveAlways;
		if (MovedRecently(a_tracked.moveKey))
			return a_tracked.actorOwned ? kMoveActor : kMoveReference;
		return MovedRecently(a_tracked.categoryNode) ? kMoveCategory : kMoveGated;
	}

	void SceneStore::QueuePlacement(RE::BSGeometry* a_geometry, Tracked& a_tracked, std::uint8_t a_take, MoveReason a_reason)
	{
		// The sun entry node is resolved here, once per entry, so the job only reads its bound.
		ResolveSunEntry(a_tracked, *a_geometry);
		placements.push_back({ a_geometry, &a_tracked, a_tracked.sunEntryNode, a_tracked.slot, a_take, a_reason });
		++stats.lightPlacedBy[a_reason];
		a_tracked.movedWalk = walkSerial;
		++delta.moved;
	}

	std::uint8_t SceneStore::TakePlacement(const Placement& a_item)
	{
		// What WriteObject writes from the geometry's placement and its skin's palette, the same way, into the kept
		// record's own fields only: the render thread may touch the record's other fields while this runs.
		std::uint8_t changed = 0;
		auto* geometry = a_item.geometry;
		const std::uint32_t slot = a_item.slot;
		if (a_item.take & kTakePalette) {
			auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			UpdateSkin(skin, geometry->world);
			const std::uint32_t rows = skin->numMatrices * 3;
			const auto* current = static_cast<const float*>(skin->boneMatrices);
			const auto* previous = static_cast<const float*>(skin->prevBoneMatrices);
			if (!current || !previous || rows != tables.boneRows[slot]) {
				changed |= kTakeDefect;
			} else {
				// The rows into the skin's own block, noted only when they differ.
				const std::size_t at = std::size_t(tables.boneOffset[slot]) * 4;
				const std::size_t bytes = std::size_t(rows) * 4 * sizeof(float);
				if (std::memcmp(&tables.bones[at], current, bytes) != 0 || std::memcmp(&tables.previousBones[at], previous, bytes) != 0) {
					std::memcpy(&tables.bones[at], current, bytes);
					std::memcpy(&tables.previousBones[at], previous, bytes);
					changed |= kTakePalette;
				}
			}
		}
		if (a_item.take & kTakePlacement) {
			float world[12];
			float previousWorld[12];
			StoreTransform(geometry->world, world);
			StoreTransform(DrawnPreviousWorld(*geometry), previousWorld);
			const float center[3]{ geometry->worldBound.center.x, geometry->worldBound.center.y, geometry->worldBound.center.z };
			const float radius = geometry->worldBound.radius;
			// SunEntryOf, for a resolved entry.
			std::array<float, 4> entry{ 0.0f, 0.0f, 0.0f, -1.0f };
			if (a_item.sunEntryNode) {
				const auto& bound = a_item.sunEntryNode->worldBound;
				entry = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
			}
			// The fade node moves with it (LodFadeNodeOf).
			const auto lodFade = LodFadeNodeOf(geometry->GetGeometryRuntimeData().shaderProperty.get());
			auto& object = tables.objects[slot];
			// Only a real change is one: a mover that stood still this frame changes nothing.
			if (std::memcmp(object.world, world, sizeof(world)) != 0 || std::memcmp(object.previousWorld, previousWorld, sizeof(previousWorld)) != 0 ||
				std::memcmp(object.boundCenter, center, sizeof(center)) != 0 || object.boundRadius != radius || tables.sunEntry[slot] != entry ||
				tables.lodFade[slot] != lodFade) {
				std::memcpy(object.world, world, sizeof(world));
				std::memcpy(object.previousWorld, previousWorld, sizeof(previousWorld));
				std::memcpy(object.boundCenter, center, sizeof(center));
				object.boundRadius = radius;
				tables.sunEntry[slot] = entry;
				tables.lodFade[slot] = lodFade;
				changed |= kTakePlacement;
				// Its layer (WriteLayer) has its placement; its fade node is its own property's.
				if (const std::uint32_t layer = slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot; layer != kNoObjectSlot) {
					auto& layerObject = tables.objects[layer];
					std::memcpy(layerObject.world, world, sizeof(world));
					std::memcpy(layerObject.previousWorld, previousWorld, sizeof(previousWorld));
					std::memcpy(layerObject.boundCenter, center, sizeof(center));
					layerObject.boundRadius = radius;
					tables.sunEntry[layer] = entry;
					tables.lodFade[layer] = LodFadeNodeOf(LayerPropertyOf(*geometry));
				}
			}
		}
		return changed;
	}

	std::uint8_t SceneStore::TakeRoot(const RootPlacement& a_item, bool a_noteAll, std::vector<std::uint32_t>& a_changed)
	{
		// SunEntryOf for every dependent that has a record: they all read this root's bound. So does a dependent's LOD fade
		// node when the root is its fade node (LodFadeNodeOf), which it usually is.
		const auto dependents = rootDependents.find(a_item.root);
		if (dependents == rootDependents.end())
			return 0;
		const auto& bound = a_item.root->worldBound;
		const std::array<float, 4> entry{ bound.center.x, bound.center.y, bound.center.z, bound.radius };
		std::uint8_t changed = 0;
		for (auto* geometry : dependents->second) {
			const auto it = tracked.find(geometry);
			if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.slot >= tables.sunEntry.size())
				continue;
			const std::uint32_t slot = it->second.slot;
			const auto lodFade = LodFadeNodeOf(geometry->GetGeometryRuntimeData().shaderProperty.get());
			if (tables.sunEntry[slot] != entry || tables.lodFade[slot] != lodFade || a_noteAll) {
				tables.sunEntry[slot] = entry;
				tables.lodFade[slot] = lodFade;
				a_changed.push_back(slot);
				changed = kTakePlacement;
				if (const std::uint32_t layer = it->second.layerSlot; layer != kNoObjectSlot && tables.IsLayer(layer)) {
					tables.sunEntry[layer] = entry;
					tables.lodFade[layer] = LodFadeNodeOf(LayerPropertyOf(*geometry));
					a_changed.push_back(layer);
				}
			}
		}
		return changed;
	}

	void SceneStore::RunPlacements(bool a_worker)
	{
		// The items, then the roots: a root's sun entries are taken after its dependents' own (TakePlacement writes them too).
		const auto items = static_cast<std::uint32_t>(placements.size());
		const auto roots = static_cast<std::uint32_t>(rootPlacements.size());
		// One item under a lease: a worker reads the engine's nodes only inside the window, and the join takes what it left.
		auto takeItem = [&](std::uint32_t a_item) {
			std::optional<EngineReadWindow::Lease> lease;
			if (a_worker && !lease.emplace())
				return false;
			placementChanges[a_item] = TakePlacement(placements[a_item]) | kTaken;
			return true;
		};
		auto takeRoot = [&](std::uint32_t a_root, std::vector<std::uint32_t>& a_changed) {
			std::optional<EngineReadWindow::Lease> lease;
			if (a_worker && !lease.emplace())
				return false;
			placementChanges[items + a_root] = TakeRoot(rootPlacements[a_root], false, a_changed) | kTaken;
			return true;
		};
		if (a_worker && inSceneTask) {
			auto& executor = SceneScheduler::Executor();
			executor.ParallelFor("CS.DCLF.Scene.Placements.Items", items, 32, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.Placements.Items");
				for (auto i = static_cast<std::uint32_t>(a_begin); i < a_end; ++i)
					if (!(placementChanges[i] & kTaken) && !takeItem(i))
						return;
			});
			std::vector<std::vector<std::uint32_t>> changed(roots);
			executor.ParallelFor("CS.DCLF.Scene.Placements.Roots", roots, 32, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.Placements.Roots");
				for (auto r = static_cast<std::uint32_t>(a_begin); r < a_end; ++r)
					if (!(placementChanges[items + r] & kTaken) && !takeRoot(r, changed[r]))
						return;
			});
			for (const auto& slots : changed)
				rootChangedSlots.insert(rootChangedSlots.end(), slots.begin(), slots.end());
			return;
		}
		for (std::uint32_t i = 0; i < items; ++i)
			if (!(placementChanges[i] & kTaken) && !takeItem(i))
				return;
		for (std::uint32_t r = 0; r < roots; ++r)
			if (!(placementChanges[items + r] & kTaken) && !takeRoot(r, rootChangedSlots))
				return;
	}

	bool SceneStore::PlacementsTaken() const
	{
		return std::all_of(placementChanges.begin(), placementChanges.end(), [](std::uint8_t a_changes) { return (a_changes & kTaken) != 0; });
	}

	void SceneStore::QueueRoots()
	{
		rootPlacements.clear();
		rootChangedSlots.clear();
		for (const auto& [root, moving] : movingRoots) {
			const bool moved = !moveGating || !moving.key || MovedRecently(moving.key) || MovedRecently(moving.categoryNode);
			if (!moved && !moveWitness) {
				++placementStats.rootsGated;
				continue;
			}
			rootPlacements.push_back({ root, !moved });
		}
		// A root RootMoves found still, whose reference had a move event this frame or the last: the cells' update passes
		// recompute its bound (FireSpitCooking's drifts by a tenth of a unit with nothing under it moving). TakeRoot writes
		// only what changed.
		if (moveGating) {
			ankerl::unordered_dense::set<const RE::NiAVObject*> still;
			for (const auto& keys : movedKeys)
				for (const void* key : keys)
					if (const auto it = referenceRoot.find(key); it != referenceRoot.end() && !movingRoots.contains(it->second) && still.insert(it->second).second)
						rootPlacements.push_back({ it->second, false });
			placementStats.stillRoots += still.size();
		}
	}

	void SceneStore::KickPlacements()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		{
			ZoneScopedN("CS.DCLF.Scene.Placements.Queue");
			// An entry a later round wrote in full (or released) took its placement and palette there.
			std::erase_if(placements, [&](const Placement& a_item) { return a_item.tracked->movedWalk != walkSerial || a_item.tracked->slot != a_item.slot; });
			QueueRoots();
		}
		placementChanges.assign(placements.size() + rootPlacements.size(), 0);
		if (placements.empty() && rootPlacements.empty())
			return;
		// The walk parity reads every record right after the walk, so its frames take them here.
		// The scene task takes them itself, from the coordinator across the preparation pool, under read leases. What a refused
		// lease left is taken and applied by the join.
		const bool inline_ = inSceneTask || !AsyncEnabled() || (SwitchEnabled(Switch::WalkParity) && ParityDue(frame));
		if (inline_) {
			RunPlacements(inSceneTask);
			if (PlacementsTaken())
				ApplyPlacements(false);
			return;
		}
		placementJob = std::static_pointer_cast<void>(std::make_shared<AsyncWorker::JobHandle>(
			AsyncWorker::Get().Submit("scene placement", [this](std::stop_token) { RunPlacements(true); })));
	}

	void SceneStore::JoinPlacements()
	{
		DCLF_FRAME_TRACE("JoinPlacements");  // TEMP frame trace
		if (!placementJob && placements.empty() && rootPlacements.empty())
			return;
		DCLF_SCENE_PART(PlacementJoin, "CS.DCLF.Scene.PlacementJoin");
		bool failed = false;
		if (auto job = std::static_pointer_cast<AsyncWorker::JobHandle>(std::exchange(placementJob, nullptr))) {
			auto& worker = AsyncWorker::Get();
			const auto result = worker.Wait(*job, AsyncWaitBudget());
			// Late: waited for when running, dropped when still queued; what it did not take is taken below.
			if (result != AsyncWorker::WaitResult::Done)
				worker.Cancel(*job);
			failed = result == AsyncWorker::WaitResult::Failed;
			placementStats.late += result == AsyncWorker::WaitResult::Late ? 1 : 0;
		}
		// What the job did not take (a refused lease, a late or failed job), taken here; an item the job threw in may be half
		// taken, so on a failure those are noted in full.
		std::vector<std::uint8_t> untaken(placementChanges.size(), 0);
		for (std::size_t i = 0; i < placementChanges.size(); ++i)
			untaken[i] = (placementChanges[i] & kTaken) ? 0 : 1;
		placementStats.inlineItems += std::count(untaken.begin(), untaken.end(), std::uint8_t(1));
		RunPlacements();
		if (failed) {
			for (std::size_t i = 0; i < placements.size(); ++i)
				if (untaken[i])
					placementChanges[i] |= kTakePlacement | kTakePalette;
			for (std::size_t i = placements.size(); i < placementChanges.size(); ++i)
				if (untaken[i])
					placementChanges[i] = TakeRoot(rootPlacements[i - placements.size()], true, rootChangedSlots) | kTaken;
		}
		ApplyPlacements(AsyncModeSetting() == AsyncMode::Probe);
	}

	std::string SceneStore::PlacementReport()
	{
		auto& p = placementStats;
		if (!p.items)
			return {};
		auto line = fmt::format("[DCLF] scene placement: {} items, {} taken inline ({} joins late), {} palette size defects", p.items, p.inlineItems, p.late, p.defects);
		std::string writers;
		for (std::size_t i = 0; i < moveWriterCalls.size(); ++i)
			if (const auto calls = moveWriterCalls[i].exchange(0, std::memory_order_relaxed))
				writers += fmt::format("{}{} {}", writers.empty() ? "" : ", ", kMoveWriterNames[i], calls);
		if (!writers.empty())
			line += fmt::format("; move writers' calls: {}", writers);
		if (p.roots || p.rootsGated)
			line += fmt::format("; roots: {} bounds taken ({} of still roots on a move event; {} sun entries changed), {} left for want of a move event", p.roots, p.stillRoots,
				p.rootSlotsChanged, p.rootsGated);
		if (p.witnessed)
			line += fmt::format("; move events: {} movers they skip taken on parity frames, {} changed{}{}", p.witnessed, p.missed,
				p.missed ? " <- MISSED; first: " : " <- OK", p.firstMissed);
		if (p.probes)
			line += fmt::format("; probe: {} joins, {} items moved between the job and the join{}{}", p.probes, p.probeMoved,
				p.probeMoved ? " <- MOVED; first: " : " <- OK", p.firstMoved);
		p = {};
		return line;
	}

	void SceneStore::ApplyPlacements(bool a_probe)
	{
		for (std::size_t i = 0; i < placements.size(); ++i) {
			const std::uint8_t changed = placementChanges[i];
			const std::uint32_t slot = placements[i].slot;
			// A mover no event named: anything taking it changed is an engine writer the move events miss.
			if (placements[i].take & kTakeWitness) {
				++placementStats.witnessed;
				const std::uint8_t moved = changed & (kTakePlacement | kTakePalette);
				if (moved && !placementStats.missed++) {
					const auto* reference = placements[i].geometry->GetUserData();
					const auto* name = placements[i].geometry->name.c_str();
					placementStats.firstMissed = fmt::format("'{}' ({}{}{}, reference {:08X})", name ? name : "?", moved & kTakePlacement ? "placement" : "",
						moved == (kTakePlacement | kTakePalette) ? " and " : "", moved & kTakePalette ? "palette" : "", reference ? reference->GetFormID() : 0u);
				}
			}
			if (changed & (kTakePlacement | kTakePalette))
				++stats.lightChangedBy[placements[i].reason];
			if (changed & kTakePlacement) {
				tables.NoteChange(slot, kChangePlacement);
				if (const std::uint32_t layer = slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot; layer != kNoObjectSlot)
					tables.NoteChange(layer, kChangePlacement);
				++stats.lightPlacedChanged;
			}
			if (changed & kTakePalette) {
				tables.NoteChange(slot, kChangePalette);
				++stats.lightSkinsChanged;
			}
			if (changed & kTakeDefect) {
				// The palette's size moved under a kept record: the next walk writes it in full.
				tables.NoteChange(slot, kChangePalette | kChangeSkin);
				pendingEvaluation.push_back(placements[i].geometry);
				if (!placementStats.defects++)
					logger::warn("[DCLF] scene placement: the palette of '{}' changed size under its kept record", placements[i].geometry->name.c_str() ? placements[i].geometry->name.c_str() : "?");
			}
		}
		for (std::size_t i = 0; i < rootPlacements.size(); ++i) {
			const bool changed = (placementChanges[placements.size() + i] & ~kTaken) != 0;
			if (rootPlacements[i].witness) {
				++placementStats.witnessed;
				if (changed && !placementStats.missed++) {
					const auto* reference = rootPlacements[i].root->GetUserData();
					const auto* name = rootPlacements[i].root->name.c_str();
					placementStats.firstMissed = fmt::format("the root '{}' (its bound, reference {:08X})", name ? name : "?", reference ? reference->GetFormID() : 0u);
				}
			}
		}
		for (const std::uint32_t slot : rootChangedSlots)
			tables.NoteChange(slot, kChangePlacement);
		placementStats.roots += rootPlacements.size();
		placementStats.rootSlotsChanged += rootChangedSlots.size();
		// CS_DCLF_ASYNC=probe: every item taken again now. Taking is idempotent, so anything it changes moved between the
		// job's read and this join: an engine writer inside the window the job assumes is quiet.
		if (a_probe) {
			++placementStats.probes;
			for (const auto& item : placements) {
				const std::uint8_t moved = TakePlacement(item) & (kTakePlacement | kTakePalette);
				if (!moved)
					continue;
				tables.NoteChange(item.slot, (moved & kTakePlacement ? kChangePlacement : 0u) | (moved & kTakePalette ? kChangePalette : 0u));
				if (const std::uint32_t layer = item.slot < tables.layerOf.size() ? tables.layerOf[item.slot] : kNoObjectSlot; layer != kNoObjectSlot && (moved & kTakePlacement))
					tables.NoteChange(layer, kChangePlacement);
				if (!placementStats.probeMoved++)
					placementStats.firstMoved = item.geometry->name.c_str() ? item.geometry->name.c_str() : "?";
			}
			rootChangedSlots.clear();
			for (const auto& root : rootPlacements) {
				if (!TakeRoot(root, false, rootChangedSlots))
					continue;
				for (const std::uint32_t slot : rootChangedSlots)
					tables.NoteChange(slot, kChangePlacement);
				rootChangedSlots.clear();
				if (!placementStats.probeMoved++)
					placementStats.firstMoved = root.root->name.c_str() ? root.root->name.c_str() : "?";
			}
		}
		placementStats.items += placements.size();
		placements.clear();
		rootPlacements.clear();
		rootChangedSlots.clear();
		placementChanges.clear();
	}
}
