// The point lights' culls' report lines (Api::ReportPoint::PrimaryCullSources: Reports::PrimaryCullLines, after the features' primary cull
// lines, before the stand-in walk parity): the point lights' Accumulate timing and, with the skip installed, its culls', category filter's
// and bits' lines, with their parity's verdicts (LightCulls).

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LightCulls.h"

#include <ranges>
#include <string>

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		/** @brief The point lights' Accumulate timing and, with the skip installed, its culls', category filter's and bits' lines. */
		std::string LocalLightLines()
		{
			auto c = LocalLightCull::Take();
			const auto p = LightCulls::Take();
			const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", c.accumulateCalls, c.accumulateTicks * Reports::MsPerTick());
			if (!c.installed)
				return timing;
			const auto lost = p.lost;
			const auto text = fmt::format("[DCLF] point lights' shadow culls: {} frames, {} with the exclusion ({} none built, {} the mode's claims not live, {} without candidates, {} translated to newer candidates); {} entries visited, {} skipped; parity {} frames: {} paraboloid passes not withheld, {} of them under an excluded entry{}",
				c.frames, c.live, c.noExclusion, c.noClaims, c.stale, c.masked, c.visited, c.skipped,
				p.frames, p.passes, lost, p.frames ? (lost ? " <- LIGHT EXCLUSION" : " <- OK") : "");
			std::string by;
			for (std::size_t r = 0; r < p.lostByReject.size(); ++r)
				if (p.lostByReject[r])
					by += fmt::format(" reject {}={}", r, p.lostByReject[r]);
			const auto withFirst = p.firstLost.empty() ? text : text + "; lost by" + by + "; first lost " + p.firstLost;
			std::string list;
			if (c.filterInstalled) {
				const double filtered = static_cast<double>(c.filtered);
				list = fmt::format("\n[DCLF] point lights' category filter: {} lights, {} filtered, {} walked whole (frames without it); {:.1f} filtered nodes a light ({} now), {:.0f} children culled and {:.0f} cut a light, {} dirty nodes walked; {} builds on the pool ({} nodes, {:.3f} ms each; {} for an older verdict, {} refused a lease); parity {} casters under a filtered node checked, {} not under a child it keeps{}{}",
					c.lights, c.filtered, c.walkedNotLive, c.filtered ? static_cast<double>(c.nodes) / filtered : 0.0, c.filterNodes,
					c.filtered ? static_cast<double>(c.children) / filtered : 0.0, c.filtered ? static_cast<double>(c.cut) / filtered : 0.0, c.dirtyNodes, c.builds,
					c.nodesRebuilt, c.builds ? static_cast<double>(c.buildTicks) * Reports::MsPerTick() / static_cast<double>(c.builds) : 0.0, c.buildsStale, c.buildsRefused,
					p.filterChecked, p.filterMissed, p.filterChecked || p.filterMissed ? (p.filterMissed ? " <- LIGHT FILTER" : " <- OK") : "",
					p.filterFirstMissed.empty() ? "" : "; first:" + p.filterFirstMissed);
			}
			auto byReach = [](const auto& a_counts) {
				std::string text;
				for (std::uint32_t k = 0; k <= LocalLightCull::kReachKindCount; ++k)
					if (const auto count = a_counts[k])
						text += fmt::format(" {} {}", LocalLightCull::kReachNames[k], count);
				return text.empty() ? std::string() : " (" + text.substr(1) + ")";
			};
			list += fmt::format("\n[DCLF] point lights' bits for engine main passes under cut entries: {} lights ({} past the mask's 32), {} geometries given them ({} any); parity {} compared, {} agree, {} with engine-only bits{}, {} with DCLF-only bits{}, {} cut but not an excluded entry's{}",
				c.bitLights, c.overflow, c.given, c.withBits, p.compared, p.agree, p.engineOnly, byReach(p.engineOnlyBy), p.dclfOnly, byReach(p.dclfOnlyBy), p.unhandled,
				p.compared || p.unhandled ? (p.engineOnly || p.unhandled ? " <- LIGHT BITS" : " <- OK") : "");
			list += fmt::format("\n[DCLF] point lights' empty culls (no engine cull: DCLF draws every paraboloid view): {} of {} lights ({} with a view not DCLF's, {} portal-strict kept native, {} past the mask's 32; {} portal-strict, their bits' gates {} tested, {} refused, {} without a portal graph, {} without a context{}); "
								"{} main registrations given their bits ({} any), {} without a slot{}; parity {} spheres checked, {} differ{}; {} geometries compared, {} agree, {} with engine-only bits{}, {} with DCLF-only bits{}",
				c.emptyLights, c.bitLights + c.overflow, c.emptyNotCovered, c.emptyPortalStrict, c.overflow, c.strictLights, c.strictTested, c.strictRefused, c.strictNoGraph, c.strictNoContext,
				c.strictNoContext ? " <- STRICT CONTEXT" : "", c.emptyGiven, c.emptyWithBits, c.emptyUnstamped,
				c.emptyUnstamped ? " <- STAMPS" : "", c.spheresChecked, c.spheresDiffer, c.spheresChecked ? (c.spheresDiffer ? " <- SPHERES" : " <- OK") : "",
				p.sphereCompared, p.sphereAgree, p.sphereEngineOnly, p.sphereCompared ? (p.sphereEngineOnly ? " <- EMPTY CULL BITS" : " <- OK") : "", p.sphereDclfOnly,
				p.sphereFirstEngineOnly.empty() ? "" : "; first engine-only: " + p.sphereFirstEngineOnly);
			return timing + "\n" + withFirst + list;
		}
	}

	void PointCullLines(std::uint32_t)
	{
		// Only on the primary cull's report frames (the point's): the counters stand until then.
		for (const auto& line : std::views::split(LocalLightLines(), '\n'))
			if (const std::string text(line.begin(), line.end()); !text.empty())
				logger::info("{}", text);
	}
}
