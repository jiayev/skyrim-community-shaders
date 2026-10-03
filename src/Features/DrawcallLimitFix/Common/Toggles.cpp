#include "Toggles.h"

namespace DCLF
{
	namespace
	{
		using S = ToggleSet;
		constexpr std::string_view kOn = "1", kStatic = "static";

		constexpr ToggleInfo kToggles[] = {
			{ &S::ownership, Switch::Ownership, kStatic, "ownership", "Main pass", "Static ownership: withhold claimed passes (CS_DCLF_OWNERSHIP=static)", nullptr, false, {} },
			{ &S::skinned, Switch::Skinned, kOn, "skinned", "Object classes", "Skinned (CS_DCLF_SKINNED)", nullptr, true, {} },
			{ &S::trees, Switch::Trees, kOn, "trees", {}, "Trees (CS_DCLF_TREES)", nullptr, true, {} },
			{ &S::decals, Switch::Decals, kOn, "decals", {}, "Decals (CS_DCLF_DECALS)", nullptr, true, {} },
			{ &S::layers, Switch::Layers, kOn, "multi-index layers", {}, "Multi-index shapes: cave walls with an ice or snow layer (CS_DCLF_LAYERS)", nullptr, true, { &S::decals } },
			{ &S::projectedUv, Switch::ProjectedUv, kOn, "projected", {}, "Projected UV (CS_DCLF_PROJECTED_UV)", nullptr, true, {} },
			{ &S::mtLand, Switch::MtLand, kOn, "terrain", {}, "Terrain (CS_DCLF_MTLAND)", nullptr, true, {} },
			{ &S::switchNodes, Switch::SwitchNodes, kOn, "switch nodes", {}, "Under switch nodes: trees, harvestables (CS_DCLF_SWITCH_NODES)", nullptr, true, {} },
			{ &S::lodObjects, Switch::LodObjects, kOn, "object LOD", {}, "Object LOD: distant objects (CS_DCLF_LOD_OBJECTS)", nullptr, true, {} },
			{ &S::skinPartitions, Switch::SkinPartitions, kOn, "skin partitions", {}, "Skins of several partitions: LOD trees, actor bodies (CS_DCLF_SKIN_PARTITIONS)", nullptr, true, { &S::skinned } },
			{ &S::actors, Switch::Actors, kOn, "actors", {}, "Actors (CS_DCLF_ACTORS)", nullptr, true, {} },
			{ &S::fading, Switch::Fading, kOn, "fading", {}, "Fading objects: the screen-door fade (CS_DCLF_FADING)", nullptr, true, {} },
			{ &S::lodCrossfade, Switch::LodCrossfade, kOn, "LOD cross-fade", {}, "LOD cross-fades: keep the object, leave the copy native (CS_DCLF_LOD_CROSSFADE)", nullptr, true, {} },
			{ &S::shadows, Switch::Shadows, kOn, "shadows", "Shadow views", "Draw the shadow views (CS_DCLF_SHADOWS)", nullptr, false, {} },
			{ &S::shadowOwnership, Switch::ShadowOwnership, kStatic, "shadow ownership", {}, "Static shadow ownership: withhold claimed casters (CS_DCLF_SHADOW_OWNERSHIP=static)", nullptr, false, { &S::shadows } },
			{ &S::skipSunAccumulation, Switch::SunSkip, kOn, "skip sun accumulation", {}, "Skip the engine's sun shadow culling and registration (CS_DCLF_SUN_SKIP)",
				"The engine stops building sun shadow passes for the casters DCLF draws; it still sets their shadow bits for the main pass.", false, { &S::shadowOwnership } },
			// `probe` runs the exclusion dry (SunAccumulation), which needs the toggle on.
			{ &S::excludeSunEntries, Switch::SunExclude, kOn, "exclude sun entries", {}, "Take DCLF's objects out of the engine's sun culls (CS_DCLF_SUN_EXCLUDE)",
				"The sun's cascade culls skip every reference whose shadows DCLF draws entirely; DCLF sets those objects' sun shadow bits for the main pass.", false, { &S::skipSunAccumulation } },
			// `probe` is PrimaryCull's census, which removes nothing (and so leaves the cut off).
			{ &S::excludePrimaryEntries, Switch::PrimaryExclude, kOn, "exclude primary entries", {}, "Take DCLF's objects out of the engine's main camera cull (CS_DCLF_PRIMARY_EXCLUDE)",
				"The main camera's cull and registration skip every reference DCLF draws entirely; DCLF builds their main passes itself and runs their fade updates.", false,
				{ &S::excludeSunEntries, &S::ownership } },
			{ &S::skyOcclusion, Switch::Skylight, kOn, "sky occlusion", {}, "Draw Skylighting's occlusion map (CS_DCLF_SKYLIGHT)",
				"With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
			{ &S::precipitationOcclusion, Switch::Precipitation, kOn, "precipitation occlusion", {}, "Draw the precipitation occlusion mask (CS_DCLF_PRECIPITATION)",
				"With Skylighting loaded, DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
		};

		// The packed word: cullMode in the low two bits, then each flag in kToggles order.
		constexpr std::uint32_t kFirstFlagBit = 2;
		static_assert(kFirstFlagBit + std::size(kToggles) <= 32);

		bool NeedsMet(const ToggleInfo& a_info, const ToggleSet& a_set)
		{
			for (const auto need : a_info.needs)
				if (need && !(a_set.*need))
					return false;
			return true;
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			const auto& cull = SwitchValue(Switch::Cull);
			set.cullMode = (cull.empty() || cull == "occlusion") ? 2 : cull == "frustum" ? 1 : 0;
			for (const auto& toggle : kToggles) {
				const auto& value = SwitchValue(toggle.seed);
				set.*toggle.member = value.empty() || value == toggle.onValue || (toggle.seed == Switch::SunExclude && value == "probe");
			}
			return set;
		}

		/** @brief The combinations that cannot hold: each flag off while one it needs is off (kToggles order resolves chains). */
		ToggleSet Normalised(ToggleSet a_set)
		{
			for (const auto& toggle : kToggles)
				a_set.*toggle.member = a_set.*toggle.member && NeedsMet(toggle, a_set);
			return a_set;
		}

		bool EntersClassification(const ToggleSet& a, const ToggleSet& b)
		{
			for (const auto& toggle : kToggles)
				if (toggle.entersClassification && a.*toggle.member != b.*toggle.member)
					return true;
			return false;
		}
	}

	std::span<const ToggleInfo> ToggleTable()
	{
		return kToggles;
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

	std::uint32_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		return bits;
	}

	ToggleSet Toggles::Unpack(std::uint32_t a_bits)
	{
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(a_bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((a_bits >> (kFirstFlagBit + i)) & 1u) != 0;
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
}
