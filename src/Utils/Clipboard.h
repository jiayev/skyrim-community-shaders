#pragma once

#include <filesystem>

namespace Util
{
	/**
	 * @brief Puts a file on the clipboard as a file (CF_HDROP), so it can be pasted into Explorer or a chat client.
	 * Any thread; retries briefly while another process holds the clipboard.
	 * @param a_absolutePath The file's absolute path.
	 * @return Whether the clipboard took it.
	 */
	bool CopyFileToClipboard(const std::filesystem::path& a_absolutePath);
}
