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
		 * @brief The Sun source's hooks (by source: first, before the Spot's light selection): the accumulation's (its cascade culls and
		 * registrations, which the point lights' culls' install at the Point source requires: Features/Shadows/Local), then the camera hook.
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
			// No switch or toggle turns the skip or the exclusion off: the engine never culls the whole scene for a sun DCLF draws (the
			// engine-only subset is what it culls, Scene/EngineCasters.h).
			// The directional views draw while the source is registered; its cameras are the hook's (the Sun board), which Core reads through
			// the source's camera and the sun volume (the colour commits' cascades, the shadow inputs' full-frustum planes).
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Sun, .camera = &SunCamera, .fullFrustumWindowEnd = &FullFrustumWindowEnd });
			a_context.RegisterSunVolume({ .name = "sun views", .cascades = &SampleSunCascades, .entryPlanes = &SampleSunEntryPlanes });
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
		return { .name = "sun shadows", .install = &Install, .sourceHooks = &SourceHooks, .preflight = &SunAccumulation::Preflight };
	}
}
