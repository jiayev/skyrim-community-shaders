// The sun views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY), render thread: the Sun board against the engine's views (the
// tally, Diagnostics::ShadowViewTally, filled by the shadow epochs' checks and here), the sun accumulation's cascades against the cascade
// culls' planes, the cascades without DCLF's views; the line every 300 frames with sun views.

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunAccumulationView.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <array>

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		using Diagnostics::ShadowView;

		constexpr std::size_t kProcessObjectArray = 0x128;  // BSCullingProcess::objectArray
		Reports::Window sunWindow;
		std::uint32_t cascadesChecked = 0;  // per cascade of the current Accumulate: compared with the cascade cull's

		void CopyPlanes(const RE::NiFrustumPlanes& a_from, std::array<std::array<float, 4>, 6>& a_to, std::uint32_t& a_mask)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				const auto& plane = a_from.cullingPlanes[p];
				a_to[p] = { plane.normal.x, plane.normal.y, plane.normal.z, plane.constant };
			}
			a_mask = a_from.activePlanes.underlying() & 0x3Fu;
		}

		/** @brief The sun views' parity line, logged, the tally reset: every 300 frames with sun views. */
		void SunViewsLines()
		{
			auto& p = Diagnostics::ShadowViewTally(ShadowView::Sun);
			const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.cascadePlanes + p.cascadeCasters + p.fullFrustumPlanes + p.missing;
			logger::info("[DCLF] sun views (T2a: DCLF's, from the cameras UpdateCamera set up, against the engine's): {} views ({} view-projection, {} eye, {} viewport, "
						 "{} caster volume differ), {} cascades ({} planes, {} caster planes differ), {} full-frustum processes ({} differ), {} without DCLF's{}{}",
				p.views, p.viewProj, p.eye, p.viewport, p.casters, p.cascades, p.cascadePlanes, p.cascadeCasters, p.fullFrusta, p.fullFrustumPlanes, p.missing,
				differ ? " <- SUN VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
			p = {};
		}

		void SunUpdated(const Diagnostics::On<Diagnostics::EnginePoint::SunViewsUpdated>&)
		{
			if (sunWindow.Tick())
				SunViewsLines();
		}

		void AccumulateBegin(const Diagnostics::On<Diagnostics::EnginePoint::SunAccumulateBegin>&)
		{
			cascadesChecked = 0;
		}

		void CascadeMissing(const Diagnostics::SunCascadeMissing& a_event)
		{
			auto& tally = Diagnostics::ShadowViewTally(ShadowView::Sun);
			++tally.missing;
			const auto& sun = ShadowCameras::Sun();
			tally.Note("missing cascade", 0.0f, fmt::format("Accumulate of frame {}, descriptor {}, DCLF's views of frame {} ({} cascades)", a_event.frame, a_event.descriptor,
				sun.sceneFrame, sun.count));
		}

		/** @brief The cascade's volume is DCLF's (Accumulate's start), checked once a cascade against the cull's. */
		void CascadeCulled(const Diagnostics::SunCascadeCulled& a_event)
		{
			// After the cull: the traversal's first call (vfunc 0xB8) has set the process up from the cascade's camera. With an empty
			// objectArray it made none, and the planes are not this cascade's.
			const std::uint32_t bit = 1u << (a_event.cascade & 31);
			if ((cascadesChecked & bit) || Engine::At<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>(a_event.fullProcess, kProcessObjectArray).empty())
				return;
			cascadesChecked |= bit;
			const auto& cascade = SunAccumulationView::Cascade(a_event.cascade);
			auto& p = Diagnostics::ShadowViewTally(ShadowView::Sun);
			++p.cascades;
			ShadowCameras::Planes engine, mine;
			CopyPlanes(a_event.process->planes, engine.plane, engine.mask);
			mine.plane = cascade.planes;
			mine.mask = cascade.planeMask;
			if (!cascade.captured || !ShadowCameras::SamePlanes(mine, engine)) {
				++p.cascadePlanes;
				p.Note("cascade planes", 1.0f, fmt::format("cascade {}: {}", a_event.cascade, ShadowCameras::DescribePlanes(mine, engine)));
			}
			ShadowCameras::Planes engineCaster, mineCaster;
			if (a_event.process->doCustomCullPlanes)
				CopyPlanes(a_event.process->customCullPlanes, engineCaster.plane, engineCaster.mask);
			mineCaster.plane = cascade.customPlanes;
			mineCaster.mask = cascade.customMask;
			if (!ShadowCameras::SamePlanes(mineCaster, engineCaster)) {
				++p.cascadeCasters;
				p.Note("cascade caster planes", 1.0f, fmt::format("cascade {}: masks {:#x} {:#x}", a_event.cascade, mineCaster.mask, engineCaster.mask));
			}
		}
	}

	void SubscribeSunViewParity()
	{
		Diagnostics::Subscribe<Diagnostics::EnginePoint::SunViewsUpdated>(SunUpdated);
		Diagnostics::Subscribe<Diagnostics::EnginePoint::SunAccumulateBegin>(AccumulateBegin);
		Diagnostics::Subscribe<Diagnostics::SunCascadeMissing>(CascadeMissing);
		Diagnostics::Subscribe<Diagnostics::SunCascadeCulled>(CascadeCulled);
	}
}
