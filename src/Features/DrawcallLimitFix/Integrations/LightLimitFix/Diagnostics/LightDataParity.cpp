// Light Limit Fix's StrictLightData parity (LightLimitFix.h's DrawLightDataParity), CaptureParity's. Render thread.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "Features/LightLimitFix.h"

namespace DCLF::Integrations::LightLimitFix
{
	void DrawLightDataParity::Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, NoteFn a_note, void* a_owner)
	{
		// LLF's StrictLightData as its SetupGeometry hook left it. The record's shadow mask is dead (DCLF's is the draw's): printed 0.
		if (globals::features::lightLimitFix.loaded) {
			const auto& native = globals::features::lightLimitFix.strictLightDataTemp;
			const std::int32_t expected = a_tables.ancestorValue[a_objectIndex].value;
			++checks;
			if (native.NumStrictLights != 0 || native.RoomIndex != expected) {
				++mismatches;
				a_note(a_owner, a_geometry, fmt::format("strict light data: DCLF room {} shadow mask {:X}, native {} strict lights, room {} shadow mask {:X}",
					expected, 0u, native.NumStrictLights, native.RoomIndex, native.ShadowBitMask));
			}
		}
	}

	void DrawLightDataParity::Report() const
	{
		logger::info("[DCLF] light data parity {}: {} draws checked, {} with different StrictLightData", mismatches == 0 ? "OK" : "MISMATCH", checks,
			mismatches);
	}
}
