// The coordinator's placement work (scene lane): the move events drained into move keys, the moving roots queued, and the placement
// plan (movers, roots and their dependents) published for the frame's placement passes.

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

	void Coordinator::QueueRoots()
	{
		rootPlacements.clear();
		for (const auto& [root, moving] : movingRoots) {
			const bool moved = !moveGating || !moving.key || MovedRecently(moving.key) || MovedRecently(moving.categoryNode);
			if (!moved) {
				++placementStats.rootsGated;
				continue;
			}
			rootPlacements.push_back(root);
		}
		// A root RootMoves found still, whose reference had a move event this frame or the last: the cells' update passes
		// recompute its bound (FireSpitCooking's drifts by a tenth of a unit with nothing under it moving).
		if (moveGating) {
			ankerl::unordered_dense::set<const RE::NiAVObject*> still;
			for (const auto& keys : movedKeys)
				for (const void* key : keys)
					if (const auto it = referenceRoot.find(key); it != referenceRoot.end() && !movingRoots.contains(it->second) && still.insert(it->second).second)
						rootPlacements.push_back(it->second);
			placementStats.stillRoots += still.size();
		}
		placementStats.roots += rootPlacements.size();
	}

	void Coordinator::PublishPlacementPlan()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		// A walk that did not take the per-frame entries (a pass of a frame that had its per-frame walk) lists no movers itself. With
		// nothing written either, it makes no plan (the frame keeps drawing with the last one's movers and roots: FrameValues); with slots
		// written, its plan carries the last per-frame walk's movers, those still recorded in the slots they were listed with (a slot freed
		// since may be another object's once its retirement returns). The roots likewise: they are per-frame work (their reference's move
		// events, the frame's), so only the per-frame walk queues them, and a later walk's plan carries its roots with their dependents.
		// What is carried is keys and slots alone (CarriedItem): the references are copied again here from the entries (PlanItemOf) and the
		// root owners, so the pump never holds the last one across passes.
		if (!perFrameWalked) {
			if (!placementPlan || placementPlan->written.empty()) {
				placementPlan.reset();
				return;
			}
			auto& plan = *placementPlan;
			// The entry of an item still recorded in the slot it was listed with (a slot freed since may be another object's).
			auto holder = [&](const CarriedItem& a_item) -> Tracked* {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_item.geometry));
				if (it == tracked.end() || !it->second.geometry || it->second.slot != a_item.slot || it->second.objectStamp != objectStamp ||
					a_item.slot >= tables.objects.size())
					return nullptr;
				return &it->second;
			};
			plan.movers.reserve(lastMovers.size());
			for (const auto& item : lastMovers)
				if (auto* entry = holder(item))
					plan.movers.push_back(PlanItemOf(entry->geometry.get(), *entry));
			plan.roots.reserve(lastRoots.size());
			for (const auto& root : lastRoots) {
				// Its owner while a dependents list names it (OwnRoot); gone, the root is no longer listed.
				const auto owner = rootOwners.find(root.root);
				if (owner == rootOwners.end() || !owner->second)
					continue;
				auto& listed = plan.roots.emplace_back();
				listed.root = owner->second;
				listed.dependents.reserve(root.dependents.size());
				for (const auto& carried : root.dependents) {
					auto* entry = holder(carried);
					if (!entry)
						continue;
					auto& dependent = listed.dependents.emplace_back();
					dependent.geometry = entry->geometry;
					dependent.slot = entry->slot;
					if (const std::uint32_t layer = entry->layerSlot; layer != kNoObjectSlot && tables.IsLayer(layer))
						dependent.layerSlot = layer;
				}
			}
		} else {
			QueueRoots();
			// The roots the plan lists, with their dependents' rows, and the plan ready for the next frame.
			auto& plan = WalkPlan();
			lastRoots.clear();
			for (const auto* root : rootPlacements) {
				const auto dependents = rootDependents.find(root);
				if (dependents == rootDependents.end())
					continue;
				auto owned = OwnedRoot(root);
				if (!owned)
					continue;
				auto& listed = plan.roots.emplace_back();
				listed.root = std::move(owned);
				auto& carried = lastRoots.emplace_back();
				carried.root = root;
				for (auto* geometry : dependents->second) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.slot >= tables.objects.size())
						continue;
					auto& dependent = listed.dependents.emplace_back();
					dependent.geometry = it->second.geometry;
					dependent.slot = it->second.slot;
					if (const std::uint32_t layer = it->second.layerSlot; layer != kNoObjectSlot && tables.IsLayer(layer))
						dependent.layerSlot = layer;
					carried.dependents.push_back({ geometry, it->second.slot });
				}
			}
			rootPlacements.clear();
			lastMovers.clear();
			lastMovers.reserve(plan.movers.size());
			for (const auto& item : plan.movers)
				lastMovers.push_back({ item.geometry.get(), item.slot });
		}
		auto& plan = *placementPlan;
		plan.slots = static_cast<std::uint32_t>(tables.objects.size());
		plan.boneCapacity = tables.BoneCapacity();
		// A plan no publication took yet (a pass that threw before its publication) is kept for the next one, which carries every plan
		// since (TakeDeltas): the frame takes every plan, so none is replaced.
		if (placementPlanReady)
			placementPlansHeld.push_back(std::move(placementPlanReady));
		placementPlanReady = std::move(placementPlan);
	}
}
