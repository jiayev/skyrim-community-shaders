#pragma once

// Texture streaming's part in DCLF (the core is src/TextureStreaming, which knows nothing of DCLF): the window builds as graph work, the swaps
// reaching the material bindings, and the demand: the distance floor from the published scene's bounds, and the shaders' feedback (their UV
// footprint per material), corrected on top. Everything but the feedback pass runs on the streaming worker's low-priority thread.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Streaming
{
	Api::Feature Feature();

	/** @brief A test's: logs how many material views draw their shell's current window, and samples of those that do not. */
	void ReportBindings();
}
