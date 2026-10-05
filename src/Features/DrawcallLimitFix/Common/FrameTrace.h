#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// TEMP (threading rework, phase 3): the order of DCLF's hooks in one frame, across threads. CS_DCLF_FRAME_TRACE=1: each traced
// site's first call of a frame (its time since Main::Draw, its thread) is logged for one frame in 600.
namespace DCLF::FrameTrace
{
	struct Entry
	{
		const char* site = nullptr;
		std::uint32_t thread = 0;
		std::int64_t us = 0;
	};
	inline constexpr std::uint32_t kEntries = 512;
	inline Entry entries[kEntries];
	inline std::atomic<std::uint32_t> count{ 0 };
	inline std::atomic<std::uint32_t> serial{ 1 };
	inline std::atomic<std::int64_t> start{ 0 };

	inline bool Enabled()
	{
		static const bool enabled = [] {
			const char* value = std::getenv("CS_DCLF_FRAME_TRACE");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}
	inline std::int64_t NowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	inline void Note(const char* a_site)
	{
		const auto i = count.fetch_add(1, std::memory_order_relaxed);
		if (i >= kEntries)
			return;
		entries[i] = { a_site, ::GetCurrentThreadId(), NowUs() - start.load(std::memory_order_relaxed) };
	}
	/** @brief Main::Draw: the frame's start. */
	inline void BeginFrame()
	{
		start.store(NowUs(), std::memory_order_relaxed);
	}
	/** @brief Present: one frame in 600 logged, then the next frame's sites are new. */
	template <class Log>
	inline void EndFrame(Log&& a_log)
	{
		const auto frame = serial.fetch_add(1, std::memory_order_relaxed);
		const auto n = (std::min)(count.exchange(0, std::memory_order_relaxed), kEntries);
		if ((frame % 600) == 0)
			a_log(entries, n);
	}
}

#define DCLF_FRAME_TRACE(a_site)                                                                                                  \
	do {                                                                                                                          \
		if (DCLF::FrameTrace::Enabled()) {                                                                                        \
			static std::atomic<std::uint32_t> traceSeen{ 0 };                                                                     \
			const auto traceSerial = DCLF::FrameTrace::serial.load(std::memory_order_relaxed);                                    \
			if (traceSeen.exchange(traceSerial, std::memory_order_relaxed) != traceSerial)                                         \
				DCLF::FrameTrace::Note(a_site);                                                                                   \
		}                                                                                                                         \
	} while (false)
