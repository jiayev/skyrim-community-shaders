#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// The joins' and the slots' observers: CS_DCLF_RESIDENT_PARITY (the residents' passes built again), CS_DCLF_SLOT_PROBE (what the used
// slots serve, re-derived), and CS_DCLF_PERSISTENT_PARITY's drained material slot check.

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
				if (const auto leaf = mirror.Leaf(geometry);
					leaf.fadeNode && !a_store.FadeOnGpu(static_cast<const RE::NiAVObject*>(leaf.property->fadeNode)) && leaf.fadeNode->currentFade < 1.0f) {
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

		void ProbeSlots(const Scene::Coordinator& a_store, bool a_resolveBuffers)
		{
			const auto& tables = V::tables(a_store);
			const std::uint32_t sceneFrame = V::sceneFrame(a_store);
			static std::uint32_t logged = 0;
			if (logged >= 40)
				return;
			auto& gpu = GpuResources::Scene();
			std::uint32_t geometryDiffers = 0, geometriesProbed = 0;
			std::string first;
			if (a_resolveBuffers) {
				for (std::uint32_t slot = 0; slot < tables.geometries.size(); ++slot) {
					if (tables.geometryLastUsed[slot] != a_store.PassStamp() || tables.geometryLayerKey[slot])
						continue;
					++geometriesProbed;
					const auto& record = tables.geometries[slot];
					const auto* triShape = tables.geometrySlotKey[slot];
					const auto vertexLease = gpu.Acquire(record.vertexBuffer);
					const auto indexLease = gpu.Acquire(record.indexBuffer);
					const auto* vertex = vertexLease ? &vertexLease->buffer : nullptr;
					const auto* index = indexLease ? &indexLease->buffer : nullptr;
					const bool same = triShape && vertex && index && vertex->address == record.vertexAddress && index->address == record.indexAddress &&
					                  reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) == record.vertexBuffer &&
					                  reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) == record.indexBuffer;
					if (!same && geometryDiffers++ == 0 && first.empty())
						first = fmt::format("geometry slot {} vb {:#x} -> {:#x} ib {:#x} -> {:#x} (trishape {} buffers {}/{} vs {}/{})", slot,
							record.vertexAddress, vertex ? vertex->address : 0ull, record.indexAddress, index ? index->address : 0ull,
							static_cast<const void*>(triShape), static_cast<const void*>(record.vertexBuffer), static_cast<const void*>(record.indexBuffer),
							triShape ? static_cast<const void*>(triShape->vertexBuffer) : nullptr, triShape ? static_cast<const void*>(triShape->indexBuffer) : nullptr);
				}
			}
			if (geometryDiffers || sceneFrame < 12) {
				++logged;
				logger::info("[DCLF] slot probe frame {}: {} of {} used geometries differ{}{}", sceneFrame, geometryDiffers, geometriesProbed, first.empty() ? "" : "; first: ",
					first);
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

		void OnResidentsKept(const ResidentsKept& a_event)
		{
			const auto& store = a_event.store;
			if (ParityDue(V::passSerial(store)) && !V::residents(store).empty())
				CheckResidentParity(store);
		}

		void OnSlotsChecked(const SlotsChecked& a_event)
		{
			ProbeSlots(a_event.store, a_event.resolveBuffers);
		}
	}

	void InstallResidentParity()
	{
		Subscribe<ResidentsKept>(OnResidentsKept);
	}

	void InstallSlotProbe()
	{
		Subscribe<SlotsChecked>(OnSlotsChecked);
	}

	void InstallSlotDrainCheck()
	{
		Subscribe<MaterialSlotDrained>(OnMaterialSlotDrained);
	}
}
