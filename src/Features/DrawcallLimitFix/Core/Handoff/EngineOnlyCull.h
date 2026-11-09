#pragma once

// An engine cull limited to the frame's engine-only subset (Scene/EngineCasters.h): the one way a view DCLF draws hands the engine what
// DCLF does not draw. While a Scope lives on a thread, every culling process's Process1 on that thread (BSCullingProcess's, the spot
// lights' and cascades'; BSParabolicCullingProcess's, the point lights'; BSGeometryListCullingProcess's, the scene lists' culls of an
// occlusion map) runs only for the subset's roots and their ancestors and skips everything else, so the engine walks down to those roots
// alone, its own tests (portals, rooms, bounds, fades) on the path.
//
// The hooks chain with the other Process1 hooks of those classes (LocalLightCull's, SunAccumulation's): unarmed, they only read a
// thread-local pointer.

#include <cstdint>
#include <functional>
#include <string>

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

	/**
	 * @brief a_cull under a Scope of a_subset's a_view, with the scene lists' cull jobs held off (their job list null for the call: the list
	 * culls run inline, on this thread, where the scope is armed). Render thread: an occlusion map's Precipitation::SetupMask.
	 */
	void RunInline(const Scene::EngineOnlySubset* a_subset, std::uint8_t a_view, const std::function<void()>& a_cull);

	/**
	 * @brief A view that DCLF draws in normal play but the engine culled whole this frame (no subset: everything it reaches registers, the
	 * members withheld). Every such case is a view DCLF does not cover yet; each is counted, warned about once, and reported
	 * (WholeCullLine). Parity and diagnostics frames are not noted.
	 */
	enum class WholeCull : std::uint8_t
	{
		SunNotCovered,   // a sun cascade not DCLF's (or too many scene lists): the full-frustum cull with the entry exclusion
		SunNoExclusion,  // and no exclusion current for it: the full-frustum cull of everything
		PointNotCovered,  // a point light's view not DCLF's, or no selection this frame
		PointPastMask,    // a point light past the light mask's 32
		SpotNotCovered,
		SpotPastMask,
		PrimaryNoSnapshot,  // the main camera's lists culled without the stand-in: no candidates yet
		OcclusionNotTaken,  // an occlusion map DCLF did not draw: the engine's SetupMask over every scene list
		Count
	};
	/** @brief Any thread: one more whole cull for a_reason (the first of each warned about in the log). */
	void NoteWholeCull(WholeCull a_reason);
	/** @brief The whole culls since the last call, by reason, as a report line (empty when there were none). */
	std::string WholeCullLine();

	/** @brief The armed culls' objects since the last call: culled (roots), descended (ancestors), skipped. */
	struct Stats
	{
		std::uint64_t culled = 0, descended = 0, skipped = 0;
	};
	Stats TakeStats();
}
