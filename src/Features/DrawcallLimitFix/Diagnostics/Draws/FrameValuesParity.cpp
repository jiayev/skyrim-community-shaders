// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's now.

#include "DrawsState.h"
#include "Observers.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		// The first of a row's fields that differs.
		const char* DifferingField(const BindlessPlacement& a_expected, const BindlessPlacement& a_got)
		{
			const auto differs = [](const auto& a_a, const auto& a_b) { return std::memcmp(a_a, a_b, sizeof(a_a)) != 0; };
			return differs(a_expected.world, a_got.world)                 ? "world" :
			       differs(a_expected.previousWorld, a_got.previousWorld) ? "previous world" :
			       differs(a_expected.bound, a_got.bound)                 ? "bound" :
			       differs(a_expected.sunEntry, a_got.sunEntry)           ? "sun entry" :
			       differs(a_expected.lodFadeNode, a_got.lodFadeNode)     ? "fade node" :
			                                                                "forward sort";
		}

		// A differing row's cause key: the first field that differs, the geometry's name and owner, and its fade node's name and kind.
		void NoteCause(DrawsState::FrameValuesParity& a_s, std::uint32_t a_slot, const RE::BSGeometry* a_geometry, const BindlessPlacement& a_expected,
			const BindlessPlacement& a_got)
		{
			if (!a_geometry)
				return;
			const char* what = DifferingField(a_expected, a_got);
			const auto* ref = a_geometry->GetUserData();
			const auto* base = ref ? ref->GetBaseObject() : nullptr;
			const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
			const auto* fadeNode = property ? property->fadeNode : nullptr;
			// The chain up to the nearest BSOrderedNode: the node classes on it (a billboard rotates in the cull), and the ordered node's.
			std::string chain;
			const RE::NiAVObject* ordered = nullptr;
			for (const RE::NiAVObject* at = a_geometry->parent; at; at = at->parent) {
				const auto* rtti = at->GetRTTI();
				const std::string_view name = rtti ? rtti->GetName() : "?";
				if (name != "NiNode")
					chain += fmt::format("{}{}", chain.empty() ? "" : "<", name);
				if (name == "BSOrderedNode")
					ordered = at;
				if (name == "BSOrderedNode" || at == fadeNode)
					break;
			}
			// Whether the ordered node holds a billboard anywhere below it (the cull turns it: the node's bound follows the camera).
			const auto holdsBillboard = [](const auto& a_self, const RE::NiAVObject* a_object, int a_depth) -> bool {
				const auto* node = a_object && a_depth < 12 ? const_cast<RE::NiAVObject*>(a_object)->AsNode() : nullptr;
				if (!node)
					return false;
				if (const auto* rtti = node->GetRTTI(); rtti && std::string_view(rtti->GetName()) == "NiBillboardNode")
					return true;
				for (const auto& child : node->GetChildren())
					if (a_self(a_self, child.get(), a_depth + 1))
						return true;
				return false;
			};
			if (ordered)
				chain += holdsBillboard(holdsBillboard, ordered, 0) ? " holding a billboard" : " holding no billboard";
			auto key = fmt::format("{} of '{}' {}/{} (property {}, fade node '{}' {}, up: {})", what, a_geometry->name.c_str(),
				ref ? magic_enum::enum_name(ref->GetFormType()) : "no ref", base ? magic_enum::enum_name(base->GetFormType()) : "no base",
				property ? property->GetRTTI()->GetName() : "none", fadeNode ? fadeNode->name.c_str() : "", fadeNode ? fadeNode->GetRTTI()->GetName() : "none", chain);
			auto& cause = a_s.causes[std::move(key)];
			if (cause.first++ == 0) {
				const auto& e = a_expected;
				const auto& g = a_got;
				cause.second = fmt::format("slot {} ref {:08x}: engine world t ({} {} {}) sort ({} {} {} {:#x}), rows world t ({} {} {}) sort ({} {} {} {:#x})", a_slot,
					ref ? ref->GetFormID() : 0u, e.world[3], e.world[7], e.world[11], e.forwardSort[0], e.forwardSort[1], e.forwardSort[2],
					std::bit_cast<std::uint32_t>(e.forwardSort[3]), g.world[3], g.world[7], g.world[11], g.forwardSort[0], g.forwardSort[1], g.forwardSort[2],
					std::bit_cast<std::uint32_t>(g.forwardSort[3]));
			}
		}
	}

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
			if (std::memcmp(&expected, &rows[slot], sizeof(expected)) != 0)
				NoteCause(s, slot, a_tables.objectGeometry[slot], expected, rows[slot]);
			if (std::memcmp(&expected, &rows[slot], sizeof(expected)) != 0 && s.differs++ == 0) {
				const auto& got = rows[slot];
				const char* what = DifferingField(expected, got);
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
