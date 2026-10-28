#pragma once

// The pass contract (Api::InstallContext::RegisterPass): a graph pass the frame's schedule (Schedule/Frame.cpp) names by key, made at the
// graph's build in the epoch the schedule puts it in.

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class RenderPass;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/PassContext.h), which the features inside DCLF include for them.
	class PassContext;  // what a pass is made from: the epoch and its extension's resources
	class PassOut;      // where an anchor adds its passes
	class PassSetup;    // what a pass's setup is handed: the device and the scene's tables

	/** @brief The kind of a pass (the graph's ExternalPassDesc). */
	enum class PassKind : std::uint8_t
	{
		Compute,
		Copy,
		Render,
	};

	/** @brief The queue a pass prefers. Default: the graphics queue for a copy or compute pass, no preference for a render pass. */
	enum class PassQueue : std::uint8_t
	{
		Default,
		Compute,
		None,
	};

	/**
	 * @brief A pass the schedule (Schedule/Frame.cpp) names by key. Exactly one of make and emit:
	 * - make: render thread, at the graph's build: the pass, or null (its conditions unmet: not added). Added under passName, as kind,
	 *   on queue.
	 * - emit: an anchor, which adds any number of passes, each with its own name, kind and queue (the diagnostics' passes at a point).
	 * passName: the graph pass's name; "{view}" in it becomes the view's prefix (ViewTraits::prefix). The strings are copied.
	 *
	 * With make, optionally:
	 * - setup: render thread, once, where the scene's GPU tables are made (the first frame with a device), after the features' scene
	 *   resources (Api/SceneResources.h), in registration order: makes the pass's programs and buffers, or takes them from its feature's
	 *   scene resources. Its result is the pass's state, held with the scene's tables (PassContext::State); while it is null the pass is
	 *   not added (make is not called). Only in a Main or View epoch (their scene's tables); the install fails otherwise.
	 * - latchWord: render thread, at each depth commit: the value of the depth segment's BuildDrawsLatch stage word (byte 300), which the
	 *   pass's shader reads from the latch. The latch has one such word: a second pass with one fails the install (grow BuildDrawsLatch,
	 *   Core/Render/Resources/GpuLayouts.h, first), as does a key the schedule names anywhere but in main.depth before core.z.build-draws.
	 * - animatedRows: render thread, at each latch of the main rows (PayloadLatch: the depth and colour segments', the views'): the device
	 *   address of the frame's copy of the installed payload's animated material rows (MaterialRowHeader::animatedRow: row k at k *
	 *   kMaterialRowBytes), BuildDrawsLatch::animatedRows, which the pass writes whole before any of the frame's draws and BuildDraws
	 *   points those rows' draws at (0: none, the draws read the installed rows). One pass may have one, scheduled as latchWord's is.
	 * - frameRows: render thread, at each latch of the main rows: the device address and row count of the frame's per-object rows the
	 *   pass writes (BuildDrawsLatch::frameRows; Features/Fades' object rows, which the pixel stages read by a frame binding), so that a
	 *   recording the graph keeps across frames finds the frame's backing. One pass may have one, scheduled as latchWord's is.
	 * - cpuPipeline: the pass is in its epoch in the CPU-driven pipeline too (dclf:pipeline=cpu), whose graph has its epochs external
	 *   (Core/Render/Epochs/GraphConditions.h): the revisions record it, and its site executes the recording in DXVK's command buffer
	 *   before its draws, on the graphics queue (a preferred queue is not taken there). False: the pass is left out there (the CPU does
	 *   its work).
	 * - recordAtExecution (with cpuPipeline): the pass is in the CPU-driven pipeline only, recorded by its site's execution on DXVK's CS
	 *   thread into DXVK's command buffer (ORG's late batch: ExternalPassDesc::RecordAtExecution), once its views' culls are in: the
	 *   direct sites' draws (Core/Render/Direct/SitePasses.cpp).
	 */
	/** @brief A pass's frame rows (PassDef::frameRows): its frame's backing's device address and rows (0: none this frame). */
	struct FrameRows
	{
		std::uint64_t address = 0;
		std::uint32_t count = 0;
	};

	struct PassDef
	{
		std::string_view key;
		std::string_view passName;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::shared_ptr<org::RenderPass> (*make)(const PassContext& a_context) = nullptr;
		void (*emit)(const PassContext& a_context, PassOut& a_out) = nullptr;
		std::shared_ptr<const void> (*setup)(const PassSetup& a_setup) = nullptr;
		std::uint32_t (*latchWord)() = nullptr;
		std::uint64_t (*animatedRows)() = nullptr;
		FrameRows (*frameRows)() = nullptr;
		bool cpuPipeline = false;
		bool recordAtExecution = false;
	};
}
