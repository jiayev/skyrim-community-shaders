// RoomVisibility: the detour of Main::IsRoomVisible's three calls (job threads), answered from PortalViews' VisibleSet while DCLF runs.
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include <array>
#include <atomic>

namespace DCLF::RoomVisibility
{
	namespace
	{
		using Set = PortalViews::VisibleSet;

		constexpr Engine::Offset kIsRoomVisible{ { 36618 }, { 35608 } };  // Main::IsRoomVisible(main, node)
		// Its calls (Ghidra, AE 1.6.1170 and SE 1.5.97; the other references are its unwind entries): the managed-nodes update
		// FUN_1402b41a0, Actor::UpdateRefLight and the actor update FUN_140771520, each with the node at +0x30 of the reference's 3D.
		constexpr std::array<Engine::Offset, 3> kCallSites{ { { { 18894, 0xc8 }, { 18463, 0xc8 } }, { { 19677, 0x22 }, { 19251, 0x22 } }, { { 41381, 0x167 }, { 40367, 0xe4 } } } };

		bool installed = false;
		// Whether DCLF runs this frame (Main::Update's entry, the main thread; read by the job threads' calls during the frame).
		std::atomic<bool> live{ false };
		std::atomic<std::uint64_t> answered{ 0 }, engineAnswered{ 0 };

		/** @brief The three calls of Main::IsRoomVisible: DCLF's answer while it runs, else the engine's. */
		struct IsRoomVisible
		{
			static bool thunk(void* a_main, RE::NiNode* a_node)
			{
				const PortalViews::VisibleSetHold hold;
				if (hold && live.load(std::memory_order_acquire)) {
					answered.fetch_add(1, std::memory_order_relaxed);
					return hold->Visible(a_node).visible;
				}
				engineAnswered.fetch_add(1, std::memory_order_relaxed);
				return func(a_main, a_node);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		for (const auto site : kCallSites)
			if (!Engine::CallsTo(base + site, base + kIsRoomVisible)) {
				logger::warn("[DCLF] room visibility (T7b3) not installed: the instruction at {:#x} is not a call of Main::IsRoomVisible", 0x140000000 + site.Get());
				return;
			}
		PortalViews::Get().PublishVisibleSets();
		for (const auto site : kCallSites)
			stl::write_thunk_call<IsRoomVisible>(base + site);
		installed = true;
		logger::info("[DCLF] room visibility (T7f checklist item 3): Main::IsRoomVisible's calls answered from PortalViews' VisibleSet while DCLF runs");
	}

	bool Installed()
	{
		return installed;
	}

	void SetLive(bool a_live)
	{
		live.store(a_live, std::memory_order_release);
	}

	bool DclfAnswer(const void* a_key)
	{
		const PortalViews::VisibleSetHold hold;
		return !hold || hold->Visible(a_key).visible;
	}

	Answers Take()
	{
		return { answered.exchange(0, std::memory_order_relaxed), engineAnswered.exchange(0, std::memory_order_relaxed) };
	}
}
