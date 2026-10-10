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
			// The mirror's chain (T6b1b).
			const void* key = nullptr;
			for (const void* at = node.get(); at && !key; at = MirrorParent(at))
				if (const auto* record = mirror.Node(at))
					key = record->userData;
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
		moveGating = MoveEventsLive() && frame > moveUngatedThrough;
	}

	const void* SceneStore::MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const
	{
		// The mirror's chain (T6b1b).
		++mirrorReads.reads[static_cast<std::size_t>(MirrorRead::MoveKey)];
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
		const void* key = keyOf([&](const void* a_key) { return MirrorParent(a_key); }, [&](const void* a_key) -> const void* {
			const auto* record = mirror.Node(a_key);
			return record ? record->userData : nullptr;
		});
		if (mirrorReadParity)
			if (const auto lease = LiveCheckLease(MirrorRead::MoveKey)) {
			const void* live = keyOf([](const void* a_key) -> const void* { return static_cast<const RE::NiAVObject*>(a_key)->parent; },
				[](const void* a_key) -> const void* { return static_cast<const RE::NiAVObject*>(a_key)->GetUserData(); });
			NoteMirrorRead(MirrorRead::MoveKey, live != key, [&] { return fmt::format("'{}' {}: mirror {}, live {}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "",
				static_cast<const void*>(&a_geometry), key, live); }, [this, geometry = static_cast<const void*>(&a_geometry), a_categoryNode, live] {
				const void* now = nullptr;
				for (const void* at : MirrorChain(geometry, a_categoryNode)) {
					const auto* record = mirror.Node(at);
					const void* reference = record ? record->userData : nullptr;
					if (!reference)
						continue;
					if (now && now != reference)
						return live == nullptr;
					now = reference;
				}
				return now == live;
			});
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
		if (p.fadeRootsUnmirrored)
			line += fmt::format("{}{} fade roots listed with no mirror record yet (their seeds hold the node's values)", line.empty() ? "[DCLF] placement plan: " : "; ", p.fadeRootsUnmirrored);
		p = {};
		return line;
	}
}
