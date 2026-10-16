// Water Effects: its caustics texture (t65), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/WaterEffects/WaterEffects.h"

#include "Features/WaterEffects.h"

namespace DCLF::Integrations::WaterEffects
{
	namespace
	{
		// The view Prepass binds at t65 every frame (the only PS binding there): the caustics texture loaded at setup, a static texture.
		// Null without the feature, or without the texture (Prepass binds null): the captured view.
		void* CausticsView()
		{
			const auto& waterEffects = globals::features::waterEffects;
			return waterEffects.loaded ? waterEffects.causticsView.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "water effects caustics", .textureRegister = kCausticsRegister, .view = &CausticsView,
				.capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "water effects", &Install };
	}
}
