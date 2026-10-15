#pragma once

// The commit hook contract (Api::InstallContext::RegisterCommitHook): a feature's part of a commit's frame data, written on the render
// thread into the versions the frame adopted, before any pass of the commit's epoch runs.

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
	 * - commit: render thread, in each commit (depth, colour, shadow), with the scene's streams (after the face positions' uploads), in
	 *   registration order: stages its rows into the versions the adopted set names (CommitContext::uploads). Each commit's hooks run
	 *   once; a frame commits each kind at most once.
	 */
	struct CommitHook
	{
		std::string_view name;
		void (*commit)(const CommitContext& a_commit) = nullptr;
	};
}
