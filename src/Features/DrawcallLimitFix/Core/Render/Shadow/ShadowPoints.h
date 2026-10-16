#pragma once

// The shadow engine's frame points (render thread), as FrameDriver calls them at the engine's shadow and occlusion points: the frame's
// shadow views listed, decided and begun; their epoch; the occlusion maps' readiness and their epoch. FrameDriver names nothing of the
// shadow engine behind them (Core/Render/Shadow: ShadowPoints.cpp). Self-contained.

#include <cstdint>

namespace DCLF::Draws
{
	class ShadowPoints
	{
	public:
		/**
		 * @brief BeforeShadowMaps: the frame's shadow views, in the order the engine is about to render them (their capacity,
		 * ShadowViews::Rebuild, the revision inputs they move), then, with shadows on, their coverage decided and the shadow frame begun.
		 */
		virtual void BeforeShadowMaps() = 0;
		/** @brief AfterShadowMaps (under shadow-maps.after): the frame's shadow epoch, every captured view in one graph execution. */
		virtual void AfterShadowMaps() = 0;
		/** @brief The occlusion map a_map's Ready point: whether DCLF draws it this frame. */
		virtual bool OcclusionReady(std::uint32_t a_map) = 0;
		/** @brief The occlusion maps' draw point (under occlusion.draw): the maps in a_maps in one epoch; returns those drawn. */
		virtual std::uint32_t DrawOcclusion(std::uint32_t a_maps) = 0;

	protected:
		~ShadowPoints() = default;
	};

	/** @brief The shadow engine's frame points. */
	ShadowPoints& ShadowEnginePoints();
}
