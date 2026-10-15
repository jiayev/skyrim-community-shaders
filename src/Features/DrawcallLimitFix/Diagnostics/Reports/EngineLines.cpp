#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/LightSelectionParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/PortalParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/SunCasters.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Engine/Cull/RoomVisibility.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Engine/Views/PortalViews.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

// The engine area's report lines (Engine.h), but the primary cull's (EnginePrimaryCull.cpp).

namespace DCLF::Reports
{
	void LightSelectionLines()
	{
		const auto p = Diagnostics::LightSelectionParity::Take();
		logger::info("[DCLF] light rooms (T5a3: DCLF's walk of each tested light against the engine's entry): {} tests ({} not walked, {} by bounds, {} walked), {} on an "
					 "entry apart from the room process's, {} room processes not as assumed{}; verdicts differ {} (+{} lights moved since the engine's walk), room sets "
					 "differ {} (+{} moved){}{}",
			p.roomTests, p.roomWalks[0], p.roomWalks[1], p.roomWalks[2], p.roomEntriesApart, p.roomProcessesOther,
			p.roomProcessFirst.empty() ? "" : " (" + p.roomProcessFirst + ")", p.roomsDiffer, p.roomsMovedDiffer, p.roomSetsDiffer, p.roomSetsMovedDiffer,
			p.roomsDiffer ? " <- LIGHT ROOMS" : " <- OK", p.roomSetsFirst.empty() ? "" : "; first set: " + p.roomSetsFirst);
		logger::info("[DCLF] light selection (T3b, T3c: DCLF's choice of shadow lights and focus hosts against CalculateActiveShadowCasterLights'): {} frames, {} local "
					 "lights kept, {} focus hosts; {} frames differ ({} slots, {} mask indices, {} counts, {} focus){}{}",
			p.frames, p.kept, p.focusHosts, p.framesDiffer, p.slotsDiffer, p.masksDiffer, p.countsDiffer, p.focusDiffer, p.framesDiffer ? " <- LIGHT SELECTION" : " <- OK",
			p.first.empty() ? "" : "; first: " + p.first);
	}

