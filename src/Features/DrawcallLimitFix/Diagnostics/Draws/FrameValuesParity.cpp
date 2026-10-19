// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's now.

#include "DrawsState.h"
#include "Observers.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

namespace DCLF::Diagnostics
{
	void CheckFrameValues(const SceneTables& a_tables, const PlacementPlan* a_fresh)
	{
		// A diagnostics frame: the render thread waits for the frame's producer.
		const auto* producedRows = FrameValues::Get().RowsIfDone(true);
		if (!producedRows)
			return;
		const auto& rows = *producedRows;
		const auto& palettes = *FrameValues::Get().PalettesIfDone();
		auto& s = DrawsState::Get().frameValues;
		std::vector<std::uint8_t> fresh(rows.size(), 0);
		if (a_fresh)
			for (const auto& item : a_fresh->written) {
				if (item.slot < fresh.size())
					fresh[item.slot] = 1;
				if (item.layerSlot < fresh.size())
					fresh[item.layerSlot] = 1;
			}
		const std::size_t count = std::min(rows.size(), a_tables.objects.size());
		BindlessPlacement expected;
		for (std::uint32_t slot = 0; slot < count; ++slot) {
			if (!FrameValues::SampleSlot(a_tables, slot, expected))
				continue;
			// A palette rows member's row is its class's (a particle system's snapshot's, Features/Particles), not the engine's transforms
			// now: the item class the coordinator plans it as (Coordinator::PlacedKeyOf: a palette block of a skinless geometry of the class's
			// type).
			const bool paletteBlock = (a_tables.objects[slot].flags & kObjectPaletteBlock) && slot < a_tables.boneRows.size() && a_tables.boneRows[slot];
			if (auto* geometry = a_tables.objectGeometry[slot]; geometry &&
				Extensions::PlannedItemClass(paletteBlock, static_cast<std::uint8_t>(geometry->GetType().get()), geometry->GetGeometryRuntimeData().skinInstance != nullptr) !=
					Api::FrameValueItemClass::None)
				continue;
			++s.checks;
			if (fresh[slot]) {
				++s.fresh;
				continue;
			}
			if (std::memcmp(&expected, &rows[slot], sizeof(expected)) != 0 && s.differs++ == 0) {
				const auto& got = rows[slot];
				const char* what = std::memcmp(expected.world, got.world, sizeof(expected.world)) ? "world" :
				                   std::memcmp(expected.previousWorld, got.previousWorld, sizeof(expected.previousWorld)) ? "previous world" :
				                   std::memcmp(expected.bound, got.bound, sizeof(expected.bound)) ? "bound" :
				                   std::memcmp(expected.sunEntry, got.sunEntry, sizeof(expected.sunEntry)) ? "sun entry" :
				                                                                                               "fade node";
				const auto* geometry = a_tables.objectGeometry[slot];
				s.first = fmt::format("slot {} '{}': {} (engine x {:.2f} r {:.2f}, rows x {:.2f} r {:.2f})", slot, geometry->name.c_str() ? geometry->name.c_str() : "?",
					what, expected.world[3], expected.bound[3], got.world[3], got.bound[3]);
			}
			// A skinned record's palettes, as this frame's update left them.
			if (!(a_tables.objects[slot].flags & kObjectPaletteBlock) || slot >= a_tables.boneRows.size() || !a_tables.boneRows[slot])
				continue;
			const std::uint32_t boneRows = a_tables.boneRows[slot];
			const auto at = PaletteRowsOf(a_tables.boneOffset[slot], boneRows);
			const float* current = nullptr;
			const float* previous = nullptr;
			if (std::size_t(at.previous) + boneRows > palettes.size() || !SkinPalettes::Of(*a_tables.objectGeometry[slot], boneRows, current, previous))
				continue;
			++s.paletteChecks;
			const std::size_t bytes = std::size_t(boneRows) * sizeof(FrameValues::PaletteRow);
			if ((std::memcmp(&palettes[at.current], current, bytes) != 0 || std::memcmp(&palettes[at.previous], previous, bytes) != 0) && s.paletteDiffers++ == 0) {
				const auto* geometry = a_tables.objectGeometry[slot];
				s.paletteFirst = fmt::format("slot {} '{}' ({} rows)", slot, geometry->name.c_str() ? geometry->name.c_str() : "?", boneRows);
			}
		}
	}

	void CheckShadingSampled(const SceneTables& a_tables)
	{
		// Every resident slot's shading row is sampled once named (NameShading: a static object's accumulate patch, the only time it is),
		// and every sampler sets written. A row still zero at two checks running was never sampled, and draws with alpha 0: a refused
		// engine-read lease's item, before FrameValues handed those back, left every decal of a session drawing nothing. CS_DCLF_SET_PARITY,
		// on the colour epoch, never waiting for the producer (a frame whose rows are not done is skipped): waiting keeps every lease.
		const auto* shading = FrameValues::Get().ShadingIfDone(false);
		if (!shading)
			return;
		auto& s = DrawsState::Get().frameValues;
		auto& before = DrawsState::Get().shadingUnsampledBefore;
		const std::size_t count = std::min(a_tables.objects.size(), a_tables.residentSlot.size());
		before.resize(count, 0);
		for (std::uint32_t slot = 0; slot < count; ++slot) {
			const bool resident = a_tables.residentSlot[slot] && !(a_tables.objects[slot].flags & (kObjectNoBindings | kObjectFree));
			const bool unsampled = resident && (slot >= shading->size() || (*shading)[slot].written == 0);
			s.shadingChecks += resident ? 1 : 0;
			if (unsampled && before[slot] && s.shadingUnsampled++ == 0) {
				const auto* geometry = slot < a_tables.objectGeometry.size() ? a_tables.objectGeometry[slot] : nullptr;
				s.shadingFirst = fmt::format("slot {} '{}' (flags {:08X})", slot, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
					a_tables.objects[slot].flags);
			}
			before[slot] = unsampled ? 1 : 0;
		}
	}
}
