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
			// Where Core's were listed: right after the shadows' (the registration skip's switch, then the entry exclusion's after it), before
			// the primary exclusion's.
			const auto skipSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SUN_SKIP", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: the engine keeps culling and registering the sun's casters (live toggle's seed)", .after = "CS_DCLF_SHADOWS" });
			const auto excludeSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SUN_EXCLUDE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: DCLF's objects stay in the sun's culls (live toggle's seed)", .after = "CS_DCLF_SUN_SKIP" });
			// Where Core's were, in the shadow views' section: after the shadows' and before the primary exclusion's, which needs the entry
			// exclusion by name (Core's "exclude primary entries": without this feature it never takes effect).
			const auto skip = a_context.RegisterToggle({ .name = "skip sun accumulation", .label = "Skip the engine's sun shadow culling and registration",
				.tooltip = "The engine stops building sun shadow passes for the casters DCLF draws; it still sets their shadow bits for the main pass.",
				.seed = skipSwitch, .after = "shadows", .needs = { "shadows" } });
			const auto exclude = a_context.RegisterToggle({ .name = "exclude sun entries", .label = "Take DCLF's objects out of the engine's sun culls",
				.tooltip = "The sun's cascade culls skip every reference whose shadows DCLF draws entirely, and while DCLF draws every cascade the engine culls nothing for the sun; DCLF sets the objects' sun shadow bits for the main pass.",
				.seed = excludeSwitch, .after = "skip sun accumulation", .needs = { "skip sun accumulation" } });
			SunAccumulation::SetToggles(skip, exclude);
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
		return { .name = "sun shadows", .install = &Install, .sourceHooks = &SourceHooks };
	}
}
