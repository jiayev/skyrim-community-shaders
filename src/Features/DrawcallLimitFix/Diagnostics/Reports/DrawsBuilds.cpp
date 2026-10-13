#include "Draws.h"
#include "Schedule.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"

// The main builds' checks (Diagnostics/Draws/BuildChecks.cpp), since the start, for the render thread's report (Frame.cpp).

namespace DCLF::Reports
{
	void BindlessParityLines()
	{
		const auto& c = Diagnostics::DrawsState::Get().buildChecks;
		if (!c.bindless)
			return;
		if (c.bindlessDiffer)
			logger::warn("[DCLF] bindless record parity MISMATCH: {} of {} components differ", c.bindlessDiffer, c.bindless);
		else
			logger::info("[DCLF] bindless record parity OK: {} components match the constant groups", c.bindless);
	}

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

	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 3>& a_totals, const std::array<std::uint32_t, 3>& a_samples)
	{
		static constexpr const char* kNames[3] = { "Z-prepass", "Z-prepass phase 2", "colour" };
		std::string text;
		for (std::uint32_t k = 0; k < 3; ++k) {
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
		logger::info("[DCLF] colour sun test: {} of {} frames without the sun's cascades{}", a_empty, kInterval, a_empty ? " <- NO CASCADES" : " <- OK");
	}

	std::string ResidentParityPart()
	{
		const auto& c = Diagnostics::DrawsState::Get().buildChecks;
		return fmt::format("parity {} entries checked, {} differ, {} missing{}", c.resident, c.residentDiffer, c.residentMissing,
			c.resident ? fmt::format("; pairs {} checked, {} resolved with no event{}", c.pairs, c.pairsStale,
							 c.residentDiffer || c.residentMissing || c.pairsStale ? " <- RESIDENT DRAW PARITY" : " <- OK") :
						 std::string());
	}
}
#else
namespace DCLF::Reports
{
	void BindlessParityLines() {}
	std::string ResidentParityPart() { return "parity 0 entries checked, 0 differ, 0 missing"; }
	void ColourSunTestLine(std::uint32_t) {}
	void ConstantTableLines() {}
	void PassStatsLine(const std::array<std::array<std::uint64_t, 4>, 3>&, const std::array<std::uint32_t, 3>&) {}
}
#endif
