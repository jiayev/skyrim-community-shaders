#pragma once

namespace RE
{
	class BSShadowLight;
}

namespace DCLF
{
	/**
	 * @brief The local shadow lights' views as DCLF computes them, into Core's board (ShadowCameras::Lights): spot (BSShadowFrustumLight)
	 * and point (BSShadowParabolicLight, one view per hemisphere) lights, from the cameras their UpdateCamera has set up - for the lights
	 * DCLF selects (LightSelection), at CalculateActiveShadowCasterLights' (0x1414cc570) return, which ran each kept light's UpdateCamera
	 * (vfunc 0x10) before its Accumulate. Nothing here reads a cull or the renderer's state at a view's draw; those captures are the
	 * observers' alone (Diagnostics::ShadowViewTally).
	 *
	 * Per descriptor, ShadowCameras' arithmetic: the matrices and eye SetCameraData builds, the viewport of the camera's port, and the
	 * planes the light's culling process tests (Process2: its custom planes, else the camera's frustum). The focus views are
	 * FocusViews'; the target, slice and raster state are read at the draw.
	 *
	 * Render thread: written as LightSelection keeps each light (the Spot and Point sources' lightKept), read by the shadow views'
	 * capture and LocalShadowLights later in the frame.
	 */
	class LightViews
	{
	public:
		static LightViews& Get();
		/** @brief A light LightSelection kept: its views from its cameras, this frame's (the first of a frame drops the last frame's). */
		void Update(const RE::BSShadowLight* a_light);

	private:
		LightViews() = default;
	};
}
