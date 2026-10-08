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
		NoteResidentTouched(a_slot);
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
		NoteResidentTouched(a_slot);
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
		// The scene work's drops reach PrimaryCull as set changes (ApplySet) or as revoked claims (RevokeUndrawnClaims); the
		// accumulate phase's, at once.
		if (a_slot < tables.objectGeometry.size() && tables.objectGeometry[a_slot]) {
			if (!holdPrimaryNotes)
				PrimaryCull::Get().NoteMemberLost(tables.objectGeometry[a_slot]);
			else if (holdLostMembers)
				lostMembersHeld.push_back(tables.objectGeometry[a_slot]);
		}
		if (a_slot < tables.residentSlot.size()) {
			tables.residentSlot[a_slot] = 0;
			tables.NoteChange(a_slot, kChangeMembership);
		}
		if (memberDecals.erase(a_slot))
			NoteDecalChanged(a_slot);
		if (a_restore)
			ResetAccumulatedHalf(a_slot);
		// A layer and its base are members together (WriteLayer): the other leaves too.
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner != kNoObjectSlot)
			DropResidentSlot(partner, a_restore);
	}

	void SceneStore::EndAllResidency()
	{
		residentMaintenanceDirty = true;
		if (holdPrimaryNotes)
			allMembersLostHeld = true;
		else
			PrimaryCull::Get().NoteAllMembersLost();
		memberDecals.clear();
		NoteDecalsCleared();
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
		auto& counted = residentCounted;
		if (counted.size() < tables.objects.size())
			counted.resize(tables.objects.size());
		if (materialMembers.size() < tables.materials.size())
			materialMembers.resize(tables.materials.size(), 0);
		std::vector<std::uint32_t> pipelinesTouched, materialsTouched, fadeRootsTouched;
		bool treesTouched = false;
		// A resident's keys now (none for a slot that is not resident).
		auto keysOf = [&](std::uint32_t a_slot) {
			ResidentCounted keys;
			if (!IsResidentSlot(a_slot) || a_slot >= tables.objects.size())
				return keys;
			const auto& object = tables.objects[a_slot];
			keys.pipeline = object.pipelineIndex;
			keys.material = object.materialIndex;
			if (a_slot < tables.objectFadeRoot.size() && tables.objectFadeRoot[a_slot] < tables.fadeRoots.size())
				keys.fadeRoot = tables.objectFadeRoot[a_slot];
			keys.tree = (object.flags & kObjectTreeAnim) != 0;
			return keys;
		};
		auto uncount = [&](std::uint32_t a_slot) {
			auto& was = counted[a_slot];
			if (was.pipeline != ResidentCounted::kNone) {
				pipelineMembers.Remove(was.pipeline, a_slot);
				pipelinesTouched.push_back(was.pipeline);
			}
			if (was.material != ResidentCounted::kNone && was.material < materialMembers.size()) {
				--materialMembers[was.material];
				materialsTouched.push_back(was.material);
			}
			if (was.fadeRoot != ResidentCounted::kNone) {
				fadeRootMembers.Remove(was.fadeRoot, a_slot);
				fadeRootsTouched.push_back(was.fadeRoot);
			}
			if (was.tree) {
				treeMembers.Remove(0, a_slot);
				treesTouched = true;
			}
			was = {};
		};
		auto count = [&](std::uint32_t a_slot) {
			const auto keys = keysOf(a_slot);
			if (keys.pipeline == ResidentCounted::kNone)
				return;
			pipelineMembers.Add(keys.pipeline, a_slot);
			pipelinesTouched.push_back(keys.pipeline);
			if (materialMembers.size() <= keys.material)
				materialMembers.resize(std::size_t(keys.material) + 1, 0);
			++materialMembers[keys.material];
			materialsTouched.push_back(keys.material);
			if (keys.fadeRoot != ResidentCounted::kNone) {
				fadeRootMembers.Add(keys.fadeRoot, a_slot);
				fadeRootsTouched.push_back(keys.fadeRoot);
			}
			if (keys.tree) {
				treeMembers.Add(0, a_slot);
				treesTouched = true;
			}
			counted[a_slot] = keys;
		};
		if (residentMaintenanceDirty) {
			// Every residency ended, or the tables were reset: everything counted again.
			ZoneScopedN("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			counted.assign(tables.objects.size(), ResidentCounted{});
			pipelineMembers.Clear();
			fadeRootMembers.Clear();
			treeMembers.Clear();
			materialMembers.assign(tables.materials.size(), 0);
			tables.ClearUsed();
			for (const std::uint32_t slot : residents)
				count(slot);
			for (std::uint32_t r = 0; r < tables.fadeRoots.size(); ++r)
				fadeRootsTouched.push_back(r);
			treesTouched = true;
			residentMaintenanceDirty = false;
		} else {
			// The slots named since: their old keys out (all of them first: a fade root freed and listed again for another node
			// within the frame), then their keys now.
			std::sort(residentsTouched.begin(), residentsTouched.end());
			residentsTouched.erase(std::unique(residentsTouched.begin(), residentsTouched.end()), residentsTouched.end());
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					uncount(slot);
			for (const std::uint32_t slot : residentsTouched)
				if (slot < counted.size())
					count(slot);
		}
		residentsTouched.clear();
		// The used sets: a pipeline or material is used while a resident holds it. The joins marked theirs as they bound; one
		// whose join failed is no resident's.
		pipelinesTouched.insert(pipelinesTouched.end(), joinMarkedPipelines.begin(), joinMarkedPipelines.end());
		materialsTouched.insert(materialsTouched.end(), joinMarkedMaterials.begin(), joinMarkedMaterials.end());
		joinMarkedPipelines.clear();
		joinMarkedMaterials.clear();
		for (const std::uint32_t p : pipelinesTouched)
			tables.SetPipelineUsed(p, pipelineMembers.Count(p) != 0);
		for (const std::uint32_t m : materialsTouched)
			tables.SetMaterialUsed(m, m < materialMembers.size() && materialMembers[m] != 0);
		// The members TreeWindCS writes the wind of, by their tree slots (in slot order).
		if (treesTouched) {
			std::vector<std::uint32_t> slots = treeMembers.lists.empty() ? std::vector<std::uint32_t>{} : treeMembers.lists[0];
			std::sort(slots.begin(), slots.end());
			std::vector<TreeObject> treeObjects;
			treeObjects.reserve(slots.size());
			for (const std::uint32_t slot : slots)
				if (slot < tables.objectTree.size() && tables.objectTree[slot] != kNoTree)
					treeObjects.push_back({ slot, tables.objectTree[slot] });
			if (treeObjects.size() != tables.treeObjects.size() ||
				std::memcmp(treeObjects.data(), tables.treeObjects.data(), treeObjects.size() * sizeof(TreeObject)) != 0) {
				tables.treeObjects = std::move(treeObjects);
				++tables.treeObjectsVersion;
				tables.NoteTreesWrite();
			}
		}
		// Each fade root's centre is a member's record (its fade node row): one that is still a member.
		for (const std::uint32_t r : fadeRootsTouched) {
			if (r >= tables.fadeRoots.size())
				continue;
			const std::uint32_t centre = fadeRootMembers.First(r);
			if (tables.fadeRoots[r].object != centre) {
				tables.fadeRoots[r].object = centre;
				tables.NoteFadeRoot(r);
			}
		}
		// A member's property is its pipeline's lighting template, read again every frame: the property is the engine's, and a
		// swap of it rewrites the member only when an event names it.
		for (std::uint32_t p = 0; p < pipelineMembers.lists.size(); ++p)
			if (const std::uint32_t slot = pipelineMembers.First(p); slot != ~0u && p < tables.geometryTemplate.size())
				tables.geometryTemplate[p] = SlotProperty(slot);
		// A tree's wind is TreeWindCS's from here (Records.h, TreeStatic): nothing is taken from the tree nodes per frame.
	}

	void SceneStore::ListTree(std::uint32_t a_slot)
	{
		const bool tree = a_slot < tables.objects.size() && (tables.objects[a_slot].flags & kObjectTreeAnim);
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto* property = tree && geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		TreeStatic row;
		const void* node = property ? TreeStaticOf(*property, row) : nullptr;
		if (tables.objectTree.size() <= a_slot) {
			tables.NoteTreesWrite();
			tables.objectTree.resize(std::size_t(a_slot) + 1, kNoTree);
		}
		auto& current = tables.objectTree[a_slot];
		const std::uint32_t wanted = !tree ? kNoTree : !node ? kNodelessTree : kNoTree;
		// Already under this node (a patch that changed something else), or nodeless and staying so.
		if (node && current < tables.treeNode.size() && tables.treeNode[current] == node)
			return;
		if (!node && current == wanted)
			return;
		tables.NoteTreesWrite();
		UnlistTree(a_slot);
		if (!node) {
			current = wanted;
			NoteResidentTouched(a_slot);
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
			// Owned while listed (step 6e E3: a published version that names it keeps it, through the retirement chain).
			if (treeOwners.size() <= t)
				treeOwners.resize(std::size_t(t) + 1);
			treeOwners[t].reset(const_cast<RE::NiAVObject*>(static_cast<const RE::NiAVObject*>(node)));
			it->second = t;
			++tables.treesVersion;
		}
		++tables.treeRefs[it->second];
		current = it->second;
		NoteResidentTouched(a_slot);
	}

	void SceneStore::ListFadeRoot(std::uint32_t a_slot)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		const RE::NiAVObject* node = property ? property->fadeNode : nullptr;
		if (tables.objectFadeRoot.size() <= a_slot) {
			tables.NoteFadeRootsWrite();
			tables.objectFadeRoot.resize(std::size_t(a_slot) + 1, kNoFadeRoot);
		}
		auto& current = tables.objectFadeRoot[a_slot];
		if (node && current < tables.fadeRootNode.size() && tables.fadeRootNode[current] == node)
			return;
		if (!node && current == kNoFadeRoot)
			return;
		tables.NoteFadeRootsWrite();
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
				row.bits |= kFadeRootOwned | (owned->second ? kFadeRootStoodIn : 0u);
			tables.fadeRoots[r] = row;
			tables.fadeRootNode[r] = node;
			// Owned while listed (step 6e E3: a published version that names it keeps it, through the retirement chain).
			if (fadeRootOwners.size() <= r)
				fadeRootOwners.resize(std::size_t(r) + 1);
			fadeRootOwners[r].reset(const_cast<RE::NiAVObject*>(node));
			if (const auto* lodSwitch = FadeState::TreeLodSwitch(*node))
				tables.fadeRootSwitch.insert_or_assign(lodSwitch, r);
			it->second = r;
			tables.NoteFadeRoot(r);
		}
		++tables.fadeRootRefs[it->second];
		current = it->second;
		// The shadow inputs name it (FadeRootOf).
		tables.NoteChange(a_slot, kChangeBindings);
	}

	void SceneStore::UnlistFadeRoot(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objectFadeRoot.size())
			return;
		tables.NoteFadeRootsWrite();
		auto& current = tables.objectFadeRoot[a_slot];
		if (current < tables.fadeRootRefs.size() && --tables.fadeRootRefs[current] == 0) {
			const auto* node = static_cast<const RE::NiAVObject*>(tables.fadeRootNode[current]);
			std::erase_if(tables.fadeRootSwitch, [&](const auto& a_entry) { return a_entry.second == current; });
			tables.fadeRootIndex.erase(node);
			tables.fadeRootNode[current] = nullptr;
			if (current < fadeRootOwners.size())
				HandBack(std::move(fadeRootOwners[current]));
			tables.fadeRoots[current] = FadeRootStatic{};
			tables.Retire(Tables::kRetiredFadeRoot, current);
			tables.NoteFadeRoot(current);
		}
		current = kNoFadeRoot;
	}

	void SceneStore::MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_standIn)
	{
		const auto* node = static_cast<const RE::NiAVObject*>(a_node);
		if (const auto before = fadeRootOwned.find(a_node); (before != fadeRootOwned.end() && before->second) != (a_owned && a_standIn))
			NoteFadeChanged(node);
		const auto it = tables.fadeRootIndex.find(a_node);
		if (it == tables.fadeRootIndex.end())
			return;
		auto& row = tables.fadeRoots[it->second];
		if (a_owned) {
			// From here the GPU's state is the members': it starts from the node as the engine left it.
			const std::uint32_t object = row.object;
			row = FadeState::StaticOf(*node);
			row.object = object;
			row.generation = ++tables.fadeRootGenerations;
			row.bits |= kFadeRootOwned | (a_standIn ? kFadeRootStoodIn : 0u);
		} else {
			row.bits &= ~(kFadeRootOwned | kFadeRootStoodIn);
		}
		tables.NoteFadeRoot(it->second);
	}

	void SceneStore::ApplyFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned)
	{
		ankerl::unordered_dense::map<const void*, bool> owned;
		for (const auto& root : a_owned)
			owned.insert_or_assign(root.node, root.standIn);
		for (const auto& [node, standIn] : fadeRootOwned)
			if (!owned.contains(node))
				MarkFadeRootOwned(node, false, false);
		for (const auto& [node, standIn] : owned)
			if (const auto it = fadeRootOwned.find(node); it == fadeRootOwned.end() || it->second != standIn)
				MarkFadeRootOwned(node, true, standIn);
		fadeRootOwned = std::move(owned);
	}

	void SceneStore::ReseedFadeRoot(const void* a_node)
	{
		const auto it = tables.fadeRootIndex.find(a_node);
		if (it == tables.fadeRootIndex.end())
			return;
		auto& row = tables.fadeRoots[it->second];
		const std::uint32_t object = row.object;
		const std::uint32_t kept = row.bits & (kFadeRootOwned | kFadeRootStoodIn);
		row = FadeState::StaticOf(*static_cast<const RE::NiAVObject*>(a_node));
		row.object = object;
		row.generation = ++tables.fadeRootGenerations;
		row.bits |= kept;
		tables.NoteFadeRoot(it->second);
	}

	void SceneStore::ApplyReseedOwnedFadeRoots()
	{
		for (const auto& [node, standIn] : fadeRootOwned)
			MarkFadeRootOwned(node, true, standIn);
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
		tables.NoteFadeRoot(it->second);
	}

	void SceneStore::UnlistTree(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objectTree.size())
			return;
		tables.NoteTreesWrite();
		auto& current = tables.objectTree[a_slot];
		if (current < tables.treeRefs.size() && --tables.treeRefs[current] == 0) {
			tables.treeIndex.erase(tables.treeNode[current]);
			tables.treeNode[current] = nullptr;
			if (current < treeOwners.size())
				HandBack(std::move(treeOwners[current]));
			tables.Retire(Tables::kRetiredTree, current);
		}
		if (current != kNoTree)
			NoteResidentTouched(a_slot);
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
			// A root the engine updates that has started to fade: its pass is not compared. A stood-in root's node is not its
			// state (FadeOnGpu), and its members' passes are the settled state's.
			if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				property && property->fadeNode && !FadeOnGpu(property->fadeNode) && property->fadeNode->GetRuntimeData().currentFade < 1.0f) {
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
