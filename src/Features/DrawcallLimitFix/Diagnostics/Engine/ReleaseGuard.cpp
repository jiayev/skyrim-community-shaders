// ReleaseGuard (CS_DCLF_RELEASE_GUARD): the NiRefObject::DeleteThis patch and its counting, on whatever thread releases.

#include "ReleaseGuard.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"

#include <array>
#include <cstring>
#include <typeinfo>

namespace DCLF
{
	void ReleaseGuard::Install()
	{
		if (installed || !Diagnostics::Observe().releaseGuard)
			return;
		// NiRefObject::DeleteThis (RELOCATION_ID(69177, 70538): the same 16 bytes on SE 1.5.97 and AE 1.6.1170): if (this) this->vtable[0](this, 1). Replaced whole by an
		// absolute jump (14 of its 16 bytes) to the same call with a count.
		const std::uintptr_t at = REL::RelocationID(69177, 70538).address();
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

	ReleaseGuard::Counters ReleaseGuard::Take()
	{
		Counters c;
		c.lane = laneReleases.exchange(0, std::memory_order_relaxed);
		c.pool = poolReleases.exchange(0, std::memory_order_relaxed);
		c.unnamed = unnamed.exchange(0, std::memory_order_relaxed);
		c.first = first.exchange(nullptr, std::memory_order_relaxed);
		c.last = last.exchange(nullptr, std::memory_order_relaxed);
		return c;
	}
}
