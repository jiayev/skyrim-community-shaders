// EngineOnlyCull: the culling processes' Process1 hooks that limit an armed cull to the engine-only subset (EngineOnlyCull.h).

#include "Features/DrawcallLimitFix/Core/Handoff/EngineOnlyCull.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Scene/EngineCasters.h"

#include <atomic>

namespace DCLF::EngineOnlyCull
{
	namespace
	{
		thread_local const Scene::EngineOnlySubset* armed = nullptr;
		thread_local std::uint8_t armedView = 0;
		std::atomic<std::uint64_t> culled{ 0 }, descended{ 0 }, skipped{ 0 };
		bool installed = false;

		/** @brief Whether an armed cull skips a_object (counted either way); false while nothing is armed. */
		bool Skips(const RE::NiAVObject* a_object)
		{
			const auto* subset = armed;
			if (!subset || !a_object)
				return false;
			switch (subset->AdmitObject(static_cast<Scene::EngineOnlySubset::View>(armedView), a_object)) {
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

	Scope::Scope(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view) noexcept :
		previous(armed), previousView(armedView)
	{
		armed = a_subset;
		armedView = a_view;
	}

	Scope::~Scope()
	{
		armed = previous;
		armedView = previousView;
	}

	void Install()
	{
		if (std::exchange(installed, true))
			return;
		// The spot lights' and the sun's cascades' processes, and the point lights' (each class has its own slot).
		stl::write_vfunc<0x16, Process1<0>>(RE::VTABLE_BSCullingProcess[0]);
		stl::write_vfunc<0x16, Process1<1>>(RE::VTABLE_BSParabolicCullingProcess[0]);
		// The scene lists' (an occlusion map's SetupMask): after the primary cull's own hook of the slot (PrimaryCull::Install, which only
		// stands in for the main camera's list processes).
		stl::write_vfunc<0x16, Process1<2>>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		logger::info("[DCLF] engine-only culls: Process1 limited to the subset on BSCullingProcess, BSParabolicCullingProcess and BSGeometryListCullingProcess while armed");
	}

	void RunInline(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view, const std::function<void()>& a_cull)
	{
		auto& jobs = Engine::Global<void*>(Engine::Address::kListCullJobs);
		void* const held = jobs;
		jobs = nullptr;
		{
			Scope scope(a_subset, a_view);
			a_cull();
		}
		jobs = held;
	}

	Stats TakeStats()
	{
		return { culled.exchange(0, std::memory_order_relaxed), descended.exchange(0, std::memory_order_relaxed), skipped.exchange(0, std::memory_order_relaxed) };
	}
}
