#pragma once

// CS_DCLF_SCHEDULE_DUMP=1: each epoch's passes as the graph extensions add them (name, pass type, queue, in add order), logged
// whenever an epoch's list changes, for tools/dclf-schedule-diff.py: the schedule rework must leave every epoch's list as it was.

#include <Render/RenderGraph/RenderGraph.h>

#include <memory>
#include <string_view>

namespace DCLF::Diagnostics
{
	/** @brief a_extension as it is, or (CS_DCLF_SCHEDULE_DUMP=1) wrapped so that each gather logs its epochs' changed pass lists. */
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> ScheduleDumped(std::string_view a_extensionId,
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> a_extension);
}
