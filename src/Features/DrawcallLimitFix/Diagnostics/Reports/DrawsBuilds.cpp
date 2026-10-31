#include "Draws.h"
#include "Schedule.h"

#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"

// The draws' build lines (constant tables, pass statistics, the colour sun test), for the render thread's report (Frame.cpp).

namespace DCLF::Reports
{
	void ConstantTableLines()
	{
		const auto ct = std::exchange(Diagnostics::DrawsState::Get().constantTables, {});
		if (ct.checks)
			logger::info("[DCLF] constant tables (T6b2c: DCLF's modules' against ShaderCache's): {} checks, {} entries ({} without ShaderCache's shaders yet), {} "
						 "variables in both, {} at another offset{}{}; in one table only: {} DCLF's (a variable the game's shader lacks{}), {} ShaderCache's (DCLF_BINDLESS's "
						 "own blocks, a block the stage never reads)",
				ct.checks, ct.entries, ct.uncached, ct.variables, ct.differ, ct.differ ? " <- CONSTANT TABLE; first: " : " <- OK", ct.first, ct.catalogOnly,
				ct.catalogOnly ? "; first: " + ct.catalogOnlyFirst : std::string(), ct.cacheOnly);
	}

	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 4>& a_totals, const std::array<std::uint32_t, 4>& a_samples)
	{
		static constexpr const char* kNames[4] = { "Z-prepass", "Z-prepass phase 2", "colour", "decals" };
		std::string text;
		for (std::uint32_t k = 0; k < 4; ++k) {
			if (!a_samples[k])
				continue;
			const double n = a_samples[k];
			text += fmt::format("; {}: {:.0f} vertices, {:.0f} primitives, {:.0f} VS invocations, {:.0f} PS invocations", kNames[k], a_totals[k][0] / n,
				a_totals[k][1] / n, a_totals[k][2] / n, a_totals[k][3] / n);
		}
		logger::info("[DCLF] pass statistics, per frame{}", text);
	}

	void ColourSunTestLine(std::uint32_t a_empty)
	{
		logger::info("[DCLF] colour sun test: {} of {} frames without the sun's cascades{}", a_empty, Interval(), a_empty ? " <- NO CASCADES" : " <- OK");
	}
}
