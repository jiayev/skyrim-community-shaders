// TreeList: the tree manager's list loop detoured (main thread, Main::Update) for TreeAnimation's stood-in trees. TreeList.h describes it.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeList.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeAnimation.h"

namespace DCLF::Features::TreeWind::TreeList
{
	namespace
	{
		// AE 1.6.1170 and SE 1.5.97 (module offsets; Ghidra). SE's update calls the loop without AE's test of the process's camera (+0x18);
		// the thunk reads neither, and the loop dereferences it on both.
		constexpr Engine::Offset kListLoop{ 0x437e50, 0x3ddd40 };      // FUN_140437e50(manager, process): the tree list's loop
		constexpr Engine::Offset kListLoopCall{ 0x437df1, 0x3ddce6 };  // its call in FUN_140437d40 (the manager's update, Main::Update 0x140646176)

		bool installed = false;

		struct ListLoop
		{
			static void thunk(std::byte* a_manager, std::byte* a_process)
			{
				// The stood-in trees' multibound results DCLF's for the loop, the engine's put back after it.
				TreeAnimation::BeforeListLoop(a_manager);
				func(a_manager, a_process);
				TreeAnimation::AfterListLoop();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		if (!Engine::CallsTo(base + kListLoopCall, base + kListLoop)) {
			logger::warn("[DCLF] tree manager (T7b5) not installed: the instruction at {:#x} is not a call of FUN_140437e50; stood-in trees keep the stale multibound result", 0x140000000 + kListLoopCall.Get());
			return;
		}
		stl::write_thunk_call<ListLoop>(base + kListLoopCall);
		installed = true;
		logger::info("[DCLF] tree manager: the list loop's call detoured (stood-in trees' multibound results from DCLF's verdicts)");
	}

	bool Installed() { return installed; }
}
