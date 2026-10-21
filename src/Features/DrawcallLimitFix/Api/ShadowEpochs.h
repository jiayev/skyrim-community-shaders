#pragma once

// The shadow epochs contract (Api::InstallContext::RegisterShadowEpochs): the shadow epoch's handlers at the engine's shadow points, the
// occlusion maps drawn in it, which the shell's hooks reach through Core's FrameDriver (src/Deferred.cpp, src/Features/Skylighting.cpp ->
// DrawcallLimitFix -> FrameDriver::BeforeShadowMaps, OcclusionReady and AfterShadowMaps), and Features/Shadows/Engine's own hooks in the
// engine's shadow-light selection (FrameDriver::BeforeShadowCasters, ShadowCasterKept, ShadowCastersSelected). FrameDriver keeps the
// frame's bookkeeping there (the shadow views listed and the revision inputs they move, its timing, the occlusion maps' phases, the maps
// taken and drawn and frame counts) and calls the handler where the epoch's work was. Features/Shadows/Engine's.

#include <cstdint>

namespace DCLF::Api
{
	/**
	 * @brief The shadow epochs (one: a second fails the install). Render thread, while DCLF runs:
	 * - beforeShadowCasters: once a frame, before the engine keeps its first shadow light (the start of its selection, before any shadow
	 *   view registers), after Core begins the frame's views: their coverage decided for the frame.
	 * - beforeShadowMaps: once a frame, after the frame's shadow views are listed: the shadow frame begun.
	 * - occlusionReady: at occlusion map a_map's Ready point (the shadow maps, before the engine draws them), with its toggle on: whether
	 *   the frame's shadow epoch can draw it (its view in the selected revision's shadow shape).
	 * - afterShadowMaps: once a frame, where the engine's shadow maps are drawn (the shadow-maps.after trigger's point): the shadow epoch,
	 *   with the occlusion maps in a_maps (those taken at their Ready point, their views set up and their maps cleared since: 0, none);
	 *   returns the maps drawn.
	 */
	struct ShadowEpochsContribution
	{
		void (*beforeShadowCasters)() = nullptr;
		void (*beforeShadowMaps)() = nullptr;
		std::uint32_t (*afterShadowMaps)(std::uint32_t a_maps) = nullptr;
		bool (*occlusionReady)(std::uint32_t a_map) = nullptr;
	};
}
