#pragma once

// The report lines contract (Api::InstallContext::RegisterReportLines): a feature's lines in DCLF's periodic report (CS_DCLF_STATS,
// Diagnostics/Reports), at a named point of it.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Where in the report a feature's lines go; lines at one point run in registration order. */
	enum class ReportPoint : std::uint8_t
	{
		// The primary cull's lines (Reports::PrimaryCullLines), after its scene lists' and culls-off lines, before the point lights':
		// only on a report frame whose window had cut frames, with the primary cull installed.
		PrimaryCull,
	};

	/**
	 * @brief A feature's report lines.
	 *
	 * - lines: render thread, at its point of a report (the report's own schedule and conditions): logs the feature's lines, taking its
	 *   counters since the last report. A parity it runs there is gated as any other (Diagnostics::Observe).
	 */
	struct ReportLines
	{
		std::string_view name;
		ReportPoint point = ReportPoint::PrimaryCull;
		void (*lines)() = nullptr;
	};
}
