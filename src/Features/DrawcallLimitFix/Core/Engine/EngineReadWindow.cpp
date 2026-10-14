// EngineReadWindow: Open/Close on the render thread, leases taken and released on any worker; the report's counters.

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <array>
#include <chrono>
#include <cstring>
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

	EngineReadWindow::Counters EngineReadWindow::Take()
	{
		Counters c;
		c.unleased = unleased.exchange(0, std::memory_order_relaxed);
		c.unleasedFirst = unleasedFirst.exchange(nullptr, std::memory_order_relaxed);
		if (!closes && !c.unleased)
			return c;
		c.any = true;
		c.closes = closes;
		c.closesWaited = closesWaited;
		c.waitedUs = waitedUs;
		c.waitedMaxUs = waitedMaxUs;
		c.refused = refused.exchange(0, std::memory_order_relaxed);
		closes = closesWaited = 0;
		waitedUs = waitedMaxUs = 0.0;
		return c;
	}
}
