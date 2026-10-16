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
	 * - emit: an anchor, which adds any number of passes, each with its own name, kind and queue (the GPU stages at a slot, the draw
	 *   families' face culls, the diagnostics' passes at a point).
	 * passName: the graph pass's name; "{view}" in it becomes the view's prefix (ViewTraits::prefix). The strings are copied.
	 */
	struct PassDef
	{
		std::string_view key;
		std::string_view passName;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::shared_ptr<org::RenderPass> (*make)(const PassContext& a_context) = nullptr;
		void (*emit)(const PassContext& a_context, PassOut& a_out) = nullptr;
	};
}
