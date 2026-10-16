#pragma once

// Light Limit Fix (Features/LightLimitFix.h): while it is loaded, a Lighting draw reads its clustered lights (t35-t37) and its
// StrictLightData (PS b3), whose only per-object value in the main pass is the room index. The integration registers what DCLF needs to
// draw both as it does:
// - the "light limit fix" classify input (Api::ClassifyInputContribution: loaded), which Features/Effects' variants and the room read;
// - the room index (Api::AncestorValueContribution): the object's room (its nearest BSMultiBoundRoom or BSPortalSharedNode, LLF's
//   GetParentRoomNode) looked up in LLF's room map, in the object record's first word;
// - its graph buffers (Api::FrameGraphBuffers): the lights, light index list and light grid at t35-t37;
// - its parity (Diagnostics/LightDataParity.cpp): CaptureParity's StrictLightData check.
//
// Core keeps the zeroed StrictLightData block every draw reads (frame-slot layout), the key walk and the column. Included by the
// integration's sources, Diagnostics/Scene/CaptureParity and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Integrations::LightLimitFix
{
	// t35-t37: its lights, light index list and light grid (LightLimitFix.hlsli's), graph buffers at consecutive registers
	// (Features/Manifest.cpp).
	inline constexpr std::uint32_t kLightsRegister = 35;
	inline constexpr std::uint32_t kGraphBuffers = 3;
	// The room index: DCLFObjectRecord's first word (BindlessObject::ancestorValue, DCLFObjects.hlsli's RoomIndex; Features/Manifest.cpp), -1 outside rooms.
	inline constexpr std::uint32_t kRoomRecordWord = 0;
	inline constexpr std::int32_t kNoRoom = -1;

	/** @brief CaptureParity's StrictLightData checks since its last report (CS_DCLF_CAPTURE_PARITY; render thread). */
	struct DrawLightDataParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;  // StrictLightData (LLF, PS b3) differs

		/** @brief A mismatch's note (a_message follows the draw's description). */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief A native draw of a_objectIndex (Light Limit Fix loaded): StrictLightData as its SetupGeometry hook left it against the
		 * tables' room. Not ShadowBitMask: DCLF's comes from BuildDrawsCS's local shadow selection, which draw parity checks
		 * (expectedLocalShadows).
		 */
		void Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		/** @brief The report's line ("light data parity ..."). */
		void Report() const;
		void Reset() { checks = mismatches = 0; }
	};

	// LightLimitFixIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
