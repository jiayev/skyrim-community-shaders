#pragma once

// The diagnostic capture: one menu button a player presses while a problem is visible. It records DCLF's state at the start and the
// end of a short window and its behaviour in between, and writes one zip to send back (docs/development/dclf-validation.md,
// "Diagnostic capture").
//
// What it holds:
// - every log line written during the window (the log tee), including a report window the clocks are flushed to, which covers
//   exactly the capture, and the one before it (the minutes leading up to the click);
// - the fade census and the growth census at the start and the end, and a per-frame sample ring;
// - the environment, the settings, the load order, the loaded DLLs, the logs and a screenshot.
//
// The render thread drives it from Present (OnPresent). The scene lane writes its own sections at a pass's end (RunSceneLaneWork).
// A worker thread writes the rest of the bundle and zips it.

#include <cstdint>
#include <filesystem>
#include <string>

namespace DCLF::Capture
{
	enum class Phase : std::uint8_t
	{
		Idle,       // nothing running
		Armed,      // waiting for the menu to close
		Recording,  // the window
		Finishing,  // the window ended: waiting for the reports and the scene lane's sections
		Packaging,  // the worker writes the bundle and zips it
	};

	/** @brief What the menu shows. */
	struct Status
	{
		Phase phase = Phase::Idle;
		float progress = 0.0f;           // Recording: the window's elapsed fraction
		std::filesystem::path lastResult;  // the last finished capture's zip (or folder when the zip failed)
		std::string lastError;           // why the last capture failed, or what it could not include
	};

	/**
	 * @brief Render thread (the menu, a test command): starts a capture of a_seconds. From the menu (a_immediate false) it starts two
	 * seconds after the menu closes, so the menu is in neither the screenshot nor the timings; a test command starts it at the next
	 * Present. Does nothing while a capture runs.
	 */
	void Request(float a_seconds, bool a_immediate);

	/** @brief Render thread: stops an armed or recording capture and deletes its folder. */
	void Cancel();

	/** @brief Any thread: the capture's state, for the menu. */
	Status GetStatus();

	/** @brief Any thread: true while a capture records its window. */
	bool Recording();

	/** @brief Render thread, every Present (DrawcallLimitFix::Reset), installed or not: advances the capture. */
	void OnPresent();

	/** @brief Scene lane, at a scene pass's end (ScenePump): the scene sections a capture asked for. */
	void RunSceneLaneWork();

	/** @brief Render thread, the menu: the capture's button, duration and status (DrawcallLimitFix::DrawSettings). */
	void DrawControls();
}
