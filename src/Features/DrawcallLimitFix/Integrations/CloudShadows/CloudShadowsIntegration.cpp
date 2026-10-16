// Cloud Shadows: its cloud occlusion cubemap (t25), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"

#include "State.h"

#include "Features/CloudShadows.h"

namespace DCLF::Integrations::CloudShadows
{
	namespace
	{
		// The view ReflectionsPrepass binds at t25 (the only PS binding there): its cloud occlusion copy, a persistent texture, copied in the
		// same prepass (this frame's at the faces' commit). Only on a frame whose water reflection ran that prepass (State::activeReflections,
		// set by Deferred::ReflectionsPrepasses until Present) with the full sky and a climate, its conditions; otherwise the binding stands
		// from an earlier frame, or was never made: the captured view.
		void* CloudOcclusionView()
		{
			const auto& cloudShadows = globals::features::cloudShadows;
			if (!cloudShadows.loaded || !globals::state->activeReflections || !cloudShadows.texCubemapCloudOccCopy)
				return nullptr;
			const auto* sky = globals::game::sky;
			if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || !sky->currentClimate)
				return nullptr;
			return cloudShadows.texCubemapCloudOccCopy->srv.get();
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "cloud shadows occlusion", .textureRegister = kCloudOcclusionRegister,
				.view = &CloudOcclusionView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "cloud shadows", &Install };
	}
}
