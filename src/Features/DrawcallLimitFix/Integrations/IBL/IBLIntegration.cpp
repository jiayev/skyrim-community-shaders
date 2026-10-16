// Image Based Lighting: its environment and sky SH (t76, t77) and static cubemaps (t78, t79), frame textures. The render thread's callbacks
// read the feature live.

#include "Features/DrawcallLimitFix/Integrations/IBL/IBL.h"

#include "State.h"

#include "Features/IBL.h"

namespace DCLF::Integrations::IBL
{
	namespace
	{
		// The SH Prepass computes and binds at t76 and t77 unless IBL is off for the scene (the only PS bindings there besides the
		// reflection prepass's, the same views or null): persistent textures made at setup (last frame's SH at the faces' commit, which
		// the reflection prepass bound). Off for the scene, Prepass binds nothing: the captured views.
		template <auto Texture>
		void* ShView()
		{
			const auto& ibl = globals::features::ibl;
			return ibl.loaded && ibl.*Texture && !ibl.IsDisabledForCurrentScene() ? (ibl.*Texture)->srv.get() : nullptr;
		}

		// The static cubemaps (DDS loaded at setup) ReflectionsPrepass binds at t78 and t79 (their only PS bindings), whatever the scene:
		// only on a frame whose water reflection ran it (State::activeReflections, set by Deferred::ReflectionsPrepasses until Present).
		// Otherwise the bindings stand from an earlier frame, or were never made: the captured views.
		template <auto Texture>
		void* StaticView()
		{
			const auto& ibl = globals::features::ibl;
			return ibl.loaded && globals::state->activeReflections && ibl.*Texture ? (ibl.*Texture)->srv.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "ibl environment", .textureRegister = kEnvironmentRegister,
				.view = &ShView<&::IBL::envIBLTexture>, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "ibl sky", .textureRegister = kSkyRegister, .view = &ShView<&::IBL::skyIBLTexture>,
				.capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "ibl static diffuse", .textureRegister = kStaticDiffuseRegister,
				.view = &StaticView<&::IBL::staticDiffuseIBLTexture>, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "ibl static specular", .textureRegister = kStaticSpecularRegister,
				.view = &StaticView<&::IBL::staticSpecularIBLTexture>, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "image based lighting", &Install };
	}
}
