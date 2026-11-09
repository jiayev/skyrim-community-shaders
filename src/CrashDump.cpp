#include "CrashDump.h"

// The dumps are written out of process, by CommunityShadersCrashDump.exe beside this DLL (tools/crash-dump-helper). MiniDumpWriteDump
// suspends every other thread and then allocates from the process heap and takes loader locks: called in here, a thread suspended
// while holding the heap's lock hung the game in the dump (2026-10-11). The crash handler only writes the faulting thread and its
// exception pointers into a shared section, signals the helper and waits for it: no allocation, no lock.

#include <mutex>
#include <thread>

namespace CrashDump
{
	namespace
	{
		// The section's layout, read by the helper (tools/crash-dump-helper/main.cpp, Shared).
		struct Shared
		{
			std::uint32_t threadId;
			std::uint32_t kind;  // 0: a crash, 1: a hang
			std::uint64_t exceptionPointers;
			wchar_t dumpPath[1024];
			wchar_t textPath[1024];
		};
		constexpr std::uint32_t kCrash = 0, kHang = 1;

		struct Helper
		{
			HANDLE request = nullptr;
			HANDLE done = nullptr;
			Shared* shared = nullptr;
		};
		Helper helper;

		// A hang request in flight: the crash handler waits for it before it uses the section.
		std::atomic<bool> hangBusy{ false };
		std::mutex hangMutex;

		// How long the faulting thread waits for the helper's dump before the exception goes on.
		constexpr DWORD kCrashDumpWaitMs = 120000;

		LONG CALLBACK OnException(EXCEPTION_POINTERS* a_exception)
		{
			if (!a_exception || !a_exception->ExceptionRecord)
				return EXCEPTION_CONTINUE_SEARCH;

			const DWORD code = a_exception->ExceptionRecord->ExceptionCode;
			// Ignore the benign ones the game raises constantly (C++ EH, debugger notifications).
			if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
				code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
				code != 0xC0000409 /* fast-fail / stack cookie */)
				return EXCEPTION_CONTINUE_SEARCH;

			static std::atomic<bool> written{ false };
			if (written.exchange(true))
				return EXCEPTION_CONTINUE_SEARCH;

			logger::critical("[CRASH] exception {:#x} at {} - writing CommunityShaders.dmp", code,
				fmt::ptr(a_exception->ExceptionRecord->ExceptionAddress));
			spdlog::default_logger()->flush();

			if (helper.shared) {
				// A hang dump being written holds the section; the helper answers it first.
				for (DWORD waited = 0; hangBusy.load(std::memory_order_acquire) && waited < kCrashDumpWaitMs; waited += 50)
					Sleep(50);
				helper.shared->threadId = GetCurrentThreadId();
				helper.shared->kind = kCrash;
				helper.shared->exceptionPointers = reinterpret_cast<std::uintptr_t>(a_exception);
				ResetEvent(helper.done);
				SetEvent(helper.request);
				WaitForSingleObject(helper.done, kCrashDumpWaitMs);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		void StartHelper()
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&StartHelper), &module);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(module, modulePath, MAX_PATH);
			const auto exe = std::filesystem::path{ modulePath }.parent_path() / L"CommunityShadersCrashDump.exe";
			wchar_t gamePath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, gamePath, MAX_PATH);
			const auto dump = std::filesystem::path{ gamePath }.parent_path() / L"CommunityShaders.dmp";
			if (!std::filesystem::exists(exe)) {
				logger::warn("[CRASH] {} is missing: crashes and hangs write no dump", exe.string());
				return;
			}

			SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
			HANDLE process = nullptr;
			DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
				PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, TRUE, 0);
			const HANDLE request = CreateEventW(&inherit, TRUE, FALSE, nullptr);
			const HANDLE done = CreateEventW(&inherit, TRUE, FALSE, nullptr);
			const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
			auto* shared = section ? static_cast<Shared*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Shared))) : nullptr;
			if (!process || !request || !done || !shared) {
				logger::warn("[CRASH] the crash dump helper's handles could not be made (error {}): crashes and hangs write no dump", GetLastError());
				return;
			}

			// The helper inherits these four handles only, not every inheritable handle of the game.
			HANDLE inherited[4]{ process, request, done, section };
			SIZE_T attributeBytes = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
			std::vector<std::byte> attributeStorage(attributeBytes);
			auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
			UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(startup);
			startup.lpAttributeList = attributes;
			std::wstring command = std::format(L"\"{}\" {} {} {} {} \"{}\"", exe.wstring(),
				reinterpret_cast<std::uintptr_t>(process), reinterpret_cast<std::uintptr_t>(request), reinterpret_cast<std::uintptr_t>(done),
				reinterpret_cast<std::uintptr_t>(section), dump.wstring());
			PROCESS_INFORMATION info{};
			const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
			DeleteProcThreadAttributeList(attributes);
			if (!started) {
				logger::warn("[CRASH] {} could not be started (error {}): crashes and hangs write no dump", exe.string(), GetLastError());
				return;
			}
			CloseHandle(info.hThread);
			CloseHandle(info.hProcess);
			helper = { request, done, shared };
			logger::info("[CRASH] crash dumps are written by {} (pid {}) to {}", exe.filename().string(), info.dwProcessId, dump.string());

			// CS_TEST_CRASH=<seconds> (a development switch): a worker thread faults that long after load, to exercise the
			// handler and the helper end to end.
			if (const char* test = std::getenv("CS_TEST_CRASH"); test && *test) {
				const int seconds = std::max(1, std::atoi(test));
				logger::warn("[CRASH] CS_TEST_CRASH: a deliberate access violation in {} s", seconds);
				std::thread([seconds] {
					std::this_thread::sleep_for(std::chrono::seconds(seconds));
					*static_cast<volatile int*>(nullptr) = 0;
				}).detach();
			}
		}

		void CopyPath(wchar_t (&a_to)[1024], const std::filesystem::path& a_from)
		{
			wcsncpy_s(a_to, a_from.c_str(), _TRUNCATE);
		}
	}

	void Start()
	{
		StartHelper();
		AddVectoredExceptionHandler(1, &OnException);
	}

	bool Available() { return helper.shared != nullptr; }

	bool RequestHangDump(std::uint32_t a_threadId, const std::filesystem::path& a_dump, const std::filesystem::path& a_text, std::uint32_t a_timeoutMs)
	{
		if (!helper.shared)
			return false;
		std::lock_guard lock(hangMutex);
		hangBusy.store(true, std::memory_order_release);
		helper.shared->threadId = a_threadId;
		helper.shared->kind = kHang;
		helper.shared->exceptionPointers = 0;
		CopyPath(helper.shared->dumpPath, a_dump);
		CopyPath(helper.shared->textPath, a_text);
		ResetEvent(helper.done);
		SetEvent(helper.request);
		const bool answered = WaitForSingleObject(helper.done, a_timeoutMs) == WAIT_OBJECT_0;
		hangBusy.store(false, std::memory_order_release);
		return answered;
	}
}
