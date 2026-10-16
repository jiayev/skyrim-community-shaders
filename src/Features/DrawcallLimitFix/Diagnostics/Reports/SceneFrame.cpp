#include "Scene.h"

#include "Engine.h"
#include "Schedule.h"
#include "Spread.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorWatch.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneState.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/WetnessParity.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

// The render thread's scene lines: what it keeps of the scene (the pipeline constants, the shading and extras checks, tree LOD's mirror,
// the frame capture), and the lines at the gate releases' point.

namespace DCLF::Reports
{
	using V = Diagnostics::SceneView;

	std::string SceneFrameLines()
	{
		// What the render thread keeps (its parities and captures), reported from the frame: nothing of the coordinator's.
		std::string text;
		auto& diagnostics = Diagnostics::SceneDiagnostics();
		if (auto& g = V::geometryStats(); g.frames) {
			auto& t = diagnostics.techniques;
			text += fmt::format("[DCLF] pipeline constants: {:.2f} full geometry evaluations and {:.2f} frame samples a frame, {:.1f} pipelines changed, frame lighting changed {:.2f}; parity {} checks, {} of {} technique blocks differ ({} more late: the sample moved since the row's write){}{}\n",
				static_cast<double>(g.full) / g.frames, static_cast<double>(g.samples) / g.frames, static_cast<double>(g.changed) / g.frames,
				static_cast<double>(g.lightingVersions) / g.frames, t.checks, t.techniquesDiffer, t.techniquesChecked, t.techniquesLate,
				t.checks ? (t.techniquesDiffer ? " <- DIFFER" : " <- OK") : "", t.techniquesDiffer ? "; technique: " + t.techniqueFirst : std::string());
			g = {};
			t = {};
		}
		if (auto& ss = V::shadingStats(Scene::Coordinator::Get()); V::shadingFrames()) {
			const double frames = static_cast<double>(std::exchange(V::shadingFrames(), 0));
			auto& sp = diagnostics.shading;
			text += fmt::format("[DCLF] shading (sampled at the frame's start for the slots its events name): {:.1f} LOD fade events, {:.1f} emittance events, {:.1f} "
								"named a frame; parity at Prepass against the frame's rows: {} checks, {} slots compared, changed since the frame's start: {} named for "
								"the next frame, {} queued, {} missed{}{}{}\n",
				static_cast<double>(ss.lodFadeEvents) / frames, static_cast<double>(ss.emittanceEvents) / frames, static_cast<double>(ss.resampled) / frames, sp.checks,
				sp.slots, sp.named, sp.late, sp.missing, sp.checks ? (sp.missing ? " <- MISSED; first: " : " <- OK") : "", sp.first,
				Integrations::AdvancedSkin::TakeRowWetnessText());
			ss = {};
			sp = {};
		}
		if (auto& ep = diagnostics.extras; ep.objects) {
			text += fmt::format("[DCLF] extras parity (the draw's completion against the engine's routines): {} objects, {} differ, largest difference {}, {} static rows "
								"stale with no event{}{}\n",
				ep.objects, ep.differ, ep.maxDifference, ep.staleStatic, ep.differ || ep.staleStatic ? " <- DIFFER" : " <- OK", ep.differ ? "; first: " + ep.first : std::string());
			ep = {};
		}
		// The features' scene lines here (Features/TreeLod's mirror line).
		Extensions::ReportTextAt(Api::ReportPoint::SceneFrame, text);
		text += MirrorWatch::TakeReport(V::frame());
		auto& captureNs = V::captureNs();
		auto& categoryCaptureNs = V::categoryCaptureNs();
		auto& categoryCapturesMade = V::categoryCapturesMade();
		auto& captureFrames = V::captureFrames();
		auto& categoryMirrorCaptures = V::categoryMirrorCaptures();
		auto& categoryMirrorRecords = V::categoryMirrorRecords();
		if (captureFrames) {
			const auto unscoped = FrameGlobals::TakeUnscopedReads();
			text += fmt::format("[DCLF] frame capture (6e F2): render thread {:.1f} us/frame for the globals, {:.1f} us/frame for the categories ({} captures made; {} subtrees the "
								"mirror lacked captured, {} records); {} reads off the render thread with no frame's capture bound{}\n",
				static_cast<double>(captureNs) / 1000.0 / captureFrames, static_cast<double>(categoryCaptureNs) / 1000.0 / captureFrames, categoryCapturesMade, categoryMirrorCaptures,
				categoryMirrorRecords, unscoped, unscoped ? " <- CAPTURE" : " <- OK");
			captureNs = categoryCaptureNs = categoryCapturesMade = captureFrames = 0;
			categoryMirrorCaptures = categoryMirrorRecords = 0;
		}
		return text;
	}

	void GateReleases(std::uint32_t a_frame)
	{
		// The lines at the gate releases' point (the LOD gates' releases and engine side's first: their component's), once a report window.
		// The frame numbers are the render thread's, each once.
		static Crossing crossing;
		if (crossing.Due(a_frame))
			Extensions::ReportLinesAt(Api::ReportPoint::GateReleases, a_frame);
	}
}
