#pragma once

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>


namespace DCLF::Reports
{
	/**
	 * @brief The report schedule: every module's report lines cover the same window of kInterval frames.
	 *
	 * The modules count (their Counters, taken by the report); the report files here (Diagnostics/Reports/<Module>.cpp) format the
	 * lines. Two threads report: the render thread at Present, once per frame number (Due), and the coordinator from its pass, which may
	 * skip frame numbers (Crossing). A module with no frame number of its own counts its calls (Window).
	 *
	 * The window is kInterval frames while the run observes anything (a parity or diagnostic switch set: validation runs set CS_DCLF_STATS),
	 * and kQuietInterval in normal play, where the always-on lines would otherwise grow a player's log by about 100 MB an hour. Every
	 * window's lines cover its own frames, so their averages hold either way.
	 */
	constexpr std::uint32_t kInterval = 300;
	constexpr std::uint32_t kQuietInterval = 18000;

	/** @brief The report window in frames (read once, after the features registered their switches). */
	inline std::uint32_t Interval()
	{
		static const std::uint32_t interval = AnyObserverSwitch() ? kInterval : kQuietInterval;
		return interval;
	}

	/** @brief True while a diagnostic capture records: the reports log their full statistics for its window, as under CS_DCLF_STATS. */
	inline std::atomic<bool> captureStatistics{ false };

	/**
	 * @brief Whether the reports log their full statistics: CS_DCLF_STATS, or a capture recording. Counters that are only kept under
	 * CS_DCLF_STATS stay empty during a capture.
	 */
	inline bool FullStatistics() { return Diagnostics::Observe().stats || captureStatistics.load(std::memory_order_relaxed); }

	/**
	 * @brief The flushes asked for so far. A flush ends the current window early on every clock: the render thread's at its next
	 * frame, each Crossing and Window at its next call. The diagnostic capture flushes at its start and its end, so its log holds
	 * one window that covers exactly the capture.
	 */
	inline std::atomic<std::uint32_t> flushGeneration{ 0 };

	/** @brief Any thread: ends the current report window on every clock (see flushGeneration). */
	inline void Flush() { flushGeneration.fetch_add(1, std::memory_order_acq_rel); }

	/** @brief True once per clock after each Flush: a_seen holds the generation that clock last saw. */
	inline bool TakeFlush(std::uint32_t& a_seen)
	{
		const auto now = flushGeneration.load(std::memory_order_acquire);
		if (now == a_seen)
			return false;
		a_seen = now;
		return true;
	}

	/** @brief The render thread's flush, seen by Due. */
	inline std::uint32_t renderThreadFlushSeen = 0;

	/** @brief The render thread's clock: frame a_frame (each number once) ends a window. Call once per frame. */
	inline bool Due(std::uint32_t a_frame)
	{
		// The flush is taken once a frame, so every Due call of that frame agrees.
		static std::uint32_t frameOfFlush = ~0u;
		if (TakeFlush(renderThreadFlushSeen))
			frameOfFlush = a_frame;
		return a_frame % Interval() == 0 || a_frame == frameOfFlush;
	}

	/** @brief A clock that may skip frame numbers: due when a_frame is in a later window than the last call's. */
	struct Crossing
	{
		std::uint32_t window = 0;
		std::uint32_t flushSeen = 0;

		bool Due(std::uint32_t a_frame)
		{
			const bool flushed = TakeFlush(flushSeen);
			const std::uint32_t now = a_frame / Interval();
			if (now == window && !flushed)
				return false;
			window = now;
			return true;
		}
	};

	/** @brief A module's own count of calls: due on every kInterval-th Tick. */
	struct Window
	{
		std::uint32_t count = 0;
		std::uint32_t flushSeen = 0;

		bool Tick()
		{
			const bool flushed = TakeFlush(flushSeen);
			if (++count < Interval() && !flushed)
				return false;
			count = 0;
			return true;
		}
	};

	/** @brief Milliseconds per QueryPerformanceCounter tick, for the reports' timings. */
	inline double MsPerTick()
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		return 1000.0 / static_cast<double>(frequency.QuadPart);
	}

	/** @brief Render thread, Present, once per frame number while DCLF runs: the frame's report lines (Frame.cpp). */
	void RenderThread(std::uint32_t a_frame);

	/** @brief The main camera's native passes since the last report, and the set members' among them (a leak: must be 0). */
	struct LeakStats
	{
		std::uint64_t offered = 0, leakedInDepth = 0, leakedInOpaque = 0;
		std::vector<std::string> samples;  // the first few leaks
	};
	/** @brief Render thread: the leak counts (FeatureInstall.cpp's observer counts them, the frame report takes them). */
	LeakStats& Leaks();
}
