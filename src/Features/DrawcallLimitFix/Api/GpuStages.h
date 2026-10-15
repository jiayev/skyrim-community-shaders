#pragma once

// The GPU stage contract (Api::InstallContext::RegisterGpuStage): a compute pass a feature adds to the scene's graph at a named place.

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class RenderPass;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/GpuStages.h), which the features inside DCLF include for them.
	class GpuStageSetup;
	class GpuStageContext;

	/** @brief Where a stage's pass goes; stages at one place run in registration order. */
	enum class GpuStageSlot : std::uint8_t
	{
		// The depth (Z-prepass) epoch, after its latched copies and the next frame's scene state, right before its BuildDraws: before
		// anything of the frame draws. On the graphics queue.
		DepthSegmentBeforeBuild,
	};

	/**
	 * @brief A compute stage.
	 *
	 * - setup: render thread, once, where the scene's GPU tables are made (the first frame with a device): makes the stage's programs and
	 *   buffers. Its result is the stage's state, held with the scene's tables; null keeps the stage out of the graph.
	 * - makePass: render thread, when the graph is built (not per frame), only for a stage whose setup gave a state: its pass, given the
	 *   main view's resources and that state. Added at slot under passName.
	 * - latchWord (optional): render thread, at each depth commit (DepthSegmentBeforeBuild only): the value of the depth segment's
	 *   BuildDrawsLatch stage word (byte 300), which the stage's shader reads from the latch. The latch has one such word: a second stage
	 *   needing one grows BuildDrawsLatch (Core/Render/Resources/GpuLayouts.h) first; registering it fails the install.
	 */
	struct GpuStage
	{
		std::string_view passName;  // the graph pass's name
		GpuStageSlot slot = GpuStageSlot::DepthSegmentBeforeBuild;
		std::shared_ptr<const void> (*setup)(const GpuStageSetup& a_setup) = nullptr;
		std::shared_ptr<org::RenderPass> (*makePass)(const GpuStageContext& a_context) = nullptr;
		std::uint32_t (*latchWord)() = nullptr;
	};
}
