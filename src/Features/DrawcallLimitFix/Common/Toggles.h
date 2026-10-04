#pragma once

#include "Switches.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF
{
	/**
	 * @brief The feature switches that can change while the game runs, for live A/B comparisons.
	 *
	 * Each is seeded from its `CS_DCLF_*` switch (Switches.h) and then edited from the feature's menu. Unset,
	 * they default to the tested configuration: everything on, both ownerships static, occlusion culling.
	 * kToggles describes every flag; cullMode, the one that is not a flag, is handled beside it.
	 */
	struct ToggleSet
	{
		// How BuildDrawsCS filters the frame's draws before it writes their sequences: 0 off, 1 frustum, 2 frustum then
		// the HZB. The inputs are the whole tracked set, so the culling has real work to do.
		std::uint8_t cullMode = 0;

		// The object classes (LightingDescriptors):
		// single-partition NiSkinInstance shapes, palettes from the engine;
		bool skinned = false;
		// the TreeAnim technique, whose vertex animation reads TreeParams and WindTimers per object (extras rows);
		bool trees = false;
		// decals (accumulation hints 2 and 3), drawn by the second pass;
		bool decals = false;
		// multi-index shapes' main passes: the shape, and its layer (the additional property's passes, hint 12) as decal group 3;
		bool layers = false;
		// kProjectedUV objects (snow and moss projection);
		bool projectedUv = false;
		// terrain, the MTLand and MTLandLODBlend techniques (MtLandEnabled adds the Terrain Blending condition);
		bool mtLand = false;
		// a leaf under an NiSwitchNode, in the frames every switch on its path selects it (trees, harvestables);
		bool switchNodes = false;
		// object LOD (TES::lodLandRoot's BSSubIndexTriShape blocks): tracked under the LOD root, drawn by their visible segments;
		bool lodObjects = false;
		// terrain LOD (TES::lodLandRoot's land blocks, the LODLand and LODLandNoise techniques): tracked under the LOD root;
		bool lodTerrain = false;
		// skins of several partitions (LOD trees, actor bodies), one draw per partition the engine would draw;
		bool skinPartitions = false;
		// geometry under an actor's 3D, and the FacegenRGBTint technique;
		bool actors = false;
		// an object the engine fades with the screen-door mask in an opaque group (blended fades, hint 9, stay native);
		bool fading = false;
		// an object in a LOD cross-fade stays DCLF's, and only the engine's hint-10 copy of the old level is native.
		bool lodCrossfade = false;

		// The shadow views are drawn by the render graph.
		bool shadows = false;
		// The engine does not build sun shadow passes for the set's casters (SunAccumulation).
		bool skipSunAccumulation = false;
		// The sun's cascade culls skip the references whose shadows DCLF draws entirely (SunAccumulation).
		bool excludeSunEntries = false;
		// DCLF draws Skylighting's occlusion map.
		bool skyOcclusion = false;
		// DCLF draws the precipitation occlusion mask (with Skylighting, whose hook renders it).
		bool precipitationOcclusion = false;
		// The main camera's cull skips the references DCLF draws entirely (PrimaryCull).
		bool excludePrimaryEntries = false;

		bool operator==(const ToggleSet&) const = default;
	};

	/** @brief One flag of ToggleSet: its seed, its menu entry, and what it needs. */
	struct ToggleInfo
	{
		bool ToggleSet::*member;
		Switch seed;
		std::string_view onValue;  // the seed's value besides unset that turns it on ("1", or "static" for the ownerships)
		std::string_view name;     // for the "This frame" line
		std::string_view section;  // the menu heading this flag opens, or empty
		const char* label;
		const char* tooltip;       // or nullptr
		bool entersClassification;  // a change drops the classification caches (SceneStore::InvalidateVerdicts)
		bool ToggleSet::*needs[2];  // the flags it needs on, earlier in kToggles (nullptr when unused)
	};

	/** @brief Every flag of ToggleSet, in menu order; a flag comes after the flags it needs. */
	std::span<const ToggleInfo> ToggleTable();

	/**
	 * @brief The requested and the active toggle sets.
	 *
	 * The menu edits the REQUESTED set. The render thread copies it into the ACTIVE set once per frame, at
	 * the frame's first DCLF point (BeforeShadowMaps), so a toggle never changes under a running frame. The
	 * active set is one packed word behind a relaxed atomic, because the registration hook reads it from
	 * whatever thread the engine registers on: a toggle is a standing statement, not a synchronisation
	 * point, and one word makes every read a single load with no lock.
	 *
	 * A change to a toggle that enters the classification (the object classes) invalidates every cached verdict and
	 * derivation (SceneStore::InvalidateVerdicts), because those caches witness the object, not the switches.
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

		/** @brief Whether a flag of the requested set would take effect: every flag it needs, directly or not, is on. */
		bool Editable(const ToggleInfo& a_info) const;

	private:
		Toggles();

		static std::uint32_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint32_t a_bits);

		ToggleSet requested;
		std::atomic<std::uint32_t> active{ 0 };
		std::atomic<std::uint32_t> generation{ 0 };
	};

	/** @brief The toggles in force for the current frame (Toggles::Active); any thread. */
	inline ToggleSet ActiveToggles() { return Toggles::Get().Active(); }
}
