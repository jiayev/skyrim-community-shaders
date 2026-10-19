// LocalLights: the light selection (installed from the Spot source's hooks), the light volumes' contribution (LocalShadowLights' sample)
// and the selection's parity (Diagnostics/).

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightContextResources.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalShadowLights.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/Diagnostics/LightSelectionParity.h"

#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <cstring>

namespace DCLF::Features::LocalLights
{
	namespace
	{
		/**
		 * @brief The light selection, at the Spot source (by source: after the sun's accumulation and camera, before the point lights'
		 * culls and the focus cameras, the install log's order).
		 */
		void SourceHooks(Api::ShadowSourceHooks& a_hooks)
		{
			if (a_hooks.source == Api::ShadowSource::Spot)
				LightSelection::Get().Install();
		}

		/** @brief Local shadow lights accumulated this frame, a volume per shadowmap descriptor, for Light Limit Fix's shadow mask. */
		void Sample(Api::LightVolumeSink& a_sink)
		{
			const auto localShadows = LocalShadowLights::Sample();
			for (const auto& light : localShadows.lights) {
				for (const auto& volume : light.volumes) {
					Api::LightVolume out;
					out.masks[0] = volume.masks[0];
					out.masks[1] = volume.masks[1];
					out.maskBit = light.maskBit;
					out.affectsLand = light.affectsLand;
					out.sphere[0] = light.center[0];
					out.sphere[1] = light.center[1];
					out.sphere[2] = light.center[2];
					out.sphere[3] = light.radius;
					std::memcpy(out.planes, volume.planes.data(), sizeof(out.planes));
					a_sink.Put(out);
				}
			}
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterLightContexts(a_context);
			// Each colour commit's volumes, from the selection's lights and the Lights board; the landscape properties take only the
			// lights that affect land.
			a_context.RegisterLightVolumes({ .name = "local shadow lights", .landscapeFlags = kLandscapeFlags, .sample = &Sample });
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the light selection and its rooms against the engine's.
			if (Diagnostics::Observe().claimsOrScene)
				Diagnostics::LightSelectionParity::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "local lights", .install = &Install, .sourceHooks = &SourceHooks };
	}
}
