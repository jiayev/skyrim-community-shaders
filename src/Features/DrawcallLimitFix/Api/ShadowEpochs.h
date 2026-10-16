#pragma once

// The shadow epochs contract (Api::InstallContext::RegisterShadowEpochs): the shadow and occlusion epochs' handlers at the engine's shadow
// and occlusion points, which the shell's hooks reach through Core's FrameDriver (src/Deferred.cpp, src/Features/Skylighting.cpp ->
// DrawcallLimitFix -> FrameDriver::BeforeShadowMaps, AfterShadowMaps, OcclusionReady and DrawOcclusion). FrameDriver keeps the frame's
// bookkeeping there (the shadow views listed and the revision inputs they move, its timing, the occlusion maps' phases, wanted maps and
// frame counts) and calls the handler where the epochs' work was. Features/Shadows/Engine's.

#include <cstdint>

namespace DCLF::Api
{
	/**
	 * @brief The shadow epochs (one: a second fails the install). Render thread, while DCLF runs:
	 * - beforeShadowMaps: once a frame, after the frame's shadow views are listed: their coverage decided and the shadow frame begun.
	 * - afterShadowMaps: once a frame, where the engine's shadow maps are drawn (the shadow-maps.after trigger's point): the shadow epoch.
	 * - occlusionReady: at occlusion map a_map's Ready point, with its toggle on: whether DCLF draws it this frame.
	 * - drawOcclusion: at the maps' draw point (the occlusion.draw trigger's), with a map wanted: the maps in a_maps in one epoch; returns
	 *   those drawn.
	 */
	struct ShadowEpochsContribution
	{
		void (*beforeShadowMaps)() = nullptr;
		void (*afterShadowMaps)() = nullptr;
		bool (*occlusionReady)(std::uint32_t a_map) = nullptr;
		std::uint32_t (*drawOcclusion)(std::uint32_t a_maps) = nullptr;
	};
}
