// Shadows/Sun: the Sun source and its camera hook (SunViews), and their parity (Diagnostics/).

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		/** @brief The Sun source's camera hook (by source: Core installs the light selection right after it). */
		void CameraHooks(Api::ShadowSource a_source)
		{
			if (a_source == Api::ShadowSource::Sun)
				SunViews::Get().Install();
		}

		void Install(Api::InstallContext& a_context)
		{
			// The directional views draw while the source is registered; its cameras are the hook's (the Sun board).
			a_context.RegisterShadowSource({ .source = Api::ShadowSource::Sun });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the sun views, the accumulation's cascades and the entry rule's planes
			// against the engine's.
			if (Diagnostics::Observe().claimsOrScene) {
				SubscribeSunViewParity();
				SubscribeSunEntryPlanesParity();
			}
		}
	}

	Api::Feature Feature()
	{
		return { .name = "sun shadows", .install = &Install, .cameraHooks = &CameraHooks };
	}
}
