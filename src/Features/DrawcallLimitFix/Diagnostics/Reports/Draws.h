#pragma once

#include <array>
#include <cstdint>
#include <string>

// The draws area's report lines (Diagnostics/Reports/Draws*.cpp), taken from the modules' counters at the render thread's report.

namespace DCLF::Reports
{
	/** @brief The frame values' producer and its parity (CS_DCLF_PERSISTENT_PARITY), or empty when nothing ran. */
	std::string FrameValuesLine();
	/** @brief The frame data through the render thread (bytes a frame, by where, largest first), or empty. */
	std::string FrameDataLine();
	/** @brief CS_DCLF_PERSISTENT_PARITY: the constant tables against ShaderCache's since the last call (logged; nothing unchecked). */
	void ConstantTableLines();
	/** @brief CS_DCLF_PASS_STATS: the main passes' pipeline statistics per frame (Z-prepass, its phase 2, colour), over a_samples frames each. */
	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 4>& a_totals, const std::array<std::uint32_t, 4>& a_samples);
	/** @brief The colour commits of the report window whose sun test ran without the sun's cascades (a_empty), logged. */
	void ColourSunTestLine(std::uint32_t a_empty);
}
