#include "MirrorWatch.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Engine/SceneCapture.h"

#include <TlHelp32.h>

namespace DCLF::MirrorWatch
{
	namespace
	{
		constexpr std::uint32_t kSlots = 4;
		constexpr std::uint32_t kMaxHits = 16;

		struct Hit
		{
			std::uintptr_t rip = 0;
			std::uint32_t slot = 0;
			std::uint32_t before = 0, value = 0;
			std::uint32_t thread = 0;
		};

		std::array<std::atomic<std::uintptr_t>, kSlots> watched{};
		std::array<std::atomic<std::uint32_t>, kSlots> lastValue{};
		std::array<std::uint32_t, kSlots> ignored{};  // per slot, the bits whose changes are not recorded
		std::array<std::string, kSlots> slotNames;
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
			if (a_info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
				return EXCEPTION_CONTINUE_SEARCH;
			auto* context = a_info->ContextRecord;
			const auto triggered = context->Dr6 & 0xF;
			if (!triggered)
				return EXCEPTION_CONTINUE_SEARCH;
			// A watchpoint the disarm did not reach on this thread (its context was not set, or it raced the disarm): this thread's
			// registers are cleared and it goes on. An unhandled one crashed the game (y57).
			if (!armed.load(std::memory_order_acquire)) {
				context->Dr7 = 0;
				context->Dr6 = 0;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			for (std::uint32_t s = 0; s < kSlots; ++s) {
				if (!(triggered & (1ull << s)))
					continue;
				const auto address = watched[s].load(std::memory_order_relaxed);
				if (!address)
					continue;
				const auto value = *reinterpret_cast<const volatile std::uint32_t*>(address);
				const auto before = lastValue[s].exchange(value, std::memory_order_relaxed);
				if (((value ^ before) & ~ignored[s]) == 0)
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

		std::string Where(std::uintptr_t a_rip)
		{
			const auto base = REL::Module::get().base();
			const auto size = REL::Module::get().segment(REL::Segment::textx).size() + (REL::Module::get().segment(REL::Segment::textx).address() - base);
			if (a_rip >= base && a_rip < base + size)
				return fmt::format("{:#x}", a_rip - base + 0x140000000);
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_rip), &module) ||
				!module)
				return fmt::format("{:#x} (no module)", a_rip);
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			const std::filesystem::path file(path);
			return fmt::format("{}+{:#x}", file.filename().string(), a_rip - reinterpret_cast<std::uintptr_t>(module));
		}

		// The switch's value: "1" (any placed fade node), a node's name (that fade node), or "geometry:<name>" (a geometry by name at
		// its world attach: its alpha and shader property pointers and the property's flags and fade node).
		const std::string& Value() { return SwitchValue(Switch::MirrorWatch); }
		constexpr std::string_view kGeometryPrefix = "geometry:";
		constexpr std::string_view kSkinPrefix = "skin:";
		constexpr std::string_view kNodePrefix = "node:";

		bool Matches(const RE::NiAVObject& a_object, std::string_view a_name)
		{
			// Alternatives separated by '|' (T6b1b).
			if (const auto bar = a_name.find('|'); bar != std::string_view::npos)
				return Matches(a_object, a_name.substr(0, bar)) || Matches(a_object, a_name.substr(bar + 1));
			// A trailing '*' matches a prefix (T6b1b).
			if (!a_name.empty() && a_name.back() == '*')
				return a_object.name.c_str() && std::string_view(a_object.name.c_str()).starts_with(a_name.substr(0, a_name.size() - 1));
			return a_name.empty() || (a_object.name.c_str() && a_name == a_object.name.c_str());
		}

		struct Slot
		{
			const void* address = nullptr;
			const char* name = "";
			std::uint32_t ignore = 0;
		};

		void ArmSlots(std::string a_name, const std::array<Slot, kSlots>& a_slots);

		void ArmSlots(const RE::NiAVObject& a_object, const std::array<Slot, kSlots>& a_slots)
		{
			auto& object = const_cast<RE::NiAVObject&>(a_object);
			ArmSlots(fmt::format("'{}' {} ({})", a_object.name.c_str() ? a_object.name.c_str() : "", static_cast<const void*>(&a_object),
						 object.GetRTTI() && object.GetRTTI()->name ? object.GetRTTI()->name : "?"),
				a_slots);
		}

