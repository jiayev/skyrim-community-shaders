#pragma once

#include <cstdint>
#include <string>

namespace DCLF::ImportTimings
{
	/**
	 * @brief T6b0 (CS_DCLF_TIMELINE): the engine's import work timed by thread, main against the others, through detours on its
	 * entry points (skyrim-engine-notes.md, "Where the engine imports the world: threads"): the grid controller, the BSTaskPool
	 * drain, a queued reference's finish, the I/O completion pump, the LOD swaps' drain and a quadtree node's block retirement.
	 * Inclusive times: a nested entry (a reference's finish in the pump) counts in both. Each is a Tracy zone too.
	 */
	void Install();
	/** @brief The render thread, every frame: the main thread's id (Skyrim's renders on its main thread). */
	void NoteMainThread(std::uint32_t a_thread);
	/** @brief Since the last call, over a_frames frames: per entry point, calls and ms a frame on the main thread and on the others. */
	std::string TakeReport(std::uint32_t a_frames);
}
