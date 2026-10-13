#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Observers.h"

// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's now.

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
			// A particle member's row is its snapshot's (ParticleSnapshots), not the engine's transforms now.
			if ((a_tables.objects[slot].flags & kObjectSkinned) && a_tables.objectGeometry[slot] &&
				a_tables.objectGeometry[slot]->GetType().get() == RE::BSGeometry::Type::kParticles)
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
			if (!(a_tables.objects[slot].flags & kObjectSkinned) || slot >= a_tables.boneRows.size() || !a_tables.boneRows[slot])
				continue;
			const std::uint32_t boneRows = a_tables.boneRows[slot];
			const auto at = PaletteRowsOf(a_tables.boneOffset[slot], boneRows);
			const float* current = nullptr;
			const float* previous = nullptr;
			if (std::size_t(at.previous) + boneRows > palettes.size() || !FrameValues::SkinPalettes(*a_tables.objectGeometry[slot], boneRows, current, previous))
				continue;
			++s.paletteChecks;
			const std::size_t bytes = std::size_t(boneRows) * sizeof(FrameValues::PaletteRow);
			if ((std::memcmp(&palettes[at.current], current, bytes) != 0 || std::memcmp(&palettes[at.previous], previous, bytes) != 0) && s.paletteDiffers++ == 0) {
				const auto* geometry = a_tables.objectGeometry[slot];
				s.paletteFirst = fmt::format("slot {} '{}' ({} rows)", slot, geometry->name.c_str() ? geometry->name.c_str() : "?", boneRows);
			}
		}
	}
}

#endif
