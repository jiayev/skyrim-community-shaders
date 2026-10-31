// CS_DCLF_SET_PARITY: every 60th commit (one commit a pass) every slot's phases and waiting decided again (Coordinator::DecideSlot) against
// what the commit's queue (its events and requeues by cause) left: a slot the requeues missed differs. The check's decisions that leave a
// slot out of a root view are announced (SetSlotRedecided: Features/FirstPerson's observer, with the commit's own, SetSlotDecided).

#include "SceneObservers.h"
#include "SceneState.h"
#include "RootViewEvents.h"

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

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
			// A decision of the check's, a slot it leaves out of a root view announced with the waiting reason the commit left.
			std::string whys;  // the full evaluation's waiting reasons for the slot last decided (SetStats::waitingBy's indices)
			auto wanted = [&](std::uint32_t a_slot, bool& a_wait) {
				const auto decision = V::DecideSlot(store, a_slot, a_event.live, a_event.drawn);
				a_wait = decision.wait;
				whys.clear();
				for (std::uint32_t w = 0; w < decision.whyCount; ++w)
					whys += fmt::format("{}{}", w ? "," : "", decision.whys[w]);
				if (decision.rootViewExcluded || decision.rootViewHeld)
					Notify(SetSlotRedecided{ store, a_slot, decision, a_slot < setWaitWhy.size() ? setWaitWhy[a_slot] : std::uint8_t{ 0 } });
				return decision.phases;
			};
			std::uint32_t differ = 0;
			std::string first;
			for (std::uint32_t slot = 0; slot < objects; ++slot) {
				bool wait = false;
				std::uint8_t phases = wanted(slot, wait);
				const std::string slotWhys = whys;
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
					first = fmt::format("'{}' (slot {}): phases {:#x} kept, {:#x} now; waiting {} kept, {} now (its causes {:#x}; the evaluation's reasons {})",
						geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", slot, setPhases[slot], phases, setWaitingMark[slot] != 0, wait, setWaitCause[slot],
						slotWhys.empty() ? "none" : slotWhys);
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
		Subscribe<SetCommitted>(OnCommitted);
	}
}
