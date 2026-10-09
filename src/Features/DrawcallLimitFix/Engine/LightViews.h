#pragma once

#include "SunViews.h"

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
	 * (BSShadowParabolicLight, one view per hemisphere) lights, from the cameras their UpdateCamera has set up - read at the light's
	 * Accumulate (vfunc 9), which CalculateActiveShadowCasterLights (0x1414cc570) calls for each active light after its UpdateCamera
	 * (vfunc 0x10) and whose first act is the light's cull (FUN_1414f0920). Nothing here reads a cull or the renderer's state at a
	 * view's draw; those captures are the parity's alone.
	 *
	 * Per descriptor, SunViews' arithmetic: the matrices and eye SetCameraData builds, the viewport of the camera's port, and the
	 * planes the light's culling process tests (Process2: its custom planes, else the camera's frustum). Which lights, their
	 * shadow-map slots and their focus views stay the engine's (T3b, T3c); the target, slice and raster state are read at the draw (T7).
	 *
	 * Render thread: written in the Accumulate hook, read by the shadow views' capture later in the frame on the same thread.
	 */
	class LightViews
	{
	public:
		static LightViews& Get();
		void Install();

		/** @brief Descriptor a_descriptor of a_light's views this frame, or null (the light not accumulated this frame). */
		const SunViews::Cascade* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const;

		SunViews::Parity& GetParity() { return parity; }
		/** @brief The parity's line (`<- LIGHT VIEW`), every 300 frames with local views; resets it. */
		void Report();

	private:
		LightViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const RE::BSShadowLight* a_light);

		struct Light
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<SunViews::Cascade> views;  // by descriptor
		};
		std::vector<Light> lights;  // this frame's, in Accumulate's order
		std::uint32_t sceneFrame = ~0u;
		std::uint32_t reportFrames = 0;
		SunViews::Parity parity;
		bool installed = false;
	};
}
