#pragma once

// Advanced Skin's material texture parity, render thread: CaptureParity's (CS_DCLF_CAPTURE_PARITY), each native Lighting draw's PS t71 and
// t74 (bound by State::Draw from its SetupMaterial hook's pending binding) against the object's material record (its contributor's feature
// texture slots, SkinTextures.cpp). Its counters are reported in CaptureParity::Report's skin line, beside the wetness parity's.

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Integrations::AdvancedSkin
{
	/** @brief Advanced Skin's loaded flag, live (CaptureParity::CompareSkin returns before both halves without it). */
	bool Loaded();

	/** @brief CaptureParity's t71/t74 checks since its last report (CS_DCLF_CAPTURE_PARITY; render thread). */
	struct DrawSkinTextureParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;

		/** @brief A mismatch's note (a_message follows the draw's description). */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief A native draw of a_objectIndex, called only while Loaded(): the views bound at t71 and t74 against the material record's, when the
		 * object has bindings and the record has either (otherwise the hook left the previous draw's: not compared).
		 */
		void Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		void Reset() { checks = mismatches = 0; }
	};
}
