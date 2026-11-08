#include "HangWatchdog.h"

#include "CrashDump.h"

#include <ctime>
#include <thread>

namespace HangWatchdog
{
	namespace
	{
		constexpr std::uint64_t kDefaultSeconds = 15;
		constexpr int kMaxReports = 3;
		constexpr std::size_t kKeptReports = 6;           // older CommunityShaders-Hang-* pairs are deleted when a new one is written
		constexpr std::uint32_t kHelperTimeoutMs = 180000;  // a dump of a large process can take a while

		std::atomic<bool> started{ false };   // Start ran
		std::atomic<bool> watching{ false };  // its settings are read: the beats count
		std::atomic<std::uint64_t> lastBeat{ 0 };  // GetTickCount64 at the last Present; 0 before the first after Start
		std::atomic<std::uint64_t> beats{ 0 };
		std::atomic<std::uint32_t> renderThread{ 0 };  // the thread of the last Present
		std::uint64_t thresholdMs = kDefaultSeconds * 1000;

		// CS_TEST_HANG=<seconds> (a development switch): the render thread sleeps past the threshold once, that long after the watch
		// starts, to exercise the watchdog and the helper end to end.
		std::uint64_t testHangAt = 0;

		std::uint64_t ReadSeconds(const char* a_name, std::uint64_t a_default)
		{
			const char* value = std::getenv(a_name);
			return value && *value ? std::strtoull(value, nullptr, 10) : a_default;
		}

		std::string Stamp()
		{
			const auto t = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &t);
			char text[32]{};
			std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
			return text;
		}

		bool GameHasForeground()
		{
			const auto window = GetForegroundWindow();
			DWORD process = 0;
			if (window)
				GetWindowThreadProcessId(window, &process);
			return process == GetCurrentProcessId() && !IsIconic(window);
		}

		/** @brief Deletes all but the newest kKeptReports hang reports beside the log. */
		void PruneReports(const std::filesystem::path& a_dir)
		{
			std::vector<std::filesystem::directory_entry> reports;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, error))
				if (entry.is_regular_file() && entry.path().filename().string().starts_with("CommunityShaders-Hang-"))
					reports.push_back(entry);
			std::ranges::sort(reports, [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
			// Two files a report: the .dmp and the .txt.
			for (std::size_t i = kKeptReports * 2; i < reports.size(); ++i)
				std::filesystem::remove(reports[i].path(), error);
		}

		void Report(std::uint64_t a_stalledMs)
		{
			const auto dir = logger::log_directory();
			if (!dir)
				return;
			const auto base = *dir / ("CommunityShaders-Hang-" + Stamp());
			const auto text = std::filesystem::path(base).concat(".txt"), dump = std::filesystem::path(base).concat(".dmp");
			const auto thread = renderThread.load(std::memory_order_relaxed);
			logger::critical("[HANG] the render thread (thread {}) has not presented for {:.1f} s; writing every thread's stack to {}", thread,
				a_stalledMs / 1000.0, text.filename().string());
			spdlog::default_logger()->flush();
			PruneReports(*dir);
			const auto begin = GetTickCount64();
			if (CrashDump::RequestHangDump(thread, dump, text, kHelperTimeoutMs))
				logger::critical("[HANG] wrote {} and {} in {:.1f} s", text.filename().string(), dump.filename().string(), (GetTickCount64() - begin) / 1000.0);
			else
				logger::error("[HANG] the crash dump helper did not answer within {} s: no report", kHelperTimeoutMs / 1000);
			spdlog::default_logger()->flush();
		}

		void Watch()
		{
			int reports = 0;
			bool stalled = false;  // the current stall was reported (or found, past the report limit)
			bool rendersUnfocused = false;
			std::uint64_t lastBeats = 0, lastMinuteLogged = 0;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				const auto beat = lastBeat.load(std::memory_order_acquire);
				if (beat == 0)
					continue;
				const auto count = beats.load(std::memory_order_relaxed);
				const bool foreground = GameHasForeground();
				// Presents while another window has the foreground: this game keeps rendering unfocused, so a stall then is a hang too.
				if (!foreground && count != lastBeats)
					rendersUnfocused = true;
				lastBeats = count;
				const auto now = GetTickCount64();
				const auto stalledMs = now > beat ? now - beat : 0;
				if (stalledMs < thresholdMs) {
					if (stalled) {
						logger::warn("[HANG] the render thread presented again; it had stopped for over {:.1f} s", static_cast<double>(thresholdMs) / 1000.0);
						stalled = false;
					}
					continue;
				}
				if ((!foreground && !rendersUnfocused) || IsDebuggerPresent())
					continue;
				if (stalled) {
					// Still stuck: one line a minute, so the log shows how long.
					if (const auto minute = stalledMs / 60000; minute != lastMinuteLogged) {
						lastMinuteLogged = minute;
						logger::warn("[HANG] the render thread has still not presented after {} s", stalledMs / 1000);
						spdlog::default_logger()->flush();
					}
					continue;
				}
				stalled = true;
				lastMinuteLogged = stalledMs / 60000;
				if (reports >= kMaxReports) {
					logger::warn("[HANG] the render thread has not presented for {:.1f} s; no report ({} written this session already)", stalledMs / 1000.0,
						kMaxReports);
					continue;
				}
				++reports;
				Report(stalledMs);
			}
		}
	}

	void Start()
	{
		if (started.exchange(true))
			return;
		const auto seconds = ReadSeconds("CS_HANG_WATCHDOG", kDefaultSeconds);
		if (seconds == 0) {
			logger::info("[HANG] the render thread watchdog is off (CS_HANG_WATCHDOG=0)");
			return;
		}
		if (!CrashDump::Available()) {
			logger::warn("[HANG] the render thread watchdog is off: the crash dump helper is not running");
			return;
		}
		thresholdMs = seconds * 1000;
		testHangAt = ReadSeconds("CS_TEST_HANG", 0);
		logger::info("[HANG] the render thread watchdog reports a Present stall of {} s or more", seconds);
		if (testHangAt)
			logger::warn("[HANG] CS_TEST_HANG: the render thread stalls for {} s, {} s after the watch starts", seconds + 10, testHangAt);
		std::thread(Watch).detach();
		watching.store(true, std::memory_order_release);
	}

	void OnPresent()
	{
		if (!watching.load(std::memory_order_acquire))
			return;
		const auto now = GetTickCount64();
		if (lastBeat.load(std::memory_order_relaxed) == 0 && testHangAt)
			testHangAt = now + testHangAt * 1000;
		// Every beat: a loading screen presents from one of the engine's job threads, the game from its main thread.
		renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		beats.fetch_add(1, std::memory_order_relaxed);
		lastBeat.store(now, std::memory_order_release);
		if (testHangAt && now >= testHangAt) {
			testHangAt = 0;
			logger::warn("[HANG] CS_TEST_HANG: the render thread sleeps now");
			std::this_thread::sleep_for(std::chrono::milliseconds(thresholdMs + 10000));
		}
	}
}
