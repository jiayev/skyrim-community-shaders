#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace DCLF::ImportTimings
{
	/**
	 * @brief CS_DCLF_TIMELINE: the engine's import work timed by thread, main against the others, through detours on its
	 * entry points (skyrim-engine-notes.md, "Where the engine imports the world: threads"): the grid controller, the BSTaskPool
	 * drain, a queued reference's finish, the I/O completion pump, the LOD swaps' drain and a quadtree node's block retirement.
	 * Inclusive times: a nested entry (a reference's finish in the pump) counts in both. Each is a Tracy zone too.
	 */
	void Install();
	/** @brief The render thread, every frame: the main thread's id (Skyrim's renders on its main thread). */
	void NoteMainThread(std::uint32_t a_thread);
	/** @brief The entry points timed, and their names. */
	inline constexpr std::size_t kEntries = 6;
	extern const std::array<const char*, kEntries> kNames;
	/** @brief Per entry point, calls and QueryPerformanceCounter ticks: [entry][0 the other threads, 1 the main thread]. */
	struct Totals
	{
		std::array<std::array<std::uint64_t, 2>, kEntries> calls{}, ticks{};
	};
	/** @brief The totals since the last call, reset (the report's: Reports::ImportTimingsLine). */
	Totals Take();

	/** @brief Whether Install ran (the LOD entries' scopes time only then). */
	bool Installed();

	/**
	 * @brief The LOD swaps' drain (FUN_140513840) and a node's block retirement (FUN_140510a10) have one detour each, LodGates' (their
	 * bodies gate the swaps; LodGates::InstallDetours, called by Install too): they time through this scope, a no-op until
	 * Install ran.
	 */
	enum class LodEntry : std::uint8_t
	{
		Drain,
		Retire
	};
	class LodScope
	{
	public:
		explicit LodScope(LodEntry a_entry);
		~LodScope();
		LodScope(const LodScope&) = delete;
		LodScope& operator=(const LodScope&) = delete;

	private:
		LodEntry entry;
		std::int64_t start = 0;  // 0: not timed
	};
}
