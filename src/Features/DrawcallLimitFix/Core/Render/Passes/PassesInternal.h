#pragma once

// The passes Core registers (CorePasses.cpp), made by the sources that define them; the schedule's executor adds them to the graph
// (Epochs/ScheduleExecutor.cpp). Included by the sources of this folder only, after Internal.h.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF::Draws
{
	// Build.cpp: BuildDraws (a_phase 0 follows the segment's shape; 2 is the phase 2 retest), the sort's scatter.
	std::shared_ptr<org::RenderPass> MakeBuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase = 0, bool a_queries = false);
	std::shared_ptr<org::RenderPass> MakeSortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment);

	// VisibilityEvents.cpp: the visibility events.
	std::shared_ptr<org::RenderPass> MakeVisibilityEventsPass(std::shared_ptr<Resources> a_resources);

	// MainExtension.cpp: the buffers a main commit may latch into (LatchedUploads); its latched copies declare them.
	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources);
}
