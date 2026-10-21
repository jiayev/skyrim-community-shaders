#pragma once

// What the two view families share (Api/Views.h: the engine views Core's bucketed view machinery draws; Api/ShadowViews.h: the
// occlusion maps and the shadow sources Core's shadow engine draws): the engine points a view's work runs at, and a feature view's own
// bits, which Core checks across both families once all are registered (Extensions::ResolveViewBits).

#include <cstdint>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/Views.h): what a view's work is handed.
	class ViewPointContext;

	/** @brief The engine points a view's work runs at (render thread, while DCLF runs). */
	enum class ViewPoint : std::uint8_t
	{
		FrameStartBeforeCoverage,  // FrameDriver::BeginFrame, after SyncFrameTables, before the shadow exclusion's frame start, Extensions::ShadowExclusionFrameStart and DecideCoverage
		FrameStartAfterTreeLod,    // FrameDriver::BeginFrame, after DecideDrawFamilies (tree LOD's), before PublishListFilter and FrameValues::Kick
		DepthCommitted,            // Main::RenderDepth's world-drawn point, inside the depth commit, before its epoch's submission (FrameDriver::DepthCommitted: a view epoch the depth epoch hosts commits there); after the depth epoch when it did not commit
		Count,
	};

	/**
	 * @brief A feature view's own bits: its set phase (the feature's SetPhase bit, one of the eight) and its view bit at the GPU cull (the
	 * feature's kView*, one bit). Neither is Core's nor another view's, of either family (checked at install and by Features/Manifest.cpp).
	 */
	struct ViewBits
	{
		std::uint8_t phase = 0;
		std::uint32_t viewBit = 0;
	};
}
