#pragma once

// TruePBR's texture parity, render thread: CaptureParity's (CS_DCLF_CAPTURE_PARITY), each native Lighting draw's PS t80-t91 (its
// landscape layers, extendedRendererState) against the material record's feature textures. Its counters and line are reported in
// CaptureParity::Report ("TruePBR texture parity ...").

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

namespace DCLF::Integrations::TruePBR
{
	/** @brief CaptureParity's t80-t91 checks since its last report (CS_DCLF_CAPTURE_PARITY; render thread). */
	struct DrawTextureParity
	{
		std::uint64_t checks = 0;
		std::uint64_t mismatches = 0;

		/** @brief A mismatch's note (a_message follows the draw's description). */
		using NoteFn = void (*)(void* a_owner, const RE::BSGeometry* a_geometry, std::string a_message);

		/**
		 * @brief Every native draw of a_objectIndex: while TruePBR is loaded, t80-t91 as State::Draw bound them from its landscape hook
		 * against the object's material record (a record without its layers leaves the previous draw's: not compared).
		 */
		void Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
			NoteFn a_note, void* a_owner);
		bool Ok() const { return mismatches == 0; }
		/** @brief The report's line ("TruePBR texture parity ..."). */
		void Report() const;
		void Reset() { checks = mismatches = 0; }
	};
}
