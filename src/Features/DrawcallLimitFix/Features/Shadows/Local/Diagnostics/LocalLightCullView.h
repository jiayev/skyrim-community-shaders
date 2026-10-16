#pragma once

#include <array>
#include <cstdint>

namespace RE
{
	class BSBatchRenderer;
	class NiAVObject;
}

namespace DCLF::Features::Shadows::Local
{
	struct SunExclusion;

	/**
	 * @brief The point light culls' observer's (LightCulls) read-only view of LocalLightCull's file-local state (defined beside it, in
	 * LocalLightCull.cpp). Any thread the hooks run on; the frame's state is published before the culls.
	 */
	struct LocalLightCullView
	{
		static bool Installed();
		/** @brief This frame's culls skip nothing (Policy::FilterLightCulls). */
		static bool CullsWhole();
		/** @brief This frame's exclusion, or null. */
		static const SunExclusion* FrameExclusion();
		/** @brief Whether a_object is an entry a_exclusion excludes. */
		static bool Excluded(const SunExclusion& a_exclusion, const void* a_object);
		/** @brief A category filter is built (and installed). */
		static bool FilterLive();
		/** @brief Whether a_object hangs from a filtered category node's kept child, when its chain passes through one (a_through). */
		static bool UnderKeptChild(const RE::NiAVObject* a_object, bool& a_through);
		/** @brief Whether a_batch is the paraboloid shadow mode's renderer this frame. */
		static bool ParaboloidRenderer(const RE::BSBatchRenderer* a_batch);
		/** @brief The point lights accumulated this frame (at most the mask's 32), and light a_index's bit and sphere. */
		static std::uint32_t FrameLights();
		struct Light
		{
			std::uint32_t bit;
			std::array<float, 3> centre;
			float radius;
		};
		static Light FrameLight(std::uint32_t a_index);
	};
}
