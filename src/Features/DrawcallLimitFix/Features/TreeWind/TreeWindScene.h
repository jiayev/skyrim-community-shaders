#pragma once

// Tree wind's scene resources (Api::SceneResources): its buffers and the commits' frame state, held with the scene's tables. Included
// first by the feature's sources that use them (Internal.h first: volk must precede every Vulkan header of the translation unit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

namespace DCLF::Features::TreeWind
{
	/**
	 * @brief The trees' GPU state (Api::SceneResources' state; render thread, but for the handles, made once at setup).
	 *
	 * Without the tree wind program (treeWind null), trees keep the wind they joined with: the wind rows exist, stay zero and are bound
	 * at t124 all the same; nothing else is made, and the stage stays out of the graph.
	 */
	struct TreeWindScene
	{
		// Tree rows (the commits' uploads) and GPU-only clocks, by tree slot.
		Draws::Versioned trees, treeClocks;
		std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow
		// Wind output (kTreeWindEntryRows per entry: nodeless trees, then tree slot s at s + 1), one buffer per frame parity: the
		// Z-prepass writes this frame's, draws read the other. Every version is zero-filled (a zero generation is never a listing's).
		std::array<Draws::Versioned, 2> treeWindRows;
		/** @brief The wind buffer a frame's draws read: the one the previous frame wrote. */
		const Draws::Versioned& WindReadBuffer(std::uint32_t a_frame) const { return treeWindRows[(a_frame + 1) & 1]; }
		org::services::FilledContents<> treesFilled;  // the tree rows' content epoch (SceneTables::treesContentEpoch)
		/** @brief The tree rows' content epoch the trees version a_sealed names holds (~0: none). */
		std::uint64_t TreesContentEpoch(const Draws::NamedVersions& a_sealed) const { return treesFilled.Since(a_sealed.Of(trees)).value_or(~0ull); }
		std::uint32_t treeCount = 0;
		std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
		TreeWindFrame treeInputs{};
		// The previous frame's, which wrote the buffer this frame's draws read. ~0u until a frame has run.
		std::uint32_t previousTreeFrame = ~0u;
		TreeWindFrame previousTreeInputs{};
		std::uint64_t previousTreesContentEpoch = ~0ull;  // the tree rows that frame's pass read
		std::uint64_t frameTreesContentEpoch = ~0ull;     // this frame's
		std::shared_ptr<const ComputeProgram> treeWind;
	};

	/** @brief The trees' GPU state in a_scene (null before the scene's tables are made). */
	TreeWindScene* SceneOf(const Draws::SceneBuffers& a_scene);
	/** @brief SceneOf, sharing its ownership (the stage's state). */
	std::shared_ptr<TreeWindScene> SharedSceneOf(const Draws::SceneBuffers& a_scene);
}
