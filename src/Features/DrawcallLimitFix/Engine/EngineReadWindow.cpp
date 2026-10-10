#include "EngineReadWindow.h"

#include <chrono>
#include <immintrin.h>
#include <thread>

namespace DCLF
{
	void EngineReadWindow::Open()
	{
		state.fetch_or(kOpen, std::memory_order_acq_rel);
	}

	void EngineReadWindow::Close()
	{
		const std::uint32_t before = state.fetch_and(~kOpen, std::memory_order_acq_rel);
		if (!(before & kOpen))
			return;
		++closes;
		if (!(before & ~kOpen))
			return;
		// Items in flight: each finishes the one item it holds a lease for (no new lease can be taken now).
		++closesWaited;
		const auto start = std::chrono::steady_clock::now();
		for (std::uint32_t spins = 0; state.load(std::memory_order_acquire) & ~kOpen; ++spins) {
			if (spins < 64)
				_mm_pause();
			else
				std::this_thread::yield();
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		waitedUs += us;
		waitedMaxUs = (std::max)(waitedMaxUs, us);
	}

	EngineReadWindow::Lease::Lease()
	{
		// One word: the lease is counted and the window seen open in the same step, so Close cannot miss it.
		if (state.fetch_add(1, std::memory_order_acq_rel) & kOpen) {
			held = true;
			++heldHere;
			return;
		}
		state.fetch_sub(1, std::memory_order_acq_rel);
		refused.fetch_add(1, std::memory_order_relaxed);
	}

	EngineReadWindow::Lease::~Lease()
	{
		if (held) {
			--heldHere;
			state.fetch_sub(1, std::memory_order_release);
		}
	}

	void EngineReadWindow::NoteUnleased(const char* a_site)
	{
		if (unleased.fetch_add(1, std::memory_order_relaxed) == 0)
			unleasedFirst.store(a_site, std::memory_order_relaxed);
	}

	std::string EngineReadWindow::Report()
	{
		// T6b1d: the scene work's accesses without a lease (any is a defect).
		const std::uint64_t lane = unleased.exchange(0, std::memory_order_relaxed);
		const char* laneFirst = unleasedFirst.exchange(nullptr, std::memory_order_relaxed);
		if (!closes && !lane)
			return {};
		auto line = fmt::format("[DCLF] engine-read window: {} closes, {} of them waited for items in flight ({:.1f} us in all, max {:.1f} us); {} leases refused (work left for the next window); "
								"scene work engine accesses without a lease {}{}{}",
			closes, closesWaited, waitedUs, waitedMaxUs, refused.exchange(0, std::memory_order_relaxed), lane, lane ? " <- LANE ENGINE ACCESS; first: " : " <- OK",
			laneFirst ? laneFirst : "");
		closes = closesWaited = 0;
		waitedUs = waitedMaxUs = 0.0;
		return line;
	}
}
