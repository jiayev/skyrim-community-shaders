// The joins' and the slots' observers: CS_DCLF_RESIDENT_PARITY (the residents' passes built again) and CS_DCLF_PERSISTENT_PARITY's drained
// material slot check.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void CheckResidentParity(const Scene::Coordinator& a_store)
		{
			auto& residentStats = SceneDiagnostics().residents;
			const auto& tables = V::tables(a_store);
			const auto& mirror = V::mirror(a_store);
			const auto& residents = V::residents(a_store);
			const auto& residentPatches = V::residentPatches(a_store);
			const Api::RootListView roots = Extensions::RootListOf(tables);
			++residentStats.parityChecks;
			std::uint32_t logged = 0;
			for (std::size_t r = 0; r < residents.size(); ++r) {
				const std::uint32_t slot = residents[r];
				const auto& patch = residentPatches[r];
				const auto* geometry = tables.objectGeometry[slot];
				if (!geometry)
					continue;
				// A root the engine updates that has started to fade: its pass is not compared. A listed root's node is not its
				// state (the GPU's alone: every listed root is DCLF's to write), and its members' passes are the settled state's.
				if (const auto leaf = mirror.Leaf(geometry);
					leaf.fadeNode && !(leaf.property->fadeNode && roots.Contains(static_cast<const RE::NiAVObject*>(leaf.property->fadeNode))) && leaf.fadeNode->currentFade < 1.0f) {
					++residentStats.parityPending;
					continue;
				}
				++residentStats.parityChecked;
				AccumulatedPass fresh;
				const bool built = PrimaryCull::FreshSyntheticPass(mirror.Leaf(geometry), fresh);
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

		void OnMaterialSlotDrained(const MaterialSlotDrained& a_event)
		{
			// A bound record still naming the slot is a reference the counts missed.
			const auto& store = a_event.store;
			const auto& tables = V::tables(store);
			const std::uint32_t a_slot = a_event.slot;
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				if (!(tables.objects[o].flags & (kObjectNoBindings | kObjectFree)) && tables.objects[o].materialIndex == a_slot) {
					static std::uint32_t logged = 0;
					if (logged++ < 10)
						logger::error("[DCLF] material slot {} drained while object {} '{}' (flags {:#x}, member {}) is bound to it, frame {}", a_slot, o,
							tables.objectGeometry[o] && tables.objectGeometry[o]->name.c_str() ? tables.objectGeometry[o]->name.c_str() : "", tables.objects[o].flags,
							V::IsResidentSlot(store, o), V::sceneFrame(store));
				}
		}

		void OnResidentsKept(const On<ScenePoint::ResidentsKept>&)
		{
			const auto& store = Scene::Coordinator::Get();
			if (ParityDue(V::passSerial(store)) && !V::residents(store).empty())
				CheckResidentParity(store);
		}
	}

	void InstallResidentParity()
	{
		Subscribe<ScenePoint::ResidentsKept>(OnResidentsKept);
	}

	void InstallSlotDrainCheck()
	{
		Subscribe<MaterialSlotDrained>(OnMaterialSlotDrained);
	}
}
