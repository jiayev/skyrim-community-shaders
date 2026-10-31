#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Reports
{
	/**
	 * @brief The engine area's report lines (Diagnostics/Reports/EngineLines.cpp, EnginePrimaryCull.cpp): each formats what its module counted since the last call
	 * (the module's Take) and resets it. The callers log them at the report schedule's moments (Schedule.h).
	 */

	/** @brief Render thread, Present (Reports::RenderThread): the primary cull's lines, with the scene lists', the culls off, the tree list's and the point lights' culls. */
	void PrimaryCullLines(std::uint32_t a_frame);

	/**
	 * @brief Render thread, inside PrimaryCullLines (its report points' lines): the entries with engine-drawn parts the engine's culls took
	 * a frame, over the window the primary cull's lines just took (the cut's counters; Features/Fades' fade roots line prints it).
	 */
	double CutMixedPerFrame();

	/** @brief The engine-read window's closes, waits and refused leases; empty while nothing happened. */
	std::string EngineReadWindowLine();
	/** @brief CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads; empty while not installed. */
	std::string ReleaseGuardLine();
	/** @brief CS_DCLF_ACCUMULATED_PARITY: the animation gate against DCLF's prediction (newline-terminated); empty without the switch. */
	std::string AnimRateLine();
}
