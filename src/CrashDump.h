#pragma once

// Crash and hang dumps, written out of process by CommunityShadersCrashDump.exe (tools/crash-dump-helper), which Start runs once at
// load.
//
// A crash: a vectored handler runs before any SEH frame swallows the fault, so it sees the exception even when something downstream
// would turn it into a silent exit (the frame-generation interop could take the process down during a load with no Windows Error
// Reporting record). It writes CommunityShaders.dmp beside the game.
//
// A hang: the watchdog (HangWatchdog.h) asks for the stuck thread's report. The helper writes every thread's stack as text and a
// minidump.

#include <cstdint>
#include <filesystem>

namespace CrashDump
{
	/** @brief SKSEPlugin_Load: starts the helper and installs the crash handler. */
	void Start();

	/** @brief Whether the helper runs (it is missing, or could not start, in a broken install). */
	bool Available();

	/**
	 * @brief Any thread but a_threadId: the helper writes every thread's stack, a_threadId's first, to a_text, then a minidump to
	 * a_dump. Returns once it is done, or after a_timeoutMs; false when the helper is not running or did not answer.
	 */
	bool RequestHangDump(std::uint32_t a_threadId, const std::filesystem::path& a_dump, const std::filesystem::path& a_text, std::uint32_t a_timeoutMs);
}
