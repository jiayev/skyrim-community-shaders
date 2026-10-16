#pragma once

// The GPU stage contract (Api::InstallContext::RegisterGpuStage): a compute or copy pass a feature adds to the scene's graph at a named place.

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
		// The depth epoch, after its latched copies: the next frame's scene state, which nothing of this frame reads, so it overlaps the
		// epoch's raster work on the compute queue (GpuQueue::Compute). The host orders it against the adjacent frames; its readers need
		// not declare it. Before DepthSegmentCullState's and DepthSegmentBeforeBuild's stages.
		DepthSegmentNextFrameState,
		// The depth epoch, after DepthSegmentNextFrameState's stages, before DepthSegmentBeforeBuild's: the next frame's cull state, which
		// the next frame's cull reads (the root states' step, then its write-back copy: Features/Fades' cs.dclf.z.fade-state on
		// GpuQueue::Compute and cs.dclf.z.fade-events, a Copy on GpuQueue::None). On the queue each stage asks for.
		DepthSegmentCullState,
		// The depth epoch, after its BuildDraws and the index pool's copies (cs.dclf.z.index-pool), right before its depth draw
		// (cs.dclf.z.depth): phase 1's work a draw family's depth draw reads (Api/DrawFamilies.h). On the graphics queue.
		DepthSegmentBeforeDepthDraw,
		// The depth epoch's phase 2, only with the HZB: after its retest build (cs.dclf.build-draws-phase2), right before its depth draw
		// (cs.dclf.depth-phase2): phase 2's work against the HZB rebuilt this frame.
		DepthSegmentPhaseTwoBeforeDraw,
		// The depth epoch, after the phase 2 block (or the depth draw, without the HZB), before the visibility events
		// (cs.dclf.z.visibility-events): what reads both phases' results (a readback copy: GpuStageKind::Copy, GpuQueue::None).
		DepthSegmentAfterPhaseTwo,
	};

	/** @brief The kind of a stage's pass (the graph's ExternalPassDesc). */
	enum class GpuStageKind : std::uint8_t
	{
		Compute,
		Copy,
	};

	/** @brief The queue a stage's pass prefers (the graph's PreferQueue). */
	enum class GpuQueue : std::uint8_t
	{
		Graphics,
		Compute,
		None,  // no preference asked (a readback copy the graph places)
	};

	/**
	 * @brief A stage: a compute pass, or a copy (kind).
	 *
	 * - setup: render thread, once, where the scene's GPU tables are made (the first frame with a device), after the features' scene
	 *   resources (Api/SceneResources.h): makes the stage's programs and buffers, or takes them from its feature's scene resources.
	 *   Its result is the stage's state, held with the scene's tables; null keeps the stage out of the graph.
	 * - makePass: render thread, when the graph is built (not per frame), only for a stage whose setup gave a state: its pass, given the
	 *   main view's resources and that state. Added at slot under passName, as kind, on queue.
	 * - latchWord (optional): render thread, at each depth commit (DepthSegmentBeforeBuild only): the value of the depth segment's
	 *   BuildDrawsLatch stage word (byte 300), which the stage's shader reads from the latch. The latch has one such word: a second stage
	 *   needing one grows BuildDrawsLatch (Core/Render/Resources/GpuLayouts.h) first; registering it fails the install.
	 */
	struct GpuStage
	{
		std::string_view passName;  // the graph pass's name
		GpuStageSlot slot = GpuStageSlot::DepthSegmentBeforeBuild;
		GpuQueue queue = GpuQueue::Graphics;
		std::shared_ptr<const void> (*setup)(const GpuStageSetup& a_setup) = nullptr;
		std::shared_ptr<org::RenderPass> (*makePass)(const GpuStageContext& a_context) = nullptr;
		std::uint32_t (*latchWord)() = nullptr;
		GpuStageKind kind = GpuStageKind::Compute;
	};
}
