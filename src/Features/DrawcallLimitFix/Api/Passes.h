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
	 */
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
	};
}
