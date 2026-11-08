#pragma once

// The session's first frames that DCLF draws, held black until DCLF covers its views. DCLF builds its views from the frames that render
// the world, so after the session's first load its sun cascades and occlusion maps are not covered for a few dozen frames (about 0.3 s).
// Rather than hand those views to the engine's whole culls, the hold:
// - keeps the sun's whole skip (the engine culls the engine-only subset's roots alone) and skips the occlusion maps DCLF does not draw
//   yet, so nothing is culled whole; and
// - draws the screen black (the CS overlay, StartupBlackout) while it lasts, so the missing shadows are never seen.
// It ends after two frames in a row that needed no hold, or at a frame limit, and never again in the session.
//
// Render thread.

#include <cstdint>

namespace DCLF::StartupHold
{
	/** @brief Whether the hold is in effect: from the session's first running frame until DCLF covers its views. */
	bool Active();

	/** @brief The views a hold skips the engine's cull of. */
	enum class View : std::uint8_t
	{
		Sun,
		Occlusion
	};
	/** @brief A view this frame was held (its engine cull skipped) because DCLF does not cover it yet. */
	void NoteHeld(View a_view);

	/** @brief The frame's end (FrameDriver::EndFrame, a_running: DCLF drew it): the hold advanced, and released once covered. */
	void OnFrameEnd(bool a_running);
}
