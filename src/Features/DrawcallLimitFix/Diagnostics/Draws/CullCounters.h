#pragma once

// The culling's counters' graph pass (CullCounters.cpp): the colour epoch's count words to the host each execution.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
namespace DCLF::Diagnostics
{
	struct MainGraphPasses;
	/** @brief MainGraphPasses (kAfterColour): adds the colour epoch's counter readback (CounterReadback), always on. */
	void AppendCullCounterPass(const MainGraphPasses& a_passes);
}
#endif
