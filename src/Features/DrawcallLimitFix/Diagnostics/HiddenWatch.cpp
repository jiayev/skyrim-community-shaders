#include "HiddenWatch.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <TlHelp32.h>

namespace DCLF::Scene
{
	std::uintptr_t HiddenStoreSiteOf(std::uintptr_t a_address);
}

namespace DCLF::HiddenWatch
{
	namespace
	{
		constexpr std::uint32_t kSlots = 4;
		constexpr std::uint32_t kMaxHits = 16;
		constexpr std::uintptr_t kFlagsOffset = 0xF4;

		struct Hit
		{
			std::uintptr_t rip = 0;
			std::uint32_t slot = 0;
			std::uint32_t value = 0;
			std::uint32_t thread = 0;
			std::int64_t ticks = 0;
		};

		std::array<std::atomic<std::uintptr_t>, kSlots> watched{};
		std::array<std::atomic<std::uint32_t>, kSlots> lastValue{};
		std::array<Hit, kMaxHits> hits{};
		std::atomic<std::uint32_t> hitCount{ 0 };
		std::atomic<bool> armed{ false };
		std::array<std::string, kSlots> names;
		std::uint32_t armedFrame = 0;
		std::uint32_t armings = 0;
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
				if (!((value ^ before) & 1u))
					continue;
				const auto at = hitCount.fetch_add(1, std::memory_order_relaxed);
				if (at < kMaxHits)
				{
					LARGE_INTEGER now{};
					QueryPerformanceCounter(&now);
					hits[at] = { static_cast<std::uintptr_t>(context->Rip), s, value, GetCurrentThreadId(), now.QuadPart };
				}
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
		static const bool enabled = SwitchEnabled(Switch::HiddenWatch);
		return enabled;
	}

	void Arm(const std::array<const RE::NiAVObject*, 4>& a_nodes)
	{
		if (!Enabled() || armed.load(std::memory_order_acquire))
			return;
		if (!handler)
			handler = AddVectoredExceptionHandler(1, OnException);
		std::array<std::uintptr_t, kSlots> addresses{};
		for (std::uint32_t s = 0; s < kSlots; ++s) {
			const auto* node = a_nodes[s];
			addresses[s] = node ? reinterpret_cast<std::uintptr_t>(node) + kFlagsOffset : 0;
			names[s] = node ? fmt::format("'{}' ({})", node->name.c_str() ? node->name.c_str() : "", node->GetRTTI() && node->GetRTTI()->name ? node->GetRTTI()->name : "?") : std::string();
			watched[s].store(addresses[s], std::memory_order_relaxed);
			lastValue[s].store(addresses[s] ? *reinterpret_cast<const std::uint32_t*>(addresses[s]) : 0u, std::memory_order_relaxed);
		}
		hitCount.store(0, std::memory_order_relaxed);
		armed.store(true, std::memory_order_release);
		armedFrame = 0;
		++armings;
		SetAllFromHelper(addresses);
		logger::info("[DCLF] hidden watch armed on {} {} {} {}", names[0], names[1], names[2], names[3]);
	}

	std::string TakeReport(std::uint32_t a_frame)
	{
		if (!armed.load(std::memory_order_acquire))
			return {};
		if (!armedFrame)
			armedFrame = a_frame;
		const std::uint32_t count = std::min(hitCount.load(std::memory_order_relaxed), kMaxHits);
		const bool done = count >= kMaxHits || a_frame - armedFrame >= 3000;
		std::string text;
		if (count) {
			const auto base = REL::Module::get().base();
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			text = fmt::format("[DCLF] hidden watch (arming {}): {} kHidden flips recorded", armings, hitCount.load(std::memory_order_relaxed));
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& hit = hits[i];
				const auto site = Scene::HiddenStoreSiteOf(hit.rip);
				const double sinceMs = i ? 1000.0 * double(hit.ticks - hits[i - 1].ticks) / double(frequency.QuadPart) : 0.0;
				text += fmt::format("\n[DCLF]   {} on {} -> {:08X} (thread {}, {:.3f} ms after the last), {}", (hit.value & 1) ? "hidden" : "shown", names[hit.slot], hit.value,
					hit.thread, sinceMs, site ? fmt::format("by the patched store at {:#x} (announced)", site) : fmt::format("after {:#x} (no patched store)", hit.rip - base + 0x140000000));
			}
		}
		if (done) {
			armed.store(false, std::memory_order_release);
			for (auto& address : watched)
				address.store(0, std::memory_order_relaxed);
			SetAllFromHelper({});
			text += text.empty() ? "[DCLF] hidden watch disarmed: nothing flipped" : "\n[DCLF] hidden watch disarmed";
		}
		return text;
	}
}
