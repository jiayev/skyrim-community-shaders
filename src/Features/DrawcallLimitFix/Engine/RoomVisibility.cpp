#include "RoomVisibility.h"

#include "EngineAccess.h"
#include "PortalViews.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>

namespace DCLF::RoomVisibility
{
	namespace
	{
		using Set = PortalViews::VisibleSet;

		constexpr std::uintptr_t kIsRoomVisible = 0x649f20;  // Main::IsRoomVisible(main, node)
		// Its calls (Ghidra, AE 1.6.1170; the other references are its unwind entries): the managed-nodes update FUN_1402b41a0,
		// Actor::UpdateRefLight and the actor update FUN_140771520, each with the node at +0x30 of the reference's 3D.
		constexpr std::array<std::uintptr_t, 3> kCallSites{ 0x2b4268, 0x2e1282, 0x771687 };

		bool installed = false;

		// The parity's counters, by kind (room, shared node, other key) and outcome; any thread.
		enum Outcome : std::uint32_t
		{
			kCompared,
			kDclfOnly,    // DCLF visible, the engine not
			kEngineOnly,  // the engine visible, DCLF not
			kOutcomes
		};
		constexpr std::array<const char*, 4> kKindNames{ "no key", "room", "shared node", "other key" };
		struct Stats
		{
			std::atomic<std::uint64_t> calls{ 0 }, noSet{ 0 }, raced{ 0 }, matchPrevious{ 0 };
			std::array<std::array<std::atomic<std::uint64_t>, kOutcomes>, 4> byKind{};
		};
		Stats stats;

		/** @brief The first difference since the report: claimed by one thread (key), written, then marked ready for the report. */
		struct First
		{
			std::atomic<const void*> key{ nullptr };
			std::atomic<bool> ready{ false };
			char name[64]{};
			Set::Kind kind = Set::kNoKey;
			bool engine = false;
			bool previous = false;
			std::uint32_t sceneFrame = 0;
		};
		First first;

		void NoteFirst(const RE::NiNode* a_node, const Set& a_set, Set::Kind a_kind, bool a_engine, bool a_previous)
		{
			const void* expected = nullptr;
			if (!first.key.compare_exchange_strong(expected, a_node, std::memory_order_acq_rel))
				return;
			// The node's name: the caller's node, read on its thread as the engine's own function just read it.
			const char* name = a_node->name.c_str();
			strncpy_s(first.name, name ? name : "", _TRUNCATE);
			first.kind = a_kind;
			first.engine = a_engine;
			first.previous = a_previous;
			first.sceneFrame = a_set.sceneFrame;
			first.ready.store(true, std::memory_order_release);
		}

		void Observe(const PortalViews::VisibleSetHold& a_hold, const RE::NiNode* a_node, bool a_engine)
		{
			stats.calls.fetch_add(1, std::memory_order_relaxed);
			if (!a_hold) {
				stats.noSet.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			// A publish during the call: the engine may have answered from either walk's entry.
			if (a_hold->generation != PortalViews::Get().VisibleGeneration()) {
				stats.raced.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			const auto answer = a_hold->Visible(a_node);
			auto& counts = stats.byKind[answer.kind];
			counts[kCompared].fetch_add(1, std::memory_order_relaxed);
			if (answer.visible == a_engine)
				return;
			counts[answer.visible ? kDclfOnly : kEngineOnly].fetch_add(1, std::memory_order_relaxed);
			const bool previous = a_hold->Visible(a_node, true).visible == a_engine;
			if (previous)
				stats.matchPrevious.fetch_add(1, std::memory_order_relaxed);
			NoteFirst(a_node, *a_hold.get(), answer.kind, a_engine, previous);
		}

		/** @brief The three calls of Main::IsRoomVisible: the engine's answer, observed. */
		struct IsRoomVisible
		{
			static bool thunk(void* a_main, RE::NiNode* a_node)
			{
				const PortalViews::VisibleSetHold hold;
				const bool engine = func(a_main, a_node);
				Observe(hold, a_node, engine);
				return engine;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (installed || !SwitchEnabled(Switch::RoomParity))
			return;
		const auto base = REL::Module::get().base();
		for (const auto site : kCallSites)
			if (!Engine::CallsTo(base + site, base + kIsRoomVisible)) {
				logger::warn("[DCLF] room visibility (T7b3) not installed: the instruction at {:#x} is not a call of Main::IsRoomVisible", 0x140000000 + site);
				return;
			}
		PortalViews::Get().PublishVisibleSets();
		for (const auto site : kCallSites)
			stl::write_thunk_call<IsRoomVisible>(base + site);
		installed = true;
		logger::info("[DCLF] room visibility (T7b3, CS_DCLF_ROOM_PARITY): Main::IsRoomVisible's answers compared with PortalViews' VisibleSet (the engine's returned)");
	}

	bool Installed()
	{
		return installed;
	}

	bool DclfAnswer(const void* a_key)
	{
		const PortalViews::VisibleSetHold hold;
		return !hold || hold->Visible(a_key).visible;
	}

	void Report()
	{
		if (!installed)
			return;
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		std::array<std::array<std::uint64_t, kOutcomes>, 4> by{};
		for (std::size_t k = 0; k < by.size(); ++k)
			for (std::size_t o = 0; o < kOutcomes; ++o)
				by[k][o] = take(stats.byKind[k][o]);
		const std::uint64_t calls = take(stats.calls), noSet = take(stats.noSet), raced = take(stats.raced), matchPrevious = take(stats.matchPrevious);
		std::uint64_t differ = 0;
		for (const auto& kind : by)
			differ += kind[kDclfOnly] + kind[kEngineOnly];
		std::string firstText;
		if (first.ready.load(std::memory_order_acquire)) {
			const void* key = first.key.load(std::memory_order_relaxed);
			firstText = std::format("; first: {} {:p} \"{}\" (scene frame {}), the engine {}, DCLF {}{}", kKindNames[first.kind], key, first.name, first.sceneFrame,
				first.engine ? "visible" : "not visible", first.engine ? "not" : "visible", first.previous ? ", as the previous walk's" : "");
			first.ready.store(false, std::memory_order_relaxed);
			first.key.store(nullptr, std::memory_order_release);
		}
		logger::info("[DCLF] room visibility (T7b3: Main::IsRoomVisible, the engine's answer against PortalViews' VisibleSet): {} calls ({} before any set, {} across a publish: "
					 "not compared); rooms {} ({} DCLF only, {} engine only), shared nodes {} ({} DCLF only, {} engine only), other keys {} ({} engine only: not in DCLF's "
					 "graph), no key {}; {} differences, {} of them as the previous walk's{}{}",
			calls, noSet, raced, by[Set::kRoom][kCompared], by[Set::kRoom][kDclfOnly], by[Set::kRoom][kEngineOnly], by[Set::kShared][kCompared], by[Set::kShared][kDclfOnly],
			by[Set::kShared][kEngineOnly], by[Set::kOther][kCompared], by[Set::kOther][kEngineOnly], by[Set::kNoKey][kCompared], differ, matchPrevious,
			differ ? " <- ROOM" : " <- OK", firstText);
	}
}
