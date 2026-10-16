#pragma once

// Advanced Skin's wetness parities, render thread: CaptureParity's (CS_DCLF_CAPTURE_PARITY), each native Lighting draw's PS b7 against the
// frame's shading row and its actor ownership against the tables' members; and the persistent parity's (CS_DCLF_PERSISTENT_PARITY, with
// the shading check at Prepass), every member's row against Skin::GetWetness's same-frame value. Their counters and text are reported in
// the parities' own lines (CaptureParity::Report's skin line, SceneFrameLines' shading line).

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct SceneTables;
	struct BindlessShading;
}

namespace DCLF::Integrations::AdvancedSkin
{
	/** @brief CaptureParity's b7 checks since its last report. */
	struct DrawWetnessParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;
		std::uint64_t wetDraws = 0;  // checked draws with sweat or water wetness (x or y)
		std::uint64_t ownerMismatches = 0;

		/** @brief A mismatch's note (a_message follows the draw's description). */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief A native draw of a_objectIndex (Advanced Skin loaded): the SetupGeometry hook's upload (bound only while enabled) against the
		 * frame's row (a_rows: FrameValues::ShadingIfDone, null while not done), and whether its owner is an actor against the tables' members.
		 */
		void Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables, const std::vector<BindlessShading>* a_rows,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0 && ownerMismatches == 0; }
		void Reset() { checks = mismatches = wetDraws = ownerMismatches = 0; }
	};

	/**
	 * @brief The persistent parity's shading check: every member's row (a_rows) against the wetness as the frame's start captured it, but for
	 * a slot named this frame (a_named: its value is the next frame's capture's).
	 */
	void CheckRowWetness(const SceneTables& a_tables, const std::vector<BindlessShading>& a_rows, const std::vector<std::uint8_t>& a_named);
	/** @brief The shading line's wetness part since the last take ("; wetness N meshes, M differ <- ..."), its counters reset. */
	std::string TakeRowWetnessText();
}
