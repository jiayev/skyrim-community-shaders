#pragma once

// The report lines contract (Api::InstallContext::RegisterReportLines): a feature's lines in DCLF's periodic report (CS_DCLF_STATS,
// Diagnostics/Reports), at a named point of it.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Where in the report a feature's lines go; lines at one point run in registration order. */
	enum class ReportPoint : std::uint8_t
	{
		// The render thread's report (Reports::RenderThread, at Present, once a frame number), first (Features/Shadows/Sun's accumulation
		// lines), before the primary cull's: every frame, so the lines test their own report frames (Reports::Due) and conditions. Logged lines, handed
		// the frame.
		RenderThread,
		// The primary cull's lines (Reports::PrimaryCullLines), after its scene lists' and culls-off lines, before the point lights':
		// only on a report frame whose window had cut frames, with the primary cull installed.
		PrimaryCull,
		// The views' lines in the draws' async work (IndirectDraws::AsyncReport), after the visibility lines and the reflection's, before
		// the forward epoch's: every report. Text lines (ReportLines::text).
		AsyncWorkViews,
		// The accumulated writes' lines (IndirectDraws::VisibilityReport, in the draws' async work), after its every-listed-root lines,
		// before the accumulated parity's (Features/Actors' animation stamp): on a report whose scene has the visibility readback. Text lines.
		AccumulatedWrites,
		// The forward epoch's lines (IndirectDraws::ForwardReport), after its own and the refraction's, before AsyncWork's: on a report
		// that prints the forward epoch's (it reached a frame since the last, or its toggle is on). Text lines.
		AsyncWorkForward,
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
		// The coordinator's set lines (Reports' scene pass report: the scene lane), right after the set's line ("DCLF set: ..."), before its
		// leaves by cause: logged lines, handed the scene frame, or a coordinator component's (logged as one line).
		SceneCoordinatorSet,
		// The coordinator's scene lines (the scene lane), after the shared bindings', where the object LOD segments' line was (Features/Lod's
		// now), before the change log parity's. Text lines, or a coordinator component's (ReportLines::componentText).
		SceneCoordinatorSegments,
		// The coordinator's scene lines (the scene lane), after the material records' line, before the material captures' (Features/Decals'
		// order parity). Text lines, or a coordinator component's.
		SceneCoordinatorMaterials,
		// Inside the coordinator's scene delta line (the scene lane), at its end, before its newline (Features/Faces' publications): text
		// fragments, each beginning "; " and ending in no newline, or a coordinator component's; only when the line prints (a walk since).
		SceneCoordinatorDelta,
		// The coordinator's statistics of its last pass (the scene lane, under CS_DCLF_STATS), after the fading line, before the derived
		// cache's (Features/Skins' skinned line, then Features/Decals' candidates): logged lines, handed the scene frame, or a coordinator
		// component's (logged as one line).
		SceneCoordinatorLastPass,
		// The shadow views' lines in the frame report's statistics (Reports::Stats, with the shadows' toggle on), after the shadow views'
		// "by the set" line, before the shadow counters' reset (IndirectDraws::ResetShadowStats): on a report frame with CS_DCLF_STATS.
		// Logged lines, handed the frame.
		ShadowViews,
		// The frame report's statistics (Reports::Stats), last, after the scene publications' line: on a report frame with CS_DCLF_STATS.
		Statistics,
	};

	/** @brief Whether a_point composes its lines into the report's text (ReportLines::text), not logs them (ReportLines::lines). */
	constexpr bool ReportPointComposesText(ReportPoint a_point)
	{
		return a_point == ReportPoint::AsyncWorkViews || a_point == ReportPoint::AccumulatedWrites || a_point == ReportPoint::AsyncWorkForward || a_point == ReportPoint::AsyncWork ||
		       a_point == ReportPoint::AsyncWorkFamilies || a_point == ReportPoint::SceneFrame || a_point == ReportPoint::SceneCoordinatorSegments ||
		       a_point == ReportPoint::SceneCoordinatorMaterials || a_point == ReportPoint::SceneCoordinatorDelta;
	}

	/** @brief Whether a_point is the coordinator's, where a coordinator component's lines go (ReportLines::componentText). */
	constexpr bool ReportPointTakesComponents(ReportPoint a_point)
	{
		return a_point == ReportPoint::SceneCoordinatorGates || a_point == ReportPoint::SceneCoordinatorSet || a_point == ReportPoint::SceneCoordinatorSegments ||
		       a_point == ReportPoint::SceneCoordinatorMaterials || a_point == ReportPoint::SceneCoordinatorDelta ||
		       a_point == ReportPoint::SceneCoordinatorLastPass;
	}

	/**
	 * @brief A feature's report lines.
	 *
	 * - lines: render thread, at its point of a report (the report's own schedule and conditions), handed the report's frame (FrameView's
	 *   number): logs the feature's lines, taking its counters since the last report. A parity it runs there is gated as any other
	 *   (Diagnostics::Observe). For a point that logs.
	 * - text: the same, at a point that composes its lines into the report's text (ReportPointComposesText): appends them to a_text, each
	 *   ending in a newline (SceneCoordinatorDelta's: fragments of its line, as it says). One of the two, as the point asks.
	 *   The SceneCoordinator points run on the scene lane, in the coordinator's report pass.
	 * - componentText (with component): at a coordinator point (ReportPointTakesComponents), in text's (or lines') place: the lines of a
	 *   coordinator component's (what it keeps, and what the coordinator keeps for it), handed the component's context. Registered after
	 *   the component.
	 */
	struct ReportLines
	{
		std::string_view name;
		ReportPoint point = ReportPoint::PrimaryCull;
		void (*lines)(std::uint32_t a_frame) = nullptr;
		void (*text)(std::string& a_text) = nullptr;
		CoordinatorComponentId component;
		void (*componentText)(const CoordinatorComponentContext& a_context, std::string& a_text) = nullptr;
	};
}
