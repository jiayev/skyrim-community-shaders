// The withheld passes' observers (Registrations.h): counted on the registering threads, taken on the render thread.

#include "Registrations.h"

#include "Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"

#include <array>
#include <atomic>

#include "State.h"

namespace DCLF::Diagnostics::Registrations
{
	namespace
	{
		std::atomic<std::uint32_t> forward{ 0 }, refraction{ 0 }, effectLightChecked{ 0 }, effectLightOther{ 0 };

		void ListPass(const MemberListPassWithheld& a_event)
		{
			const auto* pass = a_event.pass;
			if (!pass || pass->accumulationHint != 1)
				return;
			forward.fetch_add(1, std::memory_order_relaxed);
			if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Effect && ((pass->passEnum - kEffectPassEnumBase) & EffectFlag::Lighting)) {
				effectLightChecked.fetch_add(1, std::memory_order_relaxed);
				auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				const RE::BSLight* sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
				if (!pass->numLights || !pass->sceneLights || pass->sceneLights[0] != sun)
					effectLightOther.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void Group5Pass(const MemberGroup5PassWithheld& a_event)
		{
			if (a_event.pass && a_event.pass->accumulationHint == 4)
				refraction.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Install()
	{
		Subscribe<MemberListPassWithheld>(ListPass);
		Subscribe<MemberGroup5PassWithheld>(Group5Pass);
	}

	std::uint32_t TakeForward()
	{
		return forward.exchange(0, std::memory_order_relaxed);
	}

	std::uint32_t TakeRefraction()
	{
		return refraction.exchange(0, std::memory_order_relaxed);
	}

	std::pair<std::uint32_t, std::uint32_t> TakeEffectLight()
	{
		return { effectLightChecked.exchange(0, std::memory_order_relaxed), effectLightOther.exchange(0, std::memory_order_relaxed) };
	}
}
