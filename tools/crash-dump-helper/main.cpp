// CommunityShadersCrashDump: writes Community Shaders' crash minidump from outside the game.
//
// The plugin starts this helper once at load (XSEPlugin.cpp, StartCrashDumpHelper) with four inherited handles:
// the game process, the crash event, the done event and a small shared section. On a crash the plugin's vectored
// handler writes the faulting thread and its EXCEPTION_POINTERS address into the section, signals the crash event
// and waits for the done event. MiniDumpWriteDump suspends every thread of the process it dumps and then
// allocates from heaps and takes loader locks; run inside the game, a thread suspended while holding the process
// heap's lock deadlocked it (2026-10-11). Run from here, nothing it needs lives in the suspended process.
//
// Usage: CommunityShadersCrashDump.exe <process> <crash event> <done event> <section> <dump path> (handles in decimal).

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdint>
#include <cwchar>

namespace
{
	// The section's layout, written by the plugin's handler (XSEPlugin.cpp, CrashDumpShared).
	struct Shared
	{
		std::uint32_t threadId;
		std::uint32_t pad;
		std::uint64_t exceptionPointers;  // EXCEPTION_POINTERS* in the game's address space
	};

	HANDLE HandleArg(const wchar_t* a_text)
	{
		return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(a_text, nullptr, 10)));
	}
}

int wmain(int a_argc, wchar_t** a_argv)
{
	if (a_argc != 6)
		return 1;
	const HANDLE process = HandleArg(a_argv[1]);
	const HANDLE crash = HandleArg(a_argv[2]);
	const HANDLE done = HandleArg(a_argv[3]);
	const HANDLE section = HandleArg(a_argv[4]);
	const wchar_t* path = a_argv[5];

	// The game exiting (the process handle signalled) ends the helper without a dump.
	const HANDLE waits[2]{ crash, process };
	if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
		return 0;

	const auto* shared = static_cast<const Shared*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(Shared)));
	const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (shared && file != INVALID_HANDLE_VALUE) {
		MINIDUMP_EXCEPTION_INFORMATION info{};
		info.ThreadId = shared->threadId;
		info.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<std::uintptr_t>(shared->exceptionPointers));
		info.ClientPointers = TRUE;  // the pointers are the game's
		MiniDumpWriteDump(process, GetProcessId(process), file,
			static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &info, nullptr, nullptr);
	}
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	SetEvent(done);
	return 0;
}
