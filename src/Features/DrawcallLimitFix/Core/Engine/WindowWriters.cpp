// WindowWriters: the cell animations' and animated cell objects' jobs' updates between the frame's sampling and the window's resumption (WindowWriters.h).
#include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"

#include <atomic>
#include <chrono>

namespace DCLF::WindowWriters
{
	namespace
	{
		bool installed = false;
		std::atomic<bool> held{ false };  // Hold: the updates wait for Release

		// One writer's counters (Counters::Job), atomically.
		struct JobCounters
		{
			std::atomic<std::uint64_t> runs{ 0 }, waited{ 0 }, waitedUs{ 0 }, waitedMaxUs{ 0 };

			Counters::Job Take()
			{
				return { runs.exchange(0, std::memory_order_relaxed), waited.exchange(0, std::memory_order_relaxed), waitedUs.exchange(0, std::memory_order_relaxed),
					waitedMaxUs.exchange(0, std::memory_order_relaxed) };
			}
		};
		JobCounters cellAnimations, animatedObjects;

		// A writer's update: after the frame's sampling (and a parity frame's Release), with the window suspended until EndWriter.
		void BeginWriter(JobCounters& a_counters)
		{
			a_counters.runs.fetch_add(1, std::memory_order_relaxed);
			const auto start = std::chrono::steady_clock::now();
			FrameValues::Get().WaitSampled();
			held.wait(true, std::memory_order_acquire);
			if (const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count())) {
				a_counters.waited.fetch_add(1, std::memory_order_relaxed);
				a_counters.waitedUs.fetch_add(us, std::memory_order_relaxed);
				for (auto max = a_counters.waitedMaxUs.load(std::memory_order_relaxed); us > max && !a_counters.waitedMaxUs.compare_exchange_weak(max, us, std::memory_order_relaxed);) {}
			}
			EngineReadWindow::Suspend();
		}

		void EndWriter() { EngineReadWindow::Resume(); }

		template <class F>
		void AsWriter(JobCounters& a_counters, F&& a_update)
		{
			BeginWriter(a_counters);
			a_update();
			EndWriter();
		}

		struct CellAnimations
		{
			// The job's call: TES and the frame's animation time (xmm1).
			static void thunk(RE::TES* a_tes, float a_time)
			{
				AsWriter(cellAnimations, [&] { func(a_tes, a_time); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The job's queuing of its updates on its group, then (straight on) the group's start and wait: the window is suspended from
		// the first to the end of the last.
		struct AnimatedObjects
		{
			static void thunk(void* a_group)
			{
				BeginWriter(animatedObjects);
				func(a_group);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AnimatedObjectsWait
		{
			static void thunk(void* a_group)
			{
				func(a_group);
				EndWriter();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool Expected(const Engine::Offset& a_call, const Engine::Offset& a_target, const char* a_job, const char* a_what)
		{
			if (Engine::CallsTo(REL::Offset(a_call).address(), REL::Offset(a_target).address()))
				return true;
			logger::warn("[DCLF] the {} job's update is not where expected; it may write {} while the frame values sample them", a_job, a_what);
			return false;
		}

		void Installed(const char* a_job)
		{
			logger::info("[DCLF] {} job: its update waits for the frame values' sampling, the engine-read window suspended around it", a_job);
		}
	}

	void Install()
	{
		if (installed)
			return;
		if (Expected(Engine::Address::kCellAnimationsCall, Engine::Address::kCellAnimationsUpdate, "cell animations", "animated references")) {
			stl::write_thunk_call<CellAnimations>(REL::Offset(Engine::Address::kCellAnimationsCall).address());
			Installed("cell animations");
			installed = true;
		}
		// Both of its calls or neither: a suspension never resumed would refuse every lease after it.
		constexpr const char* kObjects = "animated cell object update";
		if (Expected(Engine::Address::kAnimatedObjectsCall, Engine::Address::kAnimatedObjectsUpdate, kObjects, "Havok-driven transforms") &&
			Expected(Engine::Address::kAnimatedObjectsWaitCall, Engine::Address::kAnimatedObjectsWait, kObjects, "Havok-driven transforms")) {
			stl::write_thunk_call<AnimatedObjects>(REL::Offset(Engine::Address::kAnimatedObjectsCall).address());
			stl::write_thunk_call<AnimatedObjectsWait>(REL::Offset(Engine::Address::kAnimatedObjectsWaitCall).address());
			Installed(kObjects);
			installed = true;
		}
	}

	Counters Take()
	{
		return { cellAnimations.Take(), animatedObjects.Take() };
	}

	void Hold()
	{
		if (installed)
			held.store(true, std::memory_order_release);
	}

	void Release()
	{
		if (held.exchange(false, std::memory_order_acq_rel))
			held.notify_all();
	}

	bool Installed() { return installed; }
}
