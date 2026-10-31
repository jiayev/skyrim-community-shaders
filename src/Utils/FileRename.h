#pragma once

#include <filesystem>
#include <system_error>

namespace Util
{
	/**
	 * @brief Renames a_from onto a_to (replacing it), one rename at a time in the process. Every rename the plugin makes goes through
	 * here: the shader caches, ORG's shader cache included (RenderGraphRuntime hands it this).
	 *
	 * Under Mod Organizer, usvfs (which redirects Data/) faults in its MoveFileExW hook, intermittently and with renames serialized, so
	 * there the rename never calls it: the target is opened (created if new) through usvfs, which records it in its tree and gives its
	 * real path, and a_from is renamed onto that path by handle, which usvfs does not hook. Between the two a new target is an empty
	 * file, which every cache's load rejects (header and size checks). Without usvfs, or where that fails, std::filesystem::rename.
	 */
	void RenameSerialized(const std::filesystem::path& a_from, const std::filesystem::path& a_to, std::error_code& a_ec);

	/** @brief True when Mod Organizer's usvfs is loaded in the process, so file access under Data/ is redirected. */
	bool UnderUsvfs();
}
