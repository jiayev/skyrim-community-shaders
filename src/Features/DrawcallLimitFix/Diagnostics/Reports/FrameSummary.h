#pragma once

// The menu's "This frame" lines, built in one place so the diagnostic capture writes the same text the panel shows.

#include <string>
#include <vector>

namespace DCLF::Reports
{
	/**
	 * @brief Render thread. The "This frame" panel's lines: the active toggles, the tracked set, this frame's draws, GPU timings and
	 * shadow views. Reads only; no counter is reset.
	 */
	std::vector<std::string> FrameSummary();
}
