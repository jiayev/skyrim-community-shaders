// TreeList: the tree manager's list loop detoured (main thread, Main::Update) for TreeAnimation's stood-in trees. TreeList.h describes it.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeList.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeAnimation.h"

namespace DCLF::Features::TreeWind::TreeList
{
	namespace
	{
		// Engine::Offsets (Address Library ids; Ghidra). SE's update calls the loop without AE's test of the process's camera (+0x18);
		// the thunk reads neither, and the loop dereferences it on both.
		constexpr Engine::Offset kListLoop{ { 27094 }, { 26498 } };      // FUN_140437e50(manager, process): the tree list's loop
		constexpr Engine::Offset kListLoopCall{ { 27093, 0xb1 }, { 26497, 0xb6 } };  // its call in FUN_140437d40 (the manager's update, Main::Update 0x140646176)

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
		// Chained onto whatever the call holds (Preflight required a CALL).
		const auto base = REL::Module::get().base();
		if (!Engine::CallsTo(base + kListLoopCall, base + kListLoop))
			logger::info("[DCLF] tree manager: the list loop's call is hooked by {}; DCLF's thunk chains onto it", HookSites::BranchTargetModule(base + kListLoopCall));
		stl::write_thunk_call<ListLoop>(base + kListLoopCall);
		installed = true;
		logger::info("[DCLF] tree manager: the list loop's call detoured (stood-in trees' multibound results from DCLF's verdicts)");
	}

	bool Installed() { return installed; }

	void Preflight() { HookSites::RequireCall("the tree manager's list loop call", REL::Module::get().base() + kListLoopCall); }
}
