#include "FileRename.h"

#include <Windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Util
{
	bool UnderUsvfs()
	{
		static const bool loaded = ::GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
		return loaded;
	}

	namespace
	{
		// The file's real path (\\?\ form), past any redirection; empty if it has none.
		std::wstring FinalPath(HANDLE a_file)
		{
			std::wstring path(MAX_PATH, L'\0');
			DWORD length = ::GetFinalPathNameByHandleW(a_file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
			if (length >= path.size()) {
				path.resize(length);
				length = ::GetFinalPathNameByHandleW(a_file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
			}
			if (!length || length >= path.size())
				return {};
			path.resize(length);
			return path;
		}

		// a_from onto a_to without MoveFileExW: false (a_ec unset) when it could not try, so the caller renames normally.
		bool RenameByHandle(const std::filesystem::path& a_from, const std::filesystem::path& a_to, std::error_code& a_ec)
		{
			constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
			// The source first: a missing one leaves no empty target behind.
			const HANDLE source = ::CreateFileW(a_from.c_str(), DELETE | SYNCHRONIZE, kShareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (source == INVALID_HANDLE_VALUE)
				return false;
			// Through usvfs: a new target is created where it puts new files (the overwrite), and enters its tree.
			const HANDLE target = ::CreateFileW(a_to.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (target == INVALID_HANDLE_VALUE) {
				::CloseHandle(source);
				return false;
			}
			const std::wstring real = FinalPath(target);
			::CloseHandle(target);
			if (real.empty()) {
				::CloseHandle(source);
				return false;
			}
			const std::size_t nameBytes = real.size() * sizeof(wchar_t);
			std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
			auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
			info->ReplaceIfExists = TRUE;
			info->RootDirectory = nullptr;
			info->FileNameLength = static_cast<DWORD>(nameBytes);
			std::memcpy(info->FileName, real.data(), nameBytes);
			const BOOL renamed = ::SetFileInformationByHandle(source, FileRenameInfo, info, static_cast<DWORD>(buffer.size()));
			const DWORD error = renamed ? ERROR_SUCCESS : ::GetLastError();
			::CloseHandle(source);
			// Another volume (a target in a mod folder on another drive): the normal rename moves it.
			if (error == ERROR_NOT_SAME_DEVICE)
				return false;
			a_ec = error == ERROR_SUCCESS ? std::error_code{} : std::error_code(static_cast<int>(error), std::system_category());
			return true;
		}
	}

	void RenameSerialized(const std::filesystem::path& a_from, const std::filesystem::path& a_to, std::error_code& a_ec)
	{
		static std::mutex renames;
		std::scoped_lock lock(renames);
		a_ec.clear();
		if (UnderUsvfs() && RenameByHandle(a_from, a_to, a_ec))
			return;
		std::filesystem::rename(a_from, a_to, a_ec);
	}
}
