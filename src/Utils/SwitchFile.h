#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The `CS_*` switches: a value from the environment or from the switches file, `<Documents>\My Games\Skyrim Special
 * Edition\SKSE\CommunityShaders-DCLF.ini` (a flat `NAME=value` file in the directory CS writes its log to).
 *
 * The file exists because the game does not reliably inherit the environment of whatever started it: Mod Organizer 2 hands the
 * game its own environment block, so switches exported by a launcher script never arrive. Drawcall Limit Fix keeps the registry
 * of its own switches (its Runtime/Switches.h); other code reads a switch by name here, and caches what it reads.
 */
namespace Util::SwitchFile
{
	/** @brief The directory the switches file lives in (the SKSE log directory), outside MO2's virtual file system. */
	std::filesystem::path Directory();

	/** @brief A switch's value: the environment first, then the file (unless a_environmentOnly); empty when set in neither. */
	std::string Value(std::string_view a_name, bool a_environmentOnly = false);

	/** @brief Whether a switch is set to "1". */
	inline bool Enabled(std::string_view a_name) { return Value(a_name) == "1"; }

	/** @brief Every name the file sets, read once. */
	std::vector<std::string> FileNames();
}
