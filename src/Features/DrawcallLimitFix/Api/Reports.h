#pragma once

// The report lines contract (Api::InstallContext::RegisterReportLines): a feature's lines in DCLF's periodic report (CS_DCLF_STATS,
// Diagnostics/Reports), at a named point of it.

#include <cstdint>
#include <string>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Where in the report a feature's lines go; lines at one point run in registration order. */
	enum class ReportPoint : std::uint8_t
	{
		// The primary cull's lines (Reports::PrimaryCullLines), after its scene lists' and culls-off lines, before the point lights':
		// only on a report frame whose window had cut frames, with the primary cull installed.
		PrimaryCull,
		// The draws' async work lines (IndirectDraws::AsyncReport), after the forward epoch's lines, before the first-person view's: every
		// report. Text lines (ReportLines::text), printed with the rest of that report.
		AsyncWork,
	};

	/**
	 * @brief A feature's report lines.
	 *
	 * - lines: render thread, at its point of a report (the report's own schedule and conditions): logs the feature's lines, taking its
	 *   counters since the last report. A parity it runs there is gated as any other (Diagnostics::Observe). For a point that logs.
	 * - text: the same, at a point that composes its lines into the report's text (AsyncWork): appends them to a_text, each ending in a
	 *   newline. One of the two, as the point asks.
	 */
	struct ReportLines
	{
		std::string_view name;
		ReportPoint point = ReportPoint::PrimaryCull;
		void (*lines)() = nullptr;
		void (*text)(std::string& a_text) = nullptr;
	};
}
