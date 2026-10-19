// Shadows/Local: the Spot, Point and Focus sources, the kept lights' views (LightViews), the point lights' culls (LocalLightCull), the
// focus cameras' hook (FocusViews), the shadow payload's entry exclusions (ExclusionBuild), their report lines and their parity
// (Diagnostics/).

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/ExclusionBuild.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LightCulls.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/StrictLightParity.h"

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

		/** @brief The shadow epoch's exclusion of a_source (the payload's, this feature's type), into its slot. */
		void PublishExclusion(Api::ShadowSource a_source, std::shared_ptr<void> a_exclusion)
		{
			auto exclusion = std::static_pointer_cast<SunExclusion>(std::move(a_exclusion));
			if (a_source == Api::ShadowSource::Sun)
				SunExclusionSlot::Get().Publish(std::move(exclusion));
			else if (a_source == Api::ShadowSource::Point)
				ParabolicExclusionSlot::Get().Publish(std::move(exclusion));
		}

		/** @brief The frame start: the sun's pending exclusion as the builder translated it to the frame's candidates. */
		void ExclusionFrameStart() { SunExclusionSlot::Get().BeginFrame(Scene::FrameView::Get().GetSunCandidates()); }

		/** @brief The sun's pending exclusion as the primary cull's scene-list filter reads it (empty: none pending). */
		void ListExclusion(Api::ShadowListExclusion& a_view)
		{
			auto pending = SunExclusionSlot::Get().Pending();
			if (!pending)
				return;
			a_view.candidates = pending->candidates.get();
			a_view.excluded = pending->excluded;
			a_view.version = pending->version;
			a_view.held = std::move(pending);
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto strictParity = a_context.RegisterSwitch({ .name = "CS_DCLF_STRICT_LIGHT_PARITY", .kind = Api::SwitchKind::Parity,
				.description = "1: the portal-strict point lights' native culls against DCLF's model of them, on the parity frames (AE)" });
			StrictLightParity::Configure(SwitchEnabled(strictParity));
			const auto strictCulls = a_context.RegisterSwitch({ .name = "CS_DCLF_STRICT_LIGHT_CULLS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "0: the portal-strict point lights keep the engine's culls (else DCLF's strict gates stand for them)" });
			LocalLightCull::SetStrictCulls(SwitchValue(strictCulls) != "0");
			// The frustum and parabolic views draw while their sources are registered, from the lights the selection keeps (the Lights
			// board); the focus views, whatever their host, from the focus cameras' hook (the Focus board). Core reads both through the
			// sources' cameras.
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Spot, .camera = &LightCamera, .lightKept = &LightKept });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Point, .camera = &LightCamera, .lightKept = &LightKept, .frameStart = &PointFrameStart,
				.frameEnd = &LocalLightCull::EndFrame });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Focus, .camera = &FocusCamera });
			// The next frame's exclusions, the sun's and the paraboloids' (one build, a cache each), which the shadow epoch hands back into
			// their slots for the full-frustum cull and the point lights' culls; the sun's forwarded at the frame start and read by the
			// primary cull's scene-list filter.
			a_context.RegisterShadowExclusion({ .name = "entry exclusions", .build = &BuildExclusion, .publish = &PublishExclusion,
				.frameStart = &ExclusionFrameStart, .listExclusion = &ListExclusion });
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
