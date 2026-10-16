// The first-person view's engine hooks: the accumulator render (FUN_1414a90f0) at RenderDepth's first-person draw and at
// Main::RenderFirstPersonView's (FUN_1414cb6d0), where its two epochs run. Installed from the feature's install; they read the shell's
// state (ShellState: whether DCLF runs, RenderDepth's control-driven argument).

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"

#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// AE 1.6.1170 (module offsets) of the accumulator render's calls the view's hooks take, each checked with CallsTo before it is patched.
		namespace Address
		{
			inline constexpr std::uintptr_t kDepthFirstPersonDraw = 0x14cd47e;   // its call at RenderDepth's first-person draw
			inline constexpr std::uintptr_t kFirstPersonViewDraw = 0x14cb7d1;    // its call in FUN_1414cb6d0 (the first-person view)
		}

		/**
		 * @brief RenderDepth's first-person draw (AE 0x1414cd47e, RenderDepth +0x94E: FUN_1414a90f0(first-person camera, the Z-prepass's
		 * accumulator, 3), after the root's cull with the first-person camera and before kPOST_ZPREPASS_COPY): DCLF's first-person depth
		 * epoch first (ExecuteDepth), then the engine's draw of what it still registers.
		 */
		struct RenderDepth_FirstPersonDraw
		{
			static void thunk(void* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				if (ShellState::Running()) {
					RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ZPrepass);
					// The first-person subtree's hidden bits as the engine's first-person culls (unk_FPCulling, this block's) saw them: the next
					// frame's members' chains read these (FrameGlobals::NoteFirstPersonCulled).
					if (Extensions::ViewOn(ActiveToggles(), kView))
						if (auto* player = RE::PlayerCharacter::GetSingleton())
							FrameGlobals::NoteFirstPersonCulled(player->Get3D(true));
					ExecuteDepth(ShellState::Passes().depthControlDriven);
					// DCLF's epoch leaves the context's bindings to the engine's state tracking.
					globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				}
				func(a_camera, a_accumulator, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::RenderFirstPersonView's accumulator render (AE 0x1414cb7d1, FUN_1414cb6d0 +0x101: FUN_1414a90f0(first-person
		 * camera, the first-person accumulator, firstPerson << 4 | 0x43), its targets and depth range set): DCLF's first-person colour epoch
		 * first (ExecuteColour), then the engine's residue (effects, particles, anything not DCLF's).
		 */
		struct RenderFirstPersonView_Draw
		{
			static void thunk(void* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				Diagnostics::Notify(Diagnostics::TargetPoint{ "before first person" });
				if (ShellState::Running()) {
					RenderThreadBudget::Hook budget(RenderThreadBudget::Site::AfterOpaque);
					ExecuteColour();
					globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				}
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after DCLF first person" });
				func(a_camera, a_accumulator, a_flags);
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after first person" });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		// The first-person view's two points (FUN_1414a90f0, the accumulator render, at RenderDepth's first-person draw and at
		// FUN_1414cb6d0's), always installed: one compare a frame each while CS_DCLF_FIRST_PERSON is off.
		if (Engine::CallsTo(REL::Offset(Address::kDepthFirstPersonDraw).address(), REL::Offset(Engine::Address::kAccumulatorRender).address()) &&
			Engine::CallsTo(REL::Offset(Address::kFirstPersonViewDraw).address(), REL::Offset(Engine::Address::kAccumulatorRender).address())) {
			stl::write_thunk_call<RenderDepth_FirstPersonDraw>(REL::Offset(Address::kDepthFirstPersonDraw).address());
			stl::write_thunk_call<RenderFirstPersonView_Draw>(REL::Offset(Address::kFirstPersonViewDraw).address());
		} else {
			logger::warn("[DCLF] the first-person draws are not where expected; the first-person view (T7e) never runs, and CS_DCLF_FIRST_PERSON must stay off");
		}
	}
}
