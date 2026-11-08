// EngineOnlyCull: the culling processes' Process1 hooks that limit an armed cull to the engine-only subset (EngineOnlyCull.h).

#include "Features/DrawcallLimitFix/Core/Handoff/EngineOnlyCull.h"

#include "Features/DrawcallLimitFix/Core/Scene/EngineCasters.h"

#include <atomic>

namespace DCLF::EngineOnlyCull
{
	namespace
	{
		thread_local const Scene::EngineOnlySubset* armed = nullptr;
		std::atomic<std::uint64_t> culled{ 0 }, descended{ 0 }, skipped{ 0 };
		bool installed = false;

		/** @brief Whether an armed cull skips a_object (counted either way); false while nothing is armed. */
		bool Skips(const RE::NiAVObject* a_object)
		{
			const auto* subset = armed;
			if (!subset || !a_object)
				return false;
			switch (subset->AdmitObject(a_object)) {
			case Scene::EngineOnlySubset::Admit::Cull:
				culled.fetch_add(1, std::memory_order_relaxed);
				return false;
			case Scene::EngineOnlySubset::Admit::Descend:
				descended.fetch_add(1, std::memory_order_relaxed);
				return false;
			default:
				skipped.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}

		/** @brief Process1 (vtable slot 0x16) of a culling process class: the object culled unless an armed cull skips it. */
		template <std::uint32_t Class>
		struct Process1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				if (Skips(a_object))
					return;
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	Scope::Scope(const Scene::EngineOnlySubset* a_subset) noexcept :
		previous(armed)
	{
		armed = a_subset;
	}

	Scope::~Scope() { armed = previous; }

	void Install()
	{
		if (std::exchange(installed, true))
			return;
		// The spot lights' and the sun's cascades' processes, and the point lights' (each class has its own slot).
		stl::write_vfunc<0x16, Process1<0>>(RE::VTABLE_BSCullingProcess[0]);
		stl::write_vfunc<0x16, Process1<1>>(RE::VTABLE_BSParabolicCullingProcess[0]);
		logger::info("[DCLF] engine-only culls: Process1 limited to the subset on BSCullingProcess and BSParabolicCullingProcess while armed");
	}

	Stats TakeStats()
	{
		return { culled.exchange(0, std::memory_order_relaxed), descended.exchange(0, std::memory_order_relaxed), skipped.exchange(0, std::memory_order_relaxed) };
	}
}
