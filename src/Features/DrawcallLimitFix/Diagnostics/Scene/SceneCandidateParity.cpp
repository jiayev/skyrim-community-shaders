#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_PERSISTENT_PARITY: the sun and light candidate tables (kept by changes) against the candidate sets and the dependents as they
// are, every 60 passes, after each table's update.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/** @brief a_table against a_set and the dependents now; the first difference, or empty. */
		std::string CheckCandidateTable(const Scene::Coordinator& a_store, const V::CandidateTable& a_table, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_set, bool a_sun)
		{
			// The table against the set and the dependents as they are (what a snapshot made whole would hold).
			const auto& rootDependents = V::rootDependents(a_store);
			const auto& tracked = V::tracked(a_store);
			if (a_table.index.size() != a_set.size())
				return fmt::format("{} entries, the set {}", a_table.index.size(), a_set.size());
			for (const auto* root : a_set) {
				const auto it = a_table.index.find(root);
				if (it == a_table.index.end())
					return fmt::format("a candidate without an entry ({})", static_cast<const void*>(root));
				const auto* dependents = a_sun ? (rootDependents.contains(root) ? &rootDependents.at(root) : nullptr) : V::LightDependentsOf(a_store, root);
				const auto& rows = a_table.geometries[it->second];
				std::size_t owned = 0;
				for (const auto* geometry : dependents ? *dependents : std::vector<RE::BSGeometry*>{}) {
					const auto g = a_table.geometryIndex.find(geometry);
					if (g == a_table.geometryIndex.end())
						return fmt::format("a dependent of {} with no geometry row", static_cast<const void*>(root));
					if (a_table.geometryEntry[g->second] != it->second)
						continue;  // another entry's (listed there first)
					++owned;
					if (a_table.geometrySlot[g->second] != a_store.FindObject(geometry))
						return fmt::format("a geometry of {} with object {}, now {}", static_cast<const void*>(root), a_table.geometrySlot[g->second], a_store.FindObject(geometry));
					if (a_sun) {
						const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
						const std::uint8_t allows = entry != tracked.end() ? V::PrimaryVerdict(a_store, entry->second, *geometry) : 0;
						if (a_table.primaryGeometry[g->second] != allows)
							return fmt::format("a geometry of {} with its primary verdict {}, now {}", static_cast<const void*>(root), a_table.primaryGeometry[g->second], allows);
					}
				}
				if (owned != rows.size())
					return fmt::format("{} with {} geometry rows, {} dependents its own", static_cast<const void*>(root), rows.size(), owned);
			}
			return {};
		}

		void OnCandidatesUpdated(const CandidatesUpdated& a_event)
		{
			const auto& store = a_event.store;
			if (!ParityDue(V::passSerial(store), 7))
				return;
			auto& parity = SceneDiagnostics().candidates;
			++parity.checks;
			if (auto why = a_event.sun ? CheckCandidateTable(store, V::sunTable(store), V::sunCandidateSet(store), true) :
			                             CheckCandidateTable(store, V::lightTable(store), V::lightCandidateSet(store), false);
				!why.empty() && parity.mismatches++ < 8)
				logger::warn("[DCLF] {} candidates parity at frame {}: {} <- CANDIDATES", a_event.sun ? "sun" : "light", V::sceneFrame(store), why);
		}
	}

	void InstallCandidateParity()
	{
		Subscribe<CandidatesUpdated>(OnCandidatesUpdated);
	}
}
