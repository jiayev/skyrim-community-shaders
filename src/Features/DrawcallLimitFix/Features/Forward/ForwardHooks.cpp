// The forward epoch's engine hooks: the main pass's blended draw (FUN_1414b39d0) at both its call sites in FUN_1414b3390, where the
// epoch runs and the refraction normals it drew are marked for the engine, and Main::Draw's refraction point (FUN_1414b4840's call).
// Installed from the feature's install; they read the shell's state (ShellState: whether DCLF runs).

#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <cstdint>

namespace DCLF::Features::Forward
{
	namespace
	{
		// AE 1.6.1170 and SE 1.5.97 (module offsets) of the engine code and state the epoch's hooks take and mark; the hook sites are each
		// checked with CallsTo before they are patched.
		namespace Address
		{
			inline constexpr Engine::Offset kBlendedDraw{ 0x14b39d0, 0x12cb930 };             // the main pass's blended draw (FUN_1414b39d0)
			inline constexpr Engine::Offset kBlendedUnderWater{ 0x14b33e1, 0x12cb334 };       // its call sites in FUN_1414b3390 (SE FUN_1412cb2e0)
			inline constexpr Engine::Offset kBlendedAfterWater{ 0x14b38d3, 0x12cb831 };
			inline constexpr Engine::Offset kRefractionCheck{ 0x644d20, 0x5b20c0 };           // Main::Draw's refraction point
			inline constexpr Engine::Offset kGroupFiveCount{ 0x14b4840, 0x12cc710 };          // what it calls: the accumulator's group 5 count (FUN_1414b4840)
			inline constexpr Engine::Offset kRefractionNormalsBegun{ 0x35ef130, 0x3495d20 };  // byte: the refraction normals were cleared this frame
			inline constexpr Engine::Offset kRefractionNormalsDrawn{ 0x35ef131, 0x3495d21 };  // byte: drawn (ISRefraction's IsActive reads it)
			inline constexpr Engine::Offset kClearGroup{ 0x14b4ae0, 0x12cca10 };              // FUN_1414b4ae0(accumulator, group): a group's pass list cleared, undrawn
		}

		// The forward epoch's trigger (blended.draw), found at install, before any frame.
		Api::TriggerId blendedDraw;

		/**
		 * @brief Refraction: the forward epoch cleared and drew the refraction normals, ahead of Main::Draw's own group 5 (FUN_1414b4860).
		 * Marked begun and drawn, as that function marks them (0x1435ef130, 0x1435ef131): the engine's later draws into them then add to
		 * DCLF's instead of clearing them, and ISRefraction (its IsActive reads 0x1435ef131, 0x1414f95a0) warps the frame by them. Its
		 * render clears both. Main::Draw's group 6 is cleared for it at the refraction point (Main_Draw_RefractionCheck), where the engine
		 * would never have drawn it.
		 */
		void MarkRefractionNormals()
		{
			static REL::Relocation<std::uint8_t*> normalsBegun{ REL::Offset(Address::kRefractionNormalsBegun) };
			static REL::Relocation<std::uint8_t*> normalsDrawn{ REL::Offset(Address::kRefractionNormalsDrawn) };
			*normalsBegun = 1;
			*normalsDrawn = 1;
		}

		/**
		 * @brief The main pass's blended draw (AE FUN_1414b39d0, BSShaderAccumulator's sorted blended list interleaved with the
		 * geometry groups' stack, after the deferred composite), at both its call sites in FinishAccumulatingPostResolveDepth's render
		 * mode 0 (FUN_1414b3390: +0x51, first when the camera is under water; +0x543, after the water otherwise). For the main
		 * camera's accumulator the forward epoch draws DCLF's blended objects first (SubmitForwardEpoch).
		 */
		template <int N>
		struct Accumulator_BlendedDraw
		{
			static void thunk(void* a_accumulator, std::uint32_t a_renderFlags)
			{
				// DCLF's blended objects first, behind every blended draw of the engine's (the ordering contract: dclf-architecture.md, "The
				// forward epoch"), for the main camera's accumulator alone (the same function draws every render mode 0 accumulator's).
				static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
				const bool main = a_accumulator && a_accumulator == *mainAccumulator;
				if (main)
					Diagnostics::Notify(Diagnostics::TargetPoint{ "before forward" });
				if (main && ShellState::Running()) {
					Extensions::TriggerScope trigger(blendedDraw);
					if (SubmitForwardEpoch())
						MarkRefractionNormals();
				}
				if (main)
					Diagnostics::Notify(Diagnostics::TargetPoint{ "after DCLF forward" });
				func(a_accumulator, a_renderFlags);
				if (main)
					Diagnostics::Notify(Diagnostics::TargetPoint{ "after engine blended" });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Draw's refraction point (AE 0x140644d20, its call of FUN_1414b4840: the main accumulator's
		 * geometry group 5 count, which decides whether FUN_1414ce1d0 draws the refraction normals, FUN_1414b4860). When DCLF's forward
		 * epoch drew them this frame (it marked them begun, so the engine's draw adds to them), group 6 is cleared first: the engine never
		 * draws it there (the normals are never begun at that point without DCLF), and still clears it, as FUN_1414b4860 does.
		 */
		struct Main_Draw_RefractionCheck
		{
			static std::uint32_t thunk(void* a_accumulator)
			{
				// FUN_1414b4ae0(accumulator, 6): the group's pass list cleared (FUN_1414f1940), undrawn.
				if (a_accumulator && ShellState::Running() && RefractionDrawnThisFrame()) {
					using ClearGroup = void (*)(void*, std::int32_t);
					static REL::Relocation<ClearGroup> clearGroup{ REL::Offset(Address::kClearGroup) };
					clearGroup(a_accumulator, 6);
				}
				return func(a_accumulator);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks(Api::InstallContext& a_context)
	{
		blendedDraw = a_context.FindTrigger("blended.draw");
		// The main pass's blended draw (FUN_1414b39d0), at both its call sites in FUN_1414b3390.
		if (Engine::CallsTo(REL::Offset(Address::kBlendedUnderWater).address(), REL::Offset(Address::kBlendedDraw).address()) &&
			Engine::CallsTo(REL::Offset(Address::kBlendedAfterWater).address(), REL::Offset(Address::kBlendedDraw).address())) {
			stl::write_thunk_call<Accumulator_BlendedDraw<0>>(REL::Offset(Address::kBlendedUnderWater).address());
			stl::write_thunk_call<Accumulator_BlendedDraw<1>>(REL::Offset(Address::kBlendedAfterWater).address());
		} else {
			logger::warn("[DCLF] the main pass's blended draw is not where expected; the forward epoch (T7d) never runs, and CS_DCLF_FORWARD must stay off");
		}
		// Main::Draw's refraction point (its group 5 count, FUN_1414b4840).
		if (Engine::CallsTo(REL::Offset(Address::kRefractionCheck).address(), REL::Offset(Address::kGroupFiveCount).address()))
			stl::write_thunk_call<Main_Draw_RefractionCheck>(REL::Offset(Address::kRefractionCheck).address());
		else
			logger::warn("[DCLF] Main::Draw's refraction point is not where expected; CS_DCLF_REFRACTION must stay off");
	}
}
