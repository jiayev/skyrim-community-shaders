#include "MirrorWatch.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <TlHelp32.h>

namespace DCLF::MirrorWatch
{
	namespace
	{
		constexpr std::uint32_t kSlots = 4;
		constexpr std::uint32_t kMaxHits = 16;
		constexpr std::array<std::uintptr_t, kSlots> kOffsets{ 0x108, 0x128, 0x12C, 0x150 };
		constexpr std::array<const char*, kSlots> kSlotNames{ "+0x108", "near", "far", "+0x150" };

		struct Hit
		{
			std::uintptr_t rip = 0;
			std::uint32_t slot = 0;
			std::uint32_t before = 0, value = 0;
			std::uint32_t thread = 0;
		};

		std::array<std::atomic<std::uintptr_t>, kSlots> watched{};
		std::array<std::atomic<std::uint32_t>, kSlots> lastValue{};
		std::array<Hit, kMaxHits> hits{};
		std::atomic<std::uint32_t> hitCount{ 0 };
		std::atomic<bool> armed{ false };
		std::atomic<bool> arming{ false };
		std::string name;
		std::uint32_t armedFrame = 0;
		std::uint32_t armings = 0;
		std::uint32_t armThread = 0;
		PVOID handler = nullptr;

		LONG CALLBACK OnException(PEXCEPTION_POINTERS a_info)
		{
			if (a_info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !armed.load(std::memory_order_acquire))
				return EXCEPTION_CONTINUE_SEARCH;
			auto* context = a_info->ContextRecord;
			const auto triggered = context->Dr6 & 0xF;
			if (!triggered)
				return EXCEPTION_CONTINUE_SEARCH;
			for (std::uint32_t s = 0; s < kSlots; ++s) {
				if (!(triggered & (1ull << s)))
					continue;
				const auto address = watched[s].load(std::memory_order_relaxed);
				if (!address)
					continue;
				const auto value = *reinterpret_cast<const volatile std::uint32_t*>(address);
				const auto before = lastValue[s].exchange(value, std::memory_order_relaxed);
				// +0x109's 0x40 is the cull's, every frame.
				if (value == before || (s == 0 && ((value ^ before) & ~0x4000u) == 0))
					continue;
				const auto at = hitCount.fetch_add(1, std::memory_order_relaxed);
				if (at < kMaxHits)
					hits[at] = { static_cast<std::uintptr_t>(context->Rip), s, before, value, GetCurrentThreadId() };
			}
			context->Dr6 = 0;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		/** @brief Every thread's debug registers but the caller's (a helper thread runs this, so the game's all get them). */
		void SetAll(const std::array<std::uintptr_t, kSlots>& a_addresses)
		{
			DWORD64 dr7 = 0;
			for (std::uint32_t s = 0; s < kSlots; ++s)
				if (a_addresses[s])
					dr7 |= (1ull << (s * 2)) | (0b01ull << (16 + s * 4)) | (0b11ull << (18 + s * 4));  // local enable, write, 4 bytes
			const DWORD self = GetCurrentThreadId();
			const DWORD process = GetCurrentProcessId();
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE)
				return;
			THREADENTRY32 entry{ sizeof(entry) };
			for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
				if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
					continue;
				HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
				if (!thread)
					continue;
				// The arming thread too: it waits on this helper.
				if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
					CONTEXT context{};
					context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(thread, &context)) {
						context.Dr0 = a_addresses[0];
						context.Dr1 = a_addresses[1];
						context.Dr2 = a_addresses[2];
						context.Dr3 = a_addresses[3];
						context.Dr6 = 0;
						context.Dr7 = dr7;
						SetThreadContext(thread, &context);
					}
					ResumeThread(thread);
				}
				CloseHandle(thread);
			}
			CloseHandle(snapshot);
		}

		void SetAllFromHelper(const std::array<std::uintptr_t, kSlots>& a_addresses)
		{
			std::thread helper([a_addresses] { SetAll(a_addresses); });
			helper.join();
		}
	}

	bool Enabled()
	{
		static const bool enabled = !SwitchValue(Switch::MirrorWatch).empty() && SwitchValue(Switch::MirrorWatch) != "0";
		return enabled;
	}

	void Arm(const RE::NiAVObject* a_node)
	{
		if (!a_node || !Enabled() || armed.load(std::memory_order_acquire))
			return;
		// A value other than 1 names the node to watch.
		static const std::string only = std::string(SwitchValue(Switch::MirrorWatch)) == "1" ? std::string() : std::string(SwitchValue(Switch::MirrorWatch));
		if (!only.empty() && (!a_node->name.c_str() || only != a_node->name.c_str()))
			return;
		if (arming.exchange(true, std::memory_order_acquire))
			return;
		if (!handler)
			handler = AddVectoredExceptionHandler(1, OnException);
		std::array<std::uintptr_t, kSlots> addresses{};
		for (std::uint32_t s = 0; s < kSlots; ++s) {
			addresses[s] = reinterpret_cast<std::uintptr_t>(a_node) + kOffsets[s];
			watched[s].store(addresses[s], std::memory_order_relaxed);
			lastValue[s].store(*reinterpret_cast<const std::uint32_t*>(addresses[s]), std::memory_order_relaxed);
		}
		name = fmt::format("'{}' {} ({})", a_node->name.c_str() ? a_node->name.c_str() : "", static_cast<const void*>(a_node),
			const_cast<RE::NiAVObject*>(a_node)->GetRTTI() && const_cast<RE::NiAVObject*>(a_node)->GetRTTI()->name ? const_cast<RE::NiAVObject*>(a_node)->GetRTTI()->name : "?");
		hitCount.store(0, std::memory_order_relaxed);
		armedFrame = 0;
		++armings;
		armThread = GetCurrentThreadId();
		armed.store(true, std::memory_order_release);
		SetAllFromHelper(addresses);
		arming.store(false, std::memory_order_release);
	}

	std::string TakeReport(std::uint32_t a_frame)
	{
		if (!armed.load(std::memory_order_acquire))
			return {};
		if (!armedFrame)
			armedFrame = a_frame;
		const std::uint32_t count = std::min(hitCount.load(std::memory_order_relaxed), kMaxHits);
		const bool done = count >= kMaxHits || a_frame - armedFrame >= 600;
		std::string text;
		if (count) {
			const auto base = REL::Module::get().base();
			text = fmt::format("[DCLF] mirror watch (arming {} on {}, armed on thread {}): {} changes recorded\n", armings, name, armThread,
				hitCount.load(std::memory_order_relaxed));
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& hit = hits[i];
				text += fmt::format("[DCLF]   {} {:08X} -> {:08X} (thread {}) after {:#x}\n", kSlotNames[hit.slot], hit.before, hit.value, hit.thread,
					hit.rip - base + 0x140000000);
			}
		}
		if (done) {
			armed.store(false, std::memory_order_release);
			for (auto& address : watched)
				address.store(0, std::memory_order_relaxed);
			SetAllFromHelper({});
			text += fmt::format("[DCLF] mirror watch disarmed (arming {}{})\n", armings, count ? "" : ": nothing changed");
		}
		return text;
	}
}
