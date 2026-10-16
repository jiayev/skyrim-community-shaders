// Shadows/Sun: the Sun source, its hooks (the accumulation's, SunAccumulation, and the camera hook, SunViews), their report lines and
// their parity (Diagnostics/).

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunCasters.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		/**
		 * @brief The Sun source's hooks (by source: Core installs the light selection right after them): the accumulation's (its cascade
		 * culls and registrations, which the local light culls' install requires), then the camera hook.
		 */
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source != Api::ShadowSource::Sun)
				return;
			auto& accumulation = SunAccumulation::Get();
			accumulation.Install();
			if (accumulation.Installed())
				a_hooks.CullsInstalled();
			SunViews::Get().Install();
		}

		/** @brief After the main camera's full-frustum call site: the frame's sun state ended, and whether its exclusion applied. */
		bool FullFrustumWindowEnd()
		{
			auto& accumulation = SunAccumulation::Get();
			accumulation.EndFullFrustumWindow();
			return accumulation.ExclusionLive();
		}

		void Install(Api::InstallContext& a_context)
		{
			// The directional views draw while the source is registered; its cameras are the hook's (the Sun board).
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Sun, .fullFrustumWindowEnd = &FullFrustumWindowEnd });
			// The accumulation's lines, every frame (they test their report frames and the install themselves).
			a_context.RegisterReportLines({ .name = "sun accumulation", .point = Api::ReportPoint::RenderThread, .lines = &SunAccumulationLines });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the sun views, the accumulation's cascades and the entry rule's planes
			// against the engine's.
			if (Diagnostics::Observe().claimsOrScene) {
				SubscribeSunViewParity();
				SubscribeSunEntryPlanesParity();
			}
			// CS_DCLF_PERSISTENT_PARITY: the sun exclusion's watch, an unclaimed caster under an excluded entry on the frames the cascades
			// skip nothing.
			if (Diagnostics::Observe().scene)
				SunCasters::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "sun shadows", .install = &Install, .sourceHooks = &SourceHooks };
	}
}
