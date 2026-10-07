#include "Internal.h"

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

	SceneStore::PlacementPlan& SceneStore::WalkPlan()
	{
		if (!placementPlan) {
			placementPlan = std::make_shared<PlacementPlan>();
			placementPlan->walk = walkSerial;
		}
		return *placementPlan;
	}

	SceneStore::PlacementPlan::Item SceneStore::PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		ResolveSunEntry(a_tracked, *a_geometry);
		PlacementPlan::Item item;
		// Copies of references the scene work holds (step 6e F1): the entry's, and its listed root's.
		item.geometry = a_tracked.geometry;
		item.sunEntryNode = OwnedRoot(a_tracked.sunEntryNode);
		item.slot = a_tracked.slot;
		item.layerSlot = a_tracked.slot < tables.layerOf.size() ? tables.layerOf[a_tracked.slot] : kNoObjectSlot;
		// A skinned record's palette: its block, which the record addresses.
		if (item.slot < tables.boneRows.size() && (tables.objects[item.slot].flags & kObjectSkinned) && tables.boneRows[item.slot]) {
			item.boneOffset = tables.boneOffset[item.slot];
			item.boneRows = tables.boneRows[item.slot];
		}
		return item;
	}

	void SceneStore::QueueRoots()
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

	void SceneStore::PublishPlacementPlan()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		QueueRoots();
		// The roots the plan lists, with their dependents' rows, and the plan ready for the next frame.
		auto& plan = WalkPlan();
		for (const auto* root : rootPlacements) {
			const auto dependents = rootDependents.find(root);
			if (dependents == rootDependents.end())
				continue;
			auto owned = OwnedRoot(root);
			if (!owned)
				continue;
			auto& listed = plan.roots.emplace_back();
			listed.root = std::move(owned);
			for (auto* geometry : dependents->second) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.slot >= tables.objects.size())
					continue;
				auto& dependent = listed.dependents.emplace_back();
				dependent.geometry = it->second.geometry;
				dependent.slot = it->second.slot;
				if (const std::uint32_t layer = it->second.layerSlot; layer != kNoObjectSlot && tables.IsLayer(layer))
					dependent.layerSlot = layer;
			}
		}
		rootPlacements.clear();
		plan.slots = static_cast<std::uint32_t>(tables.objects.size());
		plan.boneCapacity = tables.BoneCapacity();
		placementPlanReady = std::move(placementPlan);
	}

	std::string SceneStore::PlacementReport()
	{
		auto& p = placementStats;
		std::string line;
		std::string writers;
		for (std::size_t i = 0; i < moveWriterCalls.size(); ++i)
			if (const auto calls = moveWriterCalls[i].exchange(0, std::memory_order_relaxed))
				writers += fmt::format("{}{} {}", writers.empty() ? "" : ", ", kMoveWriterNames[i], calls);
		if (p.roots || p.rootsGated)
			line = fmt::format("[DCLF] placement plan: roots: {} listed ({} of still roots on a move event), {} left for want of a move event", p.roots, p.stillRoots,
				p.rootsGated);
		if (!writers.empty())
			line += fmt::format("{}move writers' calls: {}", line.empty() ? "[DCLF] placement plan: " : "; ", writers);
		p = {};
		return line;
	}
}
