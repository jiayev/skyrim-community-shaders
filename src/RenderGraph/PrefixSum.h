#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ComputeProgram.h"

#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <vector>

namespace org
{
	class Buffer;
	class RenderPass;
}

/**
 * @brief The exclusive prefix sum of a uint buffer on the GPU, as one render-graph pass (PrefixSumCS.hlsl).
 *
 * BasicRenderer's two-level block scan, which its material and raster-bucket binning use: a group per block of kBlockSize
 * elements scans its block, then one group scans the block sums and adds each block's prefix back. The usual pairing is a
 * counting sort: a pass counts keys into `counts` with atomics (keeping each item's rank from the atomic's return), this pass
 * turns the counts into each key's first slot, and a scatter writes item i to offsets[key] + rank.
 *
 * The owner creates the buffers (raw or structured uint, with unordered access), registers them with the graph, and adds the
 * pass (ExternalPassDesc::Compute) between the counting pass and the pass that reads the offsets; the graph orders the three.
 */
namespace PrefixSum
{
	inline constexpr std::uint32_t kBlockSize = 1024;  // PrefixSumCS.hlsl's
	inline constexpr std::uint32_t kMaxElements = kBlockSize * kBlockSize;

	constexpr std::uint32_t Blocks(std::uint32_t a_elements) { return (a_elements + kBlockSize - 1) / kBlockSize; }

	struct Programs
	{
		std::shared_ptr<const ComputeProgram> blockScan, blockOffsets;
	};

	/** @brief The two dispatches' programs; null when either could not be created. Share them between passes. */
	std::shared_ptr<const Programs> Load(rhi::Device a_device);

	struct Desc
	{
		std::shared_ptr<const Programs> programs;
		std::shared_ptr<org::Buffer> counts;     // uint[elements]
		std::shared_ptr<org::Buffer> offsets;    // uint[elements]: counts' exclusive prefix sum
		std::shared_ptr<org::Buffer> blockSums;  // uint[Blocks(elements)]: scratch
		std::shared_ptr<org::Buffer> total;      // optional: uint, the sum of every count
		std::uint32_t elements = 0;              // at most kMaxElements
		// Zero the counts as they are read, so that the next accumulation needs no clear of its own. The counts must then be
		// zero before the first accumulation, and every accumulation must be followed by this pass.
		bool clearCounts = false;
		// Whether an execution scans, when not every one does (null: every one). Called while the pass prepares, which can be
		// on a worker, so it must read state published for that.
		std::function<bool()> active;
		// Appended to the pass's invocation revision: what `active` depends on, when that is not simply its answer.
		std::function<void(std::vector<std::uint64_t>&)> revision;
	};

	std::shared_ptr<org::RenderPass> CreatePass(Desc a_desc);
}

#endif
