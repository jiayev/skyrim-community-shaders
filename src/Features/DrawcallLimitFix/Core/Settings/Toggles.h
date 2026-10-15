#pragma once

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace DCLF
{
	/**
	 * @brief The feature switches that can change while the game runs, for live A/B comparisons.
	 *
	 * Each is seeded from its `CS_DCLF_*` switch (Switches.h) and then edited from the feature's menu. Unset,
	 * they default to the tested configuration: everything on, both ownerships static, occlusion culling.
	 * kToggles describes every Core flag (the features register theirs: RegisterToggle); cullMode, the one that is not a flag, is
	 * handled beside it.
	 */
	struct ToggleSet
	{
		// How BuildDrawsCS filters the frame's draws before it writes their sequences: 0 off, 1 frustum, 2 frustum then
		// the HZB. The inputs are the whole tracked set, so the culling has real work to do.
		std::uint8_t cullMode = 0;

		// The object classes (LightingDescriptors):
		// single-partition NiSkinInstance shapes, palettes from the engine;
		bool skinned = false;
		// kProjectedUV objects (snow and moss projection);
		bool projectedUv = false;
		// terrain, the MTLand and MTLandLODBlend techniques (MtLandEnabled adds the Terrain Blending condition);
		bool mtLand = false;
		// while Terrain Blending holds the terrain, the terrain (its groups 1 and 2: Records.h, ObjectTerrainGroup), drawn as
		// that feature draws it;
		bool tbTerrain = false;
		// blended Lighting geometry (alpha-blended, or a material alpha below one), drawn by the forward epoch: a GPU depth sort of
		// the visible set, back to front, before the engine's own blended draws (on by default: CS_DCLF_FORWARD=0 seeds it off);
		bool forward = false;
		// static blended BSEffectShaderProperty meshes, drawn by the forward epoch in the same GPU sort (needs forward;
		// on by default: CS_DCLF_EFFECTS=0 seeds it off);
		bool effects = false;
		// refraction: Lighting geometry with kRefraction or kTempRefraction, whose one main pass is a Utility RENDER_NORMAL pass into
		// the refraction normals (kREFRACTION_NORMALS) that ISRefraction warps the frame by; drawn in the forward epoch (needs forward;
		// on by default: CS_DCLF_REFRACTION=0 seeds it off);
		bool refraction = false;
		// object LOD (TES::lodLandRoot's BSSubIndexTriShape blocks): tracked under the LOD root, drawn by their visible segments;
		bool lodObjects = false;
		// terrain LOD (TES::lodLandRoot's land blocks, the LODLand and LODLandNoise techniques): tracked under the LOD root;
		bool lodTerrain = false;
		// tree LOD (the LOD trees root's instanced shapes): drawn by DCLF's own cull and draws from the mirrored instance records;
		bool lodTrees = false;
		// skins of several partitions (LOD trees, actor bodies), one draw per partition the engine would draw;
		bool skinPartitions = false;

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
		// DCLF draws the water reflection's cube map faces' LOD (dclf-lod.md, "Water reflections").
		bool reflections = false;
		// The main camera's cull skips the references DCLF draws entirely (PrimaryCull).
		bool excludePrimaryEntries = false;

		// The features' toggles (RegisterToggle), bit by Api::ToggleId: an object class a feature ports (Features/TreeWind: trees;
		// Features/Particles, Features/Billboards, Features/SwitchNodes, Features/Decals, Features/Actors: theirs; Features/FirstPerson: a view), ...
		std::uint32_t registered = 0;
		/** @brief Whether a feature's toggle is on (false for none). */
		bool On(Api::ToggleId a_toggle) const { return a_toggle && ((registered >> a_toggle.bit) & 1u) != 0; }

		bool operator==(const ToggleSet&) const = default;
	};

	/** @brief One flag of ToggleSet: its seed, its menu entry, and what it needs. A feature's has no member: its bit and seed are registered. */
	struct ToggleInfo
	{
		bool ToggleSet::*member;
		Switch seed;
		std::string_view onValue;  // the seed's value besides unset that turns it on ("1", or "static" for the ownerships)
		std::string_view name;     // for the "This frame" line
		std::string_view section;  // the menu heading this flag opens, or empty
		const char* label;
		const char* tooltip;       // or nullptr
		bool entersClassification;  // a change drops the classification caches (Coordinator::InvalidateVerdicts)
		bool ToggleSet::*needs[2];  // the flags it needs on, earlier in kToggles (nullptr when unused)
		bool offByDefault = false;  // unset seeds it off: only its onValue turns it on
		Api::ToggleId registered{};       // a feature's toggle: its bit (ToggleSet::registered); member is null
		Api::SwitchId registeredSeed{};   // ... and its seed, a feature's switch
		Api::ToggleId registeredNeeds[2]{};  // the features' toggles it needs on, earlier in the table (none when unused)
	};

	/** @brief A flag's value in a_set (its member, or a feature's bit). */
	inline bool ToggleValue(const ToggleSet& a_set, const ToggleInfo& a_info)
	{
		return a_info.member ? a_set.*a_info.member : a_set.On(a_info.registered);
	}
	/** @brief Sets a flag in a_set. */
	inline void SetToggleValue(ToggleSet& a_set, const ToggleInfo& a_info, bool a_value)
	{
		if (a_info.member)
			a_set.*a_info.member = a_value;
		else if (a_info.registered)
			a_set.registered = a_value ? a_set.registered | (1u << a_info.registered.bit) : a_set.registered & ~(1u << a_info.registered.bit);
	}

	/**
	 * @brief Every flag of ToggleSet, in menu order; a flag comes after the flags it needs. The features' toggles are among them (each
	 * after the Core toggle it names) once they registered; read-only after the install.
	 */
	std::span<const ToggleInfo> ToggleTable();

	/**
	 * @brief Main thread, at install (Extensions::InstallFeatures, for a_feature): a feature's toggle, into the table after the Core
	 * toggle its after names. Fails the install past the packed word's bits, for an after Core does not have, or a need not listed before it
	 * (Core's, or a feature's registered earlier).
	 */
	Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle, std::string_view a_feature);
	/** @brief A toggle by its name (ToggleInfo::name): a feature's id (none: no such toggle, or Core's). Any thread after the install. */
	Api::ToggleId FindToggle(std::string_view a_name);

	/**
	 * @brief The requested and the active toggle sets.
	 *
	 * The menu edits the REQUESTED set. The render thread copies it into the ACTIVE set once per frame, at
	 * the frame's start (FrameDriver), so a toggle never changes under a running frame. The active set is one
	 * packed word behind a relaxed atomic, because hooks on the engine's job threads and DCLF's threads read it:
	 * a toggle is a standing statement, not a synchronisation point, and one word makes every read a single load.
	 *
	 * A change to a toggle that enters the classification (the object classes) invalidates every cached verdict and
	 * derivation (Coordinator::InvalidateVerdicts), because those caches witness the object, not the switches.
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

		/** @brief Main thread, after the features' install: their toggles seeded from their switches, into both sets. */
		void SeedRegistered();

	private:
		Toggles();

		static std::uint32_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint32_t a_bits);

		ToggleSet requested;
		std::atomic<std::uint32_t> active{ 0 };
		std::atomic<std::uint32_t> generation{ 0 };
	};

	/**
	 * @brief A thread's snapshot of the active set, which ActiveToggles reads in its place while installed (ToggleSnapshotScope):
	 * a scene pass's evaluations read the set the pass took at its start on whichever thread they run, though the render thread may
	 * change the active one meanwhile.
	 */
	inline thread_local const ToggleSet* activeToggleSnapshot = nullptr;

	/** @brief The toggles in force for the current frame (Toggles::Active), or this thread's snapshot of them; any thread. */
	inline ToggleSet ActiveToggles() { return activeToggleSnapshot ? *activeToggleSnapshot : Toggles::Get().Active(); }

	/** @brief a_set is this thread's ActiveToggles while the scope lives (null: the active set). */
	class ToggleSnapshotScope
	{
	public:
		explicit ToggleSnapshotScope(const ToggleSet* a_set) :
			previous(std::exchange(activeToggleSnapshot, a_set)) {}
		~ToggleSnapshotScope() { activeToggleSnapshot = previous; }
		ToggleSnapshotScope(const ToggleSnapshotScope&) = delete;
		ToggleSnapshotScope& operator=(const ToggleSnapshotScope&) = delete;

	private:
		const ToggleSet* previous;
	};

	/** @brief Whether DCLF draws an occlusion view (kOcclusion*): its toggle, and Skylighting, whose hook drives both maps. */
	bool OcclusionEnabled(std::uint32_t a_view);
}
