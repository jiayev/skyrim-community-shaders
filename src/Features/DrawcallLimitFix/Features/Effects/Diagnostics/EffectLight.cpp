// CS_DCLF_SET_PARITY: the effect members' withheld Lighting-variant passes (Effects.h, SubscribeLight): counted on the registering threads,
// taken on the render thread.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <atomic>

namespace DCLF::Features::Effects
{
	namespace
	{
		std::atomic<std::uint32_t> lightChecked{ 0 }, lightOther{ 0 };

		// A blended pass (accumulation hint 1, the main modes' list insertion) of a member: the Effect shader's Lighting variant's.
		void ListPass(const Diagnostics::MemberListPassWithheld& a_event)
		{
			const auto* pass = a_event.pass;
			if (!pass || pass->accumulationHint != 1)
				return;
			if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Effect && ((pass->passEnum - kEffectPassEnumBase) & EffectFlag::Lighting)) {
				lightChecked.fetch_add(1, std::memory_order_relaxed);
				auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				const RE::BSLight* sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
				if (!pass->numLights || !pass->sceneLights || pass->sceneLights[0] != sun)
					lightOther.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	void SubscribeLight()
	{
		Diagnostics::Subscribe<Diagnostics::MemberListPassWithheld>(ListPass);
	}

	std::pair<std::uint32_t, std::uint32_t> TakeLight()
	{
		return { lightChecked.exchange(0, std::memory_order_relaxed), lightOther.exchange(0, std::memory_order_relaxed) };
	}
}
