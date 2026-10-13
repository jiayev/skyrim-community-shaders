#pragma once

#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF
{
	/**
	 * @brief The local shadow lights' views as DCLF computes them (T3a, 2026-10-08): spot (BSShadowFrustumLight) and point
	 * (BSShadowParabolicLight, one view per hemisphere) lights, from the cameras their UpdateCamera has set up - for the lights DCLF
	 * selects (LightSelection, T3b), at CalculateActiveShadowCasterLights' (0x1414cc570) return, which ran each kept light's
	 * UpdateCamera (vfunc 0x10) before its Accumulate. Nothing here reads a cull or the renderer's state at a view's draw; those
	 * captures are the observers' alone (Diagnostics::ShadowViewTally).
	 *
	 * Per descriptor, SunViews' arithmetic: the matrices and eye SetCameraData builds, the viewport of the camera's port, and the
	 * planes the light's culling process tests (Process2: its custom planes, else the camera's frustum). The focus views stay the
	 * engine's (T3c); the target, slice and raster state are read at the draw (T7).
	 *
	 * Render thread: written by LightSelection, read by the shadow views' capture and LocalShadowLights later in the frame.
	 */
	class LightViews
	{
	public:
		static LightViews& Get();
		/** @brief A light LightSelection kept: its views from its cameras, this frame's (the first of a frame drops the last frame's). */
		void Update(const RE::BSShadowLight* a_light);

		/** @brief Descriptor a_descriptor of a_light's views this frame, or null (the light not selected this frame). */
		const SunViews::Cascade* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const;

		/**
		 * @brief T7f (SideEffects' actor rate): every view of the lights kept in scene frame a_sceneFrame (none when the views are older), with
		 * its light's sphere (centre, radius: what a point light's paraboloid cull tests, its views having no planes).
		 */
		template <class F>
		void ForEachView(std::uint32_t a_sceneFrame, F&& a_visit) const
		{
			if (sceneFrame != a_sceneFrame)
				return;
			for (const auto& light : lights)
				for (const auto& view : light.views)
					if (view.valid)
						a_visit(view, light.sphere);
		}


	private:
		LightViews() = default;

		struct Light
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<SunViews::Cascade> views;  // by descriptor
			std::array<float, 4> sphere{};          // the light's position and radius (radius 0: none)
		};
		std::vector<Light> lights;  // this frame's, in the selection's order
		std::uint32_t sceneFrame = ~0u;
	};
}
