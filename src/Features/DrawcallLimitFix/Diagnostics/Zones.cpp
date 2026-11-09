#include "Zones.h"

#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace DCLF::Zones
{
	namespace
	{
		struct Tally
		{
			std::uint64_t count = 0, totalNs = 0, maxNs = 0, topLevelNs = 0;
		};

		// One thread's tallies. Created at the thread's first recorded zone and kept for the process (the pools' threads live as
		// long), so a phase's end can read them after the thread is gone. The lock is the thread's own but at a phase's ends.
		struct ThreadTable
		{
			std::mutex mutex;
			std::uint32_t threadId = 0;
			std::uint32_t depth = 0;
			std::unordered_map<const basic_telemetry::Callsite*, Tally> tallies;
		};

		std::mutex tablesMutex;
		std::vector<std::unique_ptr<ThreadTable>> tables;
		thread_local ThreadTable* threadTable = nullptr;

		ThreadTable& CurrentTable()
		{
			if (!threadTable) {
				auto table = std::make_unique<ThreadTable>();
				table->threadId = GetCurrentThreadId();
				threadTable = table.get();
				std::lock_guard lock(tablesMutex);
				tables.push_back(std::move(table));
			}
			return *threadTable;
		}

		std::int64_t Now()
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		double NsPerTick()
		{
			static const double ns = [] {
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				return 1e9 / static_cast<double>(frequency.QuadPart);
			}();
			return ns;
		}

		std::string ThreadDescription(std::uint32_t a_threadId)
		{
			std::string name;
			if (const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_threadId)) {
				PWSTR description = nullptr;
				if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
					const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
					if (size > 1) {
						name.resize(static_cast<std::size_t>(size - 1));
						WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), size, nullptr, nullptr);
					}
					LocalFree(description);
				}
				CloseHandle(thread);
			}
			return name;
		}
	}

	void Scope::Begin(const basic_telemetry::Callsite& a_site) noexcept
	{
		try {
			auto& table = CurrentTable();
			topLevel = table.depth++ == 0;
		} catch (...) {
			return;
		}
		site = &a_site;
		::new (static_cast<void*>(storage)) basic_telemetry::Scope(a_site);
		start = Now();
	}

	void Scope::End() noexcept
	{
		const auto ns = static_cast<std::uint64_t>(static_cast<double>(Now() - start) * NsPerTick());
		std::launder(reinterpret_cast<basic_telemetry::Scope*>(storage))->~Scope();
		auto& table = *threadTable;
		--table.depth;
		try {
			std::lock_guard lock(table.mutex);
			auto& tally = table.tallies[site];
			++tally.count;
			tally.totalNs += ns;
			tally.maxNs = std::max(tally.maxNs, ns);
			if (topLevel)
				tally.topLevelNs += ns;
		} catch (...) {
		}
	}

	void ResetThreadRows()
	{
		std::lock_guard lock(tablesMutex);
		for (auto& table : tables) {
			std::lock_guard tableLock(table->mutex);
			table->tallies.clear();
		}
	}

	std::vector<ThreadRow> TakeThreadRows()
	{
		std::vector<ThreadRow> rows;
		std::lock_guard lock(tablesMutex);
		for (auto& table : tables) {
			std::unordered_map<const basic_telemetry::Callsite*, Tally> tallies;
			{
				std::lock_guard tableLock(table->mutex);
				tallies.swap(table->tallies);
			}
			if (tallies.empty())
				continue;
			const auto thread = ThreadDescription(table->threadId);
			for (const auto& [site, tally] : tallies)
				rows.push_back({ table->threadId, thread, std::string(site->name), tally.count, tally.totalNs, tally.maxNs, tally.topLevelNs });
		}
		return rows;
	}
}
