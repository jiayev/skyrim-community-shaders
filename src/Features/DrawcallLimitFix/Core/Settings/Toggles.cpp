// Toggles: the flag table (seed, menu entry, dependencies), packing the active set, and the per-frame apply (render thread).
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include "Features/DrawcallLimitFix/Scene/Model/Records.h"
#include "Features/Skylighting.h"

#include <deque>
#include <string>
#include <vector>

namespace DCLF
{
	namespace
	{
		using S = ToggleSet;
		constexpr std::string_view kOn = "1";

		constexpr ToggleInfo kToggles[] = {
			{ &S::skinned, Switch::Skinned, kOn, "skinned", "Object classes", "Skinned (CS_DCLF_SKINNED)", nullptr, true, {} },
			{ &S::decals, Switch::Decals, kOn, "decals", {}, "Decals (CS_DCLF_DECALS)", nullptr, true, {} },
			{ &S::layers, Switch::Layers, kOn, "multi-index layers", {}, "Multi-index shapes: cave walls with an ice or snow layer (CS_DCLF_LAYERS)", nullptr, true, { &S::decals } },
			{ &S::projectedUv, Switch::ProjectedUv, kOn, "projected", {}, "Projected UV (CS_DCLF_PROJECTED_UV)", nullptr, true, {} },
			{ &S::mtLand, Switch::MtLand, kOn, "terrain", {}, "Terrain (CS_DCLF_MTLAND)", nullptr, true, {} },
			{ &S::tbTerrain, Switch::TbTerrain, kOn, "terrain under TB", {}, "Terrain under Terrain Blending (CS_DCLF_TB_TERRAIN)",
				"With Terrain Blending on, DCLF draws the terrain as that feature does: its near blocks' depth into its terrain depth and, offset, into the main depth, and its colour blended after the opaque and decal draws, then the meshes it redraws after the terrain.",
				true, { &S::mtLand } },
			{ &S::forward, Switch::Forward, kOn, "forward", {}, "Blended objects: the forward epoch (CS_DCLF_FORWARD)",
				"T7d: DCLF draws blended Lighting geometry itself, sorted back to front on the GPU, just before the engine's own blended draws (effects, particles). On by default (validated w174/w175).",
				true, {} },
			{ &S::effects, Switch::Effects, kOn, "effects", {}, "Effect meshes: clouds, glows, mist (CS_DCLF_EFFECTS)",
				"T7d: DCLF draws blended effect-shader meshes in the forward epoch, sorted with the blended Lighting objects, and (with Decals) effect decals in its decal groups, skinned ones (with Skinned) through the skin palette. On by default (validated w174/w175; decals and skinned effects not yet).",
				true, { &S::forward } },
			{ &S::refraction, Switch::Refraction, kOn, "refraction", {}, "Refraction: heat haze, invisibility (CS_DCLF_REFRACTION)",
				"T7d: DCLF draws refraction geometry's normals into the refraction normals the engine's refraction effect warps the frame by, in the forward epoch. On by default (validated w174/w175).",
				true, { &S::forward } },
			{ &S::particles, Switch::Particles, kOn, "particles", {}, "Particle systems: smoke, flames, glows (CS_DCLF_PARTICLES)",
				"T7e: DCLF draws effect-shader particle systems in the forward epoch, sorted with the other blended objects, their quads built from snapshots taken at the engine's particle update. On by default (validated fp6).",
				true, { &S::effects } },
			{ &S::billboards, Switch::Billboards, kOn, "billboards", {}, "Billboards: fire cards, spray jets (CS_DCLF_BILLBOARDS)",
				"DCLF draws effect-shader meshes under a billboard node, turning each to the drawing camera on the GPU as the engine's cull turns the node. On by default.",
				true, { &S::effects } },
			{ &S::switchNodes, Switch::SwitchNodes, kOn, "switch nodes", {}, "Under switch nodes: trees, harvestables (CS_DCLF_SWITCH_NODES)", nullptr, true, {} },
			{ &S::lodObjects, Switch::LodObjects, kOn, "object LOD", {}, "Object LOD: distant objects (CS_DCLF_LOD_OBJECTS)", nullptr, true, {} },
			{ &S::lodTerrain, Switch::LodTerrain, kOn, "terrain LOD", {}, "Terrain LOD: distant land (CS_DCLF_LOD_TERRAIN)", nullptr, true, {} },
			{ &S::lodTrees, Switch::LodTrees, kOn, "tree LOD", {}, "Tree LOD: distant trees (CS_DCLF_LOD_TREES)", nullptr, false, {} },
			{ &S::skinPartitions, Switch::SkinPartitions, kOn, "skin partitions", {}, "Skins of several partitions: LOD trees, actor bodies (CS_DCLF_SKIN_PARTITIONS)", nullptr, true, { &S::skinned } },
			{ &S::actors, Switch::Actors, kOn, "actors", {}, "Actors (CS_DCLF_ACTORS)", nullptr, true, {} },
			{ &S::shadows, Switch::Shadows, kOn, "shadows", "Shadow views", "Draw the shadow views (CS_DCLF_SHADOWS)", nullptr, false, {} },
			{ &S::skipSunAccumulation, Switch::SunSkip, kOn, "skip sun accumulation", {}, "Skip the engine's sun shadow culling and registration (CS_DCLF_SUN_SKIP)",
				"The engine stops building sun shadow passes for the casters DCLF draws; it still sets their shadow bits for the main pass.", false, { &S::shadows } },
			{ &S::excludeSunEntries, Switch::SunExclude, kOn, "exclude sun entries", {}, "Take DCLF's objects out of the engine's sun culls (CS_DCLF_SUN_EXCLUDE)",
				"The sun's cascade culls skip every reference whose shadows DCLF draws entirely; DCLF sets those objects' sun shadow bits for the main pass.", false, { &S::skipSunAccumulation } },
			{ &S::excludePrimaryEntries, Switch::PrimaryExclude, kOn, "exclude primary entries", {}, "Take DCLF's objects out of the engine's main camera cull (CS_DCLF_PRIMARY_EXCLUDE)",
				"The main camera's cull and registration skip every reference DCLF draws entirely; DCLF builds their main passes itself and runs their fade updates.", false,
				{ &S::excludeSunEntries } },
			{ &S::skyOcclusion, Switch::Skylight, kOn, "sky occlusion", {}, "Draw Skylighting's occlusion map (CS_DCLF_SKYLIGHT)",
				"With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
			{ &S::precipitationOcclusion, Switch::Precipitation, kOn, "precipitation occlusion", {}, "Draw the precipitation occlusion mask (CS_DCLF_PRECIPITATION)",
				"With Skylighting loaded, DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
			{ &S::reflections, Switch::Reflections, kOn, "reflections", "Reflections", "Draw the water reflection's distant land, objects and trees (CS_DCLF_REFLECTIONS)",
				"DCLF draws the LOD the water's cube map reflects from its own tables on the GPU; the engine draws the sky, and any LOD DCLF cannot draw yet.", false, {} },
			{ &S::firstPerson, Switch::FirstPerson, kOn, "first person", "First person", "Draw the first-person arms and weapons (CS_DCLF_FIRST_PERSON)",
				"T7e: DCLF draws the first-person skeleton's geometry (arms, weapons, shield, magic hands) as a view of its scene, with the first-person camera, in the engine's depth and first-person passes. On by default (validated fp6).",
				true, { &S::skinned, &S::actors } },
		};

		// The packed word: cullMode in the low two bits, then each flag in kToggles order, then the features' by their bits.
		constexpr std::uint32_t kFirstFlagBit = 2;
		constexpr std::uint32_t kFirstRegisteredBit = kFirstFlagBit + static_cast<std::uint32_t>(std::size(kToggles));
		static_assert(kFirstRegisteredBit < 32);

		// The features' toggles (RegisterToggle): their strings (the table's entries view them), the table with them in it, and their
		// count. Written at install alone (the main thread, before DCLF's threads start), read-only after; before, the table is kToggles.
		struct RegisteredStrings
		{
			std::string name, label, tooltip, section, onValue;
		};
		std::deque<RegisteredStrings>& Strings()
		{
			static std::deque<RegisteredStrings> strings;
			return strings;
		}
		std::vector<ToggleInfo>& Table()
		{
			static std::vector<ToggleInfo> table(std::begin(kToggles), std::end(kToggles));
			return table;
		}
		std::atomic<std::uint32_t> registeredCount{ 0 };
		std::uint32_t RegisteredMask()
		{
			return (1u << registeredCount.load(std::memory_order_relaxed)) - 1u;
		}

		bool NeedsMet(const ToggleInfo& a_info, const ToggleSet& a_set)
		{
			for (const auto need : a_info.needs)
				if (need && !(a_set.*need))
					return false;
			return true;
		}

		const std::string& SeedValue(const ToggleInfo& a_info)
		{
			return a_info.member ? SwitchValue(a_info.seed) : SwitchValue(a_info.registeredSeed);
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			const auto& cull = SwitchValue(Switch::Cull);
			set.cullMode = (cull.empty() || cull == "occlusion") ? 2 : cull == "frustum" ? 1 : 0;
			for (const auto& toggle : Table()) {
				const auto& value = SeedValue(toggle);
				SetToggleValue(set, toggle, value.empty() ? !toggle.offByDefault : value == toggle.onValue);
			}
			return set;
		}

		/** @brief The combinations that cannot hold: each flag off while one it needs is off (the table's order resolves chains). */
		ToggleSet Normalised(ToggleSet a_set)
		{
			for (const auto& toggle : Table())
				SetToggleValue(a_set, toggle, ToggleValue(a_set, toggle) && NeedsMet(toggle, a_set));
			return a_set;
		}

		bool EntersClassification(const ToggleSet& a, const ToggleSet& b)
		{
			for (const auto& toggle : Table())
				if (toggle.entersClassification && ToggleValue(a, toggle) != ToggleValue(b, toggle))
					return true;
			return false;
		}

		/** @brief The Core toggle named a_name, by its index in the table (none: -1). */
		std::ptrdiff_t CoreToggleAt(std::string_view a_name)
		{
			const auto& table = Table();
			for (std::size_t i = 0; i < table.size(); ++i)
				if (table[i].member && table[i].name == a_name)
					return static_cast<std::ptrdiff_t>(i);
			return -1;
		}
	}

	std::span<const ToggleInfo> ToggleTable()
	{
		return Table();
	}

	Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle, std::string_view a_feature)
	{
		auto& table = Table();
		const std::uint32_t bit = registeredCount.load(std::memory_order_relaxed);
		if (a_toggle.name.empty() || a_toggle.label.empty() || !a_toggle.seed)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete toggle", a_feature));
		for (const auto& toggle : table)
			if (toggle.name == a_toggle.name)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered the toggle {}, which is taken", a_feature, a_toggle.name));
		if (kFirstRegisteredBit + bit >= 32)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} is past the toggle word's 32 bits", a_feature, a_toggle.name));
		// Right after its Core toggle (after the features' already there), or last; what it needs comes before it.
		std::size_t at = table.size();
		if (!a_toggle.after.empty()) {
			const auto after = CoreToggleAt(a_toggle.after);
			if (after < 0)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} is listed after {}, which Core does not have", a_feature, a_toggle.name, a_toggle.after));
			at = static_cast<std::size_t>(after) + 1;
			while (at < table.size() && !table[at].member)
				++at;
		}
		auto& strings = Strings().emplace_back(RegisteredStrings{ std::string(a_toggle.name), std::string(a_toggle.label), std::string(a_toggle.tooltip),
			std::string(a_toggle.section), std::string(a_toggle.onValue) });
		ToggleInfo info{ nullptr, Switch::Count, strings.onValue, strings.name, strings.section, strings.label.c_str(),
			strings.tooltip.empty() ? nullptr : strings.tooltip.c_str(), a_toggle.entersClassification, {}, a_toggle.offByDefault, Api::ToggleId{ bit }, a_toggle.seed };
		for (std::size_t n = 0; n < std::size(a_toggle.needs); ++n) {
			if (a_toggle.needs[n].empty())
				continue;
			const auto need = CoreToggleAt(a_toggle.needs[n]);
			if (need < 0 || static_cast<std::size_t>(need) >= at)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} needs {}, which Core does not have before it", a_feature, a_toggle.name, a_toggle.needs[n]));
			info.needs[n] = table[static_cast<std::size_t>(need)].member;
		}
		table.insert(table.begin() + static_cast<std::ptrdiff_t>(at), info);
		registeredCount.store(bit + 1, std::memory_order_relaxed);
		return { bit };
	}

	Api::ToggleId FindToggle(std::string_view a_name)
	{
		for (const auto& toggle : Table())
			if (toggle.name == a_name)
				return toggle.registered;
		return {};
	}

	bool Toggles::Editable(const ToggleInfo& a_info) const
	{
		return NeedsMet(a_info, Normalised(requested));
	}

	Toggles& Toggles::Get()
	{
		static Toggles toggles;
		return toggles;
	}

	Toggles::Toggles() :
		requested(FromSwitches())
	{
		active.store(Pack(Normalised(requested)), std::memory_order_relaxed);
	}

	void Toggles::SeedRegistered()
	{
		requested.registered = FromSwitches().registered;
		active.store(Pack(Normalised(requested)), std::memory_order_relaxed);
	}

	std::uint32_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		bits |= (a_set.registered & RegisteredMask()) << kFirstRegisteredBit;
		return bits;
	}

	ToggleSet Toggles::Unpack(std::uint32_t a_bits)
	{
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(a_bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((a_bits >> (kFirstFlagBit + i)) & 1u) != 0;
		set.registered = (a_bits >> kFirstRegisteredBit) & RegisteredMask();
		return set;
	}

	bool Toggles::BeginFrame()
	{
		const auto next = Normalised(requested);
		const auto current = Active();
		if (next == current)
			return false;
		active.store(Pack(next), std::memory_order_relaxed);
		generation.fetch_add(1, std::memory_order_relaxed);
		return EntersClassification(current, next);
	}

	bool OcclusionEnabled(std::uint32_t a_view)
	{
		// Skylighting's RenderOcclusion drives both maps (its hook of the engine's precipitation mask, then its own map).
		if (!globals::features::skylighting.loaded)
			return false;
		return a_view == kOcclusionSky ? ActiveToggles().skyOcclusion : a_view == kOcclusionPrecipitation && ActiveToggles().precipitationOcclusion;
	}
}
