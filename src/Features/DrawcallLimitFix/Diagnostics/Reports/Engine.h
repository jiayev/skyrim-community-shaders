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
	/** @brief Render thread, Present (Reports::RenderThread): the sun accumulation's and the sun entry exclusion's lines. */
	void SunAccumulationLines(std::uint32_t a_frame);

	/** @brief The shadow views' parity lines (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY), logged, their tallies reset: every 300 frames with such views. */
	void SunViewsLines();
	void LightViewsLines();
	/** @brief ... with the focus views' membership line. */
	void FocusViewsLines();

	/** @brief The portal views' parity line, logged (every 300 builds with CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY). */
	void PortalViewsLine();
	/** @brief Main::IsRoomVisible's answers, logged (every 300 published VisibleSets). */
	void RoomVisibilityLines();
	/** @brief The light rooms' and light selection's parity lines, logged (every 300 selections with either parity). */
	void LightSelectionLines();

	/** @brief The fade writers' line (newline-terminated); empty while nothing was skipped. */
	std::string FadeWritersLine();

	/** @brief The engine-read window's closes, waits and refused leases; empty while nothing happened. */
	std::string EngineReadWindowLine();
	/** @brief CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads; empty while not installed. */
	std::string ReleaseGuardLine();
	/** @brief The actors' animation stamp (newline-terminated); empty when it did not run. */
	std::string ActorRateLine();
	/** @brief CS_DCLF_ACCUMULATED_PARITY: the animation gate against DCLF's prediction (newline-terminated); empty without the switch. */
	std::string AnimRateLine();
	/** @brief The LOD gates' engine side over a_frames frames; empty while nothing happened. */
	std::string LodGatesLine(std::uint32_t a_frames);
}
