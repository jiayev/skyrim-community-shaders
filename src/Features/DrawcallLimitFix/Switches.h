#pragma once

#include <filesystem>
#include <string>

namespace DCLF
{
	/**
	 * @brief The value of a `CS_DCLF_*` switch, from the environment or from the switches file.
	 *
	 * The environment is checked first, then `<Documents>\My Games\Skyrim Special Edition\SKSE\
	 * CommunityShaders-DCLF.ini`, a flat `NAME=value` file in the directory CS already writes its log to.
	 * The file exists because the game does not reliably inherit the environment of whatever started it:
	 * Mod Organizer 2 hands the game its own environment block, so switches exported by a launcher script
	 * never arrive, and a run silently behaves as if every switch were off. Reading a file removes that
	 * whole class of "the run did not test what it was meant to test".
	 *
	 * Read once, on first use, so a switch cannot change under a running frame. Returned by value: the
	 * cache is a hash map whose values move when it grows, so a reference into it would not stay valid.
	 *
	 * @return the value, or an empty string when the switch is set in neither place.
	 */
	std::string SwitchValue(const char* a_name);

	/** @brief The directory the switches file lives in (the SKSE log directory), outside MO2's virtual file system. */
	std::filesystem::path SwitchesDirectory();

	/** @brief Whether a switch is set to "1". */
	bool SwitchEnabled(const char* a_name);

	/** @brief Every switch that is set, as `NAME=value` pairs, for the startup log. */
	std::string SwitchSummary();

	/**
	 * @brief The feature switches set to something other than DCLF's full featureset, as `NAME=value` pairs, or
	 * empty when the run exercises all of it. Every feature defaults to on; the startup log names a reduced
	 * run so that a measurement or a validation cannot silently leave a feature out. Diagnostics (probes,
	 * parity checks, stats) are not features and are not listed.
	 */
	std::string ReducedFeatures();
}
