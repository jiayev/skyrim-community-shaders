#include "ImportTimings.h"

#include <Tracy/Tracy.hpp>

namespace DCLF::ImportTimings
{
	namespace
	{
		enum Entry : std::size_t
		{
			kGrid,
			kTaskDrain,
			kReferenceFinish,
			kIoPump,
			kLodDrain,
			kLodRetire,
			kEntries
		};
		constexpr std::array<const char*, kEntries> kNames{ "grid controller", "BSTaskPool drain", "reference finish", "I/O completion pump", "LOD swaps' drain",
			"LOD block retirement" };
		// AE 1.6.1170, from the image base.
		constexpr std::array<std::uintptr_t, kEntries> kOffsets{ 0x19b0b0, 0x654880, 0x1a0920, 0xe014e0, 0x513840, 0x510a10 };

		struct Counter
		{
			std::atomic<std::uint64_t> calls{ 0 }, ticks{ 0 };
		};
		std::array<std::array<Counter, 2>, kEntries> counters;  // [entry][1: the main thread]
		std::atomic<std::uint32_t> mainThread{ 0 };
		bool installed = false;

		struct Scope
		{
			explicit Scope(Entry a_entry) :
				entry(a_entry)
			{
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				start = now.QuadPart;
			}
			~Scope()
			{
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				auto& counter = counters[entry][::GetCurrentThreadId() == mainThread.load(std::memory_order_relaxed) ? 1 : 0];
				counter.calls.fetch_add(1, std::memory_order_relaxed);
				counter.ticks.fetch_add(static_cast<std::uint64_t>(now.QuadPart - start), std::memory_order_relaxed);
			}
			Entry entry;
			std::int64_t start;
		};

		// FUN_14019b0b0(controller, float*, bool)
		struct Grid
		{
			static std::uint64_t thunk(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3)
			{
				ZoneScopedN("CS.DCLF.Import.GridController");
				Scope scope(kGrid);
				return func(a1, a2, a3);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// FUN_140654880(pool)
		struct TaskDrain
		{
			static void thunk(std::uintptr_t a1)
			{
				ZoneScopedN("CS.DCLF.Import.TaskPoolDrain");
				Scope scope(kTaskDrain);
				func(a1);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// FUN_1401a0920(?, TESObjectREFR*, ?, ?, bool, ?)
		struct ReferenceFinish
		{
			static void thunk(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6)
			{
				ZoneScopedN("CS.DCLF.Import.ReferenceFinish");
				Scope scope(kReferenceFinish);
				func(a1, a2, a3, a4, a5, a6);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// FUN_140e014e0(IOManager)
		struct IoPump
		{
			static std::uint64_t thunk(std::uintptr_t a1)
			{
				ZoneScopedN("CS.DCLF.Import.IoPump");
				Scope scope(kIoPump);
				return func(a1);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// FUN_140513840(queue)
		struct LodDrain
		{
			static void thunk(std::uintptr_t a1)
			{
				ZoneScopedN("CS.DCLF.Import.LodDrain");
				Scope scope(kLodDrain);
				func(a1);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// FUN_140510a10(quadtree node, which blocks)
		struct LodRetire
		{
			static void thunk(std::uintptr_t a1, std::uint32_t a2)
			{
				ZoneScopedN("CS.DCLF.Import.LodRetire");
				Scope scope(kLodRetire);
				func(a1, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		stl::detour_thunk<Grid>(base + kOffsets[kGrid]);
		stl::detour_thunk<TaskDrain>(base + kOffsets[kTaskDrain]);
		stl::detour_thunk<ReferenceFinish>(base + kOffsets[kReferenceFinish]);
		stl::detour_thunk<IoPump>(base + kOffsets[kIoPump]);
		stl::detour_thunk<LodDrain>(base + kOffsets[kLodDrain]);
		stl::detour_thunk<LodRetire>(base + kOffsets[kLodRetire]);
		installed = true;
		logger::info("[DCLF] import timings (T6b0): the engine's {} import entry points detoured", static_cast<std::size_t>(kEntries));
	}

	void NoteMainThread(std::uint32_t a_thread)
	{
		mainThread.store(a_thread, std::memory_order_relaxed);
	}

	std::string TakeReport(std::uint32_t a_frames)
	{
		if (!installed)
			return {};
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const double frames = std::max(1u, a_frames);
		const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
		std::string text = fmt::format("[DCLF] engine import work by thread (T6b0, inclusive, over {} frames): per frame, main / other", a_frames);
		for (std::size_t e = 0; e < kEntries; ++e) {
			std::array<std::uint64_t, 2> calls{}, ticks{};
			for (std::size_t t = 0; t < 2; ++t) {
				calls[t] = counters[e][t].calls.exchange(0, std::memory_order_relaxed);
				ticks[t] = counters[e][t].ticks.exchange(0, std::memory_order_relaxed);
			}
			text += fmt::format("; {}: {:.2f} calls {:.3f} ms / {:.2f} calls {:.3f} ms", kNames[e], calls[1] / frames, ticks[1] * msPerTick / frames, calls[0] / frames,
				ticks[0] * msPerTick / frames);
		}
		return text;
	}
}
