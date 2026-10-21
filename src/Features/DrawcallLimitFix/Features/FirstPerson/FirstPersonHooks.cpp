// The first-person view's engine hooks: the accumulator render (FUN_1414a90f0) at RenderDepth's first-person draw, where the frame's
// first-person culled bits are taken (the depth epoch is committed earlier, into the depth commit: the depth epoch hosts its passes), and at
// Main::RenderFirstPersonView's (FUN_1414cb6d0), where the colour epoch runs. Installed from the feature's install; they read the shell's
// state (ShellState: whether DCLF runs, RenderDepth's control-driven argument).

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
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
			inline constexpr std::uintptr_t kAccumulatorRender = 0x14a90f0;      // the accumulator render (FUN_1414a90f0)
		}

		// The colour epoch's trigger (fp.colour-render), found at install, before any frame.
		Api::TriggerId colourRender;

		/**
		 * @brief RenderDepth's first-person draw (AE 0x1414cd47e, RenderDepth +0x94E: FUN_1414a90f0(first-person camera, the Z-prepass's
		 * accumulator, 3), after the root's cull with the first-person camera and before kPOST_ZPREPASS_COPY): the frame's first-person
		 * culled bits and depth range, then the engine's draw of what it still registers. DCLF's first-person depth was drawn by the depth epoch already
		 * (ExecuteDepth, at the depth commit), into the same depth: nothing of the world draws depth between them.
		 */
		struct RenderDepth_FirstPersonDraw
		{
			static void thunk(void* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				if (ShellState::Running()) {
					RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ZPrepass);
					// The first-person subtree's hidden bits as the engine's first-person culls (unk_FPCulling, this block's) saw them: the next
					// frame's members' chains read these (FrameGlobals::NoteFirstPersonCulled).
					if (Extensions::ViewOn(SceneToggles(), kView))
						if (auto* player = RE::PlayerCharacter::GetSingleton())
							FrameGlobals::NoteFirstPersonCulled(player->Get3D(true));
					// The depth range this draw uses (RenderDepth's second argument), the shapes' viewport from the next revision on.
					TakeDepthRange(ShellState::Passes().depthControlDriven);
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
					{
						Extensions::TriggerScope trigger(colourRender);
						ExecuteColour();
					}
					globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				}
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after DCLF first person" });
				func(a_camera, a_accumulator, a_flags);
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after first person" });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks(Api::InstallContext& a_context)
	{
		colourRender = a_context.FindTrigger("fp.colour-render");
		// The first-person view's two points (FUN_1414a90f0, the accumulator render, at RenderDepth's first-person draw and at
		// FUN_1414cb6d0's), always installed: one compare a frame each while CS_DCLF_FIRST_PERSON is off.
		if (Engine::CallsTo(REL::Offset(Address::kDepthFirstPersonDraw).address(), REL::Offset(Address::kAccumulatorRender).address()) &&
			Engine::CallsTo(REL::Offset(Address::kFirstPersonViewDraw).address(), REL::Offset(Address::kAccumulatorRender).address())) {
			stl::write_thunk_call<RenderDepth_FirstPersonDraw>(REL::Offset(Address::kDepthFirstPersonDraw).address());
			stl::write_thunk_call<RenderFirstPersonView_Draw>(REL::Offset(Address::kFirstPersonViewDraw).address());
		} else {
			logger::warn("[DCLF] the first-person draws are not where expected; the first-person view (T7e) never runs, and CS_DCLF_FIRST_PERSON must stay off");
		}
	}
}
