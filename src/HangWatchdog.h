#pragma once

// The render thread's watchdog. Every Present beats it. A thread of its own checks once a second, and when the render thread has not
// presented for CS_HANG_WATCHDOG seconds (15 by default, 0 turns it off) it asks the crash dump helper (CrashDump.h) for every
// thread's stack, the render thread's first, as text, plus a minidump. Both go beside the log as CommunityShaders-Hang-<time>.txt
// and .dmp.
//
// A game that stops rendering because it lost focus is not a hang: the watchdog waits only while the game has the foreground, or
// once it has seen the game keep presenting without it (bAlwaysActive, or test runs on another monitor). It also waits while a
// debugger is attached. One report is written per stall, at most three a session; a stall that ends is logged with its length.

namespace HangWatchdog
{
	/** @brief kDataLoaded: starts the watchdog thread. It watches from the next Present on. */
	void Start();

	/** @brief Render thread, every Present: the beat. */
	void OnPresent();
}
