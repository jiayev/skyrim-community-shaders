#pragma once

// The culling's counters' graph pass (CullCounters.cpp): the colour epoch's count words to the host each execution.

namespace DCLF::Diagnostics
{
	struct MainGraphPasses;
	/** @brief MainGraphPasses (kAfterColour): adds the colour epoch's counter readback (CounterReadback), always on. */
	void AppendCullCounterPass(const MainGraphPasses& a_passes);
}