	void PortalViewsLine()
	{
		const auto p = Diagnostics::PortalParity::Take();
		const auto b = PortalViews::Get().TakeBuildCounts();
		const std::uint64_t differ = p.roomsMissing + p.roomsExtra + p.programsDiffer + p.unboundDiffer + p.accumDiffer;
		logger::info("[DCLF] portal views (T5a: DCLF's walk of the portal graph, against the engine's entry): {} frames ({} built, {} without a graph or camera), {} rooms in the "
					 "engine's maps ({} not reached, {} reached only by DCLF), {} programs ({} rooms', {} unbound space's differ), {} walks' room lists differ; programs held {} ops, "
					 "{} sets ({} portals, {} occlusion planes, {} boxes); largest plane difference {:.3g}; {} frames the camera moved after the engine's walk (by up to "
					 "{:.3g}, {} of them turned: {} differences there, not counted); boxes: {} checked, {} with other corners (by up to {:.3g}), {} sets differ; view planes other than the list process's on {} frames{}{}{}{}{}",
			p.frames, b.built, b.skipped, p.rooms, p.roomsMissing, p.roomsExtra, p.programs, p.programsDiffer, p.unboundDiffer, p.accumDiffer, p.ops, p.sets, b.programs.portals,
			b.programs.planes, b.programs.boxes, p.largest, p.eyeMoved, p.largestMove, p.turned, p.movedDiffer, p.boxChecks, p.boxCornersDiffer, p.largestCorner, p.boxSetsDiffer,
			p.planesDiffer, p.planesFirst.empty() ? "" : " (first: " + p.planesFirst + ")", p.occluderFirst.empty() ? "" : "; unbound space's occluders: " + p.occluderFirst,
			p.boxFirst.empty() ? "" : "; first box: " + p.boxFirst, differ ? " <- PORTAL VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
	}

	void RoomVisibilityLines()
	{
		if (!RoomVisibility::Installed())
			return;
		const auto a = RoomVisibility::Take();
		logger::info("[DCLF] room visibility (T7f checklist item 3): {} calls answered by DCLF's VisibleSet, {} by the engine (DCLF off, or no set yet)", a.dclf, a.engine);
	}

	std::string EngineReadWindowLine()
	{
		// The scene work's accesses without a lease (any is a defect).
		const auto c = EngineReadWindow::Take();
		if (!c.any)
			return {};
		return fmt::format("[DCLF] engine-read window: {} closes, {} of them waited for items in flight ({:.1f} us in all, max {:.1f} us); {} leases refused (work left for the next window); "
						   "scene work engine accesses without a lease {}{}{}",
			c.closes, c.closesWaited, c.waitedUs, c.waitedMaxUs, c.refused, c.unleased, c.unleased ? " <- LANE ENGINE ACCESS; first: " : " <- OK",
			c.unleasedFirst ? c.unleasedFirst : "");
	}

	std::string ReleaseGuardLine()
	{
		if (!ReleaseGuard::Installed())
			return {};
		const auto c = ReleaseGuard::Take();
		return fmt::format("[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): engine objects' last releases on DCLF's threads: scene lane {}, executor/pool {} "
						   "({} without a class name){}{}{}{}",
			c.lane, c.pool, c.unnamed, c.lane || c.pool ? " <- LANE ENGINE RELEASE; first: " : " <- OK", c.first ? c.first : "", c.lane || c.pool ? ", last: " : "",
			(c.lane || c.pool) && c.last ? c.last : "");
	}

	void SunViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Sun);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.cascadePlanes + p.cascadeCasters + p.fullFrustumPlanes + p.missing;
		logger::info("[DCLF] sun views (T2a: DCLF's, from the cameras UpdateCamera set up, against the engine's): {} views ({} view-projection, {} eye, {} viewport, "
					 "{} caster volume differ), {} cascades ({} planes, {} caster planes differ), {} full-frustum processes ({} differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.cascades, p.cascadePlanes, p.cascadeCasters, p.fullFrusta, p.fullFrustumPlanes, p.missing,
			differ ? " <- SUN VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	void LightViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Light);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] light views (T3a: DCLF's, from the cameras the local lights' UpdateCamera set up, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- LIGHT VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	void FocusViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Focus);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] focus views (T3c: DCLF's, from the cameras BSShadowDirectionalLight::sub aimed, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- FOCUS VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
		// The membership check.
		const auto members = Diagnostics::TakeFocusMembers();
		logger::info("[DCLF] focus members (T3c: the engine's registrations into DCLF's focus views against their targets' fade roots): {} registrations, {} under "
					 "another root{}{}",
			members.registrations, members.outside, members.outside ? " <- FOCUS MEMBERS" : " <- OK", members.first.empty() ? std::string() : "; first: " + members.first);
	}

	std::string ActorRateLine()
	{
		const auto c = SideEffects::TakeActorRate();
		if (!c.entries)
			return {};
		return fmt::format(
			"[DCLF] actor rate (T7f checklist item 6): {} entries, {:.1f} high actors a frame ({:.1f} without a skin property or 3D), {:.1f} in the "
			"sun's cascade culls and {:.1f} in a local shadow view a frame, {:.1f} stamped by DCLF a frame\n",
			c.entries, double(c.actors) / double(c.entries), double(c.noProperty) / double(c.entries),
			double(c.inSun) / double(c.entries), double(c.inLocal) / double(c.entries), double(c.stamped) / double(c.entries));
	}

