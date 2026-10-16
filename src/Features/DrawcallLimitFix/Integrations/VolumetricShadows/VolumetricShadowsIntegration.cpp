// Volumetric Shadows: its shared shadow map (t18), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/VolumetricShadows/VolumetricShadows.h"

#include "Features/VolumetricShadows.h"

namespace DCLF::Integrations::VolumetricShadows
{
	namespace
	{
		static_assert(::VolumetricShadows::kSharedShadowMapShaderSlot == kSharedShadowMapRegister);

		// The view CopyShadowLightData last bound at t18 (SetSharedShadowMapSRV, the only PS binding there): its downsampled copy with
		// directional shadows, a persistent texture made at its first copy (last frame's shadows at the faces' commit, which precedes this
		// frame's shadow mask), else null. Null: the captured view, also null.
		void* SharedShadowMapView()
		{
			return globals::features::volumetricShadows.loaded ? ::VolumetricShadows::boundSharedShadowMap : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "volumetric shadows shadow map", .textureRegister = kSharedShadowMapRegister,
				.view = &SharedShadowMapView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "volumetric shadows", &Install };
	}
}
