#pragma once

// Fades' part of the diagnostic capture outside the scene lane: the sizes of the containers the render thread, the write-back task and
// the producers keep (FadeCensus.cpp). The scene lane's census is Coordinator::FadeCensus (Features/Fades' "fade roots" registration at
// Api::ReportPoint::CaptureCensus, FadeRoots.cpp).

#include <string>
#include <vector>

namespace DCLF::Fades
{
	/**
	 * @brief Render thread: sizes of Fades' growable containers outside the scene lane, one "<name> <value>" line each: the write-back's
	 * pending batches, waiting stores and per-root frames, the seed copies waiting for a kick or owed by a refused lease, and the writer
	 * set. Reads atomics and takes the write-back's census lock once; nothing is reset.
	 */
	std::vector<std::string> RenderGrowthLines();
}
