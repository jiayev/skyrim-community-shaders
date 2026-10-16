// CS_DCLF_PERSISTENT_PARITY: the sun and light candidate tables (kept by changes) against the candidate sets and the dependents as they
// are, every 60 passes, after each table's update. Also each table's last snapshot (caught up by the table's log) against the table
// index by index, and the indices its published log names since the snapshot of the last check against a comparison of the two.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/** @brief a_table against a_set and the dependents now; the first difference, or empty. */
		std::string CheckCandidateTable(const Scene::Coordinator& a_store, const V::EntrySpaceTable& a_table, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_set, bool a_sun)
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

		/**
		 * @brief An earlier snapshot of a table, by reference and generation: pooled snapshots are reused, so it is read only while it
		 * still is that generation (never held: the pool reuses what only it holds, and a held one would outlive the session).
		 */
		struct EarlierSnapshot
		{
			std::weak_ptr<const EntrySpace> snapshot;
			std::uint32_t generation = 0;

			std::shared_ptr<const EntrySpace> Get() const
			{
				auto held = snapshot.lock();
				return held && held->generation == generation ? held : nullptr;
			}
			void Set(const std::shared_ptr<const EntrySpace>& a_snapshot)
			{
				snapshot = a_snapshot;
				generation = a_snapshot ? a_snapshot->generation : 0;
			}
		};
		struct EarlierSnapshots
		{
			EarlierSnapshot event, check;  // the last pass's (one publication back at most), the last check's
		};

		/** @brief The indices a_to's log names since a_from against a comparison of the two; the first difference, or empty. */
		std::string CheckCandidateLog(const std::shared_ptr<const EntrySpace>& a_from, const EntrySpace& a_to)
		{
			if (!a_from || a_from.get() == &a_to)
				return {};
			std::vector<std::uint32_t> logged, scanned;
			ChangedEntriesByLog(a_from.get(), a_to, logged);
			ChangedEntriesByScan(a_from.get(), a_to, scanned);
			if (logged == scanned)
				return {};
			return fmt::format("snapshot: the log names {} indices from generation {} to {}, a comparison {}", logged.size(), a_from->generation, a_to.generation, scanned.size());
		}

		void OnCandidatesUpdated(const CandidatesUpdated& a_event)
		{
			const auto& store = a_event.store;
			static EarlierSnapshots sun, light;
			auto& earlier = a_event.sun ? sun : light;
			const auto& table = a_event.sun ? V::sunTable(store) : V::lightTable(store);
			const auto snapshot = table.last;
			const auto previous = earlier.event.Get();
			earlier.event.Set(snapshot);
			if (!ParityDue(V::passSerial(store), 7))
				return;
			auto& parity = SceneDiagnostics().candidates;
			++parity.checks;
			auto why = a_event.sun ? CheckCandidateTable(store, V::sunTable(store), V::sunCandidateSet(store), true) :
			                         CheckCandidateTable(store, V::lightTable(store), V::lightCandidateSet(store), false);
			// The last snapshot (caught up by the log) against the table, and its log against comparisons with earlier snapshots.
			if (why.empty() && snapshot && !table.changed)
				if (auto differ = table.CheckSnapshot(*snapshot); !differ.empty())
					why = "snapshot: " + differ;
			if (why.empty() && snapshot)
				why = CheckCandidateLog(previous, *snapshot);
			if (why.empty() && snapshot)
				why = CheckCandidateLog(earlier.check.Get(), *snapshot);
			earlier.check.Set(snapshot);
			if (!why.empty() && parity.mismatches++ < 8)
				logger::warn("[DCLF] {} candidates parity at frame {}: {} <- CANDIDATES", a_event.sun ? "sun" : "light", V::sceneFrame(store), why);
		}
	}

	void InstallCandidateParity()
	{
		Subscribe<CandidatesUpdated>(OnCandidatesUpdated);
	}
}
