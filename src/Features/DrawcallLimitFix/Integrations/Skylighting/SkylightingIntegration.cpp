// Skylighting: its probe array (t50) and shadow visibility (t53), frame textures. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/Skylighting/Skylighting.h"

#include "State.h"

#include "Features/Skylighting.h"

namespace DCLF::Integrations::Skylighting
{
	namespace
	{
		// Whether this frame's Prepass binds t50 and t53 (the only PS bindings there): its conditions, outside the map menu under the full
		// sky. Otherwise the bindings stand from an earlier frame, or were never made: the captured views.
		bool Binds()
		{
			if (!globals::features::skylighting.loaded || globals::state->isMapMenuOpen)
				return false;
			const auto* sky = globals::game::sky;
			return sky && sky->mode.get() == RE::Sky::Mode::kFull;
		}

		// Persistent textures made at setup, which Prepass updates before it binds them (last frame's at the faces' commit).
		void* ProbeArrayView()
		{
			const auto* probes = globals::features::skylighting.texProbeArray;
			return probes && Binds() ? probes->srv.get() : nullptr;
		}

		void* ShadowVisibilityView()
		{
			const auto* visibility = globals::features::skylighting.texShadowVisibility;
			return visibility && Binds() ? visibility->srv.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "skylighting probes", .textureRegister = kProbeArrayRegister, .view = &ProbeArrayView,
				.capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "skylighting shadow visibility", .textureRegister = kShadowVisibilityRegister,
				.view = &ShadowVisibilityView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "skylighting", &Install };
	}
}
