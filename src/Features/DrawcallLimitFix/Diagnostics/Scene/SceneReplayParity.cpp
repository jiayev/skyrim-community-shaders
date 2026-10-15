// CS_DCLF_PERSISTENT_PARITY: every 60th publication, the snapshot the replay brought up to date against the tables it was made from: the
// material records, the object and geometry slots (with the per-object columns replayed by slot beside them: set phases, decal ordinals,
// layer links), the kept families (the pipeline family whether kept or copied) and the logs.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/** @brief The kept table families (a_kept, a bit by registration) against the tables' (Extensions::TableFamilies::Same). */
		void CheckKeptFamilies(std::uint32_t a_kept, const SceneTables& a_snapshot, const SceneTables& a_tables, std::uint64_t& a_differ)
		{
			const auto families = Extensions::RegisteredTableFamilies();
			for (std::uint32_t f = 0; f < families.size(); ++f)
				if (((a_kept >> f) & 1u) && !a_snapshot.families.Same(f, a_tables.families) && a_differ++ == 0)
					logger::error("[DCLF] tables replay (6d): the {} were kept (stamp {}) but differ from the tables'", families[f].name, a_tables.families.StampOf(f));
		}

		void OnTablesReplayed(const TablesReplayed& a_event)
		{
			if (a_event.published % 60 != 0)
				return;
			auto& replay = SceneDiagnostics().replay;
			const auto& snapshot = a_event.snapshot;
			const auto& tables = V::tables(a_event.store);
			const std::size_t slots = tables.materials.size();
			const std::size_t objectSlots = tables.objects.size();
			const std::size_t geometrySlots = tables.geometries.size();
			const std::uint64_t position = a_event.position, geometryPosition = a_event.geometryPosition;
			const bool keepFadeRoots = a_event.keepFadeRoots, keepConstants = a_event.keepConstants;
				++replay.parityChecks;
				replay.parityMaterials += slots;
				for (std::size_t slot = 0; slot < slots; ++slot)
					if (!(snapshot.materials[slot] == tables.materials[slot]) && replay.parityDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): material slot {} differs from the tables (version {})", slot, tables.materialVersion[slot]);
				replay.parityObjects += objectSlots;
				if (snapshot.objects.size() != objectSlots) {
					if (replay.parityObjectsDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): {} object slots against the tables' {}", snapshot.objects.size(), objectSlots);
				} else {
					for (std::uint32_t slot = 0; slot < objectSlots; ++slot)
						if (const auto causes = SceneTables::CausesBetween(snapshot.ColumnsOf(slot), tables.ColumnsOf(slot)); causes && replay.parityObjectsDiffer++ == 0)
							logger::error("[DCLF] tables replay (6d): object slot {} differs from the tables (causes {:#x}; replayed from log {}, now {})", slot, causes,
								position, tables.changeLog.End());
				}
				replay.parityGeometries += geometrySlots;
				if (snapshot.geometries.size() != geometrySlots) {
					++replay.parityGeometriesDiffer;
				} else {
					for (std::uint32_t slot = 0; slot < geometrySlots; ++slot) {
						const auto& a = snapshot.geometryImports[slot];
						const auto& b = tables.geometryImports[slot];
						if ((std::memcmp(&snapshot.geometries[slot], &tables.geometries[slot], sizeof(GeometryBuffers)) != 0 || a.vertexGeneration != b.vertexGeneration ||
								a.indexGeneration != b.indexGeneration || a.vertexOwner != b.vertexOwner || a.indexOwner != b.indexOwner ||
								snapshot.geometrySlotKey[slot] != tables.geometrySlotKey[slot] || snapshot.geometryLayerKey[slot] != tables.geometryLayerKey[slot]) &&
							replay.parityGeometriesDiffer++ == 0)
							logger::error("[DCLF] tables replay (6d): geometry slot {} differs from the tables (replayed from log {}, now {})", slot, geometryPosition,
								tables.geometryLog.End());
					}
				}
				// The kept families against the tables' (vectors by their bytes, maps by their entries, the journal by its version).
				auto sameMember = [](const auto& a_left, const auto& a_right) {
					using T = std::decay_t<decltype(a_left)>;
					if constexpr (requires { a_left.find(a_left.begin()->first); }) {
						if (a_left.size() != a_right.size())
							return false;
						for (const auto& [key, value] : a_left)
							if (const auto it = a_right.find(key); it == a_right.end() || !(it->second == value))
								return false;
						return true;
					} else {
						static_assert(std::is_trivially_copyable_v<typename T::value_type>);
						return a_left.size() == a_right.size() &&
						       (a_left.empty() || std::memcmp(a_left.data(), a_right.data(), a_left.size() * sizeof(a_left[0])) == 0);
					}
				};
				auto sameFamily = [&](auto&& a_family) {
					bool same = true;
					a_family(snapshot, tables, [&](auto& a_x, auto& a_y) { same = same && sameMember(a_x, a_y); });
					return same;
				};
				CheckKeptFamilies(a_event.keptFamilies, snapshot, tables, replay.parityFamiliesDiffer);
				if (keepConstants) {
					bool same = snapshot.pipelineConstants.size() == tables.pipelineConstants.size() && snapshot.techniqueConstants.size() == tables.techniqueConstants.size();
					for (std::size_t p = 0; same && p < tables.pipelineConstants.size(); ++p)
						same = snapshot.pipelineConstants[p].version == tables.pipelineConstants[p].version && snapshot.pipelineConstants[p].valid == tables.pipelineConstants[p].valid;
					for (std::size_t r = 0; same && r < tables.techniqueConstants.size(); ++r)
						same = snapshot.techniqueConstants[r].constantsVersion == tables.techniqueConstants[r].constantsVersion &&
						       snapshot.techniqueConstants[r].bindingVersion == tables.techniqueConstants[r].bindingVersion && snapshot.techniqueConstants[r].valid == tables.techniqueConstants[r].valid;
					if (!same && replay.parityFamiliesDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): the constants were kept (stamp {}) but differ from the tables'", tables.constantsStamp);
				}
				if (keepFadeRoots && !sameFamily([](auto& a_x, auto& a_y, auto&& a_member) { SceneTables::FadeRootFamily(a_x, a_y, a_member); }) && replay.parityFamiliesDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): the fade roots were kept (stamp {}) but differ from the tables'", tables.fadeRootsStamp);
				if (!sameFamily([](auto& a_x, auto& a_y, auto&& a_member) { SceneTables::PipelineFamily(a_x, a_y, a_member); }) && replay.parityFamiliesDiffer++ == 0)
					logger::error("[DCLF] tables replay (6d): the pipeline family (stamp {}, the snapshot's {}) differs from the tables'", tables.pipelinesStamp, snapshot.pipelinesStamp);
				// The per-object columns replayed by slot beside the logged ones, against the tables' whole.
				auto sameColumn = [&](const char* a_name, const auto& a_left, const auto& a_right) {
					if (sameMember(a_left, a_right))
						return;
					std::size_t at = 0;
					while (at < a_left.size() && at < a_right.size() && a_left[at] == a_right[at])
						++at;
					if (replay.parityObjectsDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): {} differs from the tables' ({} slots against {}; first at slot {}; replayed from log {}, now {})", a_name,
							a_left.size(), a_right.size(), at, position, tables.changeLog.End());
				};
				sameColumn("setPhases", snapshot.setPhases, tables.setPhases);
				sameColumn("decalOrdinal", snapshot.rangeOrdinal, tables.rangeOrdinal);
				sameColumn("layerBase", snapshot.layerBase, tables.layerBase);
				sameColumn("layerOf", snapshot.layerOf, tables.layerOf);
				auto sameLog = [](const auto& a_left, const auto& a_right) {
					return a_left.base == a_right.base && a_left.entries.size() == a_right.entries.size() &&
					       (a_left.entries.empty() || std::memcmp(a_left.entries.data(), a_right.entries.data(), a_left.entries.size() * sizeof(a_left.entries[0])) == 0);
				};
				if (!sameLog(snapshot.changeLog, tables.changeLog) || !sameLog(snapshot.geometryLog, tables.geometryLog) || !sameLog(snapshot.materialLog, tables.materialLog) ||
					!sameLog(snapshot.publicationLog, tables.publicationLog) ||
					!sameLog(snapshot.extrasBlockLog, tables.extrasBlockLog) || snapshot.extraRows.size() != tables.extraRows.size() ||
					(!tables.extraRows.empty() && std::memcmp(snapshot.extraRows.data(), tables.extraRows.data(), tables.extraRows.size() * sizeof(float)) != 0))
					if (replay.parityLogsDiffer++ == 0)
						logger::error("[DCLF] tables replay (6d): a log or the extras rows differ from the tables'");
		}
	}

	void InstallReplayParity()
	{
		Subscribe<TablesReplayed>(OnTablesReplayed);
	}
}
