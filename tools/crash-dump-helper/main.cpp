// CommunityShadersCrashDump: writes Community Shaders' crash and hang dumps from outside the game.
//
// The plugin starts this helper once at load (src/CrashDump.cpp, CrashDump::Start) with four inherited handles: the game process,
// the request event, the done event and a small shared section. A request is one of two kinds:
// - A crash. The plugin's vectored handler writes the faulting thread and its EXCEPTION_POINTERS address into the section, signals
//   the request event and waits for the done event. The helper writes a minidump to the path on its command line.
// - A hang. The plugin's watchdog (src/HangWatchdog.cpp) found the render thread stuck. It writes that thread and two paths into
//   the section. The helper writes a text file with every thread's stack (the stuck thread first, symbols from the PDBs beside the
//   modules), then a minidump.
//
// MiniDumpWriteDump suspends every thread of the process it dumps and then allocates from heaps and takes loader locks; run inside
// the game, a thread suspended while holding the process heap's lock deadlocked it (2026-10-11). Run from here, nothing it needs
// lives in the suspended process. The stacks are walked the same way: each thread is suspended, read and resumed from here.
//
// Usage: CommunityShadersCrashDump.exe <process> <request event> <done event> <section> <crash dump path> (handles in decimal).

#include <Windows.h>