		void ArmSlots(std::string a_name, const std::array<Slot, kSlots>& a_slots)
		{
			if (armed.load(std::memory_order_acquire) || arming.exchange(true, std::memory_order_acquire))
				return;
			if (!handler)
				handler = AddVectoredExceptionHandler(1, OnException);
			std::array<std::uintptr_t, kSlots> addresses{};
			for (std::uint32_t s = 0; s < kSlots; ++s) {
				addresses[s] = reinterpret_cast<std::uintptr_t>(a_slots[s].address);
				watched[s].store(addresses[s], std::memory_order_relaxed);
				lastValue[s].store(addresses[s] ? *reinterpret_cast<const std::uint32_t*>(addresses[s]) : 0u, std::memory_order_relaxed);
				ignored[s] = a_slots[s].ignore;
				slotNames[s] = a_slots[s].name;
			}
			name = std::move(a_name);
			hitCount.store(0, std::memory_order_relaxed);
			armedFrame = 0;
			++armings;
			armThread = GetCurrentThreadId();
			armed.store(true, std::memory_order_release);
			SetAllFromHelper(addresses);
			arming.store(false, std::memory_order_release);
			logger::info("[DCLF] mirror watch armed (arming {}) on {}, thread {}", armings, name, armThread);
		}
	}

	bool Enabled()
	{
		static const bool enabled = !Value().empty() && Value() != "0";
		return enabled;
	}

	void ArmNode(const RE::NiAVObject* a_node)
	{
		// current:<name> (T6b1a): a fade node at its world attach's capture, its currentFade (+0x130) and screen-door byte (+0x154).
		constexpr std::string_view kCurrentPrefix = "current:";
		if (a_node && Enabled() && !armed.load(std::memory_order_acquire) && Value().starts_with(kCurrentPrefix)) {
			if (const_cast<RE::NiAVObject*>(a_node)->AsFadeNode() && Matches(*a_node, std::string_view(Value()).substr(kCurrentPrefix.size()))) {
				const auto* fade = reinterpret_cast<const std::byte*>(a_node);
				ArmSlots(*a_node, { Slot{ fade + 0x130, "current fade" }, Slot{ fade + 0x154, "screen door" }, Slot{}, Slot{} });
			}
			return;
		}
		// parent:<name> (T6b1b): an object at its world attach's capture, its parent pointer (+0x30, both dwords).
		constexpr std::string_view kParentPrefix = "parent:";
		if (a_node && Enabled() && !armed.load(std::memory_order_acquire) && Value().starts_with(kParentPrefix)) {
			if (Matches(*a_node, std::string_view(Value()).substr(kParentPrefix.size()))) {
				const auto* base = reinterpret_cast<const std::byte*>(a_node);
				ArmSlots(*a_node, { Slot{ base + 0x30, "parent (low)" }, Slot{ base + 0x34, "parent (high)" }, Slot{}, Slot{} });
			}
			return;
		}
		if (!a_node || !Enabled() || armed.load(std::memory_order_acquire) || !Value().starts_with(kNodePrefix))
			return;
		if (!Matches(*a_node, std::string_view(Value()).substr(kNodePrefix.size())))
			return;
		const auto* base = reinterpret_cast<const std::byte*>(a_node);
		// The collision object, its body (bhkNiCollisionObject +0x20), and the Havok entity's motion type (hkpEntity +0x150 motion,
		// its type at +0x10: the dword at +0x160).
		const auto* collision = a_node->collisionObject.get();
		const auto* ni = collision ? const_cast<RE::NiCollisionObject*>(collision)->AsBhkNiCollisionObject() : nullptr;
		const auto* body = ni ? ni->body.get() : nullptr;
		const auto* entity = body ? reinterpret_cast<const std::byte*>(body->referencedObject.get()) : nullptr;
		ArmSlots(*a_node, { Slot{ base + 0x40, "collision object" }, Slot{ ni ? reinterpret_cast<const std::byte*>(ni) + 0x20 : nullptr, "body" },
							  Slot{ entity ? entity + 0x160 : nullptr, "motion type" }, Slot{ base + 0xF4, "flags", 0x0C404000u } });
	}

