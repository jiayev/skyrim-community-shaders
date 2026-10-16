#pragma once

// The depth pyramid contract (Api::InstallContext::RegisterDepthPyramid): the hierarchical depth Core's cull tests against (the HZB),
// made and filled by a feature. Core keeps the test (CS_DCLF_CULL's mode, BuildDrawsCS's HZB branch, HzbTest.hlsli), the latch's words,
// the bindings and the phase 2 block; the producer owns the texture, its program and its pass.

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class Buffer;
	class PixelBuffer;
	class RenderPass;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/DepthPyramid.h), which the features inside DCLF include for them.
	class DepthPyramidSetup;
	class DepthPyramidContext;

	/**
	 * @brief What a pyramid's setup makes. Mip 0 is half the depth's next power of two a side, so a mip is chosen from a screen extent by
	 * log2 alone (HzbTest.hlsli's and the latch's convention).
	 */
	struct DepthPyramid
	{
		std::shared_ptr<org::PixelBuffer> texture;  // null: no pyramid (frustum culling only, no phase 2 passes)
		std::uint32_t width = 0, height = 0, mips = 0;
		std::shared_ptr<org::Buffer> scratch;  // optional words Core zeroes once, at the first depth commit after setup
	};

	/**
	 * @brief The pyramid's producer (one: a second fails the install).
	 *
	 * - setup: render thread, where Core makes the main view's resources (the first colour frame, a target change), after the
	 *   refraction normals' import: fills a_out. Its result is the producer's state, held with those resources.
	 * - makePass: render thread, when the graph is built (not per frame), only while the setup gave a texture: the pass filling the
	 *   pyramid from the main depth, added under passName in the depth epoch after its depth draw (cs.dclf.z.depth), before phase 2's
	 *   retest build (cs.dclf.build-draws-phase2), on the graphics queue.
	 */
	struct DepthPyramidContribution
	{
		std::string_view resource;         // the texture's graph name
		std::string_view scratchResource;  // the scratch's graph name (registered only while setup gives one)
		std::string_view passName;         // the graph pass's name
		std::shared_ptr<const void> (*setup)(const DepthPyramidSetup& a_setup, DepthPyramid& a_out) = nullptr;
		std::shared_ptr<org::RenderPass> (*makePass)(const DepthPyramidContext& a_context) = nullptr;
	};
}
