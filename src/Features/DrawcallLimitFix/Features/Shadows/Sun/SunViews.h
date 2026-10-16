#pragma once

#include <cstdint>

namespace DCLF::Features::Shadows::Sun
{
	/**
	 * @brief The sun's camera hook: BSShadowDirectionalLight::UpdateCamera (0x141512230), which writes the sun's views into Core's board
	 * (ShadowCameras::Sun) from the shadow cameras it has just set up, before any of the engine's culls or draws of them.
	 *
	 * What UpdateCamera sets up and DCLF reads at its return: each cascade's camera (world transform, frustum and port), the caster
	 * volume it writes to the cascade's culling process (customCullPlanes), and the full-frustum camera. What DCLF derives from them is
	 * the engine's own arithmetic (ShadowCameras): each cascade's matrices, eye and planes (its process's, the sun bits' test), and each
	 * of the full-frustum cull's processes' planes, which cull through the full-frustum camera (FUN_141511f30; the sun's entry rule,
	 * kCullSunEntry).
	 *
	 * Render thread: written in the hook (CalculateActiveShadowCasterLights, before the full-frustum cull and Accumulate). Installed from
	 * the feature's source hooks, for the Sun source (Api::Feature::sourceHooks).
	 */
	class SunViews
	{
	public:
		static SunViews& Get();
		void Install();

	private:
		SunViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const void* a_light);

		bool installed = false;
	};
}
