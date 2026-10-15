// CS_DCLF_SET_PARITY: every 60th commit (one commit a pass) every slot's phases and waiting decided again (Coordinator::DecideSlot) against
// what the commit's queue (its events and requeues by cause) left: a slot the requeues missed differs. And the first-person slots the
// decisions leave out of the first-person view, the commit's and the check's.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/** @brief The first-person slot a decision left out (a_waitWhy: the waiting reason it is shown with). */
		void NoteFirstPerson(const Scene::Coordinator& a_store, std::uint32_t a_slot, const V::SlotDecision& a_decision, std::uint8_t a_waitWhy)
		{
			auto& fp = SceneDiagnostics().commit;
			const auto& tables = V::tables(a_store);
			if (a_decision.rootViewExcluded) {
				++fp.firstPersonExcluded;
				if (fp.firstFirstPersonExcluded.empty())
					if (const auto* geometry = tables.objectGeometry[a_slot]) {
						const auto p = tables.objects[a_slot].pipelineIndex;
						fp.firstFirstPersonExcluded = fmt::format("'{}' (slot {}, flags {:#x}, raster {:#x})", geometry->name.c_str() ? geometry->name.c_str() : "?",
							a_slot, tables.objects[a_slot].flags, p < tables.pipelines.size() ? tables.pipelines[p].rasterFlags : 0u);
					}
			}
			if (a_decision.rootViewHeld) {
				++fp.firstPersonHeld;
				if (fp.firstFirstPersonHeld.empty())
					if (const auto* geometry = tables.objectGeometry[a_slot])
						fp.firstFirstPersonHeld = fmt::format("'{}' (slot {}, {}, waiting reason {}, first-person readiness cause {}, pipeline slot {} raster {:#x})",
							geometry->name.c_str() ? geometry->name.c_str() : "?", a_slot, V::IsResidentSlot(a_store, a_slot) ? "resident" : "not resident",
							a_waitWhy, a_decision.rootViewSlotWhy, tables.objects[a_slot].pipelineIndex,
							tables.objects[a_slot].pipelineIndex < tables.pipelines.size() ? tables.pipelines[tables.objects[a_slot].pipelineIndex].rasterFlags : 0u);
			}
		}

		void OnSlotDecided(const SetSlotDecided& a_event)
		{
			NoteFirstPerson(a_event.store, a_event.slot, a_event.decision, a_event.waitWhy);
		}

		void OnCommitted(const SetCommitted& a_event)
		{
			const auto& store = a_event.store;
			if (!ParityDue(V::passSerial(store), 23))
				return;
			const auto& tables = V::tables(store);
			const auto& setPhases = V::setPhasesNext(store);
			const auto& setWaitingMark = V::setWaitingMark(store);
			const auto& setWaitCause = V::setWaitCause(store);
			const auto& setWaitWhy = V::setWaitWhy(store);
			const std::size_t objects = tables.objects.size();
			// A decision of the check's, its first-person slots noted with the waiting reason the commit left.
			auto wanted = [&](std::uint32_t a_slot, bool& a_wait) {
				const auto decision = V::DecideSlot(store, a_slot, a_event.live, a_event.drawn);
				a_wait = decision.wait;
				if (decision.rootViewExcluded || decision.rootViewHeld)
					NoteFirstPerson(store, a_slot, decision, a_slot < setWaitWhy.size() ? setWaitWhy[a_slot] : std::uint8_t{ 0 });
				return decision.phases;
			};
			std::uint32_t differ = 0;
			std::string first;
			for (std::uint32_t slot = 0; slot < objects; ++slot) {
				bool wait = false;
				std::uint8_t phases = wanted(slot, wait);
				const std::uint32_t partner = tables.IsLayer(slot) ? tables.layerBase[slot] : slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot;
				if (partner != kNoObjectSlot && partner < objects) {
					bool partnerWait = false;
					if (!(phases & kSetMain) != !(wanted(partner, partnerWait) & kSetMain))
						phases = 0;
				}
				// The vetoed entries' withholding (the gates'), as the commit applies it.
				if (a_event.gatesLive && V::EntryVetoed(store, slot))
					phases = 0;
				if (phases == setPhases[slot] && wait == (setWaitingMark[slot] != 0))
					continue;
				if (!differ++) {
					const auto* geometry = tables.objectGeometry[slot];
					first = fmt::format("'{}' (slot {}): phases {:#x} kept, {:#x} now; waiting {} kept, {} now (its causes {:#x})", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
						slot, setPhases[slot], phases, setWaitingMark[slot] != 0, wait, setWaitCause[slot]);
				}
			}
			auto& c = SceneDiagnostics().commit;
			++c.commitParityChecks;
			c.commitParityDiffer += differ;
			static std::uint32_t logged = 0;
			if (differ && logged++ < 8)
				logger::warn("[DCLF] set commit parity at frame {}: {} slots differ from a full evaluation; first {} <- COMMIT", V::sceneFrame(store), differ, first);
		}
	}

	void InstallCommitParity()
	{
		Subscribe<SetSlotDecided>(OnSlotDecided);
		Subscribe<SetCommitted>(OnCommitted);
	}
}
