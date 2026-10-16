#pragma once

// The shadow views' frame point, Core's half (render thread), as FrameDriver calls it at BeforeShadowMaps: the frame's shadow views listed
// and the revision inputs they move. The scene side's: the capture attributes and withholds by this list. The epochs' half (the views'
// coverage, the shadow frame, the shadow and occlusion epochs, the maps' readiness) is Features/Shadows/Engine's, which FrameDriver calls
// through Api/ShadowEpochs.h. FrameDriver names nothing of the shadow engine behind it (Core/Render/Shadow: ShadowPoints.cpp).
// Self-contained.

namespace DCLF::Draws
{
	/**
	 * @brief BeforeShadowMaps: the frame's shadow views, in the order the engine is about to render them (their capacity,
	 * ShadowViews::Rebuild), and the revision inputs they move.
	 */
	void ListShadowViews();
}
