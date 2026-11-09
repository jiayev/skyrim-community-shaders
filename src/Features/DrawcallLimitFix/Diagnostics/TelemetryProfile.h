#pragma once

#include <filesystem>
#include <string_view>

// "dclf:telemetry=<label>": BasicTelemetry sessions for a test run's phases (dclf-validation.md).
namespace DCLF::Diagnostics
{
	/**
	 * @brief Ends the running phase, writes its files, and starts a phase named a_label. "off" starts none. The new phase writes its files
	 * to a_directory, or to <log directory>/dclf-telemetry when it is empty. Present's thread.
	 */
	void TelemetryPhase(std::string_view a_label, const std::filesystem::path& a_directory = {});
	/** @brief One Present to the next is one telemetry frame. Does nothing without a running phase. Present's thread. */
	void TelemetryFrame();
}
