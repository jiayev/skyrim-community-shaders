// The engine's shadow-light selection, hooked (render thread): NiCamera::CalculateAndDrawShadowCasterLights' call from Main::Draw, and its
// light-keeping calls (FUN_1414a3fb0, the sun's and each kept light's), which reach Core's FrameDriver: the frame's shadow views begun and
// their coverage decided (the epochs' beforeShadowCasters) before the engine keeps a light, each light's views before its Accumulate
// registers into them, and the list once the selection is done (skyrim-engine-notes.md, "The shadow lights' selection"). Installed from
// the feature's install.

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/ShadowEngineAddresses.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		// Render thread: DCLF runs this frame's selection (the shell's Running, sampled at its start, so that the three points agree).
		bool castersRunning = false;

		/**
		 * @brief Main::Draw's call of NiCamera::CalculateAndDrawShadowCasterLights (AE 0x140644674): the whole selection, every shadow light's
		 * UpdateCamera and Accumulate (the registrations into its views) inside it. The views begun before, listed after.
		 */
		struct CalculateAndDrawShadowCasterLights
		{
			static void thunk(void* a_camera)
			{
				castersRunning = ShellState::Running();
				if (castersRunning)
					FrameDriver::Get().BeforeShadowCasters();
				func(a_camera);
				if (castersRunning)
					FrameDriver::Get().ShadowCastersSelected();
				castersRunning = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_1414a3fb0(shadow scene node, light, first slot, slots): the light written into its slots of shadowLightsAccum, once the
		 * engine kept it - its UpdateCamera run and its focus flag set - and right before its Accumulate. Site 0 is the sun's (AE 0x1414cbc66,
		 * its cascades' slots), site 1 each kept light's (AE 0x1414cc7e1, one slot).
		 */
		template <std::uint32_t Site>
		struct KeepLight
		{
			static void thunk(void* a_node, RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
			{
				func(a_node, a_light, a_firstSlot, a_slots);
				if (castersRunning)
					FrameDriver::Get().ShadowCasterKept(a_light, a_firstSlot, a_slots);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallCasterHooks()
	{
		const auto base = REL::Module::get().base();
		const auto keep = base + Address::kKeepLight;
		if (!DCLF::Engine::CallsTo(base + Address::kCastersCallSite, base + Address::kCalculateAndDrawShadowCasterLights) ||
			!DCLF::Engine::CallsTo(base + Address::kKeepSunCallSite, keep) || !DCLF::Engine::CallsTo(base + Address::kKeepLightCallSite, keep)) {
			logger::warn("[DCLF] the shadow lights' selection is not where expected; the shadow views are never listed, and the engine draws every one");
			return;
		}
		stl::write_thunk_call<CalculateAndDrawShadowCasterLights>(base + Address::kCastersCallSite);
		stl::write_thunk_call<KeepLight<0>>(base + Address::kKeepSunCallSite);
		stl::write_thunk_call<KeepLight<1>>(base + Address::kKeepLightCallSite);
		logger::info("[DCLF] shadow casters: the shadow lights' selection hooked (its call, and each light kept)");
	}
}
