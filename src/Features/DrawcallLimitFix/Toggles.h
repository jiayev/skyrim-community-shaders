#pragma once

#include <atomic>
#include <cstdint>

namespace DCLF
{
	/**
	 * @brief The feature switches that can change while the game runs, for live A/B comparisons.
	 *
	 * DCLF's features that can be switched live - static ownership, the culling modes, the object classes
	 * (skinned, trees, decals, projected UV, terrain, switch nodes, skin partitions, actors, fading), the
	 * shadow views and their ownership, the sun's and the primary's exclusions and Skylighting's map - are
	 * seeded from their `CS_DCLF_*` switches (Switches.h) and then edited from the feature's menu. Unset, they
	 * default to the tested configuration: everything on, both ownerships static, occlusion culling.
	 */
	struct ToggleSet
	{
		bool ownership = false;       // CS_DCLF_OWNERSHIP=static
		std::uint8_t cullMode = 0;    // CS_DCLF_CULL: 0 off, 1 frustum, 2 occlusion
		bool skinned = false;         // CS_DCLF_SKINNED=1
		bool trees = false;           // CS_DCLF_TREES=1
		bool decals = false;          // CS_DCLF_DECALS=1
		bool projectedUv = false;     // CS_DCLF_PROJECTED_UV=1
		bool mtLand = false;          // CS_DCLF_MTLAND=1
		bool switchNodes = false;     // CS_DCLF_SWITCH_NODES=1
		bool skinPartitions = false;  // CS_DCLF_SKIN_PARTITIONS=1 (needs skinned)
		bool actors = false;          // CS_DCLF_ACTORS=1
		bool fading = false;          // CS_DCLF_FADING=1
		bool lodCrossfade = false;    // CS_DCLF_LOD_CROSSFADE=1
		bool shadows = false;         // CS_DCLF_SHADOWS=1
		bool shadowOwnership = false; // CS_DCLF_SHADOW_OWNERSHIP=static (needs shadows)
		bool skipSunAccumulation = false;  // CS_DCLF_SUN_SKIP=1 (needs shadowOwnership): SunAccumulation
		bool excludeSunEntries = false;    // CS_DCLF_SUN_EXCLUDE=1 (needs skipSunAccumulation): SunAccumulation's entry exclusion
		bool skyOcclusion = false;         // CS_DCLF_SKYLIGHT=1 (needs shadows): DCLF draws Skylighting's occlusion map
		bool excludePrimaryEntries = false;  // CS_DCLF_PRIMARY_EXCLUDE=1 (needs excludeSunEntries and ownership): PrimaryCull's list filter

		bool operator==(const ToggleSet&) const = default;
	};

	/**
	 * @brief The requested and the active toggle sets.
	 *
	 * The menu edits the REQUESTED set. The render thread copies it into the ACTIVE set once per frame, at
	 * the frame's first DCLF point (BeforeShadowMaps), so a toggle never changes under a running frame -
	 * the invariant the once-read switches used to give. The active set is one packed word behind a
	 * relaxed atomic, because the registration hook reads it from whatever thread the engine registers
	 * on: a toggle is a standing statement, not a synchronisation point, and one word makes every read a
	 * single load with no lock.
	 *
	 * A change to a toggle that enters the classification (the object classes) invalidates every cached verdict and derivation (SceneStore::InvalidateVerdicts), because those
	 * caches witness the object, not the switches.
	 */
	class Toggles
	{
	public:
		static Toggles& Get();

		/** @brief The set the menu edits; render thread. */
		ToggleSet& Requested() { return requested; }

		/** @brief The set in force for the current frame; any thread. */
		ToggleSet Active() const { return Unpack(active.load(std::memory_order_relaxed)); }

		/**
		 * @brief Render thread, once per frame, before anything reads the toggles: applies the requested set.
		 * @return whether a toggle that enters the classification changed, so the caches must be dropped.
		 */
		bool BeginFrame();

		/** @brief How many times the active set has changed; a cheap witness for anything caching a toggle. */
		std::uint32_t Generation() const { return generation.load(std::memory_order_relaxed); }

		static std::uint32_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint32_t a_bits);

	private:
		Toggles();

		ToggleSet requested;
		std::atomic<std::uint32_t> active{ 0 };
		std::atomic<std::uint32_t> generation{ 0 };
	};
}
