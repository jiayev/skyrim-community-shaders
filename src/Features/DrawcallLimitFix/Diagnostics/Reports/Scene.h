#pragma once

#include <cstdint>
#include <string>

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Reports
{
	/**
	 * @brief The scene area's report lines (Diagnostics/Reports/Scene*.cpp): each formats what Coordinator and the scene observers counted
	 * since the last call and resets it, at the report schedule's moments (Schedule.h).
	 */

	/** @brief The coordinator, at a pass's end: its own lines, when its frame a_sceneFrame crossed into a new window (a pass may skip frames). */
	void ScenePassEnd(Scene::Coordinator& a_store, std::uint32_t a_sceneFrame);
	/** @brief Render thread, the frame's report: the lines of what the render thread keeps (newline-terminated). */
	std::string SceneFrameLines();
	/** @brief Render thread, the frame's start (before the components' adoption): the LOD gate releases' lines, when frame a_frame crossed into a new window. */
	void GateReleases(std::uint32_t a_frame);
}
