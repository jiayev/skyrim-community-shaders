#include "Utils/Clipboard.h"

#include <ShlObj.h>

#include <cstring>
#include <string>

namespace Util
{
	bool CopyFileToClipboard(const std::filesystem::path& a_absolutePath)
	{
		const std::wstring absolutePath = a_absolutePath.wstring();
		if (absolutePath.empty()) {
			return false;
		}

		const size_t pathChars = absolutePath.size();
		const size_t bytes = sizeof(DROPFILES) + (pathChars + 2) * sizeof(wchar_t);
		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
		if (!hMem) {
			return false;
		}

		auto* drop = static_cast<DROPFILES*>(GlobalLock(hMem));
		if (!drop) {
			GlobalFree(hMem);
			return false;
		}

		drop->pFiles = sizeof(DROPFILES);
		drop->fWide = TRUE;

		auto* files = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
		memcpy(files, absolutePath.c_str(), (pathChars + 1) * sizeof(wchar_t));

		GlobalUnlock(hMem);

		for (int attempt = 0; attempt < 8; ++attempt) {
			if (attempt > 0) {
				Sleep(1 << (attempt - 1));
			}
			if (!OpenClipboard(nullptr)) {
				continue;
			}
			EmptyClipboard();
			const bool placed = SetClipboardData(CF_HDROP, hMem) != nullptr;
			CloseClipboard();
			if (placed) {
				return true;
			}
		}

		GlobalFree(hMem);
		return false;
	}
}
