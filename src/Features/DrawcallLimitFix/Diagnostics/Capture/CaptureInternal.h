#pragma once

// The capture's shared state between its threads (Capture.cpp: the render thread and the scene lane; Bundle.cpp: the worker).

#include "Capture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spdlog::sinks
{
	class sink;
}

namespace DCLF::Capture
{
	/** @brief One Present while recording (frames.csv). */
	struct FrameSample
	{
		std::uint32_t frame = 0;
		float presentMs = 0.0f;           // Present to Present
		std::uint32_t dclfUs = 0;         // DCLF's render-thread time in the last finished frame
		std::uint32_t adoptWaitUs = 0;    // the render thread's wait for a scene snapshot
		std::uint32_t sceneLag = 0;       // frames since the last scene pass ended
		std::uint32_t notReady = 0;       // epochs skipped so far in the report window
		std::array<std::uint32_t, 4> queued{};  // the scene executor's queued jobs, by domain
		bool loading = false;
		bool running = false;             // DCLF ran this frame
		bool cpuPipeline = false;
		bool interior = false;
	};

	/** @brief What the main thread reads for the bundle (the load order, the player's place). */
	struct MainThreadInfo
	{
		std::string loadOrder;
		std::string location;  // JSON text
	};

	/** @brief One capture, from its start to its zip. */
	struct Session
	{
		std::uint32_t id = 0;
		std::filesystem::path dir;
		float seconds = 10.0f;
		bool fromMenu = true;

		std::chrono::system_clock::time_point startWall;
		std::chrono::steady_clock::time_point start, end;
		std::uint32_t startFrame = 0, endFrame = 0;

		std::shared_ptr<spdlog::sinks::sink> tee;

		std::vector<FrameSample> frames;  // reserved at the start; nothing is added past its capacity
		std::uint64_t framesDropped = 0;

		// The scene lane's sections: asked for at the start and the end, each acknowledged once written.
		std::atomic<std::uint32_t> laneAsked{ 0 }, laneDone{ 0 };
		std::chrono::steady_clock::time_point finishDeadline;
		std::uint32_t finishFrames = 0;  // Presents since the end, so the render thread's flushed report has run

		// The render thread's parts, written before packaging (JSON text).
		std::string environment, features, settings;
		std::shared_future<MainThreadInfo> mainThread;

		// What the manifest says about each section: "ok", "skipped: <why>" or "failed: <why>".
		std::mutex notesMutex;
		std::vector<std::pair<std::string, std::string>> sections;

		void Note(std::string a_section, std::string a_state)
		{
			std::lock_guard lock(notesMutex);
			sections.emplace_back(std::move(a_section), std::move(a_state));
		}
	};

	/**
	 * @brief Worker thread: writes the bundle's remaining files, zips the folder and deletes it. Returns the zip's path, or the folder's
	 * when the zip failed (a_error says why).
	 */
	std::filesystem::path Package(Session& a_session, std::string& a_error);

	/** @brief Any thread: the text with the user's profile path replaced by %USERPROFILE%. */
	std::string Redact(std::string a_text);
}
