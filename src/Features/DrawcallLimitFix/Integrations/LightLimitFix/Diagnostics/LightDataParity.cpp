// Light Limit Fix's StrictLightData parity (LightLimitFix.h's DrawLightDataParity), CaptureParity's. Render thread.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "Features/LightLimitFix.h"

namespace DCLF::Integrations::LightLimitFix
{
	void DrawLightDataParity::Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, const std::uint32_t (*a_drawn)[2],
		NoteFn a_note, void* a_owner)
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
			// The shadow lights a Lighting draw applies at all (LightLimitFix.hlsli, IsLightIgnored: a shadow light whose bit is clear is
			// skipped): the four mask channels.
			if (a_drawn) {
				const std::uint32_t ours = (*a_drawn)[0] & 0xFu, theirs = native.ShadowBitMask & 0xFu;
				++shadowChecks;
				if (ours != theirs) {
					++shadowMismatches;
					gatedMismatches += ((ours ^ theirs) & (*a_drawn)[1]) ? 1 : 0;
					if (shadowFirst.empty())
						shadowFirst = fmt::format("object {} '{}': DCLF {:X} (gated {:X}), native {:X}", a_objectIndex, a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?",
							ours, (*a_drawn)[1] & 0xFu, theirs);
				}
			}
		}
	}

	void DrawLightDataParity::Report() const
	{
		logger::info("[DCLF] light data parity {}: {} draws checked, {} with different StrictLightData", mismatches == 0 ? "OK" : "MISMATCH", checks,
			mismatches);
		logger::info("[DCLF] light data parity, shadow lights (DCLF's draws' local shadow mask, CS_DCLF_BUILD_PARITY's readback, against the native pass's "
					 "ShadowBitMask): {} draws checked, {} differ ({} in a portal-strict light's bit){}{}{}",
			shadowChecks, shadowMismatches, gatedMismatches, shadowFirst.empty() ? "" : "; first: ", shadowFirst, shadowMismatches ? " <- SHADOW MASK" : " <- OK");
	}
}
