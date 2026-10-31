// Dynamic Cubemaps: its reflections and environment cubemaps (t30, t31), frame textures. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"

#include "Features/DynamicCubemaps.h"

namespace DCLF::Integrations::DynamicCubemaps
{
	namespace
	{
		// The view PostDeferred binds at t31 every frame it runs (the only PS binding there): its BC6H environment cubemap, a persistent
		// texture made at setup, so the binding a colour commit captured, last frame's. Null without the feature: the captured view.
		void* EnvironmentView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded && dynamicCubemaps.envTextureBC6H ? dynamicCubemaps.envTextureBC6H->srv.get() : nullptr;
		}

		// The view PostDeferred last bound at t30 (the reflections cubemap while water reflections were active, else the environment one):
		// last frame's choice, the binding a colour commit captured. Null before its first PostDeferred or without the feature: the captured view.
		void* ReflectionsView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsView : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps reflections", .textureRegister = kReflectionsRegister,
				.view = &ReflectionsView, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps environment", .textureRegister = kEnvironmentRegister,
				.view = &EnvironmentView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "dynamic cubemaps", &Install };
	}
}
