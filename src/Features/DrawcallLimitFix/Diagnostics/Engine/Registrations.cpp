// The withheld passes' observers (Registrations.h): counted on the registering threads, taken on the render thread.

#include "Registrations.h"

#include "Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Engine/Capture/ParticleSnapshots.h"
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
		std::atomic<std::uint64_t> particlePasses{ 0 }, particlesUncovered{ 0 };
		// The first uncovered particle pass, named where it registered (0 free, 1 being written, 2 written).
		std::atomic<std::uint32_t> firstState{ 0 };
		std::array<char, 160> first{};

		void Particle(const RE::BSGeometry* a_geometry)
		{
			particlePasses.fetch_add(1, std::memory_order_relaxed);
			bool followed = false;
			if (ParticleSnapshots::Get().Covered(a_geometry, followed))
				return;
			particlesUncovered.fetch_add(1, std::memory_order_relaxed);
			// Named here, on the registering thread (the engine's own pass of it): the report never reads the geometry.
			std::uint32_t free = 0;
			if (a_geometry && firstState.compare_exchange_strong(free, 1, std::memory_order_acquire)) {
				const auto* parent = a_geometry->parent;
				const auto written = fmt::format_to_n(first.data(), first.size() - 1, "'{}' under '{}'{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "",
					parent && parent->name.c_str() ? parent->name.c_str() : "", followed ? "" : " (not followed)");
				*written.out = '\0';
				firstState.store(2, std::memory_order_release);
			}
		}

		void ListPass(const MemberListPassWithheld& a_event)
		{
			const auto* pass = a_event.pass;
			if (!pass || pass->accumulationHint != 1)
				return;
			forward.fetch_add(1, std::memory_order_relaxed);
			if (pass->geometry && pass->geometry->GetType().get() == RE::BSGeometry::Type::kParticles)
				Particle(pass->geometry);
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

	Particles TakeParticles()
	{
		Particles p;
		p.passes = particlePasses.exchange(0);
		if (!p.passes)
			return p;
		p.uncovered = particlesUncovered.exchange(0);
		if (firstState.load(std::memory_order_acquire) == 2) {
			p.first = first.data();
			firstState.store(0, std::memory_order_release);
		}
		return p;
	}
}
