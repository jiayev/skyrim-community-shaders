#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace DCLF::Api
{
	class ShadowSourceCamera;
	class SunCascadeSink;
	class SunEntryPlanesSink;
}

namespace DCLF::Features::Shadows::Sun
{
	/**
	 * @brief The sun's views: from the shadow cameras BSShadowDirectionalLight::UpdateCamera (0x141512230) has just set up, before any
	 * of the engine's culls or draws of them: each cascade's camera (its caster volume, written to the cascade's culling process, the
	 * sun bits' test), and what each process of the full-frustum cull tests, through the full-frustum camera (FUN_141511f30; the sun's
	 * entry rule, kCullSunEntry). Nothing here reads the engine's cascade culls or the renderer's state at a view's draw; those captures
	 * are the observers' alone (Diagnostics::ShadowViewTally).
	 *
	 * Render thread: written by the sun's camera hook (CalculateActiveShadowCasterLights, before the full-frustum cull and Accumulate),
	 * read by Core on the same thread through the Sun source's camera (the shadow views' capture) and the sun volume (the colour commits'
	 * cascades, the inputs' preparation); the Sun source's accumulation reads the cascades on its registration threads only after its
	 * bitsReady release, which follows; Features/Actors' actor rate (ActorRate.cpp) reads the last scene frame's.
	 */
	struct SunFrame
	{
		static constexpr std::uint32_t kMaxCascades = 4;

		std::uint32_t sceneFrame = ~0u;
		std::uint32_t count = 0;
		std::array<ShadowCameras::Camera, kMaxCascades> cascades{};
		std::vector<ShadowCameras::Planes> fullFrusta;  // what each process of the full-frustum cull tests (Process2), by process
		bool valid = false;

		/** @brief Cascade a_descriptor of this frame's views (valid, sceneFrame the frame's), or null. */
		const ShadowCameras::Camera* CascadeOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (!valid || sceneFrame != a_sceneFrame || a_descriptor >= count || !cascades[a_descriptor].valid)
				return nullptr;
			return &cascades[a_descriptor];
		}
	};

	/** @brief The board (render thread; see SunFrame). Written in place by the camera hook: its storage is reused. */
	SunFrame& SunBoard();

	/**
	 * @brief The sun's camera hook: BSShadowDirectionalLight::UpdateCamera (0x141512230), which writes the sun's views into the feature's
	 * board (SunBoard) from the shadow cameras it has just set up, before any of the engine's culls or draws of them.
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

	/** @brief The Sun source's camera (Api::ShadowSourceContribution::camera): a_camera's cascade of this frame's views, from the board. */
	void SunCamera(Api::ShadowSourceCamera& a_camera);
	/** @brief The sun volume's cascades (Api::SunVolumeContribution::cascades): this frame's, from the board, in descriptor order. */
	void SampleSunCascades(Api::SunCascadeSink& a_sink);
	/** @brief The sun volume's entry planes (Api::SunVolumeContribution::entryPlanes): the board's full-frustum planes of a_frame or the one before. */
	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame);
}