	std::string AnimRateLine()
	{
		if (!Diagnostics::Observe().accumulated)
			return {};
		const auto r = Diagnostics::Accumulated::TakeAnimRate();
		std::string sites;
		std::uint64_t differ = 0;
		std::string first;
		for (std::size_t s = 0; s < r.sites.size(); ++s) {
			const auto& c = r.sites[s];
			differ += c.differ;
			if (c.firstDiffer && first.empty())
				first = fmt::format("; first: {:p} ({})", c.firstDiffer, Diagnostics::Accumulated::kAnimGateCallers[s]);
			sites += fmt::format("; {}: {} stood-in calls ({} never fading, {} fading), {} rejected ({} never fading), {} written out of view ({} of them updated by "
								 "their fade step), {} written visible but faded out, {} differ",
				Diagnostics::Accumulated::kAnimGateCallers[s], c.calls, c.neverFade, c.fading, c.rejected, c.rejectedNeverFade, c.predictedOut, c.acceptedOut, c.fadedOut, c.differ);
		}
		return fmt::format("[DCLF] anim rate (T7b2): {} predictions, {:.1f} stood-in roots a frame ({:.1f} written visible){}{}{}\n", r.predictions,
			r.predictions ? double(r.roots) / double(r.predictions) : 0.0, r.predictions ? double(r.set) / double(r.predictions) : 0.0, sites,
			!r.predictions ? " <- NOT COMPARED" : differ ? " <- ANIM RATE" : " <- OK", first);
	}

	void SunAccumulationLines(std::uint32_t a_frame)
	{
		auto& sun = SunAccumulation::Get();
		if (!sun.Installed() || !Due(a_frame))
			return;
		const auto stats = sun.TakeStats();
		const auto watch = Diagnostics::SunCasters::TakeWatch();
		if (!stats.frames)
			return;
		const double toMs = MsPerTick();
		const double frames = stats.frames;
		const auto timing = fmt::format("; per frame, render thread: full-frustum cull {:.3f} ms (max {:.3f}), Accumulate {:.3f} ms (max {:.3f})",
			stats.fullFrustumTicks * toMs / frames, stats.fullFrustumMax * toMs, stats.accumulateTicks * toMs / frames, stats.accumulateMax * toMs);
		logger::info("[DCLF] sun accumulation: skipping registration on {} of {} frames; {:.0f} claimed geometries a frame not registered, {:.0f} registered by the engine, {} sun registrations outside Accumulate{}",
			stats.activeFrames, stats.frames, stats.skipped / frames, stats.registered / frames, stats.offThread, timing);
		if (stats.exclusionFrames || stats.exclusionStale || stats.exclusionMissing || stats.exclusionUnforwarded) {
			const double applied = std::max<double>(stats.exclusionFrames, 1.0);
			const auto c = sun.TakeExclusionCounts();
			logger::info("[DCLF] sun entry exclusion: applied on {} frames ({} stale, {} with none; the builder's translation taken at {} frame starts, none for the frame's candidates at {}); per frame {:.0f} of {:.0f} objectArray entries removed, "
						 "{:.0f} of {:.0f} candidates excluded, filter {:.3f} ms; {:.0f} registrations took DCLF's bits ({:.0f} with a cascade), {} before the cascades, "
						 "{} cascade registrations under a removed entry; excluded entries the cascades reached anyway and skipped {:.0f} a frame, parity {} frames: {} unclaimed casters under an excluded entry{}",
				stats.exclusionFrames, stats.exclusionStale, stats.exclusionMissing, stats.exclusionForwarded, stats.exclusionUnforwarded,
				stats.entriesRemoved / applied, stats.entriesSeen / applied, stats.excluded / applied, stats.candidates / applied,
				stats.filterTicks * toMs / applied, c.written / applied, c.withBits / applied, c.notReady, stats.cascadeRegistrationsUnderRemoved,
				c.skipped / applied, watch.frames, watch.lost,
				watch.frames ? (watch.lost ? " <- SUN EXCLUSION; first " + watch.first + " (" + Scene::Coordinator::Get().DescribeSunCandidate(watch.firstGeometry) + ")" : std::string(" <- OK")) : std::string());
		}
	}
}
