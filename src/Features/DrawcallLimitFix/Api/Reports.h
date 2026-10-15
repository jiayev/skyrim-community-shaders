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
		// The views' lines in the draws' async work (IndirectDraws::AsyncReport), after the visibility lines and the reflection's, before
		// the forward epoch's: every report. Text lines (ReportLines::text).
		AsyncWorkViews,
		// The draws' async work lines (IndirectDraws::AsyncReport), after the forward epoch's lines, before the tree LOD draws': every
		// report. Text lines (ReportLines::text), printed with the rest of that report.
		AsyncWork,
		// The draw families' lines in the draws' async work (IndirectDraws::AsyncReport), right after AsyncWork's, where the tree LOD
		// draws' lines are (before them while Core prints them), before the frame values' line: every report. Text lines.
		AsyncWorkFamilies,
		// The render thread's scene lines (Reports::SceneFrameLines), after the extras parity's, where tree LOD's mirror line is (before
		// it while Core prints it), before the mirror watch's: every report. Text lines.
		SceneFrame,
		// The LOD gates' releases (Reports::GateReleases, render thread), once a report window, after the releases' and the engine
		// side's gate lines: logged lines, handed the frame.
		GateReleases,
		// The coordinator's lines (Reports' scene pass report: the scene lane, not the render thread), after the LOD gates' line, at the
		// end of the pump's lines: logged lines, handed the scene frame.
		SceneCoordinatorGates,
		// The coordinator's scene lines (the scene lane), after the shared bindings', where the object LOD segments' line is (before it
		// while Core prints it), before the change log parity's. Text lines.
		SceneCoordinatorSegments,
		// The frame report's statistics (Reports::Stats), last, after the scene publications' line: on a report frame with CS_DCLF_STATS.
		Statistics,
	};

	/**
	 * @brief A feature's report lines.
	 *
	 * - lines: render thread, at its point of a report (the report's own schedule and conditions), handed the report's frame (FrameView's
	 *   number): logs the feature's lines, taking its counters since the last report. A parity it runs there is gated as any other
	 *   (Diagnostics::Observe). For a point that logs.
	 * - text: the same, at a point that composes its lines into the report's text (AsyncWorkViews, AsyncWork, AsyncWorkFamilies,
	 *   SceneFrame, SceneCoordinatorSegments): appends them to a_text, each ending in a newline. One of the two, as the point asks.
	 *   The SceneCoordinator points run on the scene lane, in the coordinator's report pass.
	 */
	struct ReportLines
	{
		std::string_view name;
		ReportPoint point = ReportPoint::PrimaryCull;
		void (*lines)(std::uint32_t a_frame) = nullptr;
		void (*text)(std::string& a_text) = nullptr;
	};
}
