#pragma once

// The engine's writers inside the engine-read window (EngineReadWindow.h), kept out of DCLF's reads there. Installed with the native pass
// hooks (DrawcallLimitFix::Hooks::Install); the report's engine-read window line reads the counters.

#include <cstdint>

namespace DCLF::WindowWriters
{
	/**
	 * @brief The cell animations job (the "Main render scene" job list's "Cell animations": SE FUN_140640300, AE FUN_1406d29d0), whose call
	 * of TES's animated references' update (SE FUN_140152c90, AE FUN_14019c0f0) is replaced by the thunk's.
	 *
	 * The job list runs beside Main::Draw: the job waits for the main thread's stage 5 ("Wait For Shadow Culling", set by
	 * FUN_1412e2660 once the shadow cull is done, inside Main::Draw), so it updates the cells' animated references - their controllers'
	 * writes to transforms, the effect falloff and base colour - while the window is open and FrameValues samples them. The engine's own
	 * draws of the frame read them as they happen to be; DCLF's sample would mix the two frames, and the texture transforms
	 * parity sees the write between a sample and its re-read (dclf-open-defects.md, "effect frame variables written inside
	 * the engine-read window"). Seen on SE 1.5.97 in most of a run's parity windows, on AE 1.6.1170 (the same job list) in a few.
	 *
	 * The thunk waits for the frame's producer to finish sampling (FrameValues::WaitSampled: Kick, at Main::Draw's start, precedes
	 * stage 5 on the main thread), then suspends the window (the leases in flight finish, and other readers' leases are refused: their
	 * items wait for the next window) for the update, and resumes it. So a frame's values are the engine's before the update, every
	 * frame. The producer waits for nothing of the engine's, so the job's wait cannot close a cycle.
	 */
	void Install();

	/** @brief The thunk's runs since the last Take, and how long they waited for the frame's sampling (whole microseconds). */
	struct Counters
	{
		std::uint64_t runs = 0, waited = 0, waitedUs = 0, waitedMaxUs = 0;
	};
	Counters Take();
	bool Installed();
}
