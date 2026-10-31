#include "FileRename.h"

#include <Windows.h>
#include <winternl.h>

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
			// usvfs rewrites name queries on handles opened through its hooks back to the virtual path.
			// A duplicate refers to the same file without that tracked name, so the rename gets its real destination.
			HANDLE query = INVALID_HANDLE_VALUE;
			if (!::DuplicateHandle(::GetCurrentProcess(), a_file, ::GetCurrentProcess(), &query, 0, FALSE, DUPLICATE_SAME_ACCESS))
				return {};
			// GetFinalPathNameByHandle queries FileNameInformation with a small internal buffer. Older usvfs
			// versions read that non-terminated result as a string and can fault during parallel cache writes.
			// ObjectNameInformation carries an explicit UNICODE_STRING length and the full device path.
			using QueryObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
			static const auto queryObject = reinterpret_cast<QueryObject>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
			std::vector<std::byte> buffer(65536 + sizeof(UNICODE_STRING));
			ULONG needed = 0;
			const NTSTATUS status = queryObject ? queryObject(query, 1, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) : -1;
			::CloseHandle(query);
			if (status < 0)
				return {};
			const auto& name = *reinterpret_cast<const UNICODE_STRING*>(buffer.data());
			if (!name.Buffer || !name.Length)
				return {};
			return L"\\\\?\\GLOBALROOT" + std::wstring(name.Buffer, name.Length / sizeof(wchar_t));
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
