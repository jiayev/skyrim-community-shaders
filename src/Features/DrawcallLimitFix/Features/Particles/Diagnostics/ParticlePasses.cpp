// CS_DCLF_SET_PARITY: the particle members' withheld passes (Particles.h, SubscribePasses): counted on the registering threads, taken on
// the render thread.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <array>
#include <atomic>

namespace DCLF::Features::Particles
{
	namespace
	{
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

		// A blended pass (accumulation hint 1, the main modes' list insertion) of a particle member.
		void ListPass(const Diagnostics::MemberListPassWithheld& a_event)
		{
			const auto* pass = a_event.pass;
			if (!pass || pass->accumulationHint != 1)
				return;
			if (pass->geometry && pass->geometry->GetType().get() == RE::BSGeometry::Type::kParticles)
				Particle(pass->geometry);
		}
	}

	void SubscribePasses()
	{
		Diagnostics::Subscribe<Diagnostics::MemberListPassWithheld>(ListPass);
	}

	PassCounts TakePasses()
	{
		PassCounts p;
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
