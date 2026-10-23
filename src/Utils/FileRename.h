#pragma once

#include <filesystem>
#include <mutex>
#include <system_error>

namespace Util
{
	/**
	 * @brief Renames a_from onto a_to (replacing it), one rename at a time in the process. Mod Organizer's usvfs, which redirects
	 * Data/, faults in its MoveFileExW hook under concurrent renames (a full shader recompile writes from many threads), so every
	 * rename the plugin makes goes through here: the shader caches, ORG's shader cache included (RenderGraphRuntime hands it this).
	 */
	inline void RenameSerialized(const std::filesystem::path& a_from, const std::filesystem::path& a_to, std::error_code& a_ec)
	{
		static std::mutex renames;
		std::scoped_lock lock(renames);
		std::filesystem::rename(a_from, a_to, a_ec);
	}
}
