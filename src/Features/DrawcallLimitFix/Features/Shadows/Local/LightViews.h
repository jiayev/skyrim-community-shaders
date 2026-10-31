#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Api
{
	class ShadowSourceCamera;
}

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief The local shadow lights' views: spot (BSShadowFrustumLight) and point (BSShadowParabolicLight, one view per hemisphere)
	 * lights, from the cameras their UpdateCamera has set up, for the lights DCLF selects (the lights it kept, at
	 * CalculateActiveShadowCasterLights' return: Features/LocalLights' selection). Per descriptor: the camera, and the planes the light's
	 * culling process tests.
	 *
	 * Render thread: written as the selection keeps each light (LightViews), read by Core through the Spot and Point sources' camera (the
	 * shadow views' capture) and by Features/LocalLights' light volumes' sample (Api::LightVolumesContribution, through the same) later
	 * in the frame; Features/Actors' actor rate (ActorRate.cpp) reads the last scene frame's.
	 */
	struct LightFrame
	{
		struct Light
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<ShadowCameras::Camera> views;  // by descriptor
			std::array<float, 4> sphere{};  // the light's position and radius (radius 0: none)
		};
		std::vector<Light> lights;  // this frame's, in the selection's order
		std::uint32_t sceneFrame = ~0u;

		/** @brief Descriptor a_descriptor of a_light's views this frame, or null (the light not selected this frame). */
		const ShadowCameras::Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& entry : lights)
				if (entry.light == a_light)
					return a_descriptor < entry.views.size() && entry.views[a_descriptor].valid ? &entry.views[a_descriptor] : nullptr;
			return nullptr;
		}

		/**
		 * @brief For Features/Actors' actor rate (ActorRate.cpp): every view of the lights kept in scene frame a_sceneFrame (none when the
		 * views are older), with its light's sphere (centre, radius: what a point light's paraboloid cull tests, its views having no planes).
		 */
		template <class F>
		void ForEachView(std::uint32_t a_sceneFrame, F&& a_visit) const
		{
			if (sceneFrame != a_sceneFrame)
				return;
			for (const auto& entry : lights)
				for (const auto& view : entry.views)
					if (view.valid)
						a_visit(view, entry.sphere);
		}
	};

	/** @brief The board (render thread; see LightFrame). Written in place by LightViews: its storage is reused. */
	LightFrame& LightsBoard();

	/**
	 * @brief The local shadow lights' views as DCLF computes them, into the feature's board (LightsBoard): spot (BSShadowFrustumLight)
	 * and point (BSShadowParabolicLight, one view per hemisphere) lights, from the cameras their UpdateCamera has set up - for the lights
	 * DCLF selects (LightSelection), at CalculateActiveShadowCasterLights' (0x1414cc570) return, which ran each kept light's UpdateCamera
	 * (vfunc 0x10) before its Accumulate. Nothing here reads a cull or the renderer's state at a view's draw; those captures are the
	 * observers' alone (Diagnostics::ShadowViewTally).
	 *
	 * Per descriptor, ShadowCameras' arithmetic: the matrices and eye SetCameraData builds, the viewport of the camera's port, and the
	 * planes the light's culling process tests (Process2: its custom planes, else the camera's frustum). The focus views are
	 * FocusViews'; the target, slice and raster state are read at the draw.
	 *
	 * Render thread: written as LightSelection keeps each light (the Spot and Point sources' lightKept), read through the sources'
	 * camera (LightCamera) by the shadow views' capture and LocalShadowLights later in the frame.
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

	/** @brief The Spot and Point sources' camera (Api::ShadowSourceContribution::camera): a_camera's light's view this frame, from the board. */
	void LightCamera(Api::ShadowSourceCamera& a_camera);
}
