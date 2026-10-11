#include "EngineReadWindow.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <array>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <thread>
#include <typeinfo>

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

	void ReleaseGuard::Install()
	{
		if (installed || !SwitchEnabled(Switch::ReleaseGuard))
			return;
		// NiRefObject::DeleteThis (AE 1.6.1170, RELOCATION_ID(69177, 70538)): if (this) this->vtable[0](this, 1). Replaced whole by an
		// absolute jump (14 of its 16 bytes) to the same call with a count.
		const std::uintptr_t at = REL::Offset(0xd27520).address();
		constexpr std::array<std::uint8_t, 16> kExpected{ 0x48, 0x85, 0xC9, 0x74, 0x0B, 0x48, 0x8B, 0x01, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x48, 0xFF, 0x20 };
		if (std::memcmp(reinterpret_cast<const void*>(at), kExpected.data(), kExpected.size()) != 0) {
			logger::error("[DCLF] release guard (CS_DCLF_RELEASE_GUARD): NiRefObject::DeleteThis is not the expected code; not installed");
			return;
		}
		std::array<std::uint8_t, 14> jump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
		const auto target = reinterpret_cast<std::uintptr_t>(&ReleaseGuard::DeleteThis);
		std::memcpy(jump.data() + 6, &target, sizeof(target));
		REL::safe_write(at, jump.data(), jump.size());
		installed = true;
		logger::info("[DCLF] release guard (CS_DCLF_RELEASE_GUARD): the engine's last releases on DCLF's threads are counted (NiRefObject::DeleteThis)");
	}

	void ReleaseGuard::DeleteThis(void* a_object)
	{
		if (!a_object)
			return;
		if (EngineReadWindow::sceneWork || dclfThread)
			Note(a_object);
		// The original: the scalar deleting destructor, vtable slot 0, with 1 (delete).
		using Destroy = void (*)(void*, std::uint32_t);
		(*static_cast<Destroy* const*>(a_object))[0](a_object, 1);
	}

	void ReleaseGuard::Note(void* a_object)
	{
		(EngineReadWindow::sceneWork || dclfThread == 1 ? laneReleases : poolReleases).fetch_add(1, std::memory_order_relaxed);
		// The class by its RTTI (the object is whole until its destructor runs); a class name lives as long as the module.
		const char* name = nullptr;
		try {
			name = typeid(*static_cast<RE::NiRefObject*>(a_object)).name();
		} catch (...) {
			unnamed.fetch_add(1, std::memory_order_relaxed);
		}
		if (!name)
			return;
		const char* none = nullptr;
		first.compare_exchange_strong(none, name, std::memory_order_relaxed);
		last.store(name, std::memory_order_relaxed);
	}

	std::string ReleaseGuard::Report()
	{
		if (!installed)
			return {};
		const std::uint64_t lane = laneReleases.exchange(0, std::memory_order_relaxed);
		const std::uint64_t pool = poolReleases.exchange(0, std::memory_order_relaxed);
		const std::uint64_t nameless = unnamed.exchange(0, std::memory_order_relaxed);
		const char* firstName = first.exchange(nullptr, std::memory_order_relaxed);
		const char* lastName = last.exchange(nullptr, std::memory_order_relaxed);
		return fmt::format("[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): engine objects' last releases on DCLF's threads: scene lane {}, executor/pool {} "
						   "({} without a class name){}{}{}{}",
			lane, pool, nameless, lane || pool ? " <- LANE ENGINE RELEASE; first: " : " <- OK", firstName ? firstName : "", lane || pool ? ", last: " : "",
			(lane || pool) && lastName ? lastName : "");
	}
}
