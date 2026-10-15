#pragma once

// The commit hook contract (Api::InstallContext::RegisterCommitHook): a feature's part of a commit's frame data, written on the render
// thread into the versions the frame adopted, before any pass of the commit's epoch runs. And the frame constant row contract
// (RegisterFrameConstantRow): a feature's float4 in Core's feature rows of PS b13, sampled and latched by each main commit.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/Commits.h), which the features inside DCLF include for it.
	class CommitContext;

	/** @brief The commit a hook runs in. */
	enum class CommitKind : std::uint8_t
	{
		Depth,   // the main view's depth (Z-prepass) epoch: the one whose graph runs the next frame's state (GpuStageSlot::DepthSegmentNextFrameState)
		Colour,  // the main view's colour epoch
		Shadow,  // the shadow epoch's shared inputs
	};

	/**
	 * @brief A commit hook.
	 *
	 * - commit: render thread, in each commit (depth, colour, shadow), with the scene's streams (after the vertex stream's uploads:
	 *   Api/VertexStreams.h), in registration order: stages its rows into the versions the adopted set names (CommitContext::uploads),
	 *   and in a main commit its per-frame values into the commit's latched copies (CommitContext::latched: its scene resources' latched
	 *   buffers). Each commit's hooks run once; a frame commits each kind at most once.
	 */
	struct CommitHook
	{
		std::string_view name;
		void (*commit)(const CommitContext& a_commit) = nullptr;
	};

	/**
	 * @brief A frame constant row (Api::InstallContext::RegisterFrameConstantRow): a float4 of the frame's every draw reads in Core's
	 * feature rows of PS b13 (c17 up to Extensions::kFrameConstantRowsEnd; the row is the feature's compile-time constant, with its HLSL
	 * declaration).
	 *
	 * - sample: render thread, in each main commit (depth and colour), where the frame slots are latched: the row's value, one 16-byte
	 *   latched copy. The rows are latched in ascending row order, whatever the registration order.
	 */
	struct FrameConstantRow
	{
		std::string_view name;
		std::uint32_t row = ~0u;
		void (*sample)(float (&a_out)[4]) = nullptr;
	};
}
