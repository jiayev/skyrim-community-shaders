// The coordinator's placement work (scene lane): the move events drained into move keys, the light path's gate on them, the
// placement set (movers, moving and still roots with their dependents), and the placement plan published when that set changes.

#include "Internal.h"

namespace DCLF::Scene
{
	void Coordinator::DrainMoveEvents(bool a_full)
	{
		// A reference's animation pushes several in a row.
		const void* last = nullptr;
		// "This frame and the last": the keys turn over when the pass's frame moved on, so a burst's passes add to the frame's.
		if (std::exchange(movedKeysFrame, sceneFrame) != sceneFrame) {
			std::swap(movedKeys[0], movedKeys[1]);
			movedKeys[0].clear();
			movedKeysQueued = 0;
		}
		stats.moveEvents += moveEvents.Drain([&](const void* a_key) {
			if (a_key != std::exchange(last, a_key)) {
				movedFrame[a_key] = sceneFrame;
				movedKeys[0].push_back(a_key);
			}
		});
		// A node event names a node: the reference above it, or its category node when no node up to it has one.
		for (const auto& node : nodeChanged) {
			// The mirror's chain (the scene work reads no engine memory).
			const void* key = nullptr;
			for (const void* at = node.get(); at && !key; at = mirror.Parent(at))
				if (const auto* record = mirror.Node(at))
					key = record->userData;
			if (!key)
				key = FindCategoryNode(node.get(), nullptr);
			if (key) {
				movedFrame[key] = sceneFrame;
				movedKeys[0].push_back(key);
			}
		}
		if (a_full)
			moveUngatedThrough = sceneFrame + 1;
		// Pruned once 256 frames went by (by the frames the passes saw, which may skip the multiples of 256).
		if (sceneFrame - movedPruneFrame >= 256) {
			movedPruneFrame = sceneFrame;
			std::erase_if(movedFrame, [&](const auto& a_entry) { return sceneFrame - a_entry.second > 1; });
		}
		moveGating = MoveEventsLive() && sceneFrame > moveUngatedThrough;
	}

