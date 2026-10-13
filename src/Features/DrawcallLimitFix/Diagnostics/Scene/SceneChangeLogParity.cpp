#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_CHANGE_LOG_PARITY: every 60 passes each slot's columns are kept, and a pass later every slot whose columns changed in between
// must be in the log for that pass with the causes that changed. At the scene phase's start, before anything of the pass writes.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void OnScenePhaseBegin(const ScenePhaseBegin& a_event)
		{
			const auto& store = a_event.store;
			const auto& tables = V::tables(store);
			const auto tablesGeneration = V::tablesGeneration(store);
			const auto sceneFrame = V::sceneFrame(store);
			auto& c = SceneDiagnostics().changeLog;
			if (c.cursor.active) {
				const bool continues = c.cursor.Continues(tables.changeLog, tablesGeneration);
				c.cursor.active = false;
				if (!continues) {
					++c.skipped;  // the log started again (a load, a reset): every reader resyncs anyway
				} else {
					++c.checks;
					// What the log says changed since the snapshot, per slot.
					ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> logged;
					for (const auto& change : tables.changeLog.From(c.cursor.position))
						logged[change.slot] |= change.causes;
					for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot) {
						++c.slots;
						const auto now = tables.ColumnsOf(slot);
						const bool grown = slot >= c.snapshot.size();
						// A slot grown since is new: a live one must be in the log at all.
						const std::uint32_t causes = grown ? ((tables.objects[slot].flags & kObjectFree) ? 0u : kChangeAll) : SceneTables::CausesBetween(c.snapshot[slot], now);
						if (!causes)
							continue;
						++c.changed;
						const auto it = logged.find(slot);
						const std::uint32_t missing = grown ? (it == logged.end() ? causes : 0u) : causes & ~(it == logged.end() ? 0u : it->second);
						if (!missing)
							continue;
						++c.missing;
						if (c.first.empty()) {
							std::string names;
							for (std::uint32_t bits = missing; bits; bits &= bits - 1)
								names += fmt::format("{}{}", names.empty() ? "" : "+", kChangeCauseNames[std::countr_zero(bits)]);
							const auto* geometry = tables.objectGeometry[slot];
							c.first = fmt::format("frame {}: slot {} '{}' changed {} with no log entry for it", sceneFrame, slot,
								geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", names);
						}
					}
				}
			}
			if (ParityDue(V::passSerial(store))) {
				c.snapshot.resize(tables.objects.size());
				for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
					c.snapshot[slot] = tables.ColumnsOf(slot);
				c.cursor.Restart(tablesGeneration);
				c.cursor.Advance(tables.changeLog);
			}
		}
	}

	void InstallChangeLogParity()
	{
		Subscribe<ScenePhaseBegin>(OnScenePhaseBegin);
	}
}
