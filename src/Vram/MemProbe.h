#pragma once

#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/**
 * @brief One memory measure for A/B runs of different builds, native D3D11 or DXVK alike.
 *
 * It asks the kernel, so what it reads does not depend on the API or on either build's own accounting. CS_AB_PROBE=1 writes
 * CS-AB-memory.csv beside the log: a row a second, and a row at each Mark. Tools: tools/cs-ab. The same file is in both builds.
 */
namespace MemProbe
{
	struct Reading
	{
		std::uint64_t localUsage = 0, localBudget = 0, nonLocalUsage = 0;
		std::uint64_t privateBytes = 0, workingSet = 0;
	};

	namespace detail
	{
		struct AdapterInfo
		{
			UINT handle;
			LUID luid;
			ULONG sources;
			BOOL precisePresent;
		};
		struct EnumAdapters2
		{
			ULONG count;
			AdapterInfo* adapters;
		};
		struct VideoMemoryInfo
		{
			HANDLE process;
			UINT adapter;
			UINT segmentGroup;
			UINT64 budget, usage, reservation, available;
			UINT physicalAdapter;
		};

		inline FILE*& File()
		{
			static FILE* file = nullptr;
			return file;
		}
		inline std::mutex& Lock()
		{
			static std::mutex lock;
			return lock;
		}
	}

	/** @brief This process' use of the adapter it uses most, and its own memory. */
	inline Reading Read()
	{
		using namespace detail;
		using EnumFn = LONG(APIENTRY*)(EnumAdapters2*);
		using QueryFn = LONG(APIENTRY*)(VideoMemoryInfo*);
		static const HMODULE gdi = LoadLibraryW(L"gdi32.dll");
		static const auto enumerate = reinterpret_cast<EnumFn>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
		static const auto query = reinterpret_cast<QueryFn>(GetProcAddress(gdi, "D3DKMTQueryVideoMemoryInfo"));
		static const std::vector<UINT> adapterHandles = [] {
			AdapterInfo adapters[16]{};
			EnumAdapters2 list{ 16, adapters };
			std::vector<UINT> handles;
			if (enumerate && query && enumerate(&list) == 0)
				for (ULONG i = 0; i < list.count; ++i)
					handles.push_back(adapters[i].handle);
			return handles;
		}();
		Reading reading;
		// The adapter this process uses most: a machine also has an integrated GPU and the software adapter.
		for (const UINT adapter : adapterHandles) {
			VideoMemoryInfo local{ GetCurrentProcess(), adapter, 0 };
			if (query(&local) != 0 || local.usage <= reading.localUsage)
				continue;
			reading.localUsage = local.usage;
			reading.localBudget = local.budget;
			VideoMemoryInfo nonLocal{ GetCurrentProcess(), adapter, 1 };
			if (query(&nonLocal) == 0)
				reading.nonLocalUsage = nonLocal.usage;
		}
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
			reading.privateBytes = counters.PrivateUsage;
			reading.workingSet = counters.WorkingSetSize;
		}
		return reading;
	}

	inline void Row(std::string_view a_label, std::uint32_t a_frame = 0)
	{
		const auto reading = Read();
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		std::lock_guard guard(detail::Lock());
		if (auto* file = detail::File()) {
			std::fprintf(file, "%lld,%u,%.*s,%llu,%llu,%llu,%llu,%llu\n", static_cast<long long>(now), a_frame, static_cast<int>(a_label.size()), a_label.data(),
				static_cast<unsigned long long>(reading.localUsage >> 20), static_cast<unsigned long long>(reading.localBudget >> 20),
				static_cast<unsigned long long>(reading.nonLocalUsage >> 20), static_cast<unsigned long long>(reading.privateBytes >> 20),
				static_cast<unsigned long long>(reading.workingSet >> 20));
			std::fflush(file);
		}
	}

	/** @brief Opens the file and starts the once-a-second rows, if CS_AB_PROBE=1. Any thread, once is enough. */
	inline void Start(const std::filesystem::path& a_directory)
	{
		static std::once_flag once;
		std::call_once(once, [&] {
			char value[2]{};
			if (!GetEnvironmentVariableA("CS_AB_PROBE", value, sizeof(value)) || value[0] != '1')
				return;
			{
				std::lock_guard guard(detail::Lock());
				detail::File() = _wfopen((a_directory / L"CS-AB-memory.csv").c_str(), L"w");
				if (auto* file = detail::File())
					std::fputs("epochMs,frame,label,localMiB,budgetMiB,nonLocalMiB,privateMiB,workingSetMiB\n", file);
			}
			std::thread([] {
				for (;;) {
					Row("");
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}).detach();
		});
	}

	/** @brief A named row, with where the player is. Any thread that may read the player. */
	inline void Mark(std::string_view a_label, std::uint32_t a_frame)
	{
		std::string label(a_label);
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const auto position = player->GetPosition();
			const auto* cell = player->GetParentCell();
			const char* name = cell ? cell->GetFormEditorID() : nullptr;
			label += std::format(" @{:.0f}/{:.0f} {}", position.x, position.y, name && *name ? name : (cell && cell->IsExteriorCell() ? "exterior" : "?"));
			std::replace(label.begin(), label.end(), ',', ' ');
		}
		Row(label, a_frame);
	}
}
