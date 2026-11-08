#pragma once

// An engine cull limited to the frame's engine-only subset (Scene/EngineCasters.h): the one way a view DCLF draws hands the engine what
// DCLF does not draw. While a Scope lives on a thread, every culling process's Process1 on that thread (BSCullingProcess's, the spot
// lights' and cascades'; BSParabolicCullingProcess's, the point lights') runs only for the subset's roots and their ancestors and skips
// everything else, so the engine walks down to those roots alone, its own tests (portals, rooms, bounds, fades) on the path.
//
// The hooks chain with the other Process1 hooks of those classes (LocalLightCull's, SunAccumulation's): unarmed, they only read a
// thread-local pointer.

#include <cstdint>

namespace DCLF::Scene
{
	struct EngineOnlySubset;
}

namespace DCLF::EngineOnlyCull
{
	/** @brief Arms a_subset's a_view roots for the culls this thread runs while it lives (null: nothing armed). Scopes nest. */
	class Scope
	{
	public:
		Scope(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view) noexcept;
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		const Scene::EngineOnlySubset* previous = nullptr;
		std::uint8_t previousView = 0;
	};

	/** @brief The Process1 hooks (once; PostPostLoad, main thread). */
	void Install();

	/** @brief The armed culls' objects since the last call: culled (roots), descended (ancestors), skipped. */
	struct Stats
	{
		std::uint64_t culled = 0, descended = 0, skipped = 0;
	};
	Stats TakeStats();
}
