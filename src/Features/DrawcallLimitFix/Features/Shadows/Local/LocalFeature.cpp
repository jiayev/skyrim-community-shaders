// Shadows/Local: the Spot, Point and Focus sources, the kept lights' views (LightViews), the point lights' culls (LocalLightCull) and the
// focus cameras' hook (FocusViews), their report lines and their parity (Diagnostics/).

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/SunExclusion.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LightCulls.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		/** @brief A kept spot or point light's views, from its cameras (LightViews: the Lights board). */
		void LightKept(const Api::ShadowLightKept& a_light) { LightViews::Get().Update(a_light.light); }

		/**
		 * @brief The point lights' culls at the Point source (after the Sun's registration hooks, which they require, and the light
		 * selection; before the primary cull reads NiNode's OnVisible slot, which the category filter takes), and the Focus source's
		 * camera hook.
		 */
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source == Api::ShadowSource::Point) {
				LocalLightCull::Install();
				if (LocalLightCull::Installed())
					a_hooks.CullsInstalled();
			}
			if (a_hooks.source == Api::ShadowSource::Focus)
				FocusViews::Get().Install();
		}

		/** @brief The frame start: the point lights' culls select this frame's exclusion, the one the shadow epoch published (null: none). */
		void PointFrameStart(std::uint32_t a_frame) { LocalLightCull::SelectFrame(a_frame, ParabolicExclusionSlot::Get().Pending()); }

		void Install(Api::InstallContext& a_context)
		{
			// The frustum and parabolic views draw while their sources are registered, from the lights the selection keeps; the focus
			// views, whatever their host, from the focus cameras' hook (the Focus board).
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Spot, .lightKept = &LightKept });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Point, .lightKept = &LightKept, .frameStart = &PointFrameStart, .frameEnd = &LocalLightCull::EndFrame });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Focus });
			// The point lights' culls' category filter follows the children attached to and detached from its nodes.
			a_context.RegisterStandInObserver({ .structure = &LocalLightCull::NoteStructure });
			// The point lights' culls' lines, on the primary cull's report frames.
			a_context.RegisterReportLines({ .name = "point light culls", .point = Api::ReportPoint::PrimaryCullSources, .lines = &PointCullLines });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the light and focus views' lines, the focus views' membership and the local
			// shadow volumes against the engine's.
			if (Diagnostics::Observe().claimsOrScene) {
				SubscribeLightParity();
				SubscribeFocusParity();
				SubscribeShadowVolumeParity();
			}
			// CS_DCLF_PERSISTENT_PARITY: the point lights' culls on the frames they skip nothing.
			if (Diagnostics::Observe().scene)
				LightCulls::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "local shadows", .install = &Install, .sourceHooks = &SourceHooks };
	}
}