#include <DbgHelp.h>
#include <TlHelp32.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
	// The section's layout, written by the plugin (src/CrashDump.cpp, Shared). The first 16 bytes are the crash request's.
	struct Shared
	{
		std::uint32_t threadId;
		std::uint32_t kind;               // 0: a crash, 1: a hang
		std::uint64_t exceptionPointers;  // a crash's EXCEPTION_POINTERS* in the game's address space
		wchar_t dumpPath[1024];           // a hang's minidump
		wchar_t textPath[1024];           // a hang's stacks
	};

	constexpr std::uint32_t kHang = 1;  // Shared::kind; 0 is a crash
	constexpr int kMaxFrames = 96;

	HANDLE HandleArg(const wchar_t* a_text)
	{
		return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(a_text, nullptr, 10)));
	}

	void WriteMinidump(HANDLE a_process, const wchar_t* a_path, const Shared* a_shared, bool a_exception)
	{
		const HANDLE file = CreateFileW(a_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return;
		MINIDUMP_EXCEPTION_INFORMATION info{};
		if (a_exception) {
			info.ThreadId = a_shared->threadId;
			info.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<std::uintptr_t>(a_shared->exceptionPointers));
			info.ClientPointers = TRUE;  // the pointers are the game's
		}
		MiniDumpWriteDump(a_process, GetProcessId(a_process), file,
			static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), a_exception ? &info : nullptr, nullptr,
			nullptr);
		CloseHandle(file);
	}

	std::string Utf8(const wchar_t* a_text)
	{
		if (!a_text || !*a_text)
			return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		std::string out(size > 0 ? size - 1 : 0, '\0');
		if (size > 1)
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), size, nullptr, nullptr);
		return out;
	}

	/** @brief The directories of every module in the game, for the PDBs that sit beside their DLLs. */
	std::wstring SymbolPath(DWORD a_processId)
	{
		std::wstring path;
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, a_processId);
		if (snapshot == INVALID_HANDLE_VALUE)
			return path;
		MODULEENTRY32W entry{ sizeof(entry) };
		for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
			std::wstring dir = entry.szExePath;
			if (const auto slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos)
				dir.resize(slash);
			if (path.find(dir + L";") == std::wstring::npos)
				path += dir + L";";
		}
		CloseHandle(snapshot);
		return path;
	}

	void WriteFrame(FILE* a_out, HANDLE a_process, int a_index, DWORD64 a_address)
	{
		char moduleName[64] = "?";
		IMAGEHLP_MODULE64 module{ sizeof(module) };
		DWORD64 moduleBase = 0;
		if (SymGetModuleInfo64(a_process, a_address, &module)) {
			strncpy_s(moduleName, module.ModuleName, _TRUNCATE);
			moduleBase = module.BaseOfImage;
		}
		alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
		auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 511;
		DWORD64 displacement = 0;
		if (SymFromAddr(a_process, a_address, &displacement, symbol)) {
			IMAGEHLP_LINE64 line{ sizeof(line) };
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddr64(a_process, a_address, &lineDisplacement, &line))
				std::fprintf(a_out, "  %2d  %s!%s+0x%llx  (%s:%lu)\n", a_index, moduleName, symbol->Name, displacement, line.FileName, line.LineNumber);
			else
				std::fprintf(a_out, "  %2d  %s!%s+0x%llx\n", a_index, moduleName, symbol->Name, displacement);
		} else
			std::fprintf(a_out, "  %2d  %s+0x%llx  (0x%016llx)\n", a_index, moduleName, a_address - moduleBase, a_address);
	}

	void WriteThread(FILE* a_out, HANDLE a_process, DWORD a_threadId, bool a_stuck)
	{
		const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_threadId);
		if (!thread) {
			std::fprintf(a_out, "\nthread %lu: could not be opened (error %lu)\n", a_threadId, GetLastError());
			return;
		}
		std::string name;
		PWSTR description = nullptr;
		if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
			name = Utf8(description);
			LocalFree(description);
		}
		std::fprintf(a_out, "\nthread %lu%s%s%s\n", a_threadId, name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"");
		if (a_stuck)
			std::fprintf(a_out, "  (the thread the watchdog found stuck)\n");
		if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
			std::fprintf(a_out, "  could not be suspended (error %lu)\n", GetLastError());
			CloseHandle(thread);
			return;
		}
		CONTEXT context{};
		context.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(thread, &context)) {
			STACKFRAME64 frame{};
			frame.AddrPC.Offset = context.Rip;
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = context.Rbp;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = context.Rsp;
			frame.AddrStack.Mode = AddrModeFlat;
			for (int i = 0; i < kMaxFrames; ++i) {
				if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, a_process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
					frame.AddrPC.Offset == 0)
					break;
				WriteFrame(a_out, a_process, i, frame.AddrPC.Offset);
			}
		} else
			std::fprintf(a_out, "  no context (error %lu)\n", GetLastError());
		ResumeThread(thread);
		CloseHandle(thread);
	}

	/** @brief Every thread's stack, the stuck one first, as text. */
	void WriteStacks(HANDLE a_process, const Shared* a_shared)
	{
		FILE* out = nullptr;
		if (_wfopen_s(&out, a_shared->textPath, L"w") != 0 || !out)
			return;
		const DWORD processId = GetProcessId(a_process);
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
		const auto searchPath = SymbolPath(processId);
		const bool symbols = SymInitializeW(a_process, searchPath.c_str(), TRUE) != FALSE;
		std::fprintf(out, "Community Shaders hang report: the render thread stopped presenting.\nEvery thread's stack follows, the stuck thread first%s.\n",
			symbols ? "" : " (no symbols: SymInitialize failed)");
		std::vector<DWORD> threads;
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot != INVALID_HANDLE_VALUE) {
			THREADENTRY32 entry{ sizeof(entry) };
			for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
				if (entry.th32OwnerProcessID == processId && entry.th32ThreadID != a_shared->threadId)
					threads.push_back(entry.th32ThreadID);
			CloseHandle(snapshot);
		}
		WriteThread(out, a_process, a_shared->threadId, true);
		for (const auto thread : threads)
			WriteThread(out, a_process, thread, false);
		std::fclose(out);
		if (symbols)
			SymCleanup(a_process);
	}
}

int wmain(int a_argc, wchar_t** a_argv)
{
	if (a_argc != 6)
		return 1;
	const HANDLE process = HandleArg(a_argv[1]);
	const HANDLE request = HandleArg(a_argv[2]);
	const HANDLE done = HandleArg(a_argv[3]);
	const HANDLE section = HandleArg(a_argv[4]);
	const wchar_t* crashPath = a_argv[5];
	const auto* shared = static_cast<const Shared*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(Shared)));

	// One request after another, until a crash or the game exiting (the process handle signalled).
	for (;;) {
		const HANDLE waits[2]{ request, process };
		if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
			return 0;
		ResetEvent(request);
		if (!shared) {
			SetEvent(done);
			return 1;
		}
		if (shared->kind == kHang) {
			WriteStacks(process, shared);
			WriteMinidump(process, shared->dumpPath, shared, false);
			SetEvent(done);
			continue;
		}
		WriteMinidump(process, crashPath, shared, true);
		SetEvent(done);
		return 0;
	}
}
