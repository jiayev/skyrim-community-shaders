// WindowWriters: the cell animations job's update between the frame's sampling and the window's resumption (WindowWriters.h).
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
		std::atomic<std::uint64_t> runs{ 0 }, waited{ 0 }, waitedUs{ 0 }, waitedMaxUs{ 0 };

		struct CellAnimations
		{
			// The job's call: TES and the frame's animation time (xmm1).
			static void thunk(RE::TES* a_tes, float a_time)
			{
				runs.fetch_add(1, std::memory_order_relaxed);
				const auto start = std::chrono::steady_clock::now();
				FrameValues::Get().WaitSampled();
				if (const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count())) {
					waited.fetch_add(1, std::memory_order_relaxed);
					waitedUs.fetch_add(us, std::memory_order_relaxed);
					for (auto max = waitedMaxUs.load(std::memory_order_relaxed); us > max && !waitedMaxUs.compare_exchange_weak(max, us, std::memory_order_relaxed);) {}
				}
				EngineReadWindow::Suspend();
				func(a_tes, a_time);
				EngineReadWindow::Resume();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (installed)
			return;
		const auto site = REL::Offset(Engine::Address::kCellAnimationsCall).address();
		if (!Engine::CallsTo(site, REL::Offset(Engine::Address::kCellAnimationsUpdate).address())) {
			logger::warn("[DCLF] the cell animations job's update is not where expected; it may write animated references while the frame values sample them");
			return;
		}
		stl::write_thunk_call<CellAnimations>(site);
		installed = true;
		logger::info("[DCLF] cell animations job: its update waits for the frame values' sampling, the engine-read window suspended around it");
	}

	Counters Take()
	{
		return { runs.exchange(0, std::memory_order_relaxed), waited.exchange(0, std::memory_order_relaxed), waitedUs.exchange(0, std::memory_order_relaxed),
			waitedMaxUs.exchange(0, std::memory_order_relaxed) };
	}

	bool Installed() { return installed; }
}
