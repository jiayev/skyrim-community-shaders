#pragma once

// Fades' writer hooks (FadeWriters.cpp): DCLF as the one fade writer of the roots it lists. The writer tables (the scene tables accepted
// at Main::Update's entry, the set the engine's fade writers skip), the fade stores written back there, the animation job's fade updates
// (AnimatedFade, batched at the list jobs' end for FadeStateCS) and the fade OnVisibles without their stores (FadeOnVisible, LeafOnVisible,
// TreeOnVisible); and the root test's fade-out distance. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Features/Fades/FadeRecords.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::SceneCapture
{
	struct NodeRecord;
}

namespace DCLF::Features::Fades
{
	/**
	 * @brief Main thread, at Main::Update's entry (the stand-in observer's updateEntry, last of the observers): publishes the writer
	 * tables, then writes the fade stores back. a_running false drops the writes.
	 */
	void WriteFadeWriters(bool a_running);
	/**
	 * @brief Any thread: whether DCLF is a_node's one fade writer this frame (kFadeRootWritten in the tables published at
	 * Main::Update's entry). The gate's fade update (FUN_1402cff60 -> FUN_14147a160) and FadeOnVisible skip their stores on
	 * those; FadeStateCS makes the update on the GPU and the write-back carries it to the node.
	 */
	bool FadeWrittenByDclf(const void* a_node);
	/** @brief a_node listed by a_tables (SceneTables) with kFadeRootWritten. Any thread; the tables are immutable. */
	bool WrittenByTables(const void* a_tables, const void* a_node);
	/** @brief Since the last call: the gate's fade updates and OnVisible fade steps skipped. */
	struct FadeWriterCounts
	{
		std::uint64_t gateSkipped = 0, onVisibleSkipped = 0;
		std::size_t roots = 0;  // the roots in the writer set
	};
	FadeWriterCounts TakeFadeWriterCounts();

	/**
	 * @brief One animation-job fade update, its root resolved on the job thread through the writer tables (the SceneTables
	 * accepted at Main::Update's entry, immutable and held until the next entry, so stable through the animation job): the slot in
	 * those tables, its row's generation and the node. A consumer reading other tables (the frame's, after a swap at frame start)
	 * takes the slot only where its root list's nodes[slot] is the node and rows[slot].generation the generation; no hashing.
	 */
	struct AnimatedFadeRoot
	{
		const void* node = nullptr;
		std::uint32_t slot = 0;
		std::uint32_t generation = 0;
	};
	/**
	 * @brief Render thread, after the list jobs: the animation job's fade updates since the previous list jobs (roots listed by
	 * the writer tables, one per update, and inputs), which FadeStateCS repeats before its cull test (FadeFrame anim*).
	 */
	void TakeAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs);
	/**
	 * @brief Render thread, at the list jobs' Finish (the stand-in observer's listJobsDone): the animation job's queued updates and
	 * inputs become the batch TakeAnimatedBatch hands out.
	 */
	void DrainAnimatedFades();

	/**
	 * @brief The root test (per join): where FUN_14147b110's fade value falls below BSFadeNode::OnVisible's fade-out threshold
	 * (kObjectFadeTest), from the fade node's record and the frame's captured globals. > 0: scaled by the camera's LOD factor; < 0:
	 * by a constant, folded in; 0: never fades out by distance.
	 */
	float FadeDistanceOf(const SceneCapture::NodeRecord& a_root);
}
