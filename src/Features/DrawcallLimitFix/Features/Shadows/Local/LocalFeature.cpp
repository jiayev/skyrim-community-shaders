// Shadows/Local: the Spot, Point and Focus sources, the kept lights' views (LightViews) and the focus cameras' hook (FocusViews).

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		/** @brief A kept spot or point light's views, from its cameras (LightViews: the Lights board). */
		void LightKept(const Api::ShadowLightKept& a_light) { LightViews::Get().Update(a_light.light); }

		/** @brief The Focus source's camera hook (by source: after the Sun's and Core's light selection). */
		void CameraHooks(Api::ShadowSource a_source)
		{
			if (a_source == Api::ShadowSource::Focus)
				FocusViews::Get().Install();
		}

		void Install(Api::InstallContext& a_context)
		{
			// The frustum and parabolic views draw while their sources are registered, from the lights the selection keeps; the focus
			// views, whatever their host, from the focus cameras' hook (the Focus board).
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Spot, .lightKept = &LightKept });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Point, .lightKept = &LightKept });
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Focus });
		}
	}

	Api::Feature Feature()
	{
		return { .name = "local shadows", .install = &Install, .cameraHooks = &CameraHooks };
	}
}
