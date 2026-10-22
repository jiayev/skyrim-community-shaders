// Light Limit Fix's StrictLightData parity (LightLimitFix.h's DrawLightDataParity), CaptureParity's. Render thread.

#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "Features/LightLimitFix.h"

#include <algorithm>

namespace DCLF::Integrations::LightLimitFix
{
	namespace
	{
		std::string NodeName(const RE::NiAVObject* a_object)
		{
			const auto* rtti = a_object->GetRTTI();
			return fmt::format("{}:'{}'", rtti && rtti->name ? rtti->name : "?", a_object->name.c_str() ? a_object->name.c_str() : "");
		}

		/**
		 * @brief A room mismatch's detail: the live parent chain up to the native room node (LLF's GetParentRoomNode, as its SetupGeometry
		 * read it), and the room node a_index names in LLF's map now, with whether it is on the chain.
		 */
		std::string RoomDetail(const RE::BSGeometry* a_geometry, std::int32_t a_index)
		{
			const auto& llf = globals::features::lightLimitFix;
			std::string chain;
			const RE::NiNode* nativeRoom = llf.GetRoomNode(const_cast<RE::BSGeometry*>(a_geometry));
			std::uint32_t hops = 0;
			for (const RE::NiAVObject* object = a_geometry->parent; object; object = object->parent) {
				if (hops++ < 6 || object == nativeRoom)
					chain += " under " + NodeName(object);
				if (object == nativeRoom)
					break;
			}
			const RE::NiNode* named = nullptr;
			for (const auto& [node, index] : llf.roomNodes)
				if (static_cast<std::int32_t>(index) == a_index)
					named = node;
			bool onChain = false;
			for (const RE::NiAVObject* object = a_geometry->parent; object && named && !onChain; object = object->parent)
				onChain = object == named;
			return fmt::format("'{}':{} (native room node {}; DCLF's index {} names {}{})", a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", chain,
				static_cast<const void*>(nativeRoom), a_index, named ? NodeName(named) : std::string("no node"),
				named ? fmt::format(" {}, {}", static_cast<const void*>(named), onChain ? "an ancestor" : "not an ancestor") : std::string());
		}
	}

	void DrawLightDataParity::Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, const std::uint32_t (*a_drawn)[2],
		bool a_moved, bool a_reassigned, NoteFn a_note, void* a_owner)
	{
		// LLF's StrictLightData as its SetupGeometry hook left it. The record's shadow mask is dead (DCLF's is the draw's): printed 0.
		if (globals::features::lightLimitFix.loaded) {
			const auto& native = globals::features::lightLimitFix.strictLightDataTemp;
			const std::int32_t expected = a_tables.ancestorValue[a_objectIndex].value;
			const bool roomDiffers = native.RoomIndex != expected;
			++checks;
			if (native.NumStrictLights != 0 || roomDiffers) {
				++mismatches;
				a_note(a_owner, a_geometry, fmt::format("strict light data: DCLF room {} shadow mask {:X}, native {} strict lights, room {} shadow mask {:X}",
					expected, 0u, native.NumStrictLights, native.RoomIndex, native.ShadowBitMask));
				if (roomDiffers && a_geometry && roomDetails.size() < 4 && std::ranges::find(roomDetailed, a_geometry) == roomDetailed.end()) {
					roomDetailed.push_back(a_geometry);
					roomDetails.push_back(fmt::format("DCLF room {}, native {}: {}", expected, native.RoomIndex, RoomDetail(a_geometry, expected)));
				}
			}
			// The shadow lights a Lighting draw applies at all (LightLimitFix.hlsli, IsLightIgnored: a shadow light whose bit is clear is
			// skipped): the four mask channels.
			if (a_drawn) {
				const std::uint32_t ours = (*a_drawn)[0] & 0xFu, theirs = native.ShadowBitMask & 0xFu;
				++shadowChecks;
				shadowReassignedChecks += a_reassigned ? 1 : 0;
				if (ours != theirs) {
					++shadowMismatches;
					gatedMismatches += ((ours ^ theirs) & (*a_drawn)[1]) ? 1 : 0;
					shadowRoomMismatches += roomDiffers ? 1 : 0;
					shadowMovedMismatches += a_moved ? 1 : 0;
					shadowReassignedMismatches += a_reassigned && !a_moved ? 1 : 0;
					auto& first = a_moved || a_reassigned ? shadowFirst : shadowFirstStill;
					if (first.empty())
						first = fmt::format("object {} '{}': DCLF {:X} (gated {:X}, room {}), native {:X} (room {})", a_objectIndex,
							a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", ours, (*a_drawn)[1] & 0xFu, expected, theirs, native.RoomIndex);
				}
			}
		}
	}

	void DrawLightDataParity::Report() const
	{
		logger::info("[DCLF] light data parity {}: {} draws checked, {} with different StrictLightData", mismatches == 0 ? "OK" : "MISMATCH", checks,
			mismatches);
		for (const auto& detail : roomDetails)
			logger::info("[DCLF]   light data room: {}", detail);
		logger::info("[DCLF] light data parity, shadow lights (DCLF's draws' local shadow mask, CS_DCLF_BUILD_PARITY's readback, against the native pass's "
					 "ShadowBitMask): {} draws checked ({} of them with channels reassigned since the readback), {} differ ({} in a portal-strict light's bit, {} of a draw whose room differs too, {} of a receiver that "
					 "moved since the readback, {} more of one whose channels were reassigned since){}{}{}{}{}",
			shadowChecks, shadowReassignedChecks, shadowMismatches, gatedMismatches, shadowRoomMismatches, shadowMovedMismatches, shadowReassignedMismatches, shadowFirstStill.empty() ? "" : "; first still: ", shadowFirstStill,
			shadowFirst.empty() ? "" : "; first moved or reassigned: ", shadowFirst, shadowMismatches > shadowMovedMismatches + shadowReassignedMismatches ? " <- SHADOW MASK" : " <- OK");
	}
}
