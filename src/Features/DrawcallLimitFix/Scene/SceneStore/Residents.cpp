#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"

namespace DCLF
{
	bool SceneStore::ResidentParityEnabled()
	{
		return SwitchEnabled(Switch::ResidentParity);
	}

	void SceneStore::MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch)
	{
		residentMaintenanceDirty = true;
		if (residentPos.size() <= a_slot)
			residentPos.resize(std::max<std::size_t>(a_slot + 1, tables.objects.size()), kNotResident);
		// The accumulate phase notes the join, and a patch that changed, with the rest of its write.
		ListTree(a_slot);
		ListFadeRoot(a_slot);
		if (residentPos[a_slot] != kNotResident) {
			residentPatches[residentPos[a_slot]] = a_patch;
			return;
		}
		residentPos[a_slot] = static_cast<std::uint32_t>(residents.size());
		residents.push_back(a_slot);
		residentPatches.push_back(a_patch);
		if (a_slot < tables.residentSlot.size())
			tables.residentSlot[a_slot] = 1;
	}

	void SceneStore::DropResidentSlot(std::uint32_t a_slot, bool a_restore)
	{
		if (!IsResidentSlot(a_slot))
			return;
		residentMaintenanceDirty = true;
		const std::uint32_t at = residentPos[a_slot];
		const std::uint32_t last = residents.back();
		residents[at] = last;
		residentPatches[at] = residentPatches.back();
		residentPos[last] = at;
		residents.pop_back();
		residentPatches.pop_back();
		residentPos[a_slot] = kNotResident;
		UnlistTree(a_slot);
		UnlistFadeRoot(a_slot);
		if (a_slot < tables.objectGeometry.size() && tables.objectGeometry[a_slot])
			PrimaryCull::Get().NoteMemberLost(tables.objectGeometry[a_slot]);
		if (a_slot < tables.residentSlot.size()) {
			tables.residentSlot[a_slot] = 0;
			tables.NoteChange(a_slot, kChangeMembership);
		}
		if (memberDecals.erase(a_slot))
			memberDecalsChanged = true;
		if (a_restore)
			ResetAccumulatedHalf(a_slot);
	}

	void SceneStore::EndAllResidency()
	{
		residentMaintenanceDirty = true;
		PrimaryCull::Get().NoteAllMembersLost();
		memberDecals.clear();
		memberDecalsChanged = true;
		for (const std::uint32_t slot : residents) {
			UnlistTree(slot);
			UnlistFadeRoot(slot);
			ResetAccumulatedHalf(slot);
			residentPos[slot] = kNotResident;
			if (slot < tables.residentSlot.size()) {
				tables.residentSlot[slot] = 0;
				tables.NoteChange(slot, kChangeMembership);
			}
		}
		residents.clear();
		residentPatches.clear();
	}

	void SceneStore::KeepResidentsAlive()
	{
		ZoneScopedN("CS.DCLF.Accumulate.KeepResidents");
		if (residentMaintenanceDirty) {
			// Membership changed: the used sets are the residents' slots again (a slot whose last member left leaves them).
			ZoneScopedN("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			residentPipelines.clear();
			residentTrees.clear();
			tables.ClearUsed();
			for (const std::uint32_t slot : residents) {
				const auto& object = tables.objects[slot];
				if (!tables.PipelineUsed(object.pipelineIndex)) {
					tables.MarkPipelineUsed(object.pipelineIndex);
					residentPipelines.emplace_back(object.pipelineIndex, slot);
				}
				if (!tables.MaterialUsed(object.materialIndex))
					tables.MarkMaterialUsed(object.materialIndex);
				if (object.flags & kObjectTreeAnim)
					residentTrees.push_back(slot);
			}
			// The members TreeWindCS writes the wind of, by their tree slots.
			std::vector<TreeObject> treeObjects;
			treeObjects.reserve(residentTrees.size());
			for (const std::uint32_t slot : residentTrees)
				if (slot < tables.objectTree.size() && tables.objectTree[slot] != kNoTree)
					treeObjects.push_back({ slot, tables.objectTree[slot] });
			if (treeObjects.size() != tables.treeObjects.size() ||
				std::memcmp(treeObjects.data(), tables.treeObjects.data(), treeObjects.size() * sizeof(TreeObject)) != 0) {
				tables.treeObjects = std::move(treeObjects);
				++tables.treeObjectsVersion;
			}
			// Each fade root's centre is a member's record (its fade node row): one that is still a member.
			std::vector<std::uint32_t> centres(tables.fadeRoots.size(), ~0u);
			for (const std::uint32_t slot : residents)
				if (slot < tables.objectFadeRoot.size() && tables.objectFadeRoot[slot] < centres.size() && centres[tables.objectFadeRoot[slot]] == ~0u)
					centres[tables.objectFadeRoot[slot]] = slot;
			bool moved = false;
			for (std::size_t r = 0; r < centres.size(); ++r)
				if (tables.fadeRoots[r].object != centres[r]) {
					tables.fadeRoots[r].object = centres[r];
					moved = true;
				}
			if (moved)
				++tables.fadeRootsVersion;
			residentMaintenanceDirty = false;
		}
		// A member's property is its pipeline's lighting template, read again every frame: the property is the engine's, and a
		// swap of it rewrites the member only when an event names it.
		for (const auto& [pipeline, slot] : residentPipelines) {
			const auto* geometry = tables.objectGeometry[slot];
			tables.geometryTemplate[pipeline] = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		}
		// A tree's wind is TreeWindCS's from here (Records.h, TreeStatic): nothing is taken from the tree nodes per frame.
	}

	void SceneStore::ListTree(std::uint32_t a_slot)
	{
		const bool tree = a_slot < tables.objects.size() && (tables.objects[a_slot].flags & kObjectTreeAnim);
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto* property = tree && geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		TreeStatic row;
		const void* node = property ? TreeStaticOf(*property, row) : nullptr;
		if (tables.objectTree.size() <= a_slot)
			tables.objectTree.resize(std::size_t(a_slot) + 1, kNoTree);
		auto& current = tables.objectTree[a_slot];
		const std::uint32_t wanted = !tree ? kNoTree : !node ? kNodelessTree : kNoTree;
		// Already under this node (a patch that changed something else), or nodeless and staying so.
		if (node && current < tables.treeNode.size() && tables.treeNode[current] == node)
			return;
		if (!node && current == wanted)
			return;
		UnlistTree(a_slot);
		if (!node) {
			current = wanted;
			residentMaintenanceDirty = true;
			return;
		}
		auto [it, inserted] = tables.treeIndex.try_emplace(node, 0u);
		if (inserted) {
			std::uint32_t t;
			if (!tables.treeFree.empty()) {
				t = tables.treeFree.back();
				tables.treeFree.pop_back();
			} else {
				t = static_cast<std::uint32_t>(tables.trees.size());
				tables.trees.emplace_back();
				tables.treeRefs.push_back(0);
				tables.treeNode.push_back(nullptr);
			}
			row.generation = ++tables.treeGenerations;
			tables.trees[t] = row;
			tables.treeNode[t] = node;
			it->second = t;
			++tables.treesVersion;
		}
		++tables.treeRefs[it->second];
		current = it->second;
		residentMaintenanceDirty = true;
	}

	void SceneStore::ListFadeRoot(std::uint32_t a_slot)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		const RE::NiAVObject* node = property ? property->fadeNode : nullptr;
		if (tables.objectFadeRoot.size() <= a_slot)
			tables.objectFadeRoot.resize(std::size_t(a_slot) + 1, kNoFadeRoot);
		auto& current = tables.objectFadeRoot[a_slot];
		if (node && current < tables.fadeRootNode.size() && tables.fadeRootNode[current] == node)
			return;
		if (!node && current == kNoFadeRoot)
			return;
		UnlistFadeRoot(a_slot);
		if (!node)
			return;
		auto [it, inserted] = tables.fadeRootIndex.try_emplace(node, 0u);
		if (inserted) {
			std::uint32_t r;
			if (!tables.fadeRootFree.empty()) {
				r = tables.fadeRootFree.back();
				tables.fadeRootFree.pop_back();
			} else {
				r = static_cast<std::uint32_t>(tables.fadeRoots.size());
				tables.fadeRoots.emplace_back();
				tables.fadeRootRefs.push_back(0);
				tables.fadeRootNode.push_back(nullptr);
			}
			auto row = FadeState::StaticOf(*node);
			row.object = a_slot;
			row.generation = ++tables.fadeRootGenerations;
			if (const auto owned = fadeRootOwned.find(node); owned != fadeRootOwned.end())
				row.bits |= kFadeRootOwned | (owned->second ? kFadeRootWriteBack : 0u);
			tables.fadeRoots[r] = row;
			tables.fadeRootNode[r] = node;
			if (const auto* lodSwitch = FadeState::TreeLodSwitch(*node))
				tables.fadeRootSwitch.insert_or_assign(lodSwitch, r);
			it->second = r;
			++tables.fadeRootsVersion;
		}
		++tables.fadeRootRefs[it->second];
		current = it->second;
	}

	void SceneStore::UnlistFadeRoot(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objectFadeRoot.size())
			return;
		auto& current = tables.objectFadeRoot[a_slot];
		if (current < tables.fadeRootRefs.size() && --tables.fadeRootRefs[current] == 0) {
			const auto* node = static_cast<const RE::NiAVObject*>(tables.fadeRootNode[current]);
			std::erase_if(tables.fadeRootSwitch, [&](const auto& a_entry) { return a_entry.second == current; });
			tables.fadeRootIndex.erase(node);
			tables.fadeRootNode[current] = nullptr;
			tables.fadeRoots[current] = FadeRootStatic{};
			tables.fadeRootFree.push_back(current);
			++tables.fadeRootsVersion;
		}
		current = kNoFadeRoot;
	}

	void SceneStore::MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_writeBack)
	{
		const auto it = tables.fadeRootIndex.find(a_node);
		if (it == tables.fadeRootIndex.end())
			return;
		auto& row = tables.fadeRoots[it->second];
		if (a_owned) {
			// From here the GPU's state is the members': it starts from the node as the engine left it.
			const std::uint32_t object = row.object;
			row = FadeState::StaticOf(*static_cast<const RE::NiAVObject*>(a_node));
			row.object = object;
			row.generation = ++tables.fadeRootGenerations;
			row.bits |= kFadeRootOwned | (a_writeBack ? kFadeRootWriteBack : 0u);
		} else {
			row.bits &= ~(kFadeRootOwned | kFadeRootWriteBack);
		}
		++tables.fadeRootsVersion;
	}

	void SceneStore::SetFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned)
	{
		ankerl::unordered_dense::map<const void*, bool> owned;
		for (const auto& root : a_owned)
			owned.insert_or_assign(root.node, root.writeBack);
		for (const auto& [node, writeBack] : fadeRootOwned)
			if (!owned.contains(node))
				MarkFadeRootOwned(node, false, false);
		for (const auto& [node, writeBack] : owned)
			if (const auto it = fadeRootOwned.find(node); it == fadeRootOwned.end() || it->second != writeBack)
				MarkFadeRootOwned(node, true, writeBack);
		fadeRootOwned = std::move(owned);
	}

	void SceneStore::ReseedFadeRoot(const void* a_node)
	{
		const auto it = tables.fadeRootIndex.find(a_node);
		if (it == tables.fadeRootIndex.end())
			return;
		auto& row = tables.fadeRoots[it->second];
		const std::uint32_t object = row.object;
		const std::uint32_t kept = row.bits & (kFadeRootOwned | kFadeRootWriteBack);
		row = FadeState::StaticOf(*static_cast<const RE::NiAVObject*>(a_node));
		row.object = object;
		row.generation = ++tables.fadeRootGenerations;
		row.bits |= kept;
		++tables.fadeRootsVersion;
	}

	void SceneStore::ReseedOwnedFadeRoots()
	{
		for (const auto& [node, writeBack] : fadeRootOwned)
			MarkFadeRootOwned(node, true, writeBack);
	}

	void SceneStore::ApplyFadeChanges(const std::vector<FadeChange>& a_changes)
	{
		for (const auto& change : a_changes) {
			const std::uint32_t r = change.root;
			if (r >= tables.fadeRoots.size() || !tables.fadeRootNode[r] || tables.fadeRoots[r].generation != change.state.generation ||
				!(tables.fadeRoots[r].bits & kFadeRootWriteBack)) {
				++fadeWriteStats.dropped;
				continue;
			}
			auto* node = static_cast<RE::NiAVObject*>(const_cast<void*>(tables.fadeRootNode[r]));
			++fadeWriteStats.applied;
			if (FadeState::WriteNode(*node, change.state)) {
				NoteFadeChanged(node);
				++fadeWriteStats.watched;
			}
		}
	}

	void SceneStore::RefreshFadeRootSwitch(const RE::NiAVObject* a_switch)
	{
		const auto it = tables.fadeRootSwitch.find(a_switch);
		if (it == tables.fadeRootSwitch.end() || it->second >= tables.fadeRoots.size() || !tables.fadeRootNode[it->second])
			return;
		auto& row = tables.fadeRoots[it->second];
		const bool selected = FadeState::TreeLodSelected(*static_cast<const RE::NiAVObject*>(tables.fadeRootNode[it->second]));
		if (selected == ((row.bits & kFadeRootTreeLod) != 0))
			return;
		// An input, not state: the row changes without a new generation, so the GPU's state row is kept.
		row.bits = selected ? (row.bits | kFadeRootTreeLod) : (row.bits & ~kFadeRootTreeLod);
		++tables.fadeRootsVersion;
	}

	void SceneStore::UnlistTree(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objectTree.size())
			return;
		auto& current = tables.objectTree[a_slot];
		if (current < tables.treeRefs.size() && --tables.treeRefs[current] == 0) {
			tables.treeIndex.erase(tables.treeNode[current]);
			tables.treeNode[current] = nullptr;
			tables.treeFree.push_back(current);
		}
		if (current != kNoTree)
			residentMaintenanceDirty = true;
		current = kNoTree;
	}

	void SceneStore::CheckResidentParity()
	{
		++residentStats.parityChecks;
		std::uint32_t logged = 0;
		for (std::size_t r = 0; r < residents.size(); ++r) {
			const std::uint32_t slot = residents[r];
			const auto& patch = residentPatches[r];
			const auto* geometry = tables.objectGeometry[slot];
			if (!geometry)
				continue;
			// A root that has started to fade since the feedback's last decode: that decode's successor ends the residency (the
			// frame of latency every root state the feedback services has), so its pass is not compared.
			if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f) {
				++residentStats.parityPending;
				continue;
			}
			++residentStats.parityChecked;
			AccumulatedPass fresh;
			const bool built = PrimaryCull::FreshSyntheticPass(*geometry, fresh);
			const bool passSame = built && fresh.technique == patch.pass.technique && fresh.subPass == patch.pass.subPass && fresh.hint == patch.pass.hint &&
			                      fresh.lodRow == patch.pass.lodRow && fresh.sunTest == patch.pass.sunTest;
			const auto& object = tables.objects[slot];
			const bool recordSame = object.pipelineIndex == patch.pipeline && object.materialIndex == patch.material && (object.flags & kObjectMember) &&
			                        !(object.flags & kObjectNoBindings);
			residentStats.parityPass += passSame ? 0 : 1;
			residentStats.parityRecord += recordSame ? 0 : 1;
			if ((!passSame || !recordSame) && logged++ < 4)
				logger::info("[DCLF] resident parity: '{}' pass {} (technique {:X} -> {:X}, hint {} -> {}, LOD row {} -> {}, sun test {} -> {}), record {} (pipeline {} -> {}, material {} -> {}, flags {:X})",
					geometry->name.c_str() ? geometry->name.c_str() : "?", passSame ? "same" : built ? "DIFFERS" : "NOT BUILT", patch.pass.technique, fresh.technique,
					patch.pass.hint, fresh.hint, patch.pass.lodRow, fresh.lodRow, patch.pass.sunTest, fresh.sunTest, recordSame ? "same" : "DIFFERS", patch.pipeline,
					object.pipelineIndex, patch.material, object.materialIndex, object.flags);
		}
	}
}
