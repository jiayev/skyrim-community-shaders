// Exponential Height Fog: its integrated light scattering volume (t19), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"

#include "Features/ExponentialHeightFog.h"

namespace DCLF::Integrations::ExponentialHeightFog
{
	namespace
	{
		// The view Prepass binds at t19 under its own conditions (every path past them ends in BindIntegratedLightScattering; otherwise
		// ReleaseVolumetricResources binds null). Prepass runs after the faces' commit, so they read last frame's volume, or the previous
		// texture's while Prepass remakes it (a grid change), held by their binding. Null without the feature or the volume: the captured view.
		void* LightScatteringView()
		{
			const auto& fog = globals::features::exponentialHeightFog;
			const auto& settings = fog.settings;
			return fog.loaded && settings.enabled && settings.volumetricFogEnabled && settings.volumetricFogExtinctionScale > 0.0f ? fog.IntegratedLightScatteringView() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "exponential height fog light scattering", .textureRegister = kLightScatteringRegister,
				.view = &LightScatteringView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "exponential height fog", &Install };
	}
}