	void Arm(const RE::NiAVObject* a_node)
	{
		if (!a_node || !Enabled() || armed.load(std::memory_order_acquire) || Value().starts_with(kGeometryPrefix) || Value().starts_with(kSkinPrefix) ||
			Value().starts_with(kNodePrefix))
			return;
		if (!Matches(*a_node, Value() == "1" ? std::string_view() : std::string_view(Value())))
			return;
		const auto* base = reinterpret_cast<const std::byte*>(a_node);
		// +0x109's 0x40 (+0x108's 0x4000) is the cull's, every frame.
		ArmSlots(*a_node, { Slot{ base + 0x108, "+0x108", 0x4000 }, Slot{ base + 0x128, "near" }, Slot{ base + 0x12C, "far" }, Slot{ base + 0x150, "+0x150" } });
	}

	void ArmGeometry(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry || !Enabled() || armed.load(std::memory_order_acquire))
			return;
		// parent:<name> watches geometries too (a particle system is one).
		if (Value().starts_with("parent:")) {
			ArmNode(a_geometry);
			return;
		}
		const auto* base = reinterpret_cast<const std::byte*>(a_geometry);
		if (Value().starts_with(kSkinPrefix)) {
			// A dismember skin's first partitions (editorVisible is each Data's first byte; 4 bytes each).
			if (!Matches(*a_geometry, std::string_view(Value()).substr(kSkinPrefix.size())))
				return;
			const auto* skin = netimmerse_cast<const RE::BSDismemberSkinInstance*>(a_geometry->GetGeometryRuntimeData().skinInstance.get());
			const auto& data = skin ? skin->GetRuntimeData() : RE::BSDismemberSkinInstance::RUNTIME_DATA{};
			if (!skin || !data.partitions || data.numPartitions <= 0)
				return;
			const auto* partitions = reinterpret_cast<const std::byte*>(data.partitions);
			const std::int32_t n = data.numPartitions;
			ArmSlots(*a_geometry, { Slot{ base + 0x130, "skin" }, Slot{ partitions, "partition 0" }, Slot{ n > 1 ? partitions + 4 : nullptr, "partition 1" },
									  Slot{ n > 2 ? partitions + 8 : nullptr, "partition 2" } });
			return;
		}
		// projected:<name>, land:<name> (T6b1a): the property's projected UV parameters, or its landscape material's land blend, from the
		// world attach's capture on.
		constexpr std::string_view kProjectedPrefix = "projected:";
		constexpr std::string_view kLandPrefix = "land:";
		const bool projected = Value().starts_with(kProjectedPrefix);
		if (projected || Value().starts_with(kLandPrefix)) {
			if (!Matches(*a_geometry, std::string_view(Value()).substr(projected ? kProjectedPrefix.size() : kLandPrefix.size())))
				return;
			const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get());
			if (!lighting)
				return;
			if (projected) {
				ArmSlots(*a_geometry, { Slot{ &lighting->projectedUVParams.red, "params.r" }, Slot{ &lighting->projectedUVParams.green, "params.g" },
										  Slot{ &lighting->projectedUVParams.alpha, "params.a" }, Slot{ &lighting->projectedUVColor.red, "colour.r" } });
			} else if (const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(lighting->material)) {
				const auto feature = const_cast<RE::BSLightingShaderMaterialLandscape*>(material)->GetFeature();
				if (feature != RE::BSShaderMaterial::Feature::kMultiTexLand && feature != RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend)
					return;
				ArmSlots(*a_geometry, { Slot{ &material->landBlendParams.red, "land blend.r" }, Slot{ &material->landBlendParams.green, "land blend.g" },
										  Slot{ &material->landBlendParams.blue, "land blend.b" }, Slot{ &material->landBlendParams.alpha, "land blend.a" } });
			}
			return;
		}
		if (!Value().starts_with(kGeometryPrefix) || !Matches(*a_geometry, std::string_view(Value()).substr(kGeometryPrefix.size())))
			return;
		const auto* property = reinterpret_cast<const std::byte*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get());
		ArmSlots(*a_geometry, { Slot{ base + 0x120, "alpha" }, Slot{ base + 0x128, "property" }, Slot{ property ? property + 0x3C : nullptr, "property flags (high: 32-63)" },
								  Slot{ property ? property + 0x60 : nullptr, "property fade node" } });
	}

	std::uint32_t PropertyFields()
	{
		using F = SceneCapture::PropertyRecord::Field;
		if (!Enabled())
			return 0;
		const auto& value = Value();
		return value == "parity" ? F::kFlags : value == "parity:alpha" ? F::kAlphaValue : value == "parity:projected" ? F::kProjected : value == "parity:land" ? F::kLandBlend : 0u;
	}

	void ArmProperty(const void* a_property)
	{
		using F = SceneCapture::PropertyRecord::Field;
		const std::uint32_t fields = PropertyFields();
		if (!a_property || !fields || armed.load(std::memory_order_acquire))
			return;
		const auto* base = static_cast<const std::byte*>(a_property);
		const auto& property = *static_cast<const RE::BSShaderProperty*>(a_property);
		if (fields == F::kAlphaValue) {
			ArmSlots(fmt::format("property {} alpha", a_property), { Slot{ &property.alpha, "alpha" }, Slot{}, Slot{}, Slot{} });
		} else if (fields == F::kProjected) {
			const auto& params = static_cast<const RE::BSLightingShaderProperty&>(property).projectedUVParams;
			const auto& colour = static_cast<const RE::BSLightingShaderProperty&>(property).projectedUVColor;
			ArmSlots(fmt::format("property {} projected UV", a_property),
				{ Slot{ &params.red, "params.r" }, Slot{ &params.blue, "params.b" }, Slot{ &params.alpha, "params.a" }, Slot{ &colour.red, "colour.r" } });
		} else if (fields == F::kLandBlend) {
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property.material);
			if (!material)
				return;
			const auto& blend = material->landBlendParams;
			ArmSlots(fmt::format("property {} material {} land blend", a_property, static_cast<const void*>(material)),
				{ Slot{ &blend.red, "land blend.r" }, Slot{ &blend.green, "land blend.g" }, Slot{ &blend.blue, "land blend.b" }, Slot{ &blend.alpha, "land blend.a" } });
		} else {
			ArmSlots(fmt::format("property {}", a_property),
				{ Slot{ base + 0x38, "flags (low)" }, Slot{ base + 0x3C, "flags (high)" }, Slot{ base + 0x60, "fade node" }, Slot{ base + 0x78, "material" } });
		}
	}

	bool CurrentFromParity()
	{
		return Enabled() && Value() == "parity:current";
	}

	void ArmCurrent(const void* a_node)
	{
		if (!a_node || !CurrentFromParity() || armed.load(std::memory_order_acquire))
			return;
		const auto* fade = static_cast<const std::byte*>(a_node);
		ArmSlots(*static_cast<const RE::NiAVObject*>(a_node), { Slot{ fade + 0x130, "current fade" }, Slot{ fade + 0x154, "screen door" }, Slot{}, Slot{} });
	}

	void ArmAlpha(const void* a_alpha)
	{
		if (!a_alpha || !Enabled() || !Value().starts_with("parity") || armed.load(std::memory_order_acquire))
			return;
		const auto* base = static_cast<const std::byte*>(a_alpha);
		// The threshold (+0x32) has its writer's event (FUN_1414ab770): the flags only.
		ArmSlots(fmt::format("alpha {}", a_alpha), { Slot{ base + 0x30, "flags", 0xFFFF0000u }, Slot{}, Slot{}, Slot{} });
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
			text = fmt::format("[DCLF] mirror watch (arming {} on {}, armed on thread {}): {} changes recorded\n", armings, name, armThread,
				hitCount.load(std::memory_order_relaxed));
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& hit = hits[i];
				text += fmt::format("[DCLF]   {} {:08X} -> {:08X} (thread {}) after {}\n", slotNames[hit.slot], hit.before, hit.value, hit.thread, Where(hit.rip));
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
