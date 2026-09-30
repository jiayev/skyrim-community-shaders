#include "Internal.h"

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
		memberDecals.clear();
		memberDecalsChanged = true;
		for (const std::uint32_t slot : residents) {
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
			ZoneScopedN("CS.DCLF.Accumulate.RebuildResidentMaintenance");
			residentPipelines.clear();
			residentMaterials.clear();
			residentTrees.clear();
			ankerl::unordered_dense::set<std::uint32_t> pipelines, materials;
			for (const std::uint32_t slot : residents) {
				const auto& object = tables.objects[slot];
				if (pipelines.insert(object.pipelineIndex).second)
					residentPipelines.emplace_back(object.pipelineIndex, slot);
				if (materials.insert(object.materialIndex).second)
					residentMaterials.push_back(object.materialIndex);
				if (object.flags & kObjectTreeAnim)
					residentTrees.push_back(slot);
			}
			residentMaintenanceDirty = false;
		}
		for (const auto& [pipeline, slot] : residentPipelines) {
			if (pipeline < tables.pipelineLastUsed.size() && tables.pipelineLastUsed[pipeline] != frame) {
				// No member joined on the pipeline this frame: a member's property is its lighting template.
				const auto* geometry = tables.objectGeometry[slot];
				tables.MarkPipelineUsed(pipeline, frame);
				tables.geometryTemplate[pipeline] = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			}
		}
		for (const std::uint32_t material : residentMaterials)
			if (material < tables.materialLastUsed.size())
				tables.MarkMaterialUsed(material, frame);
		for (const std::uint32_t slot : residentTrees) {
			const auto* geometry = tables.objectGeometry[slot];
			// A tree's wind state is the tree manager's, advanced while the feedback keeps the root's kAccumulated: taken
			// every frame, as the accumulate phase takes it for every other drawn tree.
			if (geometry)
				if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get()) {
					ObjectTreeAnim tree = tables.treeAnim[slot];
					DeriveTreeAnim(*property, tree);
					if (std::memcmp(&tree, &tables.treeAnim[slot], sizeof(tree)) != 0) {
						tables.treeAnim[slot] = tree;
						tables.NoteChange(slot, kChangeTree);
					}
				}
		}
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
			const bool recordSame = object.pipelineIndex == patch.pipeline && object.materialIndex == patch.material && (object.flags & kObjectNativeVisible) &&
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
