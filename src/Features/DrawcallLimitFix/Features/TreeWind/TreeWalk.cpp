// TreeWalk: the tree manager's walk, its trees' update pass followed by a move event (main thread, Main::Update).
// TreeWalk.h describes it.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWalk.h"

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"

#include <array>
#include <cstring>

namespace DCLF::Features::TreeWind::TreeWalk
{
	namespace
	{
		// `call [rax + 0x168]`: SE's in its walk (FUN_1403df7d0); AE's (1.6 and 1.7 alike) in the manager's update FUN_1404381e0, the near
		// list's loop, right after the bones' wind (FUN_140438ed0).
		constexpr Engine::Offset kWalkUpdateCall{ { 27095, 0x315 }, { 26517, 0x10e } };
		constexpr std::array<std::uint8_t, 6> kWalkUpdateBytes{ 0xFF, 0x90, 0x68, 0x01, 0x00, 0x00 };
		constexpr std::size_t kUpdateSelectedSlot = 0x2D;  // NiAVObject::UpdateSelectedDownwardPass (vtable +0x168)

		bool installed = false;
		Api::MoveWriterId moveWriter;

		// The site's virtual call, made here (the tree in rcx, the update data and the flags as the walk passed them), then the event.
		void Walked(RE::NiAVObject* a_tree, RE::NiUpdateData* a_data, std::uint32_t a_flags)
		{
			using Pass = void (*)(RE::NiAVObject*, RE::NiUpdateData*, std::uint32_t);
			reinterpret_cast<Pass>((*reinterpret_cast<void***>(a_tree))[kUpdateSelectedSlot])(a_tree, a_data, a_flags);
			Extensions::PushReservedMove(moveWriter, a_tree->GetUserData(), Api::MoveReach::WithPlacements);
		}
	}

	void Install(Api::InstallContext& a_context)
	{
		if (installed || !kWalkUpdateCall.Known())
			return;
		const auto site = REL::Module::get().base() + kWalkUpdateCall;
		if (std::memcmp(reinterpret_cast<const void*>(site), kWalkUpdateBytes.data(), kWalkUpdateBytes.size()) != 0) {
			logger::warn("[DCLF] tree manager walk not installed: the instruction at {:#x} is not its trees' update pass call; a walked tree's sun entry "
						 "keeps the bound it was listed with",
				0x140000000 + kWalkUpdateCall.Get());
			return;
		}
		// The report's "tree animation" (the slot after Features/Particles'), before the call goes in.
		moveWriter = a_context.ReserveMoveWriter("tree animation");
		SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&Walked));
		REL::safe_write(site + 5, std::uint8_t{ 0x90 });
		Extensions::MoveWriterInstalled(moveWriter);
		installed = true;
		logger::info("[DCLF] tree manager walk: its trees' update pass followed by a move event");
	}

	bool Installed() { return installed; }
}