	const void* Coordinator::MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const
	{
		// The mirror's chain.
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::MoveKey)];
		auto keyOf = [&](auto&& a_parent, auto&& a_reference) -> const void* {
			const void* key = nullptr;
			for (const void* object = &a_geometry; object && object != a_categoryNode; object = a_parent(object)) {
				const void* reference = a_reference(object);
				if (!reference)
					continue;
				if (key && key != reference)
					return nullptr;
				key = reference;
			}
			return key;
		};
		const void* key = keyOf([&](const void* a_key) { return mirror.Parent(a_key); }, [&](const void* a_key) -> const void* {
			const auto* record = mirror.Node(a_key);
			return record ? record->userData : nullptr;
		});
		Diagnostics::Notify(Diagnostics::MoveKeyRead{ *this, a_geometry, a_categoryNode, key });
		return key;
	}

	std::pair<const void*, const void*> Coordinator::MoveKeysOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const
	{
		if (const void* key = MoveKeyOf(a_geometry, a_categoryNode))
			return { key, nullptr };
		// No one reference: the innermost and the outermost on the chain (an outer reference's update moves the inner's 3D, and the inner
		// may move on its own), or with none the category node (a cell's update pass keys its dynamic node's subtree by the node).
		const void* inner = nullptr;
		const void* outer = nullptr;
		for (const void* object = &a_geometry; object && object != a_categoryNode; object = mirror.Parent(object))
			if (const auto* record = mirror.Node(object); record && record->userData) {
				inner = inner ? inner : record->userData;
				outer = record->userData;
			}
		if (!inner)
			return { a_categoryNode, nullptr };
		return { inner, outer != inner ? outer : nullptr };
	}

	bool Coordinator::MoverDue(const Tracked& a_tracked, const RE::BSGeometry* a_geometry, std::uint32_t& a_why) const
	{
		// No light path: its record cannot be kept (it has none), so it is written in full every frame.
		if (!a_tracked.lightTraits) {
			a_why = 4;
			return true;
		}
		// A hidden event on its chain owes an actor's verdict a retake.
		if (a_tracked.hiddenEventOwed) {
			a_why = 2;
			return true;
		}
		// First-person chains are recaptured every frame (ActorRecordKept takes their verdict every frame).
		if ((a_tracked.lightTraits & kTraitActor) && FirstPersonEntry(a_tracked.categoryNode)) {
			a_why = 3;
			return true;
		}
		if (!a_tracked.moveKey) {
			a_why = 5;
			return true;
		}
		a_why = 1;
		return MovedRecently(a_geometry) || MovedRecently(a_tracked.moveKey) || (a_tracked.moveKeyOuter && MovedRecently(a_tracked.moveKeyOuter));
	}

	Coordinator::PlacementPlan& Coordinator::WalkPlan()
	{
		if (!placementPlan) {
			placementPlan = std::make_shared<PlacementPlan>();
			placementPlan->walk = walkSerial;
		}
		return *placementPlan;
	}

	Coordinator::PlacementPlan::Item Coordinator::PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		ResolveSunEntry(a_tracked, *a_geometry);
		PlacementPlan::Item item;
		// Copies of references the scene work holds: the entry's, and its listed root's.
		item.geometry = a_tracked.geometry;
		item.sunEntryNode = OwnedRoot(a_tracked.sunEntryNode);
		item.slot = a_tracked.slot;
		item.layerSlot = a_tracked.slot < tables.layerOf.size() ? tables.layerOf[a_tracked.slot] : kNoObjectSlot;
		item.firstPerson = FirstPersonEntry(a_tracked.categoryNode);
		// A skinned record's palette: its block, which the record addresses.
		if (item.slot < tables.boneRows.size() && (tables.objects[item.slot].flags & kObjectSkinned) && tables.boneRows[item.slot]) {
			item.boneOffset = tables.boneOffset[item.slot];
			item.boneRows = tables.boneRows[item.slot];
			// A particle member's block holds its quads (ParticleSnapshots), not a skin's palette (the mirror's record says which).
			const auto* record = mirror.Geometry(a_geometry);
			item.particles = record && record->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles) && !record->skin;
		}
		return item;
	}

	bool Coordinator::IsPlacedMover(const Tracked& a_tracked) const
	{
		// A recorded light-path entry that moves, is skinned or is an actor's (an actor's skeleton moves with no controller or body on its
		// chain: Havok's behaviour graph). FrameValues places every one each frame from the newest plan.
		return a_tracked.perFrame && (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin | kTraitActor)) && a_tracked.geometry &&
		       a_tracked.slot != kNoObjectSlot && a_tracked.objectStamp == objectStamp && a_tracked.slot < tables.objects.size();
	}

	bool Coordinator::NoteMover(RE::BSGeometry* a_geometry, const Tracked& a_tracked)
	{
		const bool changed = IsPlacedMover(a_tracked) ? moverSet.insert(a_geometry).second : moverSet.erase(a_geometry) != 0;
		placementDirty = placementDirty || changed;
		return changed;
	}

	void Coordinator::QueueRoots()
	{
		// A root RootMoves found still, whose reference had a move event: the cells' update passes recompute its bound
		// (FireSpitCooking's drifts by a tenth of a unit with nothing under it moving). Listed from its first event until its last
		// dependent goes; a moving root is listed already. Only the keys this frame added since the last pass.
		auto& keys = movedKeys[0];
		for (; movedKeysQueued < keys.size(); ++movedKeysQueued) {
			const auto it = referenceRoot.find(keys[movedKeysQueued]);
			if (it == referenceRoot.end() || movingRoots.contains(it->second) || !rootDependents.contains(it->second))
				continue;
			if (stillRoots.insert(it->second).second) {
				++placementStats.stillRoots;
				placementDirty = true;
			}
		}
	}

	void Coordinator::SyncPlacementSet()
	{
		// A planned slot's palette block or identity (kChangeSkin: a release's, a rewrite's, a moved block) may change what the plan's
		// rows name; any other column is not a plan's.
		auto& log = tables.changeLog;
		if (!placementCursor.Continues(log, tablesGeneration)) {
			placementCursor.Restart(tablesGeneration);
			placementDirty = true;
		} else if (!placementDirty) {
			for (const auto& change : placementCursor.Unread(log))
				if ((change.causes & kChangeSkin) && plannedSlots.contains(change.slot)) {
					placementDirty = true;
					break;
				}
		}
		placementCursor.Advance(log);
		if (tables.objects.size() != plannedObjectSlots || tables.BoneCapacity() != plannedBoneCapacity)
			placementDirty = true;
	}

	void Coordinator::PublishPlacementPlan()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		// A plan is the whole placement set (dclf A1): every recorded per-frame mover, every moving root and every still root listed, with
		// their dependents, plus the slots this walk wrote. FrameValues keeps the newest plan and samples its movers and roots every frame,
		// so motion alone needs no plan: one is made only when the walk wrote a slot or the set changed (a candidate is compared with the
		// last plan's keys). What the coordinator keeps is keys and slots alone; the references are copied from the entries (PlanItemOf)
		// and the root owners only when a plan is made, so the pump never holds one across passes.
		QueueRoots();
		SyncPlacementSet();
		const bool written = placementPlan && !placementPlan->written.empty();
		if (!written && !placementDirty) {
			placementPlan.reset();
			moverGateStats.perFrameWithoutPlan += perFrameWalked ? 1u : 0u;
			return;
		}
		placementDirty = false;
		// The candidate's keys first (no references taken), movers by geometry, then the roots by node.
		std::vector<PlannedKey> keys;
		std::vector<std::pair<RE::BSGeometry*, Tracked*>> movers;
		{
			std::vector<RE::BSGeometry*> members(moverSet.begin(), moverSet.end());
			std::sort(members.begin(), members.end());
			movers.reserve(members.size());
			for (auto* geometry : members) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || !IsPlacedMover(it->second)) {
					moverSet.erase(geometry);
					continue;
				}
				auto& entry = it->second;
				ResolveSunEntry(entry, *geometry);
				PlannedKey key{ geometry, entry.slot, entry.slot < tables.layerOf.size() ? tables.layerOf[entry.slot] : kNoObjectSlot, 0, 0, entry.sunEntryNode,
					FirstPersonEntry(entry.categoryNode), false };
				if (entry.slot < tables.boneRows.size() && (tables.objects[entry.slot].flags & kObjectSkinned) && tables.boneRows[entry.slot]) {
					key.boneOffset = tables.boneOffset[entry.slot];
					key.boneRows = tables.boneRows[entry.slot];
					const auto* record = mirror.Geometry(geometry);
					key.particles = record && record->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles) && !record->skin;
				}
				keys.push_back(key);
				movers.emplace_back(geometry, &entry);
			}
		}
		struct Dependent
		{
			RE::BSGeometry* geometry;
			Tracked* entry;
			std::uint32_t layerSlot;
		};
		std::vector<std::pair<const RE::NiAVObject*, std::vector<Dependent>>> roots;
		{
			std::vector<const RE::NiAVObject*> listed;
			listed.reserve(movingRoots.size() + stillRoots.size());
			for (const auto& [root, moving] : movingRoots)
				listed.push_back(root);
			for (const auto* root : stillRoots)
				if (!movingRoots.contains(root))
					listed.push_back(root);
			std::sort(listed.begin(), listed.end());
			for (const auto* root : listed) {
				const auto dependents = rootDependents.find(root);
				if (dependents == rootDependents.end() || !rootOwners.contains(root)) {
					stillRoots.erase(root);
					continue;
				}
				auto& [node, rows] = roots.emplace_back(root, std::vector<Dependent>{});
				keys.push_back({ root, kRootMarker });
				std::vector<RE::BSGeometry*> sorted(dependents->second.begin(), dependents->second.end());
				std::sort(sorted.begin(), sorted.end());
				for (auto* geometry : sorted) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.slot >= tables.objects.size())
						continue;
					const std::uint32_t layer = it->second.layerSlot != kNoObjectSlot && tables.IsLayer(it->second.layerSlot) ? it->second.layerSlot : kNoObjectSlot;
					rows.push_back({ geometry, &it->second, layer });
					keys.push_back({ geometry, it->second.slot, layer });
				}
			}
		}
		const auto objectSlots = static_cast<std::uint32_t>(tables.objects.size());
		const std::uint32_t boneCapacity = tables.BoneCapacity();
		const bool changed = keys != plannedKeys || objectSlots != plannedObjectSlots || boneCapacity != plannedBoneCapacity;
		if (!changed && !written) {
			placementPlan.reset();
			++moverGateStats.candidatesSame;
			moverGateStats.perFrameWithoutPlan += perFrameWalked ? 1u : 0u;
			return;
		}
		// The plan: the whole set with references, and the slots it names for the change log's check.
		auto& plan = WalkPlan();
		plan.movers.reserve(movers.size());
		for (const auto& [geometry, entry] : movers)
			plan.movers.push_back(PlanItemOf(geometry, *entry));
		plan.roots.reserve(roots.size());
		for (const auto& [root, rows] : roots) {
			auto& listed = plan.roots.emplace_back();
			listed.root = OwnedRoot(root);
			listed.dependents.reserve(rows.size());
			for (const auto& row : rows) {
				auto& dependent = listed.dependents.emplace_back();
				dependent.geometry = row.entry->geometry;
				dependent.slot = row.entry->slot;
				dependent.layerSlot = row.layerSlot;
			}
		}
		plannedSlots.clear();
		for (const auto& key : keys)
			if (key.slot != kRootMarker) {
				plannedSlots.insert(key.slot);
				if (key.layerSlot != kNoObjectSlot)
					plannedSlots.insert(key.layerSlot);
			}
		plannedKeys = std::move(keys);
		plannedObjectSlots = objectSlots;
		plannedBoneCapacity = boneCapacity;
		auto& g = moverGateStats;
		++g.plans;
		g.plansWritten += written ? 1u : 0u;
		g.plansChanged += changed ? 1u : 0u;
		placementStats.roots += plan.roots.size();
		plan.slots = objectSlots;
		plan.boneCapacity = boneCapacity;
		// A plan no publication took yet (a pass that threw before its publication) is kept for the next one, which carries every plan
		// since (TakeDeltas): the frame takes every plan, so none is replaced.
		if (placementPlanReady)
			placementPlansHeld.push_back(std::move(placementPlanReady));
		placementPlanReady = std::move(placementPlan);
	}
}
