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
		// kProjectedUV objects (snow and moss projection);
		bool projectedUv = false;
		// A multi-index snow layer whose material is the plain class (vanilla glaciers: SetupMaterial reads its SparkleParams past the
		// material's end) sparkles with the Snow class's default; off, it draws without sparkle (MaterialPortVanilla, technique 0xe).
		bool plainSnowSparkle = false;
		// (Features/Landscape's "terrain", then Integrations/TerrainBlending's "terrain under TB", follow the projected's in the table;
		// then Features/Forward's "forward", Features/Effects' "effects" and Features/Forward's "refraction".)

		// The shadow views are drawn by the render graph.
		bool shadows = false;
		// (Features/Shadows/Sun's "skip sun accumulation" and "exclude sun entries" follow the shadows' in the table.)
		// The main camera's cull skips the references DCLF draws entirely (PrimaryCull; needs Features/Shadows/Sun's "exclude sun
		// entries", by name).
		bool excludePrimaryEntries = false;

		// The features' toggles (RegisterToggle), bit by Api::ToggleId: an object class a feature ports (Features/Skins: the skinned and
		// skins of several partitions; Features/TreeWind: trees;
		// Features/Effects, Features/Particles, Features/Billboards, Features/SwitchNodes, Features/Decals, Features/Actors: theirs; Features/Lod: object and
		// terrain LOD; Features/TreeLod: tree LOD; Features/FirstPerson: a view; Features/OcclusionMaps: its maps), or a source's engine work
		// removed (Features/Shadows/Sun: the sun's accumulation and its entries' culls), ...
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
		// A Core flag's needs by name, which may be a feature's toggle: Toggles::SeedRegistered resolves each into needs or registeredNeeds
		// at the same index (so that slot's member pointer stays null); empty when unused.
		std::string_view needNames[2]{};
		bool needsMissing = false;  // a named need no feature registered: the flag never takes effect
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
	 * after or before the toggle it names) once every feature registered (Toggles::SeedRegistered); read-only after the install.
	 */
	std::span<const ToggleInfo> ToggleTable();

	/**
	 * @brief Main thread, at install (Extensions::InstallFeatures, for a_feature): a feature's toggle, its bit taken now; Toggles::
	 * SeedRegistered places it in the table by its after or before (Core's toggle or a feature's, registered before it or after) once every
	 * feature registered. Fails the install past the packed word's bits, for a name taken, or both an after and a before.
	 */
	Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle, std::string_view a_feature);
	/** @brief A toggle by its name (ToggleInfo::name): a feature's id (none: no such toggle, or Core's). Any thread after the install. */
	Api::ToggleId FindToggle(std::string_view a_name);

	/**
	 * @brief The requested, the active and the frame's toggle sets.
	 *
	 * The menu edits the REQUESTED set. The render thread copies it into the ACTIVE set once per frame, at the frame's start
	 * (FrameDriver); the frame inputs carry it to the scene side, which commits its next publication under it (ScenePublication::
	 * toggles). The FRAME's set is the installed publication's (FrameView::HandOverAtFrameStart: Adopt), so a toggle never changes
	 * under a running frame and takes effect for the frames only with a publication committed under it: until then they draw,
	 * withhold and cover as the installed one's toggles say (a toggle withdraws nothing). Each set is one packed word behind a
	 * relaxed atomic, because hooks on the engine's job threads and DCLF's threads read them: a toggle is a standing statement, not a
	 * synchronisation point, and one word makes every read a single load.
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

		/** @brief The newest set applied, which the next publication is committed under (SceneToggles); any thread. */
		ToggleSet Active() const { return Unpack(active.load(std::memory_order_relaxed)); }

		/** @brief The installed publication's set, the active one before the first (FrameToggles); any thread. */
		ToggleSet Frame() const { return Unpack(frame.load(std::memory_order_relaxed)); }

		/** @brief Render thread, frame start (FrameView::HandOverAtFrameStart): a_set, an installed publication's, is the frames'. */
		void Adopt(const ToggleSet& a_set) { frame.store(Pack(a_set), std::memory_order_relaxed); }

		/**
		 * @brief Render thread, once per frame, before anything reads the toggles: applies the requested set.
		 * @return whether a toggle that enters the classification changed, so the caches must be dropped.
		 */
		bool BeginFrame();

		/** @brief How many times the active set has changed; a cheap witness for anything caching a toggle. */
		std::uint32_t Generation() const { return generation.load(std::memory_order_relaxed); }

		/** @brief Whether a flag of the requested set would take effect: every flag it needs, directly or not, is on. */
		bool Editable(const ToggleInfo& a_info) const;

		/**
		 * @brief Main thread, after the features' install: the features' toggles placed in the table by their anchors and their needs
		 * resolved (fails the install for an anchor no toggle is, anchors that loop, or a need not listed before its toggle), the Core
		 * flags' needs by name resolved (ToggleInfo::needNames; fails the install for one listed after its flag), then the features'
		 * toggles seeded from their switches, into both sets.
		 */
		void SeedRegistered();

	private:
		Toggles();

		static std::uint32_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint32_t a_bits);

		ToggleSet requested;
		std::atomic<std::uint32_t> active{ 0 };
		std::atomic<std::uint32_t> frame{ 0 };
		std::atomic<std::uint32_t> generation{ 0 };
	};

	/**
	 * @brief A thread's snapshot of the toggles, which ActiveToggles reads in its place while installed (ToggleSnapshotScope): a scene
	 * pass's evaluations, its commit and its publication read the set its frame inputs carried (the one it commits under) on whichever
	 * thread they run, though the render thread may change the active one meanwhile.
	 */
	inline thread_local const ToggleSet* activeToggleSnapshot = nullptr;

	/** @brief The frame's toggles (Toggles::Frame): the installed publication's; any thread. */
	inline ToggleSet FrameToggles() { return Toggles::Get().Frame(); }

	/**
	 * @brief The newest toggles (Toggles::Active), which the next publication is committed under: the scene side's producers that run
	 * outside a scene pass read these (the captures, the frame requests, the lookup and shadow inputs, the builds ahead's context); any
	 * thread.
	 */
	inline ToggleSet SceneToggles() { return Toggles::Get().Active(); }

	/**
	 * @brief The toggles in force where they are read: this thread's snapshot inside a scene pass (the committed set), else the frame's
	 * (FrameToggles: the render thread, the engine's hooks, the epochs, the coverage); any thread.
	 */
	inline ToggleSet ActiveToggles() { return activeToggleSnapshot ? *activeToggleSnapshot : FrameToggles(); }

	/** @brief a_set is this thread's ActiveToggles while the scope lives (null: the frame's set). */
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
}
